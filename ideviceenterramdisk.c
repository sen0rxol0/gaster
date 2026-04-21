#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/param.h>
#include <zlib.h>
#ifdef __APPLE__
#include <sys/xattr.h>
#endif

#include "log.h"
#include "ideviceenterramdisk.h"
#include "ideviceloaders.h"

#include "gastera1n.h"
#include "kernel64patcher.h"

#include <plist/plist.h>
#include <libfragmentzip/libfragmentzip.h>
#include <libirecovery.h>

#include "kerneldiff.c"


/* ── Native C helpers (replace shell popen calls) ──────────────────────── */

/* Recursively remove a directory tree (mirrors `rm -rf`). */
static int rm_rf(const char *path)
{
    struct stat st;
    if (lstat(path, &st) != 0)
        return (errno == ENOENT) ? 0 : -1;

    if (!S_ISDIR(st.st_mode))
        return unlink(path);

    DIR *d = opendir(path);
    if (!d) return -1;

    struct dirent *ent;
    int ret = 0;
    while ((ent = readdir(d)) != NULL) {
        if (!strcmp(ent->d_name, ".") || !strcmp(ent->d_name, ".."))
            continue;
        char child[PATH_MAX];
        snprintf(child, sizeof(child), "%s/%s", path, ent->d_name);
        if (rm_rf(child) != 0) { ret = -1; break; }
    }
    closedir(d);
    return (ret == 0) ? rmdir(path) : -1;
}

/* Create a directory and all missing parents (mirrors `mkdir -p`).
   Returns 0 if the directory exists or was created, -1 on error. */
static int mkdir_p(const char *path, mode_t mode)
{
    char tmp[PATH_MAX];
    snprintf(tmp, sizeof(tmp), "%s", path);
    size_t len = strlen(tmp);
    if (len && tmp[len - 1] == '/')
        tmp[--len] = '\0';

    for (char *p = tmp + 1; *p; p++) {
        if (*p != '/') continue;
        *p = '\0';
        if (mkdir(tmp, mode) != 0 && errno != EEXIST) return -1;
        *p = '/';
    }
    return (mkdir(tmp, mode) != 0 && errno != EEXIST) ? -1 : 0;
}

/* Copy src → dst as a plain binary file.  Returns 0 on success, -1 on error.
   Mirrors `cp src dst`. */
static int file_copy(const char *src, const char *dst)
{
    FILE *in = fopen(src, "rb");
    if (!in) {
        log_error("file_copy: cannot open %s: %s\n", src, strerror(errno));
        return -1;
    }
    FILE *out = fopen(dst, "wb");
    if (!out) {
        log_error("file_copy: cannot create %s: %s\n", dst, strerror(errno));
        fclose(in);
        return -1;
    }

    char buf[65536];
    size_t n;
    int ret = 0;
    while ((n = fread(buf, 1, sizeof(buf), in)) > 0) {
        if (fwrite(buf, 1, n, out) != n) {
            log_error("file_copy: write error on %s: %s\n", dst, strerror(errno));
            ret = -1;
            break;
        }
    }
    if (!feof(in)) { ret = -1; }

    /* Preserve source permissions on the copy. */
    struct stat st;
    if (ret == 0 && fstat(fileno(in), &st) == 0)
        fchmod(fileno(out), st.st_mode & 0777);

    fclose(in);
    fclose(out);
    return ret;
}

/* Decompress a gzip-compressed file src.gz → dst.
   Removes the .gz source on success (mirrors `gunzip src.gz`).
   Returns 0 on success, -1 on error. */
static int gunzip_file(const char *gz_path, const char *out_path)
{
    gzFile gz = gzopen(gz_path, "rb");
    if (!gz) {
        log_error("gunzip_file: cannot open %s\n", gz_path);
        return -1;
    }
    FILE *out = fopen(out_path, "wb");
    if (!out) {
        log_error("gunzip_file: cannot create %s: %s\n", out_path, strerror(errno));
        gzclose(gz);
        return -1;
    }

    char buf[65536];
    int n;
    int ret = 0;
    while ((n = gzread(gz, buf, sizeof(buf))) > 0) {
        if (fwrite(buf, 1, (size_t)n, out) != (size_t)n) {
            log_error("gunzip_file: write error on %s\n", out_path);
            ret = -1;
            break;
        }
    }
    if (n < 0) {
        int zerr;
        log_error("gunzip_file: decompress error: %s\n", gzerror(gz, &zerr));
        ret = -1;
    }

    fclose(out);
    gzclose(gz);

    if (ret == 0)
        unlink(gz_path); /* mirror gunzip behaviour: remove the .gz */
    else
        unlink(out_path); /* clean up partial output */

    return ret;
}

/* Make a file executable and strip the macOS quarantine xattr.
   Mirrors `chmod +x path && xattr -d com.apple.quarantine path`. */
static int make_executable(const char *path)
{
    struct stat st;
    if (stat(path, &st) != 0) {
        log_error("make_executable: stat failed for %s: %s\n", path, strerror(errno));
        return -1;
    }
    if (chmod(path, st.st_mode | S_IXUSR | S_IXGRP | S_IXOTH) != 0) {
        log_error("make_executable: chmod failed for %s: %s\n", path, strerror(errno));
        return -1;
    }
#ifdef __APPLE__
    /* Ignore ENOATTR — the xattr may not be present. */
    if (removexattr(path, "com.apple.quarantine", 0) != 0 && errno != ENOATTR)
        log_error("make_executable: removexattr warning for %s: %s\n", path, strerror(errno));
#endif
    return 0;
}

/* Write a single line to a file (mirrors `echo 'text' > path`). */
static int file_write_line(const char *path, const char *line)
{
    FILE *f = fopen(path, "w");
    if (!f) {
        log_error("file_write_line: cannot open %s: %s\n", path, strerror(errno));
        return -1;
    }
    int ret = (fputs(line, f) == EOF) ? -1 : 0;
    if (ret == 0 && fputc('\n', f) == EOF) ret = -1;
    fclose(f);
    return ret;
}

/* Find the newest file matching a prefix+suffix in a directory.
   Fills best_path (size PATH_MAX) with the winner.
   Returns 0 if found, -1 if nothing matched. */
static int find_newest_file(const char *dir,
                             const char *suffix,
                             char *best_path)
{
    DIR *d = opendir(dir);
    if (!d) return -1;

    struct dirent *ent;
    time_t best_mtime = -1;
    best_path[0] = '\0';

    while ((ent = readdir(d)) != NULL) {
        size_t nlen = strlen(ent->d_name);
        size_t slen = strlen(suffix);
        if (nlen < slen) continue;
        if (strcmp(ent->d_name + nlen - slen, suffix) != 0) continue;

        char full[PATH_MAX];
        snprintf(full, sizeof(full), "%s/%s", dir, ent->d_name);
        struct stat st;
        if (stat(full, &st) != 0) continue;
        if (st.st_mtime > best_mtime) {
            best_mtime = st.st_mtime;
            snprintf(best_path, PATH_MAX, "%s", full);
        }
    }
    closedir(d);
    return (best_path[0] != '\0') ? 0 : -1;
}

/* ── End native C helpers ───────────────────────────────────────────────── */
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

/* Tool binary names — single source of truth used in all run_cmd() calls
   that invoke these executables, eliminating the "unused variable" warning
   that the bare string literals caused under -Weverything. */
#define TOOL_LDID2       "ldid2"
#define TOOL_TSSCHECKER  "tsschecker_macOS_v440"
#define TOOL_IBOOT64PATCHER  "iBoot64Patcher"

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
    /* FIX #3c: declare err before the loop so it remains in scope for the
       i==5 error path; declaring inside the loop body left it out of scope
       at the bottom-of-loop check, which is undefined behaviour. */
    irecv_error_t err = IRECV_E_SUCCESS;

    for (int i = 0; i <= 5; i++) {
        log_debug("Attempting to connect...\n");

        err = irecv_open_with_ecid(&client, ecid);
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
static int im4m_from_shsh(const char *path, const char *im4m_path)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        log_error("im4m_from_shsh: cannot open %s\n", path);
        return -1;
    }

    fseek(f, 0, SEEK_END);
    size_t size = (size_t)ftell(f);
    fseek(f, 0, SEEK_SET);

    char *data = malloc(size);
    if (!data) {
        log_error("im4m_from_shsh: out of memory\n");
        fclose(f);
        return -1;
    }

    if (fread(data, 1, size, f) != size) {
        log_error("im4m_from_shsh: short read on %s\n", path);
        free(data);
        fclose(f);
        return -1;
    }
    fclose(f);

    plist_t shsh_plist = NULL;
    plist_from_memory(data, (uint32_t)size, &shsh_plist, NULL);
    free(data);

    if (!shsh_plist) {
        log_error("im4m_from_shsh: failed to parse plist from %s\n", path);
        return -1;
    }

    plist_t ticket = plist_dict_get_item(shsh_plist, "ApImg4Ticket");
    if (!ticket) {
        log_error("im4m_from_shsh: ApImg4Ticket key not found in %s\n", path);
        plist_free(shsh_plist);
        return -1;
    }

    char *im4m = NULL;
    uint64_t im4m_size = 0;
    plist_get_data_val(ticket, &im4m, &im4m_size);

    if (!im4m || im4m_size == 0) {
        log_error("im4m_from_shsh: ApImg4Ticket is empty\n");
        plist_free(shsh_plist);
        return -1;
    }

    f = fopen(im4m_path, "wb");
    if (!f) {
        log_error("im4m_from_shsh: cannot write %s\n", im4m_path);
        free(im4m);
        plist_free(shsh_plist);
        return -1;
    }

    if (fwrite(im4m, 1, im4m_size, f) != im4m_size) {
        log_error("im4m_from_shsh: short write to %s\n", im4m_path);
        fclose(f);
        free(im4m);
        plist_free(shsh_plist);
        return -1;
    }
    fclose(f);

    free(im4m);
    plist_free(shsh_plist);
    return 0;
}

/* Native C replacement for:
     gunzip tool.gz && xattr -d com.apple.quarantine tool && chmod +x tool
   Uses zlib (gunzip_file) + make_executable helper — no shell subprocess. */
static int ensure_tool(const char *tool)
{
    if (access(tool, X_OK) == 0)
        return 0;

    char gz_path[PATH_MAX];
    snprintf(gz_path, sizeof(gz_path), "%s.gz", tool);

    if (access(gz_path, F_OK) == 0) {
        log_info("Decompressing %s.gz", tool);
        if (gunzip_file(gz_path, tool) != 0) {
            log_error("ensure_tool: failed to decompress %s.gz\n", tool);
            return -1;
        }
    }

    if (make_executable(tool) != 0) {
        log_error("ensure_tool: failed to make %s executable\n", tool);
        return -1;
    }

    if (access(tool, X_OK) != 0) {
        log_error("ensure_tool: %s still not executable after setup\n", tool);
        return -1;
    }

    return 0;
}

static int stage_ensure_tools(void)
{
    if (ensure_tool(TOOL_TSSCHECKER)) return -1;
    
    if (ensure_tool(TOOL_LDID2)) return -1;
    
    if (ensure_tool(TOOL_IBOOT64PATCHER)) return -1;

    if (access("restored_external.gz", F_OK) == 0) {
        if (gunzip_file("restored_external.gz", "restored_external") != 0) {
            log_error("stage_ensure_tools: failed to decompress restored_external.gz\n");
            return -1;
        }
    }

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

    /* Native C: remove stale staging dir, recreate staging + mountpoint. */
    if (rm_rf(ctx->staging) != 0 && errno != ENOENT) {
        log_error("stage_prepare: failed to remove %s\n", ctx->staging);
        return -1;
    }
    if (mkdir_p(ctx->staging, 0755) != 0) {
        log_error("stage_prepare: failed to create %s\n", ctx->staging);
        return -1;
    }
    if (mkdir_p(ctx->mount, 0755) != 0) {
        log_error("stage_prepare: failed to create %s\n", ctx->mount);
        return -1;
    }

    /* Native C: copy boot image into staging directory. */
    char bootim_dst[PATH_MAX];
    snprintf(bootim_dst, sizeof(bootim_dst),
             "%s/bootim@750x1334.im4p", ctx->staging);
    if (file_copy("bootim@750x1334.im4p", bootim_dst) != 0) {
        log_error("stage_prepare: failed to copy boot image\n");
        return -1;
    }

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
        if (!run_cmd("./img4 -i %s -o %s.dec", list[i], list[i]))
            return -1;
    }

    char out[PATH_MAX];
    snprintf(out, sizeof(out), "%s.dec", ctx->ibec);
    if (gastera1n_decrypt(ctx->ibec, out)) return -1;

    snprintf(out, sizeof(out), "%s.dec", ctx->ibss);
    return gastera1n_decrypt(ctx->ibss, out);
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

    bool ok = run_cmd("./%s -e %s -d %s -B %s -b -l -s --save-path %s",
        TOOL_TSSCHECKER, ecid, ptype, model, ctx->staging
    );

    free(ecid); free(ptype); free(model);

    if (!ok) return -1;

    /* Native C: find the newest .shsh2 in staging, rename to canonical path.
       Replaces the fragile `ls -t | head -1 | mv` shell pipeline. */
    char newest[PATH_MAX];
    if (find_newest_file(ctx->staging, ".shsh2", newest) != 0) {
        log_error("stage_get_shsh: no .shsh2 file found in %s\n", ctx->staging);
        return -1;
    }
    if (rename(newest, shsh) != 0) {
        log_error("stage_get_shsh: rename %s -> %s failed: %s\n",
                  newest, shsh, strerror(errno));
        return -1;
    }
    return 0;
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
        return run_cmd("./%s %s %s %s", TOOL_IBOOT64PATCHER, in, out, extra) ? 0 : -1;
    else
        return run_cmd("./%s %s %s", TOOL_IBOOT64PATCHER, in, out) ? 0 : -1;
}

static int stage_patch_iboot(rdsk_ctx_t *ctx)
{
    if (patch_iboot(ctx->ibec, "-n -b \"rd=md0 -v\""))
        return -1;

    return patch_iboot(ctx->ibss, NULL);
}

static int patch_restored_external_in_ramdisk(rdsk_ctx_t *ctx)
{
    char hax[PATH_MAX], plist[PATH_MAX], dst_bin[PATH_MAX];
    snprintf(hax,     sizeof(hax),     "%s/restored_external_hax",       ctx->staging);
    snprintf(plist,   sizeof(plist),   "%s/restored_external.plist",      ctx->staging);
    snprintf(dst_bin, sizeof(dst_bin), "%s/usr/local/bin/restored_external", ctx->mount);

    /* Native C: copy the local restored_external binary as the working copy. */
    if (file_copy("restored_external", hax) != 0) {
        log_error("patch_restored_external: failed to copy binary\n");
        return -1;
    }

    /* ldid2 operations stay as shell calls — external binary. */
    if (!run_cmd("./%s -e %s > %s", TOOL_LDID2, dst_bin, plist)) {
        log_error("patch_restored_external: ldid2 -e failed\n");
        unlink(hax);
        return -1;
    }
    if (!run_cmd("./%s -M -S%s %s", TOOL_LDID2, plist, hax)) {
        log_error("patch_restored_external: ldid2 -M failed\n");
        unlink(hax); unlink(plist);
        return -1;
    }

    /* Native C: move patched binary to destination, clean up plist. */
    if (rename(hax, dst_bin) != 0) {
        log_error("patch_restored_external: rename failed: %s\n", strerror(errno));
        unlink(hax); unlink(plist);
        return -1;
    }
    unlink(plist);
    return 0;
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

    /* Native C: copy decrypted ramdisk image to working copy. */
    char rdsk_dec[PATH_MAX], rdsk_dmg[PATH_MAX];
    snprintf(rdsk_dec, sizeof(rdsk_dec), "%s.dec", ctx->ramdisk);
    snprintf(rdsk_dmg, sizeof(rdsk_dmg), "%s/rdsk.dmg", ctx->staging);
    if (file_copy(rdsk_dec, rdsk_dmg) != 0) {
        log_error("stage_build_ramdisk: failed to copy ramdisk image\n");
        return -1;
    }

    /* Resize the DMG to hold the SSH payload (requires hdiutil). */
    if (!run_cmd("hdiutil resize -size 180MB %s", rdsk_dmg))
        return -1;
    
    sleep(3);
    
    /* mount */
    if (!run_cmd("hdiutil attach %s -mountpoint %s", rdsk_dmg, ctx->mount))
        return -1;

    sleep(3);
    
    /* All steps below this point must go through 'fail' to ensure detach. */
#define RDSK_FAIL(msg) do { log_error(msg "\n"); goto fail; } while (0)

    /* Native C: create required directories and write motd. */
    {
        char motd_path[PATH_MAX];
        snprintf(motd_path, sizeof(motd_path), "%s/etc/motd", ctx->mount);
        if (file_write_line(motd_path, "WELCOME BACK!") != 0)
            RDSK_FAIL("stage_build_ramdisk: failed to write motd");

        char pvr[PATH_MAX], pvrn[PATH_MAX], sshd[PATH_MAX];
        snprintf(pvr,  sizeof(pvr),  "%s/private/var/root", ctx->mount);
        snprintf(pvrn, sizeof(pvrn), "%s/private/var/run",  ctx->mount);
        snprintf(sshd, sizeof(sshd), "%s/sshd",             ctx->mount);
        if (mkdir_p(pvr,  0755) != 0) RDSK_FAIL("stage_build_ramdisk: mkdir private/var/root failed");
        if (mkdir_p(pvrn, 0755) != 0) RDSK_FAIL("stage_build_ramdisk: mkdir private/var/run failed");
        if (mkdir_p(sshd, 0755) != 0) RDSK_FAIL("stage_build_ramdisk: mkdir sshd failed");
    }

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
        "cd %s && "
        "rm -rf ./sshd "
                "./usr/local/standalone/firmware/* "
                "./usr/share/progressui "
                "./usr/share/terminfo "
                "./etc/apt "
                "./etc/dpkg",
        ctx->mount))
        RDSK_FAIL("stage_build_ramdisk: cleanup failed");

    if (patch_restored_external_in_ramdisk(ctx))
        RDSK_FAIL("stage_build_ramdisk: patch_restored_external failed");

    /* detach dmg */
    if (!run_cmd("hdiutil detach -force %s", ctx->mount))
        RDSK_FAIL("stage_build_ramdisk: hdiutil detach failed");

    sleep(3);

    /* shrink to minimal */
    if (!run_cmd("hdiutil resize -sectors min %s/rdsk.dmg", ctx->staging)) {
        log_error("stage_build_ramdisk: hdiutil resize failed\n");
        return -1;
    }

    sleep(1);

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
    if (im4m_from_shsh(shsh, ctx->im4m) != 0)
        return -1;

    return run_cmd(
        "cd %s && "
        "./img4 -i %s -o kernelcache.img4 -P kc.bpatch -M IM4M -T rkrn && "
        "./img4 -i %s -o trustcache.img4 -M IM4M -T rtsc && "
        "./img4 -i rdsk.dmg -o rdsk.img4 -M IM4M -A -T rdsk && "
        "./img4 -i %s -o dtree.img4 -M IM4M -T rdtr && "
        "./img4 -i bootim@750x1334.im4p -o bootlogo.img4 -A -M IM4M -T rlgo && "
        "./img4 -i %s.pwn -o ibec.img4 -A -M IM4M -T ibec && "
        "./img4 -i %s.pwn -o ibss.img4 -A -M IM4M -T ibss",
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

    if (stage_ensure_tools())    return -1;
    if (stage_get_shsh(ctx))     return -1;
    if (stage_patch_kernel(ctx)) return -1;
    if (stage_patch_iboot(ctx))  return -1;
    if (stage_build_ramdisk(ctx))return -1;
    if (stage_build_img4(ctx))   return -1;

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

    if (stage_prepare(&ctx))         return -1;
    if (stage_download_images(&ctx)) return -1;
    if (stage_decrypt(&ctx))         return -1;
    if (stage_patch(&ctx))           return -1;

    return stage_boot_ramdisk(&ctx);
}
