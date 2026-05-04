/*
 * ideviceenterramdisk.c
 *
 * Orchestrates the full gastera1n SSH-ramdisk boot flow:
 *   prepare → download → decrypt → patch → boot
 *
 * Tool resolution
 * ───────────────
 * img4 is built as part of this project and resolved at runtime via
 * g_tool_dir.  Companion pre-built tools (ldid2, iBoot64Patcher,
 * tsschecker, Kernel64Patcher) are expected in the same directory.
 *
 * Shell usage
 * ───────────
 * popen() is used only for calls that are irreducibly shell-based
 * (hdiutil, tar, rsync) or for external tools whose stdout must be
 * captured (ldid2 -e).  All other tool invocations use exec_tool(),
 * which bypasses /bin/sh entirely.
 */

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <zlib.h>

#include <sys/param.h>
#include <sys/stat.h>
#include <sys/wait.h>

#ifdef __APPLE__
#  include <sys/xattr.h>
#endif

#include <libfragmentzip/libfragmentzip.h>
#include <libirecovery.h>
#include <plist/plist.h>

#include "gastera1n.h"
#include "ideviceenterramdisk.h"
#include "ideviceloaders.h"
#include "kerneldiff.h"
#include "log.h"


/* ═══════════════════════════════════════════════════════════════════════════
 * Constants
 * ═══════════════════════════════════════════════════════════════════════════ */

#define STAGING_DIR      "/tmp/gastera1n_rdsk"
#define MOUNT_DIR        STAGING_DIR "/dmg_mountpoint"
#define CACHE_BASE_DIR   ".gastera1n_cache"

#define IBSS_INITIAL_DELAY_MS    2000u
#define IBEC_INITIAL_DELAY_MS    4000u
#define DFU_TIMEOUT_SECS         5u

#define TOOL_IMG4            "img4"
#define TOOL_LDID2           "ldid2"
#define TOOL_TSSCHECKER      "tsschecker"
#define TOOL_IBOOT64PATCHER  "iBoot64Patcher"
#define TOOL_KERNEL64PATCHER "Kernel64Patcher"

#define CACHE_MANIFEST_NAME  ".complete"


/* ═══════════════════════════════════════════════════════════════════════════
 * Globals
 * ═══════════════════════════════════════════════════════════════════════════ */

static char g_tool_dir[PATH_MAX] = ".";

/* Fetched once via ensure_device_info() and reused by all stages. */
static char g_ecid[64];
static char g_cpid[16];
static char g_product_type[64];
static char g_model[64];

static void tool_path(const char *name, char *out)
{
    snprintf(out, PATH_MAX, "%s/%s", g_tool_dir, name);
}


/* ═══════════════════════════════════════════════════════════════════════════
 * Native C file-system helpers
 * ═══════════════════════════════════════════════════════════════════════════ */

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

/* Create a directory and all missing parents (mirrors `mkdir -p`). */
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

/* Binary file copy, preserving source permissions. */
static int file_copy(const char *src, const char *dst)
{
    FILE *in = fopen(src, "rb");
    if (!in) {
        log_error("file_copy: cannot open '%s': %s\n", src, strerror(errno));
        return -1;
    }
    FILE *out = fopen(dst, "wb");
    if (!out) {
        log_error("file_copy: cannot create '%s': %s\n", dst, strerror(errno));
        fclose(in);
        return -1;
    }

    char buf[65536];
    size_t n;
    int ret = 0;
    while ((n = fread(buf, 1, sizeof(buf), in)) > 0) {
        if (fwrite(buf, 1, n, out) != n) {
            log_error("file_copy: write error on '%s': %s\n", dst, strerror(errno));
            ret = -1;
            break;
        }
    }
    if (!feof(in)) ret = -1;

    struct stat st;
    if (ret == 0 && stat(src, &st) == 0)
        chmod(dst, st.st_mode & 0777);

    fclose(in);
    fclose(out);
    if (ret != 0) unlink(dst);
    return ret;
}

/* Decompress gz_path → out_path via zlib, removing gz_path on success. */
static int gunzip_file(const char *gz_path, const char *out_path)
{
    gzFile gz = gzopen(gz_path, "rb");
    if (!gz) {
        log_error("gunzip_file: cannot open '%s'\n", gz_path);
        return -1;
    }
    FILE *out = fopen(out_path, "wb");
    if (!out) {
        log_error("gunzip_file: cannot create '%s': %s\n", out_path, strerror(errno));
        gzclose(gz);
        return -1;
    }

    char buf[65536];
    int n, ret = 0;
    while ((n = gzread(gz, buf, sizeof(buf))) > 0) {
        if (fwrite(buf, 1, (size_t)n, out) != (size_t)n) {
            log_error("gunzip_file: write error on '%s'\n", out_path);
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
        unlink(gz_path);
    else
        unlink(out_path);

    return ret;
}

/* chmod +x and strip com.apple.quarantine. */
static int make_executable(const char *path)
{
    struct stat st;
    if (stat(path, &st) != 0) {
        log_error("make_executable: stat '%s': %s\n", path, strerror(errno));
        return -1;
    }
    if (chmod(path, st.st_mode | S_IXUSR | S_IXGRP | S_IXOTH) != 0) {
        log_error("make_executable: chmod '%s': %s\n", path, strerror(errno));
        return -1;
    }
#ifdef __APPLE__
    if (removexattr(path, "com.apple.quarantine", 0) != 0 && errno != ENOATTR) {
        log_error("make_executable: removexattr '%s': %s\n", path, strerror(errno));
        return -1;
    }
#endif
    return 0;
}

/* Write a single text line to path (mirrors `echo 'line' > path`). */
static int file_write_line(const char *path, const char *line)
{
    FILE *f = fopen(path, "w");
    if (!f) {
        log_error("file_write_line: cannot open '%s': %s\n", path, strerror(errno));
        return -1;
    }
    int ret = (fputs(line, f) == EOF || fputc('\n', f) == EOF) ? -1 : 0;
    fclose(f);
    return ret;
}

/* Find the file with a given suffix that has the newest mtime in dir.
   Writes the absolute path into best_path (size PATH_MAX).
   Returns 0 if found, -1 if nothing matched. */
static int find_newest_file(const char *dir, const char *suffix, char *best_path)
{
    DIR *d = opendir(dir);
    if (!d) return -1;

    struct dirent *ent;
    time_t best_mtime = -1;
    best_path[0] = '\0';

    while ((ent = readdir(d)) != NULL) {
        size_t nlen = strlen(ent->d_name);
        size_t slen = strlen(suffix);
        if (nlen < slen || strcmp(ent->d_name + nlen - slen, suffix) != 0)
            continue;

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

/* Allocate a formatted string.  Caller must free(). */
static char *dup_printf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int len = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);

    char *buf = malloc((size_t)len + 1);
    if (!buf) return NULL;

    va_start(ap, fmt);
    vsnprintf(buf, (size_t)len + 1, fmt, ap);
    va_end(ap);
    return buf;
}


/* ═══════════════════════════════════════════════════════════════════════════
 * Process execution helpers
 * ═══════════════════════════════════════════════════════════════════════════ */

/*
 * exec_toolv – fork/exec from a NULL-terminated argv array.
 * argv[0] must be the absolute path of the executable.
 * Returns 0 on success (exit status 0), -1 otherwise.
 */
static int exec_toolv(const char **argv)
{
    pid_t pid = fork();
    if (pid < 0) {
        log_error("exec_toolv: fork failed: %s\n", strerror(errno));
        return -1;
    }
    if (pid == 0) {
        execv(argv[0], (char *const *)argv);
        _exit(127);
    }

    int status;
    if (waitpid(pid, &status, 0) < 0) {
        log_error("exec_toolv: waitpid failed: %s\n", strerror(errno));
        return -1;
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        log_error("exec_toolv: '%s' exited with status %d\n",
                  argv[0], WIFEXITED(status) ? WEXITSTATUS(status) : -1);
        return -1;
    }
    return 0;
}

/*
 * exec_tool – varargs wrapper around exec_toolv.
 * Pass the executable path, then all arguments as (const char *) strings,
 * terminated by NULL.  Never pass a single space-separated string.
 */
static int exec_tool(const char *path, ...)
{
    va_list ap;
    int argc = 1;
    va_start(ap, path);
    while (va_arg(ap, const char *) != NULL) argc++;
    va_end(ap);

    const char **argv = malloc(((size_t)argc + 1) * sizeof(char *));
    if (!argv) return -1;

    argv[0] = path;
    va_start(ap, path);
    for (int i = 1; i < argc; i++)
        argv[i] = va_arg(ap, const char *);
    va_end(ap);
    argv[argc] = NULL;

    int ret = exec_toolv(argv);
    free(argv);
    return ret;
}

/*
 * shell_cmd – run a shell command via popen(), streaming output to stdout.
 * Use only for calls requiring /bin/sh features (pipelines, globs) or
 * macOS-specific tools (hdiutil, tar, rsync).
 */
static int shell_cmd(const char *fmt, ...)
{
    char cmd[4096];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(cmd, sizeof(cmd), fmt, ap);
    va_end(ap);

    log_debug("shell: %s\n", cmd);

    FILE *fp = popen(cmd, "r");
    if (!fp) {
        log_error("shell_cmd: popen failed for: %s\n", cmd);
        return -1;
    }

    char line[256];
    while (fgets(line, sizeof(line), fp))
        fputs(line, stdout);

    int status = pclose(fp);
    if (status != 0) {
        log_error("shell_cmd: command exited with status %d: %s\n", status, cmd);
        return -1;
    }
    return 0;
}

/*
 * shell_cmd_capture – like shell_cmd but returns stdout as a heap-allocated
 * string (caller must free()).  Returns NULL on failure.
 * Used solely for `ldid2 -e` whose output must be written to a file.
 */
static char *shell_cmd_capture(const char *fmt, ...)
{
    char cmd[4096];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(cmd, sizeof(cmd), fmt, ap);
    va_end(ap);

    log_debug("shell_capture: %s\n", cmd);

    FILE *fp = popen(cmd, "r");
    if (!fp) return NULL;

    char *out = NULL;
    size_t cap = 0, len = 0;
    char buf[4096];
    size_t n;

    while ((n = fread(buf, 1, sizeof(buf), fp)) > 0) {
        if (len + n + 1 > cap) {
            cap = (cap + n + 1) * 2;
            char *tmp = realloc(out, cap);
            if (!tmp) { free(out); pclose(fp); return NULL; }
            out = tmp;
        }
        memcpy(out + len, buf, n);
        len += n;
    }

    if (out) out[len] = '\0';
    if (pclose(fp) != 0) { free(out); return NULL; }
    return out;
}


/* ═══════════════════════════════════════════════════════════════════════════
 * Context
 * ═══════════════════════════════════════════════════════════════════════════ */

typedef struct {
    device_loader loader;
    char *ipsw_url;

    char staging[PATH_MAX];
    char mount[PATH_MAX];
    char cache_dir[PATH_MAX];   /* CACHE_BASE_DIR/<ecid>_<cpid>/ */

    /* Raw (im4p) downloads */
    char kernelcache[PATH_MAX];
    char trustcache[PATH_MAX];
    char ramdisk[PATH_MAX];
    char devicetree[PATH_MAX];
    char ibec[PATH_MAX];
    char ibss[PATH_MAX];

    /* Final img4 payloads (written directly to cache_dir) */
    char kernelcache_img4[PATH_MAX];
    char trustcache_img4[PATH_MAX];
    char ramdisk_img4[PATH_MAX];
    char devicetree_img4[PATH_MAX];
    char ibec_img4[PATH_MAX];
    char ibss_img4[PATH_MAX];
    char bootim_img4[PATH_MAX];

    char im4m[PATH_MAX];
} rdsk_ctx_t;

static void ctx_init(rdsk_ctx_t *ctx)
{
    memset(ctx, 0, sizeof(*ctx));

    snprintf(ctx->staging, sizeof(ctx->staging), STAGING_DIR);
    snprintf(ctx->mount,   sizeof(ctx->mount),   MOUNT_DIR);

#define STAGE(field, name) \
    snprintf(ctx->field, sizeof(ctx->field), "%s/" name, ctx->staging)

    STAGE(kernelcache,      "kernelcache.release");
    STAGE(trustcache,       "dmg.trustcache");
    STAGE(ramdisk,          "ramdisk.dmg");
    STAGE(devicetree,       "DeviceTree.im4p");
    STAGE(ibec,             "iBEC.im4p");
    STAGE(ibss,             "iBSS.im4p");
    STAGE(im4m,             "IM4M");

    STAGE(kernelcache_img4, "kernelcache.img4");
    STAGE(trustcache_img4,  "trustcache.img4");
    STAGE(ramdisk_img4,     "rdsk.img4");
    STAGE(devicetree_img4,  "dtree.img4");
    STAGE(bootim_img4,      "bootlogo.img4");
    STAGE(ibec_img4,        "ibec.img4");
    STAGE(ibss_img4,        "ibss.img4");

#undef STAGE
}

/*
 * ctx_set_cache_dir – derive cache_dir from ecid/cpid and redirect all
 * img4 and im4m fields into it so stage_build_img4() writes directly there.
 */
static int ctx_set_cache_dir(rdsk_ctx_t *ctx)
{
    snprintf(ctx->cache_dir, sizeof(ctx->cache_dir),
             "%s/%s_%s", CACHE_BASE_DIR, g_ecid, g_cpid);

    if (mkdir_p(ctx->cache_dir, 0755) != 0) {
        log_error("ctx_set_cache_dir: failed to create '%s': %s\n",
                  ctx->cache_dir, strerror(errno));
        return -1;
    }

#define CACHE(field, name) \
    snprintf(ctx->field, sizeof(ctx->field), "%s/" name, ctx->cache_dir)

    CACHE(kernelcache_img4, "kernelcache.img4");
    CACHE(trustcache_img4,  "trustcache.img4");
    CACHE(ramdisk_img4,     "rdsk.img4");
    CACHE(devicetree_img4,  "dtree.img4");
    CACHE(bootim_img4,      "bootlogo.img4");
    CACHE(ibec_img4,        "ibec.img4");
    CACHE(ibss_img4,        "ibss.img4");
    CACHE(im4m,             "IM4M");

#undef CACHE

    log_info("Device cache directory: %s", ctx->cache_dir);
    return 0;
}


/* ═══════════════════════════════════════════════════════════════════════════
 * Device cache helpers
 * ═══════════════════════════════════════════════════════════════════════════ */

static const char *k_cached_img4_names[] = {
    "kernelcache.img4",
    "trustcache.img4",
    "rdsk.img4",
    "dtree.img4",
    "bootlogo.img4",
    "ibec.img4",
    "ibss.img4",
    NULL
};

static void cache_manifest_path(const rdsk_ctx_t *ctx, char *out)
{
    snprintf(out, PATH_MAX, "%s/%s", ctx->cache_dir, CACHE_MANIFEST_NAME);
}

static bool cache_is_valid(const rdsk_ctx_t *ctx)
{
    if (ctx->cache_dir[0] == '\0') return false;

    char manifest[PATH_MAX];
    cache_manifest_path(ctx, manifest);
    if (access(manifest, F_OK) != 0) return false;

    for (int i = 0; k_cached_img4_names[i]; i++) {
        char p[PATH_MAX];
        snprintf(p, sizeof(p), "%s/%s", ctx->cache_dir, k_cached_img4_names[i]);
        if (access(p, F_OK) != 0) {
            log_debug("cache_is_valid: missing '%s'\n", p);
            return false;
        }
    }
    return true;
}

/* Write the manifest sentinel.  Call only after all img4 files are written. */
static int cache_mark_complete(const rdsk_ctx_t *ctx)
{
    char manifest[PATH_MAX];
    cache_manifest_path(ctx, manifest);
    return file_write_line(manifest, "complete");
}

/* Remove the manifest sentinel so the next run will rebuild. */
static void cache_invalidate(const rdsk_ctx_t *ctx)
{
    if (ctx->cache_dir[0] == '\0') return;
    char manifest[PATH_MAX];
    cache_manifest_path(ctx, manifest);
    unlink(manifest);
}

/*
 * cache_load_for_boot – populate cache_dir and verify a valid cache exists.
 * Returns 0 on cache hit, -1 on miss or if device info is unavailable.
 */
static int cache_load_for_boot(rdsk_ctx_t *ctx)
{
    if (ctx_set_cache_dir(ctx) != 0) return -1;

    if (!cache_is_valid(ctx)) {
        log_info("No valid cache found for this device — full build required.");
        return -1;
    }

    log_info("Cache hit: using pre-built payloads from %s", ctx->cache_dir);
    return 0;
}


/* ═══════════════════════════════════════════════════════════════════════════
 * Progress callbacks
 * ═══════════════════════════════════════════════════════════════════════════ */

static void render_progress(unsigned int progress)
{
    if (progress > 100) progress = 100;

    printf("\r[");
    for (unsigned int i = 0; i < 50; i++)
        printf(i < progress / 2 ? "=" : " ");
    printf("] %3u%%", progress);
    fflush(stdout);

    if (progress == 100) printf("\n");
}

static void default_prog_cb(unsigned int progress)
{
    render_progress(progress);
}

int dfu_progress_cb(irecv_client_t client, const irecv_event_t *event)
{
    (void)client;
    if (event->type != IRECV_PROGRESS) return 0;

    double p = event->progress;
    if (p < 0)   p = 0;
    if (p > 100) p = 100;

    render_progress((unsigned int)p);
    return 0;
}


/* ═══════════════════════════════════════════════════════════════════════════
 * DFU / irecovery helpers
 * ═══════════════════════════════════════════════════════════════════════════ */

/* Open and validate a DFU/recovery client — single attempt, no retry. */
static irecv_client_t dfu_open_client(void)
{
    irecv_client_t client = NULL;
    irecv_error_t  err    = irecv_open_with_ecid(&client, 0);

    if (err == IRECV_E_UNSUPPORTED) {
        log_error("irecv: %s\n", irecv_strerror(err));
        return NULL;
    }
    if (err != IRECV_E_SUCCESS)
        return NULL;

    int mode = 0;
    irecv_get_mode(client, &mode);

    switch (mode) {
        case IRECV_K_DFU_MODE:
        case IRECV_K_RECOVERY_MODE_1:
        case IRECV_K_RECOVERY_MODE_2:
        case IRECV_K_RECOVERY_MODE_3:
        case IRECV_K_RECOVERY_MODE_4:
            return client;
        default:
            log_debug("dfu_open_client: unexpected mode 0x%04X\n", mode);
            irecv_close(client);
            return NULL;
    }
}

/*
 * dfu_poll – sleep initial_delay_ms, then probe up to timeout_secs times
 * with a 1-second interval between each attempt.
 *
 * Returns 0 on success, -1 if the device was not found within timeout_secs.
 */
static int dfu_poll(unsigned int initial_delay_ms, unsigned int timeout_secs)
{
    if (initial_delay_ms != 0)
        usleep(initial_delay_ms * 1000u);
 
    for (unsigned int i = 0; i < timeout_secs; i++) {
        irecv_client_t client = dfu_open_client();
        if (client) {
            irecv_close(client);
            log_info("Device ready after %u s.", i + 1);
            return 0;
        }
 
        sleep(1);
    }
 
    log_error("dfu_poll: device not found within %u s\n", timeout_secs);
    return -1;
}
 
/* Fixed delay, then up to timeout_secs 1-second probes. */
int dfu_wait_ready(unsigned int initial_delay_ms, unsigned int timeout_secs)
{
    return dfu_poll(initial_delay_ms, timeout_secs);
}

/* Spin until any DFU/recovery device appears — no timeout. */
int dfu_wait_for_device(void)
{
    log_info("Searching for DFU mode device...");
    for (;;) {
        irecv_client_t client = dfu_open_client();
        if (client) {
            irecv_close(client);
            return 0;
        }
        sleep(1);
    }
}

/* Callback type for dfu_with_client.  Must not close the client. */
typedef int (*dfu_client_cb_t)(irecv_client_t client, void *ctx);

/* Open a client, invoke cb, then close.  Returns 0 on success, -1 on error. */
static int dfu_with_client(dfu_client_cb_t cb, void *ctx)
{
    irecv_client_t client = dfu_open_client();
    if (!client) {
        log_error("dfu_with_client: could not open client\n");
        return -1;
    }
    int ret = cb(client, ctx);
    irecv_close(client);
    return ret;
}

static int cb_get_all_info(irecv_client_t client, void *opaque)
{
    (void)opaque;

    const struct irecv_device_info *devinfo = irecv_get_device_info(client);
    if (!devinfo) return -1;

    irecv_device_t device = NULL;
    irecv_devices_get_device_by_client(client, &device);
    if (!device || !device->product_type || !device->hardware_model) return -1;

    snprintf(g_ecid,         sizeof(g_ecid),         "0x%016llX", (unsigned long long)devinfo->ecid);
    snprintf(g_cpid,         sizeof(g_cpid),         "0x%04X",    devinfo->cpid);
    snprintf(g_product_type, sizeof(g_product_type), "%s",        device->product_type);
    snprintf(g_model,        sizeof(g_model),        "%s",        device->hardware_model);

    return 0;
}

static int ensure_device_info(void)
{
    if (g_ecid[0] != '\0')
        return 0;

    if (dfu_with_client(cb_get_all_info, NULL) != 0) {
        log_error("ensure_device_info: failed to retrieve device info\n");
        return -1;
    }

    log_info("Device: %s (%s) ECID=%s CPID=%s",
             g_product_type, g_model, g_ecid, g_cpid);
    return 0;
}

typedef struct { const char *filepath; } send_file_ctx_t;

static int cb_send_file(irecv_client_t client, void *opaque)
{
    const send_file_ctx_t *ctx = opaque;
    irecv_event_subscribe(client, IRECV_PROGRESS, &dfu_progress_cb, NULL);
    irecv_error_t err = irecv_send_file(client, ctx->filepath, 0);
    return (err == IRECV_E_SUCCESS) ? 0 : -1;
}

int dfu_send_file(const char *filepath)
{
    send_file_ctx_t ctx = { filepath };
    return dfu_with_client(cb_send_file, &ctx);
}

typedef struct { const char *command; } send_cmd_ctx_t;

static int cb_send_cmd(irecv_client_t client, void *opaque)
{
    const send_cmd_ctx_t *ctx = opaque;
    irecv_error_t err = irecv_send_command(client, ctx->command);
    return (err == IRECV_E_SUCCESS) ? 0 : -1;
}

int dfu_send_cmd(const char *command)
{
    send_cmd_ctx_t ctx = { command };
    return dfu_with_client(cb_send_cmd, &ctx);
}


/* ═══════════════════════════════════════════════════════════════════════════
 * SHSH / IM4M helpers
 * ═══════════════════════════════════════════════════════════════════════════ */

/* Extract ApImg4Ticket from a .shsh2 plist and write it to im4m_path. */
static int im4m_from_shsh(const char *shsh_path, const char *im4m_path)
{
    FILE *f = fopen(shsh_path, "rb");
    if (!f) {
        log_error("im4m_from_shsh: cannot open '%s': %s\n", shsh_path, strerror(errno));
        return -1;
    }

    fseek(f, 0, SEEK_END);
    size_t size = (size_t)ftell(f);
    rewind(f);

    char *data = malloc(size);
    if (!data) {
        log_error("im4m_from_shsh: out of memory\n");
        fclose(f);
        return -1;
    }

    if (fread(data, 1, size, f) != size) {
        log_error("im4m_from_shsh: short read on '%s'\n", shsh_path);
        free(data);
        fclose(f);
        return -1;
    }
    fclose(f);

    plist_t shsh_plist = NULL;
    plist_from_memory(data, (uint32_t)size, &shsh_plist);
    free(data);

    if (!shsh_plist) {
        log_error("im4m_from_shsh: failed to parse plist from '%s'\n", shsh_path);
        return -1;
    }

    plist_t ticket = plist_dict_get_item(shsh_plist, "ApImg4Ticket");
    if (!ticket) {
        log_error("im4m_from_shsh: ApImg4Ticket key not found in '%s'\n", shsh_path);
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
        log_error("im4m_from_shsh: cannot write '%s': %s\n", im4m_path, strerror(errno));
        free(im4m);
        plist_free(shsh_plist);
        return -1;
    }

    int ret = 0;
    if (fwrite(im4m, 1, (size_t)im4m_size, f) != (size_t)im4m_size) {
        log_error("im4m_from_shsh: short write to '%s'\n", im4m_path);
        ret = -1;
    }

    fclose(f);
    free(im4m);
    plist_free(shsh_plist);
    return ret;
}


/* ═══════════════════════════════════════════════════════════════════════════
 * Tool management
 * ═══════════════════════════════════════════════════════════════════════════ */

/*
 * ensure_tool – verify the named tool binary exists and is executable.
 * make_executable() also strips com.apple.quarantine on macOS.
 */
static int ensure_tool(const char *name)
{
    char bin[PATH_MAX];
    tool_path(name, bin);

    if (make_executable(bin) != 0) {
        log_error("ensure_tool: failed to make '%s' executable\n", bin);
        return -1;
    }
    return 0;
}

static int stage_ensure_tools(void)
{
    if (ensure_tool(TOOL_IMG4)            != 0) return -1;
    if (ensure_tool(TOOL_LDID2)           != 0) return -1;
    if (ensure_tool(TOOL_IBOOT64PATCHER)  != 0) return -1;
    if (ensure_tool(TOOL_TSSCHECKER)      != 0) return -1;
    if (ensure_tool(TOOL_KERNEL64PATCHER) != 0) return -1;
    
    return 0;
}


/* ═══════════════════════════════════════════════════════════════════════════
 * Stage: prepare
 * ═══════════════════════════════════════════════════════════════════════════ */

static int stage_prepare(rdsk_ctx_t *ctx)
{
    int found = 0;
    for (int i = 0; device_loaders[i].identifier; i++) {
        if (!strcmp(device_loaders[i].identifier, g_product_type)) {
            ctx->loader   = device_loaders[i];
            ctx->ipsw_url = (char *)ctx->loader.ipsw_url;
            found = 1;
            break;
        }
    }
    if (!found) {
        log_error("stage_prepare: unsupported device\n");
        return -1;
    }

    if (ctx_set_cache_dir(ctx) != 0) return -1;

    /* Invalidate any existing cache before rebuilding. */
    cache_invalidate(ctx);

    if (rm_rf(ctx->staging) != 0 && errno != ENOENT) {
        log_error("stage_prepare: failed to remove '%s'\n", ctx->staging);
        return -1;
    }
    if (mkdir_p(ctx->staging, 0755) != 0 || mkdir_p(ctx->mount, 0755) != 0) {
        log_error("stage_prepare: failed to create staging directories\n");
        return -1;
    }

    char bootim_src[PATH_MAX], bootim_dst[PATH_MAX];
    snprintf(bootim_src, sizeof(bootim_src), "%s/bootim@750x1334.im4p", g_tool_dir);
    snprintf(bootim_dst, sizeof(bootim_dst), "%s/bootim@750x1334.im4p", ctx->staging);
    if (file_copy(bootim_src, bootim_dst) != 0) {
        log_error("stage_prepare: failed to copy boot image\n");
        return -1;
    }
    return 0;
}


/* ═══════════════════════════════════════════════════════════════════════════
 * Stage: download
 * ═══════════════════════════════════════════════════════════════════════════ */

static int download_component(rdsk_ctx_t *ctx, const char *remote, const char *local)
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
    struct { const char *remote; const char *local; } files[] = {
        { ctx->loader.kernelcache_path, ctx->kernelcache },
        { ctx->loader.trustcache_path,  ctx->trustcache  },
        { ctx->loader.ramdisk_path,     ctx->ramdisk     },
        { ctx->loader.devicetree_path,  ctx->devicetree  },
        { ctx->loader.ibec_path,        ctx->ibec        },
        { ctx->loader.ibss_path,        ctx->ibss        },
        { NULL, NULL }
    };

    for (int i = 0; files[i].remote; i++)
        if (download_component(ctx, files[i].remote, files[i].local) != 0)
            return -1;
    return 0;
}


/* ═══════════════════════════════════════════════════════════════════════════
 * Stage: decrypt
 * ═══════════════════════════════════════════════════════════════════════════ */

static int img4_decrypt(const char *img4_bin, const char *in, const char *out)
{
    return exec_tool(img4_bin, "-i", in, "-o", out, NULL);
}

static int stage_decrypt(rdsk_ctx_t *ctx)
{
    char img4_bin[PATH_MAX];
    tool_path(TOOL_IMG4, img4_bin);

    const char *plain[] = {
        ctx->kernelcache,
        ctx->trustcache,
        ctx->ramdisk,
        ctx->devicetree,
        NULL
    };

    for (int i = 0; plain[i]; i++) {
        char out[PATH_MAX];
        snprintf(out, sizeof(out), "%s.dec", plain[i]);
        if (img4_decrypt(img4_bin, plain[i], out) != 0) {
            log_error("stage_decrypt: img4 failed on '%s'\n", plain[i]);
            return -1;
        }
    }

    char ibec_dec[PATH_MAX], ibss_dec[PATH_MAX];
    snprintf(ibec_dec, sizeof(ibec_dec), "%s.dec", ctx->ibec);
    snprintf(ibss_dec, sizeof(ibss_dec), "%s.dec", ctx->ibss);

    if (gastera1n_decrypt(ctx->ibec, ibec_dec) != 0) return -1;
    if (gastera1n_decrypt(ctx->ibss, ibss_dec) != 0) return -1;
    return 0;
}


/* ═══════════════════════════════════════════════════════════════════════════
 * Stage: patch
 * ═══════════════════════════════════════════════════════════════════════════ */

static int stage_get_shsh(rdsk_ctx_t *ctx)
{
    char shsh[PATH_MAX];
    snprintf(shsh, sizeof(shsh), "%s/latest.shsh2", ctx->staging);

    if (access(shsh, F_OK) == 0) return 0;

    char tss_bin[PATH_MAX];
    tool_path(TOOL_TSSCHECKER, tss_bin);

    int ret = exec_tool(tss_bin,
                        "-e", g_ecid,
                        "-d", g_product_type,
                        "-B", g_model,
                        "-b", "-l", "-s",
                        "--save-path", ctx->staging,
                        NULL);
    if (ret != 0) {
        log_error("stage_get_shsh: tsschecker failed\n");
        return -1;
    }

    char newest[PATH_MAX];
    if (find_newest_file(ctx->staging, ".shsh2", newest) != 0) {
        log_error("stage_get_shsh: no .shsh2 file found in '%s'\n", ctx->staging);
        return -1;
    }
    if (rename(newest, shsh) != 0) {
        log_error("stage_get_shsh: rename '%s' → '%s': %s\n",
                  newest, shsh, strerror(errno));
        return -1;
    }
    return 0;
}

/*
 * stage_patch_kernel – patch the decrypted kernelcache with Kernel64Patcher,
 * then produce a binary diff for the img4 stage.
 *
 * The wrapper selects Kernel64Patcher or KPlooshFinder based on the iOS
 * version in the kernelcache and silently drops flags inapplicable to the
 * detected tool.  Flags passed here cover the full iOS 15/16 surface:
 *
 *   -a   AMFI (15 + 16)
 *   -f   AppleFirmwareUpdate img4 sig check (15 + 16)
 *   -t   tfp0 (15 + 16)
 *   -d   developer mode (15 + 16)
 *   -s   SPUFirmwareValidation (15 only)
 *   -r   RootVPNotAuthenticatedAfterMounting (15 only)
 *   -o   could_not_authenticate_personalized_root_hash (15 only)
 *   -e   root volume seal is broken (15 only)
 *   -u   update_rootfs_rw (15 only)
 */
static int stage_patch_kernel(rdsk_ctx_t *ctx)
{
    char k64_bin[PATH_MAX];
    tool_path(TOOL_KERNEL64PATCHER, k64_bin);

    char kdec[PATH_MAX], kpwn[PATH_MAX], diff[PATH_MAX];
    snprintf(kdec, sizeof(kdec), "%s.dec",       ctx->kernelcache);
    snprintf(kpwn, sizeof(kpwn), "%s.pwn",       ctx->kernelcache);
    snprintf(diff, sizeof(diff), "%s/kc.bpatch", ctx->staging);

    if (exec_tool(k64_bin, kdec, kpwn, "-a", NULL) != 0) {
        log_error("stage_patch_kernel: Kernel64Patcher failed\n");
        return -1;
    }
    return kerneldiff(kdec, kpwn, diff);
}

/*
 * patch_iboot – patch one iBoot image (iBEC or iBSS) with iBoot64Patcher.
 * Each flag is a separate argv token; execv does not perform shell splitting.
 */
static int patch_iboot(const char *path, bool is_ibec)
{
    char iboot_bin[PATH_MAX];
    tool_path(TOOL_IBOOT64PATCHER, iboot_bin);

    char in[PATH_MAX], out[PATH_MAX];
    snprintf(in,  sizeof(in),  "%s.dec", path);
    snprintf(out, sizeof(out), "%s.pwn", path);

    if (is_ibec)
        return exec_tool(iboot_bin, in, out, "-n", "-b", "rd=md0 debug=0x2014e -v wdt=-1", NULL);
    else
        return exec_tool(iboot_bin, in, out, NULL);
}

static int stage_patch_iboot(rdsk_ctx_t *ctx)
{
    if (patch_iboot(ctx->ibec, true)  != 0) return -1;
    if (patch_iboot(ctx->ibss, false) != 0) return -1;
    return 0;
}


/* ═══════════════════════════════════════════════════════════════════════════
 * Stage: build ramdisk
 * ═══════════════════════════════════════════════════════════════════════════ */

/*
 * Patch the restored_external binary inside the mounted ramdisk.
 * ldid2 -e captures entitlements, ldid2 -M re-signs the patched binary.
 */
static int patch_restored_external_in_ramdisk(rdsk_ctx_t *ctx)
{
    char ldid2_bin[PATH_MAX];
    tool_path(TOOL_LDID2, ldid2_bin);

    char re_gz[PATH_MAX], re_src[PATH_MAX], hax[PATH_MAX], plist[PATH_MAX], dst_bin[PATH_MAX];
    snprintf(re_gz,  sizeof(re_gz),  "%s/restored_external.gz", g_tool_dir);
    snprintf(re_src,  sizeof(re_src),  "%s/restored_external",              g_tool_dir);
    snprintf(hax,     sizeof(hax),     "%s/restored_external_hax",          ctx->staging);
    snprintf(plist,   sizeof(plist),   "%s/restored_external.plist",         ctx->staging);
    snprintf(dst_bin, sizeof(dst_bin), "%s/usr/local/bin/restored_external", ctx->mount);

    if (access(re_src, F_OK) != 0 && access(re_gz, F_OK) == 0) {
        if (gunzip_file(re_gz, re_src) != 0) {
            log_error("patch_restored_external: failed to decompress restored_external.gz\n");
            return -1;
        }
    }
    
    if (file_copy(re_src, hax) != 0) {
        log_error("patch_restored_external: failed to copy binary\n");
        return -1;
    }

    char *ents = shell_cmd_capture("%s -e %s", ldid2_bin, dst_bin);
    if (!ents) {
        log_error("patch_restored_external: ldid2 -e failed\n");
        unlink(hax);
        return -1;
    }

    FILE *pf = fopen(plist, "w");
    if (!pf || fputs(ents, pf) == EOF) {
        log_error("patch_restored_external: failed to write plist\n");
        if (pf) fclose(pf);
        free(ents);
        unlink(hax);
        return -1;
    }
    fclose(pf);
    free(ents);

    char *sflag = dup_printf("-S%s", plist);
    if (!sflag) {
        log_error("patch_restored_external: out of memory for -S flag\n");
        unlink(hax); unlink(plist);
        return -1;
    }
    int ret = exec_tool(ldid2_bin, "-M", sflag, hax, NULL);
    free(sflag);
    unlink(plist);

    if (ret != 0) {
        log_error("patch_restored_external: ldid2 -M failed\n");
        unlink(hax);
        return -1;
    }

    if (file_copy(hax, dst_bin) != 0) {
        log_error("patch_restored_external: failed to copy patched binary "
                  "('%s' → '%s')\n", hax, dst_bin);
        unlink(hax);
        return -1;
    }

    if (chmod(dst_bin, 0755) != 0) {
        log_error("patch_restored_external: chmod dst_bin failed: %s\n", strerror(errno));
        unlink(hax);
        return -1;
    }

    unlink(hax);
    return 0;
}

static int stage_build_ramdisk(rdsk_ctx_t *ctx)
{
    log_info("Building ramdisk...");

    char ssh64_gz[PATH_MAX];
    snprintf(ssh64_gz, sizeof(ssh64_gz), "%s/ssh64.tar.gz", g_tool_dir);

    if (access(ssh64_gz, F_OK) != 0) {
        if (shell_cmd("cat '%s'/ssh64.tar.gz_* > '%s'", g_tool_dir, ssh64_gz) != 0)
            return -1;

        struct stat st;
        if (stat(ssh64_gz, &st) != 0 || st.st_size == 0) {
            log_error("stage_build_ramdisk: reassembled ssh64.tar.gz is missing or empty\n");
            unlink(ssh64_gz);
            return -1;
        }
    }

    char rdsk_dec[PATH_MAX], rdsk_dmg[PATH_MAX];
    snprintf(rdsk_dec, sizeof(rdsk_dec), "%s.dec",      ctx->ramdisk);
    snprintf(rdsk_dmg, sizeof(rdsk_dmg), "%s/rdsk.dmg", ctx->staging);

    if (rename(rdsk_dec, rdsk_dmg) != 0) {
        log_error("stage_build_ramdisk: failed to move ramdisk image\n");
        return -1;
    }

    if (shell_cmd("hdiutil resize -size 208MB '%s'", rdsk_dmg) != 0)
        return -1;
    if (shell_cmd("hdiutil attach '%s' -mountpoint '%s'", rdsk_dmg, ctx->mount) != 0)
        return -1;

    /* Wait up to ~5 s for the mount to become usable. */
    int mount_ready = 0;
    for (int i = 0; i < 50; i++) {
        char probe[PATH_MAX];
        snprintf(probe, sizeof(probe), "%s/usr/local/bin", ctx->mount);
        struct stat pst;
        if (stat(probe, &pst) == 0 && S_ISDIR(pst.st_mode)) {
            mount_ready = 1;
            break;
        }
        usleep(100000);
    }
    if (!mount_ready) {
        log_error("stage_build_ramdisk: mount timed out — "
                  "'/usr/local/bin' never appeared under '%s'\n", ctx->mount);
        shell_cmd("hdiutil detach -force '%s'", ctx->mount);
        return -1;
    }

    int ret = 0;

#define RDSK_FAIL(msg, ...) \
    do { log_error(msg "\n", ##__VA_ARGS__); ret = -1; goto detach; } while (0)

    char *motd_path = dup_printf("%s/etc/motd", ctx->mount);
    if (!motd_path || file_write_line(motd_path, "WELCOME BACK!") != 0) {
        free(motd_path);
        RDSK_FAIL("stage_build_ramdisk: failed to write /etc/motd");
    }
    free(motd_path);

    char *var_root = dup_printf("%s/private/var/root", ctx->mount);
    char *var_run  = dup_printf("%s/private/var/run",  ctx->mount);
    char *sshd_dir = dup_printf("%s/sshd",             ctx->mount);
    if (!var_root || !var_run || !sshd_dir ||
        mkdir_p(var_root, 0755) != 0 ||
        mkdir_p(var_run,  0755) != 0 ||
        mkdir_p(sshd_dir, 0755) != 0) {
        free(var_root); free(var_run); free(sshd_dir);
        RDSK_FAIL("stage_build_ramdisk: failed to create required directories");
    }
    free(var_root); free(var_run); free(sshd_dir);

    if (shell_cmd("tar -C '%s/sshd' --preserve-permissions -xf '%s'",
                  ctx->mount, ssh64_gz) != 0)
        RDSK_FAIL("stage_build_ramdisk: tar extract of ssh64 failed");

    if (shell_cmd("rsync --ignore-existing -auK '%s/sshd/' '%s/'", ctx->mount, ctx->mount) != 0)
        RDSK_FAIL("stage_build_ramdisk: rsync of sshd tree failed");

    if (patch_restored_external_in_ramdisk(ctx) != 0)
        RDSK_FAIL("stage_build_ramdisk: patch_restored_external failed");

    if (shell_cmd(
            "for d in"
            " '%1$s/bin'"
            " '%1$s/usr/bin'"
            " '%1$s/usr/sbin'"
            " '%1$s/usr/local/bin'; do"
            "  chmod -R 0755 \"$d\"; "
            "done",
            ctx->mount) != 0)
        RDSK_FAIL("stage_build_ramdisk: chmod on merged binary dirs failed");

    if (shell_cmd(
            "cd '%s' && rm -rf "
            "./sshd "
            "./usr/local/standalone/firmware/* "
            "./usr/share/progressui "
            "./etc/apt "
            "./etc/dpkg",
            ctx->mount) != 0)
        RDSK_FAIL("stage_build_ramdisk: cleanup pass failed");

detach:
    for (int i = 0; i < 3; i++) {
        if (shell_cmd("hdiutil detach -force '%s'", ctx->mount) == 0) break;
        sleep(1);
    }

    if (ret != 0) {
        unlink(rdsk_dmg);
        return -1;
    }

    if (shell_cmd("hdiutil resize -sectors min '%s'", rdsk_dmg) != 0)
        return -1;

#undef RDSK_FAIL
    return 0;
}


/* ═══════════════════════════════════════════════════════════════════════════
 * Stage: build img4 payloads
 * ═══════════════════════════════════════════════════════════════════════════ */

static int stage_build_img4(rdsk_ctx_t *ctx)
{
    char shsh[PATH_MAX];
    snprintf(shsh, sizeof(shsh), "%s/latest.shsh2", ctx->staging);

    if (im4m_from_shsh(shsh, ctx->im4m) != 0) return -1;

    char img4_bin[PATH_MAX];
    tool_path(TOOL_IMG4, img4_bin);

    /* Runtime-constructed paths filled in below the table. */
    char rdsk_dmg[PATH_MAX], ibec_pwn[PATH_MAX], ibss_pwn[PATH_MAX];
    char kc_patch[PATH_MAX], bootim_src[PATH_MAX];

    snprintf(rdsk_dmg,   sizeof(rdsk_dmg),   "%s/rdsk.dmg",             ctx->staging);
    snprintf(ibec_pwn,   sizeof(ibec_pwn),   "%s.pwn",                  ctx->ibec);
    snprintf(ibss_pwn,   sizeof(ibss_pwn),   "%s.pwn",                  ctx->ibss);
    snprintf(kc_patch,   sizeof(kc_patch),   "%s/kc.bpatch",            ctx->staging);
    snprintf(bootim_src, sizeof(bootim_src), "%s/bootim@750x1334.im4p", ctx->staging);

    struct {
        const char *in;
        const char *out;
        const char *patch;  /* -P patch file, or NULL */
        const char *type;   /* -T tag */
        bool        plain;  /* pass -A (plain input, no im4p wrapper) */
    } steps[] = {
        { ctx->kernelcache, ctx->kernelcache_img4, kc_patch, "rkrn", false },
        { ctx->trustcache,  ctx->trustcache_img4,  NULL,     "rtsc", false },
        { rdsk_dmg,         ctx->ramdisk_img4,     NULL,     "rdsk", true  },
        { ctx->devicetree,  ctx->devicetree_img4,  NULL,     "rdtr", false },
        { bootim_src,       ctx->bootim_img4,      NULL,     "rlgo", true  },
        { ibec_pwn,         ctx->ibec_img4,        NULL,     "ibec", true  },
        { ibss_pwn,         ctx->ibss_img4,        NULL,     "ibss", true  },
    };

    for (size_t i = 0; i < sizeof(steps)/sizeof(steps[0]); i++) {
        int r;
        if (steps[i].patch) {
            r = exec_tool(img4_bin,
                          "-i", steps[i].in,
                          "-o", steps[i].out,
                          "-P", steps[i].patch,
                          "-M", ctx->im4m,
                          "-T", steps[i].type,
                          "-J",
                          NULL);
        } else if (steps[i].plain) {
            r = exec_tool(img4_bin,
                          "-i", steps[i].in,
                          "-o", steps[i].out,
                          "-M", ctx->im4m,
                          "-A",
                          "-T", steps[i].type,
                          NULL);
        } else {
            r = exec_tool(img4_bin,
                          "-i", steps[i].in,
                          "-o", steps[i].out,
                          "-M", ctx->im4m,
                          "-T", steps[i].type,
                          NULL);
        }
        if (r != 0) {
            log_error("stage_build_img4: img4 failed on step %zu (out=%s)\n",
                      i, steps[i].out);
            return -1;
        }
    }

    if (cache_mark_complete(ctx) != 0) {
        log_error("stage_build_img4: failed to write cache manifest\n");
        return -1;
    }

    log_info("Payloads cached to %s", ctx->cache_dir);
    return 0;
}

static int stage_patch(rdsk_ctx_t *ctx)
{
    log_info("Patching images...");

    if (stage_ensure_tools()     != 0) return -1;
    if (stage_get_shsh(ctx)      != 0) return -1;
    if (stage_patch_kernel(ctx)  != 0) return -1;
    if (stage_patch_iboot(ctx)   != 0) return -1;
    if (stage_build_ramdisk(ctx) != 0) return -1;
    if (stage_build_img4(ctx)    != 0) return -1;
    return 0;
}


/* ═══════════════════════════════════════════════════════════════════════════
 * Stage: boot
 * ═══════════════════════════════════════════════════════════════════════════ */

/* Send an img4 file and immediately issue the commit command. */
static int send_payload(const char *label,
                        const char *filepath,
                        const char *commit_cmd)
{
    log_info("Sending %s...", label);
    if (dfu_send_file(filepath) != 0) {
        log_error("stage_boot_ramdisk: failed to send %s ('%s')\n", label, filepath);
        return -1;
    }
    if (dfu_send_cmd(commit_cmd) != 0) {
        log_error("stage_boot_ramdisk: commit command '%s' failed after sending %s\n",
                  commit_cmd, label);
        return -1;
    }
    return 0;
}

static bool needs_go_cmd(uint32_t cpid)
{
    return cpid == 0x8015 || cpid == 0x8012 ||
           cpid == 0x8011 || cpid == 0x8010;
}

static int stage_boot_ramdisk(rdsk_ctx_t *ctx)
{
    uint32_t cpid = (uint32_t)strtoul(g_cpid, NULL, 16);
    log_info("Booting SSH ramdisk (cpid=0x%04X)...", cpid);

    if (gastera1n_reset() != 0) {
        log_error("stage_boot_ramdisk: failed to reset USB before send\n");
        return -1;
    }

    sleep(1);
    
    /* ── iBSS ────────────────────────────────────────────────────────── */
    log_info("Sending iBSS...");
    if (dfu_send_file(ctx->ibss_img4) != 0) {
        log_error("stage_boot_ramdisk: iBSS send failed\n");
        return -1;
    }
    if (dfu_wait_ready(IBSS_INITIAL_DELAY_MS, DFU_TIMEOUT_SECS) != 0) {
        log_error("stage_boot_ramdisk: device never re-appeared after iBSS\n");
        return -1;
    }

    /* ── iBEC ────────────────────────────────────────────────────────── */
    log_info("Sending iBEC...");
    if (dfu_send_file(ctx->ibec_img4) != 0) {
        log_error("stage_boot_ramdisk: iBEC send failed\n");
        return -1;
    }
    if (needs_go_cmd(cpid)) {
        log_info("Sending go command...");
        if (dfu_send_cmd("go") != 0) {
            log_error("stage_boot_ramdisk: 'go' command failed\n");
            return -1;
        }
    }
    if (dfu_wait_ready(IBEC_INITIAL_DELAY_MS, DFU_TIMEOUT_SECS) != 0) {
        log_error("stage_boot_ramdisk: device did not reconnect after iBEC\n");
        return -1;
    }

    /* ── Boot image (cosmetic — non-fatal) ───────────────────────────── */
    log_info("Setting boot image...");
    if (dfu_send_file(ctx->bootim_img4) != 0 ||
        dfu_send_cmd("setpicture 0")    != 0 ||
        dfu_send_cmd("bgcolor 0 0 0")   != 0)
        log_warn("stage_boot_ramdisk: boot image setup failed (non-fatal)\n");

    /* ── Remaining payloads ──────────────────────────────────────────── */
    if (send_payload("ramdisk",     ctx->ramdisk_img4,     "ramdisk")    != 0) return -1;
    if (send_payload("device tree", ctx->devicetree_img4,  "devicetree") != 0) return -1;
    if (send_payload("trust cache", ctx->trustcache_img4,  "firmware")   != 0) return -1;
    if (send_payload("kernelcache", ctx->kernelcache_img4, "bootx")      != 0) return -1;

    log_info("Boot sequence complete — device is starting up.");
    return 0;
}


/* ═══════════════════════════════════════════════════════════════════════════
 * Public entry point
 * ═══════════════════════════════════════════════════════════════════════════ */

int ideviceenterramdisk_load(void)
{
    rdsk_ctx_t ctx;
    ctx_init(&ctx);

    if (dfu_wait_for_device() != 0) {
        log_error("ideviceenterramdisk_load: no device in DFU mode\n");
        return -1;
    }
    if (ensure_device_info() != 0) {
        log_error("ideviceenterramdisk_load: could not populate device info\n");
        return -1;
    }

    /*
     * ramdiskBootMode fast path — if a valid per-device cache exists,
     * skip the full build pipeline and boot directly from cached payloads.
     */
    if (ramdiskBootMode) {
        if (cache_load_for_boot(&ctx) == 0)
            return stage_boot_ramdisk(&ctx);

        log_info("Cache miss — running full build pipeline...");
    }

    if (stage_prepare(&ctx)         != 0) return -1;
    if (stage_download_images(&ctx) != 0) return -1;
    if (stage_decrypt(&ctx)         != 0) return -1;
    if (stage_patch(&ctx)           != 0) return -1;

    return stage_boot_ramdisk(&ctx);
}
