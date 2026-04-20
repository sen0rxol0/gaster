#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

#include "log.h"
#include "ideviceenterramdisk.h"
#include "ideviceloaders.h"

#include "gastera1n.h"
#include "kernel64patcher.h"

#include <plist/plist.h>
#include <libfragmentzip/libfragmentzip.h>
#include <libirecovery.h>

#include "kerneldiff.c"

typedef struct {
    device_loader loader;
    char *ipsw_url;

    char staging[PATH_MAX];
    char mount[PATH_MAX];

    char kernelcache[PATH_MAX];
    char trustcache[PATH_MAX];
    char ramdisk[PATH_MAX];
    char devicetree[PATH_MAX];
    char ibec[PATH_MAX];
    char ibss[PATH_MAX];

	char kernelcache_img4[PATH_MAX];
	char trustcache_img4[PATH_MAX];
	char ramdisk_img4[PATH_MAX];
	char devicetree_img4[PATH_MAX];
	char ibec_img4[PATH_MAX];
	char ibss_img4[PATH_MAX];
	char bootim_img4[PATH_MAX];

    char im4m[PATH_MAX];
} rdsk_ctx_t;

static const char *ldid2 = "ldid2";
static const char *tsschecker = "tsschecker_macOS_v440";

static void ctx_init(rdsk_ctx_t *ctx)
{
    /* FIX #9: use snprintf throughout to prevent overflow if staging prefix
       were ever made dynamic. Sizes are sizeof each field (all PATH_MAX). */
    snprintf(ctx->staging, sizeof(ctx->staging), "/tmp/gastera1n_rdsk");
    snprintf(ctx->mount,   sizeof(ctx->mount),   "/tmp/gastera1n_rdsk/dmg_mountpoint");
 
    snprintf(ctx->kernelcache, sizeof(ctx->kernelcache), "%s/kernelcache.release", ctx->staging);
    snprintf(ctx->trustcache,  sizeof(ctx->trustcache),  "%s/dmg.trustcache",      ctx->staging);
    snprintf(ctx->ramdisk,     sizeof(ctx->ramdisk),     "%s/ramdisk.dmg",         ctx->staging);
    snprintf(ctx->devicetree,  sizeof(ctx->devicetree),  "%s/DeviceTree.im4p",     ctx->staging);
    snprintf(ctx->ibec,        sizeof(ctx->ibec),        "%s/iBEC.im4p",           ctx->staging);
    snprintf(ctx->ibss,        sizeof(ctx->ibss),        "%s/iBSS.im4p",           ctx->staging);
    snprintf(ctx->im4m,        sizeof(ctx->im4m),        "%s/IM4M",                ctx->staging);
 
    snprintf(ctx->kernelcache_img4, sizeof(ctx->kernelcache_img4), "%s/kernelcache.img4", ctx->staging);
    snprintf(ctx->trustcache_img4,  sizeof(ctx->trustcache_img4),  "%s/trustcache.img4",  ctx->staging);
    snprintf(ctx->ramdisk_img4,     sizeof(ctx->ramdisk_img4),     "%s/rdsk.img4",        ctx->staging);
    snprintf(ctx->devicetree_img4,  sizeof(ctx->devicetree_img4),  "%s/dtree.img4",       ctx->staging);
    snprintf(ctx->bootim_img4,      sizeof(ctx->bootim_img4),      "%s/bootlogo.img4",    ctx->staging);
    snprintf(ctx->ibec_img4,        sizeof(ctx->ibec_img4),        "%s/ibec.img4",        ctx->staging);
    snprintf(ctx->ibss_img4,        sizeof(ctx->ibss_img4),        "%s/ibss.img4",        ctx->staging);
}

static bool run_cmd(const char *fmt, ...)
{
    char cmd[4096];
    va_list args;
    va_start(args, fmt);
    vsnprintf(cmd, sizeof(cmd), fmt, args);
    va_end(args);
 
    log_debug("Executing: %s\n", cmd);
 
    FILE *fp = popen(cmd, "r");
    if (!fp) return false;
 
    char line[256];
    while (fgets(line, sizeof(line), fp))
        fputs(line, stdout);
 
    return pclose(fp) == 0;
}

/* FIX #8: progress is unsigned int; the (progress < 0) guard was dead code
   and triggered -Wtype-limits. Removed the dead branch. */
static void default_prog_cb(unsigned int progress)
{
    if (progress > 100)
        progress = 100;
 
    printf("\r[");
 
    for (unsigned int i = 0; i < 50; i++) {
        if (i < progress / 2)
            printf("=");
        else
            printf(" ");
    }
 
    printf("] %3u%%", progress);
    fflush(stdout);
 
    if (progress == 100)
        printf("\n");
}
 
int dfu_progress_cb(irecv_client_t client, const irecv_event_t *event)
{
    if (event->type == IRECV_PROGRESS) {
        double progress = event->progress;
 
        if (progress < 0) return 0;
        if (progress > 100) progress = 100;
 
        printf("\r[");
        for (int i = 0; i < 50; i++) {
            if (i < progress / 2) printf("=");
            else                   printf(" ");
        }
        printf("] %3.1f%%", progress);
        fflush(stdout);
 
        if (progress == 100) printf("\n");
    }
    return 0;
}
 
static char *dup_printf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int len = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
 
    char *buf = malloc(len + 1);
    if (!buf) return NULL;
    va_start(ap, fmt);
    vsnprintf(buf, len + 1, fmt, ap);
    va_end(ap);
 
    return buf;
}
 
static irecv_client_t dfu_open_client(void)
{
    uint64_t ecid = 0;
    irecv_client_t client = NULL;
 
    for (int i = 0; i <= 5; i++) {
        log_debug("Attempting to connect...\n");
 
        irecv_error_t err = irecv_open_with_ecid(&client, ecid);
        if (err == IRECV_E_UNSUPPORTED) {
            log_error("%s\n", irecv_strerror(err));
            return NULL;
        } else if (err != IRECV_E_SUCCESS) {
            sleep(1);
        } else {
            break;
        }
 
        if (i == 5) {
            log_error("%s\n", irecv_strerror(err));
            return NULL;
        }
    }
 
    return client;
}

int dfu_wait_for_device(void)
{
    log_info("Searching for DFU mode device...");
 
    irecv_device_t device = NULL;
    irecv_client_t client = dfu_open_client();
 
    /* FIX #2: guard against NULL client before any use */
    if (!client)
        return -1;
 
    irecv_devices_get_device_by_client(client, &device);
 
    /* FIX #5: close client whether or not device was found */
    if (!device) {
        irecv_close(client);
        return -1;
    }
 
    irecv_error_t error = irecv_setenv(client, "auto-boot", "true");
    if (error != IRECV_E_SUCCESS)
        log_error("%s\n", irecv_strerror(error));
 
    error = irecv_saveenv(client);
    if (error != IRECV_E_SUCCESS)
        log_error("%s\n", irecv_strerror(error));
 
    irecv_close(client);
    return 0;
}

/* FIX #1 (caller side): callers must free() the returned string.
   FIX #2: guard against NULL client / devinfo before dereferencing. */
char *dfu_get_info(const char *t)
{
    log_debug("Getting device info: %s", t);
 
    irecv_client_t client = dfu_open_client();
    if (!client) {
        log_error("dfu_get_info: could not open client\n");
        return NULL;
    }
 
    const struct irecv_device_info *devinfo = irecv_get_device_info(client);
    irecv_device_t device = NULL;
    irecv_devices_get_device_by_client(client, &device);
 
    char *info = NULL;
 
    if (!strcmp(t, "ecid") && devinfo)
        info = dup_printf("0x%016llX", devinfo->ecid);
 
    else if (!strcmp(t, "cpid") && devinfo)
        info = dup_printf("0x%04X", devinfo->cpid);
 
    else if (!strcmp(t, "product_type") && device)
        info = strdup(device->product_type);
 
    else if (!strcmp(t, "model") && device)
        info = strdup(device->hardware_model);
 
    irecv_close(client);
 
    if (info)
        log_debug("%s\n", info);
    else
        log_error("dfu_get_info: unknown key or missing device info: %s\n", t);
 
    return info;
}
 
/* FIX #2: guard against NULL client. */
int dfu_send_file(const char *filepath)
{
    irecv_client_t client = dfu_open_client();
    if (!client) return -1;
 
    irecv_event_subscribe(client, IRECV_PROGRESS, &dfu_progress_cb, NULL);
    irecv_error_t err = irecv_send_file(client, filepath,
                                        IRECV_SEND_OPT_DFU_NOTIFY_FINISH);
    irecv_close(client);
    return err == IRECV_E_SUCCESS ? 0 : -1;
}
 
/* FIX #2: guard against NULL client. */
int dfu_send_cmd(const char *command)
{
    irecv_client_t client = dfu_open_client();
    if (!client) return -1;
 
    irecv_error_t err = irecv_send_command(client, command);
    irecv_close(client);
    return err == IRECV_E_SUCCESS ? 0 : -1;
}

/* FIX #4: check plist parse result, check that ticket key exists,
   avoid writing an empty file on parse failure, free allocated memory. */
void im4m_from_shsh(char *path, char *im4m_path)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        log_error("im4m_from_shsh: cannot open %s\n", path);
        return;
    }
 
    fseek(f, 0, SEEK_END);
    size_t size = (size_t)ftell(f);
    fseek(f, 0, SEEK_SET);
 
    char *data = malloc(size);
    if (!data) {
        log_error("im4m_from_shsh: out of memory\n");
        fclose(f);
        return;
    }
 
    if (fread(data, 1, size, f) != size) {
        log_error("im4m_from_shsh: short read on %s\n", path);
        free(data);
        fclose(f);
        return;
    }
    fclose(f);
 
    plist_t shsh_plist = NULL;
    plist_from_memory(data, (uint32_t)size, &shsh_plist, NULL);
    free(data);
 
    if (!shsh_plist) {
        log_error("im4m_from_shsh: failed to parse plist from %s\n", path);
        return;
    }
 
    plist_t ticket = plist_dict_get_item(shsh_plist, "ApImg4Ticket");
    if (!ticket) {
        log_error("im4m_from_shsh: ApImg4Ticket key not found in %s\n", path);
        plist_free(shsh_plist);
        return;
    }
 
    char *im4m = NULL;
    uint64_t im4m_size = 0;
    plist_get_data_val(ticket, &im4m, &im4m_size);
 
    if (!im4m || im4m_size == 0) {
        log_error("im4m_from_shsh: ApImg4Ticket is empty\n");
        plist_free(shsh_plist);
        return;
    }
 
    f = fopen(im4m_path, "wb");
    if (!f) {
        log_error("im4m_from_shsh: cannot write %s\n", im4m_path);
        free(im4m);
        plist_free(shsh_plist);
        return;
    }
 
    fwrite(im4m, 1, im4m_size, f);
    fclose(f);
 
    free(im4m);
    plist_free(shsh_plist);
}

/* FIX #7: if gunzip succeeds but chmod fails the file is left non-executable
   but present, so subsequent access() checks pass silently.  We now verify
   the file is actually executable after the pipeline. */
static int ensure_tool(const char *tool)
{
    if (access(tool, X_OK) == 0)
        return 0;
 
    /* File may exist but not be executable — run the setup pipeline. */
    bool ok = run_cmd(
        "gunzip -f %s.gz && "
        "xattr -d com.apple.quarantine %s >/dev/null 2>&1; "
        "chmod +x %s",
        tool, tool, tool
    );
 
    if (!ok) {
        log_error("ensure_tool: failed to prepare %s\n", tool);
        return -1;
    }
 
    /* Verify the result is actually executable. */
    if (access(tool, X_OK) != 0) {
        log_error("ensure_tool: %s still not executable after setup\n", tool);
        return -1;
    }
 
    return 0;
}

static int stage_ensure_tools(void)
{
    if (ensure_tool("tsschecker_macOS_v440")) return -1;

    if (access("iBoot64Patcher.gz", F_OK) == 0)
        if (ensure_tool("iBoot64Patcher")) return -1;

    if (ensure_tool("ldid2")) return -1;

    if (access("restored_external.gz", F_OK) == 0)
        if (!run_cmd("gunzip restored_external.gz")) return -1;

    return 0;
}

/* FIX #1: free strings returned by dfu_get_info(). */
static int stage_prepare(rdsk_ctx_t *ctx)
{
    char *identifier = dfu_get_info("product_type");
    if (!identifier) {
        log_error("stage_prepare: could not get product type\n");
        return -1;
    }
 
    int found = 0;
    for (int i = 0; device_loaders[i].identifier; i++) {
        if (!strcmp(device_loaders[i].identifier, identifier)) {
            ctx->loader = device_loaders[i];
            ctx->ipsw_url = (char *)ctx->loader.ipsw_url;
            found = 1;
            break;
        }
    }
 
    if (!found) {
        log_error("Unsupported device: %s\n", identifier);
        free(identifier);
        return -1;
    }
 
    free(identifier);
 
    if (!run_cmd("bash -c 'rm -rf %s && mkdir -p %s %s'",
                 ctx->staging, ctx->mount, ctx->staging))
        return -1;
 
    if (!run_cmd("cp bootim@750x1334.im4p %s/", ctx->staging))
        return -1;
 
    return 0;
}
 
static int download_component(rdsk_ctx_t *ctx,
                              const char *remote,
                              const char *local)
{
    fragmentzip_t *ipsw = fragmentzip_open(ctx->ipsw_url);
    if (!ipsw) return -1;
 
    log_info("Downloading %s", remote);
    int r = fragmentzip_download_file(ipsw, remote, local, default_prog_cb);
    fragmentzip_close(ipsw);
    return r;
}
 
static int stage_download_images(rdsk_ctx_t *ctx)
{
    struct {
        const char *remote;
        const char *local;
    } files[] = {
        { ctx->loader.kernelcache_path, ctx->kernelcache },
        { ctx->loader.trustcache_path,  ctx->trustcache  },
        { ctx->loader.ramdisk_path,     ctx->ramdisk     },
        { ctx->loader.devicetree_path,  ctx->devicetree  },
        { ctx->loader.ibec_path,        ctx->ibec        },
        { ctx->loader.ibss_path,        ctx->ibss        },
        { NULL, NULL }
    };
 
    for (int i = 0; files[i].remote; i++)
        if (download_component(ctx, files[i].remote, files[i].local))
            return -1;
 
    return 0;
}

/* FIX #3: run_cmd() returns true on success, false on failure.
   The original code used `if (run_cmd(...)) return -1` which returned -1
   on SUCCESS. All img4 decrypt calls now use `if (!run_cmd(...)) return -1`.
 
   FIX #3 also: the iBEC/iBSS calls used gastera1n_decrypt() which follows
   normal C convention (0 = success), so those were correct and are unchanged. */
static int stage_decrypt(rdsk_ctx_t *ctx)
{
    const char *list[] = {
        ctx->kernelcache,
        ctx->trustcache,
        ctx->ramdisk,
        ctx->devicetree,
        NULL
    };
 
    for (int i = 0; list[i]; i++) {
        if (!run_cmd("img4 -i %s -o %s.dec", list[i], list[i]))
            return -1;
    }
 
    char dest[PATH_MAX];
    snprintf(dest, sizeof(dest), "%s.dec", ctx->ibec);
    if (gastera1n_decrypt(ctx->ibec, dest)) return -1;
 
    snprintf(dest, sizeof(dest), "%s.dec", ctx->ibss);
    return gastera1n_decrypt(ctx->ibss, dest);
}

static void build_shsh_path(rdsk_ctx_t *ctx, char *out)
{
    snprintf(out, PATH_MAX, "%s/latest.shsh2", ctx->staging);
}

/* FIX #1: free strings returned by dfu_get_info().
   FIX #6: use a glob-safe rename: sort by modification time and pick the
   newest .shsh2 rather than relying on shell glob expansion which breaks
   when multiple files are present. */
static int stage_get_shsh(rdsk_ctx_t *ctx)
{
    char shsh[PATH_MAX];
    build_shsh_path(ctx, shsh);
 
    if (access(shsh, F_OK) == 0)
        return 0;
 
    char *ecid  = dfu_get_info("ecid");
    char *ptype = dfu_get_info("product_type");
    char *model = dfu_get_info("model");
 
    if (!ecid || !ptype || !model) {
        log_error("stage_get_shsh: failed to read device info\n");
        free(ecid); free(ptype); free(model);
        return -1;
    }
 
    bool ok = run_cmd(
        "./tsschecker_macOS_v440 -e %s -d %s -B %s -b -l -s --save-path %s",
        ecid, ptype, model, ctx->staging
    );
 
    free(ecid); free(ptype); free(model);
 
    if (!ok) return -1;
 
    /* Move the newest .shsh2 file to the canonical path.
       'ls -t' sorts by time; head -1 picks the newest. */
    return run_cmd(
        "f=$(ls -t %s/*.shsh2 2>/dev/null | head -1) && "
        "[ -n \"$f\" ] && mv \"$f\" %s",
        ctx->staging, shsh
    ) ? 0 : -1;
}

static int stage_patch_kernel(rdsk_ctx_t *ctx)
{
    char kdec[PATH_MAX];
    char kpwn[PATH_MAX];
    char diff[PATH_MAX];
 
    snprintf(kdec, sizeof(kdec), "%s.dec", ctx->kernelcache);
    snprintf(kpwn, sizeof(kpwn), "%s.pwn", ctx->kernelcache);
    snprintf(diff, sizeof(diff), "%s/kc.bpatch", ctx->staging);
 
    if (kernel64patcher_amfi(kdec, kpwn))
        return -1;
 
    if (kerneldiff(kdec, kpwn, diff))
        return -1;
 
    return 0;
}
 
static int patch_iboot(const char *path, const char *extra)
{
    char in[PATH_MAX], out[PATH_MAX];
    snprintf(in,  sizeof(in),  "%s.dec", path);
    snprintf(out, sizeof(out), "%s.pwn", path);
 
    if (extra)
        return run_cmd("./iBoot64Patcher %s %s %s", in, out, extra) ? 0 : -1;
    else
        return run_cmd("./iBoot64Patcher %s %s", in, out) ? 0 : -1;
}
 
static int stage_patch_iboot(rdsk_ctx_t *ctx)
{
    if (patch_iboot(ctx->ibec, "-n -b \"rd=md0 -v\""))
        return -1;
 
    return patch_iboot(ctx->ibss, NULL);
}

static int patch_restored_external_in_ramdisk(rdsk_ctx_t *ctx)
{
    return run_cmd(
        "cd %s && "
        "cp restored_external restored_external_hax && "
        "./ldid2 -e %s/usr/local/bin/restored_external > restored_external.plist && "
        "./ldid2 -M -Srestored_external.plist restored_external_hax && "
        "mv restored_external_hax %s/usr/local/bin/restored_external && "
        "rm restored_external.plist",
        ctx->staging,
        ctx->mount,
        ctx->mount
    ) ? 0 : -1;
}

/* FIX #10: detach the DMG on every early-return failure path after attach,
   to prevent leaving the mountpoint in a stale mounted state. */
static int stage_build_ramdisk(rdsk_ctx_t *ctx)
{
    log_info("Building ramdisk...");
 
    /* merge split ssh archive if needed */
    if (access("ssh64.tar.gz", F_OK) != 0)
        if (!run_cmd("cat ssh64.tar.gz* > ssh64.tar.gz"))
            return -1;
 
    /* create working dmg */
    if (!run_cmd(
        "cp %s.dec %s/rdsk.dmg && "
        "hdiutil resize -size 180MB %s/rdsk.dmg",
        ctx->ramdisk, ctx->staging, ctx->staging))
        return -1;
 
    /* mount */
    if (!run_cmd("hdiutil attach %s/rdsk.dmg -mountpoint %s",
                 ctx->staging, ctx->mount))
        return -1;
 
    /* All steps below this point must go through 'fail' to ensure detach. */
#define RDSK_FAIL(msg) do { log_error(msg "\n"); goto fail; } while (0)
 
    /* basic filesystem prep */
    if (!run_cmd(
        "echo 'WELCOME BACK!' > %s/etc/motd && "
        "mkdir -p %s/private/var/root "
                  "%s/private/var/run "
                  "%s/sshd",
        ctx->mount, ctx->mount, ctx->mount, ctx->mount))
        RDSK_FAIL("stage_build_ramdisk: filesystem prep failed");
 
    /* extract SSH payload */
    if (!run_cmd("tar -C %s/sshd --preserve-permissions -xf ssh64.tar.gz",
                 ctx->mount))
        RDSK_FAIL("stage_build_ramdisk: tar extract failed");
 
    /* install SSH payload into root */
    if (!run_cmd(
        "cd %s/sshd && "
        "chmod 0755 bin/* usr/bin/* usr/sbin/* usr/local/bin/* && "
        "rsync --ignore-existing -auK . %s/",
        ctx->mount, ctx->mount))
        RDSK_FAIL("stage_build_ramdisk: rsync failed");
 
    /* cleanup unneeded files */
    if (!run_cmd(
        "rm -rf %s/sshd "
                "%s/usr/local/standalone/firmware/* "
                "%s/usr/share/progressui "
                "%s/usr/share/terminfo "
                "%s/etc/apt "
                "%s/etc/dpkg",
        ctx->mount, ctx->mount, ctx->mount,
        ctx->mount, ctx->mount, ctx->mount))
        RDSK_FAIL("stage_build_ramdisk: cleanup failed");
 
    if (patch_restored_external_in_ramdisk(ctx))
        RDSK_FAIL("stage_build_ramdisk: patch_restored_external failed");
 
    /* detach dmg */
    if (!run_cmd("hdiutil detach -force %s", ctx->mount))
        RDSK_FAIL("stage_build_ramdisk: hdiutil detach failed");
 
    /* shrink to minimal */
    if (!run_cmd("hdiutil resize -sectors min %s/rdsk.dmg", ctx->staging)) {
        log_error("stage_build_ramdisk: hdiutil resize failed\n");
        return -1;
    }
 
#undef RDSK_FAIL
    return 0;
 
fail:
    run_cmd("hdiutil detach -force %s", ctx->mount);
    return -1;
}

static int stage_build_img4(rdsk_ctx_t *ctx)
{
    char shsh[PATH_MAX];
    build_shsh_path(ctx, shsh);
    im4m_from_shsh(shsh, ctx->im4m);
 
    return run_cmd(
        "cd %s && "
        "img4 -i %s -o kernelcache.img4 -P kc.bpatch -M IM4M -T rkrn && "
        "img4 -i %s -o trustcache.img4 -M IM4M -T rtsc && "
        "img4 -i rdsk.dmg -o rdsk.img4 -M IM4M -A -T rdsk && "
        "img4 -i %s -o dtree.img4 -M IM4M -T rdtr && "
        "img4 -i bootim@750x1334.im4p -o bootlogo.img4 -A -M IM4M -T rlgo && "
        "img4 -i %s.pwn -o ibec.img4 -A -M IM4M -T ibec && "
        "img4 -i %s.pwn -o ibss.img4 -A -M IM4M -T ibss",
        ctx->staging,
        ctx->kernelcache,
        ctx->trustcache,
        ctx->devicetree,
        ctx->ibec,
        ctx->ibss
    ) ? 0 : -1;
}

static int stage_patch(rdsk_ctx_t *ctx)
{
    log_info("Patching images...");

    if (stage_ensure_tools()) return -1;
    if (stage_get_shsh(ctx)) return -1;
    if (stage_patch_kernel(ctx)) return -1;
    if (stage_patch_iboot(ctx)) return -1;
    if (stage_build_ramdisk(ctx)) return -1;
    if (stage_build_img4(ctx)) return -1;

    return 0;
}

static int stage_boot_ramdisk(rdsk_ctx_t *ctx)
{
    log_info("Waiting for DFU device...");
    if (dfu_wait_for_device() != 0)
        return -1;
 
    log_info("Booting SSH ramdisk...");
 
    if (gastera1n_reset() != 0)
        return -1;
 
    sleep(2);
 
    if (dfu_send_file(ctx->ibss_img4)) return -1;
    sleep(1);
 
    if (dfu_send_file(ctx->ibec_img4)) return -1;
    sleep(1);
 
    if (dfu_send_cmd("go")) return -1;
    sleep(5);
 
    if (dfu_send_file(ctx->bootim_img4))    return -1;
    if (dfu_send_cmd("setpicture 0x1"))     return -1;
    if (dfu_send_cmd("bgcolor 255 55 55"))  return -1;
 
    if (dfu_send_file(ctx->devicetree_img4)) return -1;
    if (dfu_send_cmd("devicetree"))          return -1;
 
    if (dfu_send_file(ctx->ramdisk_img4)) return -1;
    if (dfu_send_cmd("ramdisk"))          return -1;
 
    if (dfu_send_file(ctx->trustcache_img4)) return -1;
    if (dfu_send_cmd("firmware"))            return -1;
 
    if (dfu_send_file(ctx->kernelcache_img4)) return -1;
    if (dfu_send_cmd("bootx"))                return -1;
 
    log_info("Device should be booting now.");
    return 0;
}

int ideviceenterramdisk_load(void)
{
    rdsk_ctx_t ctx;
    ctx_init(&ctx);

    if (ramdiskBootMode == 1)
        return stage_boot_ramdisk(&ctx);

    if (stage_prepare(&ctx))  return -1;
    if (stage_download_images(&ctx))    return -1;
    if (stage_decrypt(&ctx))            return -1;
    if (stage_patch(&ctx))              return -1;

    return stage_boot_ramdisk(&ctx);
}
