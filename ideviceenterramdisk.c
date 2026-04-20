#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

#include "log.h"
#include "ideviceenterramdisk.h"
#include "idevicedfu.h"
#include "ideviceloaders.h"

#include "gastera1n.h"
#include "kernel64patcher.h"

#include <plist/plist.h>
#include <libfragmentzip/libfragmentzip.h>

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

    char im4m[PATH_MAX];
} rdsk_ctx_t;


static const char *iBSS_img4_path = "/tmp/gastera1n_rdsk/ibss.img4",\
*iBEC_img4_path = "/tmp/gastera1n_rdsk/ibec.img4",\
*bootim_img4_path = "/tmp/gastera1n_rdsk/bootim.img4",\
*devicetree_img4_path = "/tmp/gastera1n_rdsk/dtree.img4",\
*ramdisk_img4_path = "/tmp/gastera1n_rdsk/rdsk.img4",\
*trustcache_img4_path = "/tmp/gastera1n_rdsk/trustcache.img4",\
*kernelcache_img4_path = "/tmp/gastera1n_rdsk/kernelcache.img4";

static const char *ldid2 = "ldid2";
static const char *tsschecker = "tsschecker_macOS_v440";

static void ctx_init(rdsk_ctx_t *ctx)
{
    strcpy(ctx->staging, "/tmp/gastera1n_rdsk");
    strcpy(ctx->mount,   "/tmp/gastera1n_rdsk/dmg_mountpoint");

    sprintf(ctx->kernelcache, "%s/kernelcache.release", ctx->staging);
    sprintf(ctx->trustcache,  "%s/dmg.trustcache",      ctx->staging);
    sprintf(ctx->ramdisk,     "%s/ramdisk.dmg",         ctx->staging);
    sprintf(ctx->devicetree,  "%s/DeviceTree.im4p",     ctx->staging);
    sprintf(ctx->ibec,        "%s/iBEC.im4p",           ctx->staging);
    sprintf(ctx->ibss,        "%s/iBSS.im4p",           ctx->staging);
    sprintf(ctx->im4m,        "%s/IM4M",                ctx->staging);
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


void im4m_from_shsh(char *path, char *im4m_path)
{
    FILE *f = fopen(path, "r");
    if (!f) return;
    fseek(f, 0, SEEK_END);
    size_t size = ftell(f);
    fseek(f, 0, SEEK_SET);
    char* data = (char*)malloc(size);
    if (data) fread(data, size, 1, f);
    fclose(f);
    plist_t shsh_plist = NULL;
    plist_from_memory((const char*)data, (uint32_t)size, &shsh_plist, NULL);
    plist_t ticket = plist_dict_get_item(shsh_plist, "ApImg4Ticket");
    char *im4m = 0;
    uint64_t im4m_size = 0;
    plist_get_data_val(ticket, &im4m, &im4m_size);
    f = fopen(im4m_path, "wb");
    fwrite(im4m, 1, im4m_size, f);
    fclose(f);
}

static void default_prog_cb(unsigned int progress) {

	if(progress < 0) {
		return;
	}

	if(progress > 100) {
		progress = 100;
	}

	printf("\r[");

	for(unsigned int i = 0; i < 50; i++) {
		if(i < progress / 2) {
			printf("=");
		} else {
			printf(" ");
		}
	}

	printf("] %3.1d%%", progress);

	fflush(stdout);

	if(progress == 100) {
		printf("\n");
	}
}

static int stage_prepare_workspace(rdsk_ctx_t *ctx)
{
    return run_cmd(
        "bash -c 'mkdir -p %s %s && rm -rf %s/*'",
        ctx->staging, ctx->mount, ctx->staging
    ) ? 0 : -1;
}

static int stage_detect_loader(rdsk_ctx_t *ctx)
{
    char *identifier = idevicedfu_info("product_type");

    for (int i = 0; device_loaders[i].identifier; i++) {
        if (!strcmp(device_loaders[i].identifier, identifier)) {
            ctx->loader = device_loaders[i];
            ctx->ipsw_url = (char*)ctx->loader.ipsw_url;
            return 0;
        }
    }
    return -1;
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
        { ctx->loader.trustcache_path,  ctx->trustcache },
        { ctx->loader.ramdisk_path,     ctx->ramdisk },
        { ctx->loader.devicetree_path,  ctx->devicetree },
        { ctx->loader.ibec_path,        ctx->ibec },
        { ctx->loader.ibss_path,        ctx->ibss },
        { NULL, NULL }
    };

    for (int i = 0; files[i].remote; i++)
        if (download_component(ctx, files[i].remote, files[i].local))
            return -1;

    return 0;
}

static int decrypt_img4(const char *path)
{
    return run_cmd("img4 -i %s -o %s.dec", path, path) ? 0 : -1;
}

static int stage_decrypt(rdsk_ctx_t *ctx)
{
    const char *list[] = {
        ctx->kernelcache,
        ctx->trustcache,
        ctx->ramdisk,
        ctx->devicetree,
        NULL
    };

    for (int i = 0; list[i]; i++)
        if (decrypt_img4(list[i])) return -1;

    char out[PATH_MAX];
    sprintf(out, "%s.dec", ctx->ibec);
    if (gastera1n_decrypt(ctx->ibec, out)) return -1;

    sprintf(out, "%s.dec", ctx->ibss);
    return gastera1n_decrypt(ctx->ibss, out);
}

static int ensure_tool(const char *tool)
{
    if (access(tool, F_OK) == 0)
        return 0;

    return run_cmd(
        "gunzip %s.gz && xattr -d com.apple.quarantine %s >/dev/null 2>&1 && chmod +x %s",
        tool, tool, tool
    ) ? 0 : -1;
}

static void build_shsh_path(rdsk_ctx_t *ctx, char *out)
{
    sprintf(out, "%s/latest.shsh2", ctx->staging);
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

static int stage_get_shsh(rdsk_ctx_t *ctx)
{
    char shsh[PATH_MAX];
    build_shsh_path(ctx, shsh);

    if (access(shsh, F_OK) == 0)
        return 0;

    char *ecid  = idevicedfu_info("ecid");
    char *ptype = idevicedfu_info("product_type");
    char *model = idevicedfu_info("model");

    if (!run_cmd(
        "./tsschecker_macOS_v440 -e %s -d %s -B %s -b -l -s --save-path %s",
        ecid, ptype, model, ctx->staging))
        return -1;

    return run_cmd("mv %s/*shsh2 %s", ctx->staging, shsh) ? 0 : -1;
}

static int stage_patch_kernel(rdsk_ctx_t *ctx)
{
    char kdec[PATH_MAX];
    char kpwn[PATH_MAX];
    char diff[PATH_MAX];

    sprintf(kdec, "%s.dec", ctx->kernelcache);
    sprintf(kpwn, "%s.pwn", ctx->kernelcache);
    sprintf(diff, "%s/kc.bpatch", ctx->staging);

    if (kernel64patcher_amfi(kdec, kpwn))
        return -1;

    if (kerneldiff(kdec, kpwn, diff))
        return -1;

    return 0;
}

static int patch_iboot(const char *path, const char *extra)
{
    char in[PATH_MAX], out[PATH_MAX];
    sprintf(in, "%s.dec", path);
    sprintf(out,"%s.pwn", path);

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
    if (!run_cmd(
        "hdiutil attach %s/rdsk.dmg -mountpoint %s",
        ctx->staging, ctx->mount))
        return -1;

    /* basic filesystem prep */
    if (!run_cmd(
        "echo 'WELCOME BACK!' > %s/etc/motd && "
        "mkdir -p %s/private/var/root "
                  "%s/private/var/run "
                  "%s/sshd",
        ctx->mount, ctx->mount, ctx->mount, ctx->mount))
        return -1;

    /* extract SSH payload */
    if (!run_cmd(
        "tar -C %s/sshd --preserve-permissions -xf ssh64.tar.gz",
        ctx->mount))
        return -1;

    /* install SSH payload into root */
    if (!run_cmd(
        "cd %s/sshd && "
        "chmod 0755 bin/* usr/bin/* usr/sbin/* usr/local/bin/* && "
        "rsync --ignore-existing -auK . %s/",
        ctx->mount, ctx->mount))
        return -1;

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
        return -1;

    if (patch_restored_external_in_ramdisk(ctx))
        return -1;

    /* detach dmg */
    if (!run_cmd("hdiutil detach -force %s", ctx->mount))
        return -1;

    /* shrink to minimal */
    if (!run_cmd("hdiutil resize -sectors min %s/rdsk.dmg", ctx->staging))
        return -1;

    return 0;
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

static int
ideviceenterramdisk_bootrd() {
    log_info("Booting device into SSH ramdisk mode...");
    int ret = 0;
    ret = gastera1n_reset();
    sleep(1);

    if (ret == 0) {

        idevicedfu_sendfile(iBSS_img4_path);
        sleep(1);
        idevicedfu_sendfile(iBEC_img4_path);
        sleep(1);
        idevicedfu_sendcommand("go");
        sleep(5);
        idevicedfu_sendfile(bootim_img4_path);
        idevicedfu_sendcommand("setpicture 0x1");
        idevicedfu_sendcommand("bgcolor 255 55 55");
        idevicedfu_sendfile(devicetree_img4_path);
        idevicedfu_sendcommand("devicetree");
        idevicedfu_sendfile(ramdisk_img4_path);
        idevicedfu_sendcommand("ramdisk");
        idevicedfu_sendfile(trustcache_img4_path);
        idevicedfu_sendcommand("firmware");
        idevicedfu_sendfile(kernelcache_img4_path);
        idevicedfu_sendcommand("bootx");
        log_info("Device should be booting now.");
    }

    return ret;
}

int ideviceenterramdisk_load(void)
{
    rdsk_ctx_t ctx;
    ctx_init(&ctx);

    if (ramdiskBootMode == 1)
        return ideviceenterramdisk_bootrd();

    if (stage_prepare_workspace(&ctx))  return -1;
    if (stage_detect_loader(&ctx))      return -1;
    if (stage_download_images(&ctx))    return -1;
    if (stage_decrypt(&ctx))            return -1;
    if (stage_patch(&ctx))              return -1;

    return ideviceenterramdisk_bootrd();
}
