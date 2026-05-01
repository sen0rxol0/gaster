/*
 * ideviceenterramdisk.c
 *
 * Orchestrates the full gastera1n SSH-ramdisk boot flow:
 *   prepare → download → decrypt → patch → boot
 *
 * Tool resolution
 * ───────────────
 * img4 is built as part of this project and resolved at runtime via
 * g_tool_dir (set once by ideviceenterramdisk_set_tool_dir()).  The
 * companion pre-built tools (ldid2, iBoot64Patcher, tsschecker,
 * Kernel64Patcher) are expected in the same directory as img4,
 * decompressed on first use by ensure_tool().
 *
 * Device cache
 * ────────────
 * All patched img4 payloads are saved into a per-device cache directory:
 *
 *   /tmp/gastera1n_cache/<ecid>_<cpid>/
 *
 * On subsequent runs with ramdiskBootMode the cache is checked first; if
 * all required img4 files are present the prepare/download/decrypt/patch
 * pipeline is skipped entirely and boot proceeds immediately.
 *
 * Shell usage
 * ───────────
 * popen() is retained only for calls that are irreducibly shell-based
 * (hdiutil, tar, rsync) or that invoke the external tool binaries whose
 * stdout must be captured (ldid2 -e).  All file-system operations use
 * the native C helpers defined below.  Tool arguments are passed through
 * exec_tool() which bypasses /bin/sh entirely, eliminating shell-
 * injection risk on paths that contain spaces.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdarg.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/param.h>
#include <sys/wait.h>
#include <zlib.h>
#ifdef __APPLE__
#  include <sys/xattr.h>
#endif

#include "log.h"
#include "ideviceenterramdisk.h"
#include "ideviceloaders.h"
#include "gastera1n.h"
/* kernel64patcher.h removed: kernel patching is now done via the
   Kernel64Patcher wrapper binary (exec_tool), not the embedded library. */
#include "kerneldiff.h"

#include <plist/plist.h>
#include <libfragmentzip/libfragmentzip.h>
#include <libirecovery.h>


/* ═══════════════════════════════════════════════════════════════════════════
 * Constants
 * ═══════════════════════════════════════════════════════════════════════════ */

#define STAGING_DIR      "/tmp/gastera1n_rdsk"
#define MOUNT_DIR        STAGING_DIR "/dmg_mountpoint"

#define CACHE_BASE_DIR   ".gastera1n_cache"
/* Cache manifest file – presence signals a complete, valid cache entry. */
#define CACHE_MANIFEST_NAME  ".complete"

/* Seconds to wait after hdiutil attach/detach so the kernel can settle. */
#define SLEEP_HDIUTIL_ATTACH  3
#define SLEEP_HDIUTIL_DETACH  3
#define SLEEP_AFTER_RESIZE    1
/* Seconds between DFU send steps to let iBSS/iBEC negotiate. */
#define SLEEP_IBSS_AFTER_SEND 1
#define SLEEP_IBEC_AFTER_SEND 1
#define SLEEP_AFTER_GO        5
/* Reset recovery before boot. */
#define SLEEP_AFTER_RESET     2

/* Tool binary base-names. */
#define TOOL_IMG4            "img4"
#define TOOL_LDID2           "ldid2"
#define TOOL_TSSCHECKER      "tsschecker"
#define TOOL_IBOOT64PATCHER  "iBoot64Patcher"
#define TOOL_KERNEL64PATCHER "Kernel64Patcher"


/* ═══════════════════════════════════════════════════════════════════════════
 * Global tool directory
 * ═══════════════════════════════════════════════════════════════════════════ */

/*
 * Set once at start-up by the host application via
 * ideviceenterramdisk_set_tool_dir().  All tool paths are resolved
 * relative to this directory.
 */
static char g_tool_dir[PATH_MAX] = ".";

void ideviceenterramdisk_set_tool_dir(const char *dir)
{
    snprintf(g_tool_dir, sizeof(g_tool_dir), "%s", dir);
}

/* Build the absolute path for a named tool into out (size PATH_MAX). */
static void tool_path(const char *name, char *out)
{
    snprintf(out, PATH_MAX, "%s/%s", g_tool_dir, name);
}


/* ═══════════════════════════════════════════════════════════════════════════
 * Native C file-system helpers
 * ═══════════════════════════════════════════════════════════════════════════ */

/* Recursively remove a directory tree (mirrors `rm -rf`).
   Returns 0 on success or if path does not exist, -1 on error. */
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
   Returns 0 if the directory already exists or was created, -1 on error. */
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

/* Binary file copy (mirrors `cp src dst`).
   Preserves source permissions.  Returns 0 on success, -1 on error. */
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
    if (ret == 0 && fstat(fileno(in), &st) == 0)
        fchmod(fileno(out), st.st_mode & 0777);

    fclose(in);
    fclose(out);
    if (ret != 0) unlink(dst);
    return ret;
}

/* Decompress gz_path → out_path using zlib.
   Removes gz_path on success (mirrors `gunzip`).
   Returns 0 on success, -1 on error. */
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
        unlink(gz_path);   /* mirror gunzip: remove the .gz source */
    else
        unlink(out_path);  /* clean up partial output */

    return ret;
}

/* Make path executable and strip the macOS quarantine xattr. */
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
    /* ENOATTR means the quarantine xattr is simply absent — not an error. */
    if (removexattr(path, "com.apple.quarantine", 0) != 0 && errno != ENOATTR)
        log_error("make_executable: removexattr warning for '%s': %s\n",
                  path, strerror(errno));
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
 * exec_toolv – fork/exec a tool from a pre-built NULL-terminated argv array.
 *
 * argv[0] must be the absolute path of the executable.
 * stdout/stderr of the child are inherited from the parent.
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
        /* execv only returns on error. */
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
 *
 * Pass the executable path as the first argument, then all additional
 * arguments as separate (const char *) strings, terminated by a NULL
 * sentinel.  Each argument must be a distinct string — never pass a single
 * string containing spaces expecting the shell to split it.
 *
 * Example:
 *   exec_tool("/usr/bin/foo", "-a", "-b", "value", NULL);
 */
static int exec_tool(const char *path, ...)
{
    /* Count arguments to size argv. */
    va_list ap;
    int argc = 1; /* path itself */
    va_start(ap, path);
    while (va_arg(ap, const char *) != NULL) argc++;
    va_end(ap);

    /* Allocate argv: argc entries + 1 NULL terminator. */
    const char **argv = malloc(((size_t)argc + 1) * sizeof(char *));
    if (!argv) return -1;

    argv[0] = path;
    va_start(ap, path);
    for (int i = 1; i < argc; i++)
        argv[i] = va_arg(ap, const char *);
    va_end(ap);
    argv[argc] = NULL; /* explicit NULL terminator — do not rely on sentinel */

    int ret = exec_toolv(argv);
    free(argv);
    return ret;
}

/*
 * shell_cmd – run an arbitrary shell command via popen(), streaming its
 * output to stdout.  Use only for calls that genuinely require /bin/sh
 * features (pipelines, redirects, globs) or macOS-specific tools such as
 * hdiutil, tar, rsync.
 *
 * Returns 0 on success (exit status 0), -1 otherwise.
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
 * shell_cmd_capture – like shell_cmd but returns the full stdout as a
 * heap-allocated string (caller must free()).  Returns NULL on failure.
 * Used solely for `ldid2 -e` whose output must be redirected to a file
 * in a race-free way.
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

    /* Staging / mount directories */
    char staging[PATH_MAX];
    char mount[PATH_MAX];

    /* Per-device cache directory: CACHE_BASE_DIR/<ecid>_<cpid>/ */
    char cache_dir[PATH_MAX];

    /* Raw (im4p) downloads */
    char kernelcache[PATH_MAX];
    char trustcache[PATH_MAX];
    char ramdisk[PATH_MAX];
    char devicetree[PATH_MAX];
    char ibec[PATH_MAX];
    char ibss[PATH_MAX];

    /* Final img4 payloads (inside staging during build, then cached) */
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

    /* cache_dir is populated after device identification in stage_prepare(). */
    ctx->cache_dir[0] = '\0';

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
 * ctx_set_cache_dir – derive cache_dir from ecid/cpid and (re)point all
 * img4 fields to the cache directory so that stage_build_img4() writes
 * directly there and stage_boot_ramdisk() reads from the same paths.
 *
 * The im4m file is also stored in the cache so SHSH blobs survive across
 * runs.
 */
static int ctx_set_cache_dir(rdsk_ctx_t *ctx,
                              const char *ecid,
                              const char *cpid)
{
    snprintf(ctx->cache_dir, sizeof(ctx->cache_dir),
             "%s/%s_%s", CACHE_BASE_DIR, ecid, cpid);

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

/*
 * Names of all img4 files that must be present for a cache hit.
 * Must stay in sync with the CACHE() assignments in ctx_set_cache_dir().
 */
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

/* Path of the manifest sentinel inside cache_dir. */
static void cache_manifest_path(const rdsk_ctx_t *ctx, char *out)
{
    snprintf(out, PATH_MAX, "%s/%s", ctx->cache_dir, CACHE_MANIFEST_NAME);
}

/*
 * cache_is_valid – return true if all required img4 files and the manifest
 * are present in ctx->cache_dir.
 */
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

/*
 * cache_mark_complete – write the manifest sentinel to signal a valid cache.
 * Call only after all img4 files have been successfully written.
 */
static int cache_mark_complete(const rdsk_ctx_t *ctx)
{
    char manifest[PATH_MAX];
    cache_manifest_path(ctx, manifest);
    return file_write_line(manifest, "complete");
}

/*
 * cache_invalidate – remove the manifest sentinel so the next run
 * rebuilds.  Call before beginning a fresh patch pipeline for this device
 * so a partial run doesn't leave a stale-but-complete-looking cache.
 */
static void cache_invalidate(const rdsk_ctx_t *ctx)
{
    if (ctx->cache_dir[0] == '\0') return;
    char manifest[PATH_MAX];
    cache_manifest_path(ctx, manifest);
    unlink(manifest);
}

/*
 * cache_load_for_boot – populate ctx->cache_dir from device info and
 * verify that a valid cache exists.  Used by the ramdiskBootMode fast path.
 *
 * Returns  0 on a cache hit (boot can proceed immediately).
 * Returns -1 on a cache miss or if device info cannot be retrieved.
 */
static int cache_load_for_boot(rdsk_ctx_t *ctx)
{
    char *ecid = dfu_get_info("ecid");
    char *cpid = dfu_get_info("cpid");

    if (!ecid || !cpid) {
        log_error("cache_load_for_boot: failed to read device info\n");
        free(ecid); free(cpid);
        return -1;
    }

    int ret = ctx_set_cache_dir(ctx, ecid, cpid);
    free(ecid); free(cpid);
    if (ret != 0) return -1;

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

/*
 * render_progress – single progress bar renderer used by all callers.
 * progress is clamped to [0, 100] before display.
 */
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

/* fragmentzip download callback – signature matches fragmentzip_progress_t. */
static void default_prog_cb(unsigned int progress)
{
    render_progress(progress);
}

/* irecovery event callback – extracts the progress value and delegates. */
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

static irecv_client_t dfu_open_client(void)
{
    irecv_client_t client = NULL;
    irecv_error_t err = IRECV_E_SUCCESS;

    for (int i = 0; i <= 5; i++) {
        log_debug("Attempting to connect to DFU device...\n");
        err = irecv_open_with_ecid(&client, 0);

        if (err == IRECV_E_UNSUPPORTED) {
            log_error("irecv: %s\n", irecv_strerror(err));
            return NULL;
        }
        if (err == IRECV_E_SUCCESS) {
            /* Verify we're in a mode that exposes device info.
               DFU and all recovery modes are acceptable; WTF mode is not. */
            int mode = 0;
            irecv_get_mode(client, &mode);
            if (mode == IRECV_K_DFU_MODE          ||
                mode == IRECV_K_RECOVERY_MODE_1   ||
                mode == IRECV_K_RECOVERY_MODE_2   ||
                mode == IRECV_K_RECOVERY_MODE_3   ||
                mode == IRECV_K_RECOVERY_MODE_4) {
                break;
            }
            /* Wrong mode — close and retry. */
            log_debug("irecv: unexpected mode 0x%04X, retrying...\n", mode);
            irecv_close(client);
            client = NULL;
            err = IRECV_E_NO_DEVICE;
        }

        sleep(1);

        if (i == 5) {
            log_error("irecv: %s\n", irecv_strerror(err));
            return NULL;
        }
    }
    return client;
}

int dfu_wait_for_device(void)
{
    log_info("Searching for DFU mode device...");

    irecv_client_t client = dfu_open_client();
    if (!client) return -1;

    irecv_device_t device = NULL;
    irecv_devices_get_device_by_client(client, &device);

    if (!device) {
        irecv_close(client);
        return -1;
    }

    irecv_error_t err = irecv_setenv(client, "auto-boot", "true");
    if (err != IRECV_E_SUCCESS)
        log_error("irecv_setenv: %s\n", irecv_strerror(err));

    err = irecv_saveenv(client);
    if (err != IRECV_E_SUCCESS)
        log_error("irecv_saveenv: %s\n", irecv_strerror(err));

    irecv_close(client);
    return 0;
}

/*
 * dfu_get_info – return a heap-allocated string for the given key.
 * Caller must free().  Returns NULL on error.
 */
char *dfu_get_info(const char *key)
{
    log_debug("Getting device info: %s\n", key);

    irecv_client_t client = dfu_open_client();
    if (!client) {
        log_error("dfu_get_info: could not open client\n");
        return NULL;
    }

    const struct irecv_device_info *devinfo = irecv_get_device_info(client);
    irecv_device_t device = NULL;
    irecv_devices_get_device_by_client(client, &device);

    char *info = NULL;

    if (!strcmp(key, "ecid") && devinfo)
        info = dup_printf("0x%016llX", (unsigned long long)devinfo->ecid);
    else if (!strcmp(key, "cpid") && devinfo)
        info = dup_printf("0x%04X", devinfo->cpid);
    else if (!strcmp(key, "product_type") && device && device->product_type)
        info = strdup(device->product_type);
    else if (!strcmp(key, "model") && device && device->hardware_model)
        info = strdup(device->hardware_model);

    irecv_close(client);

    if (info)
        log_debug("  %s = %s\n", key, info);
    else
        log_error("dfu_get_info: failed to retrieve '%s'\n", key);

    return info;
}

int dfu_send_file(const char *filepath)
{
    irecv_client_t client = dfu_open_client();
    if (!client) return -1;

    irecv_event_subscribe(client, IRECV_PROGRESS, &dfu_progress_cb, NULL);
    irecv_error_t err = irecv_send_file(client, filepath, 0);
    irecv_close(client);
    return (err == IRECV_E_SUCCESS) ? 0 : -1;
}

int dfu_send_cmd(const char *command)
{
    irecv_client_t client = dfu_open_client();
    if (!client) return -1;

    irecv_error_t err = irecv_send_command(client, command);
    irecv_close(client);
    return (err == IRECV_E_SUCCESS) ? 0 : -1;
}


/* ═══════════════════════════════════════════════════════════════════════════
 * SHSH / IM4M helpers
 * ═══════════════════════════════════════════════════════════════════════════ */

/* Extract ApImg4Ticket from a .shsh2 plist file and write it to im4m_path. */
static int im4m_from_shsh(const char *shsh_path, const char *im4m_path)
{
    FILE *f = fopen(shsh_path, "rb");
    if (!f) {
        log_error("im4m_from_shsh: cannot open '%s': %s\n",
                  shsh_path, strerror(errno));
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
        log_error("im4m_from_shsh: cannot write '%s': %s\n",
                  im4m_path, strerror(errno));
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
 * ensure_tool – make sure the named tool binary exists and is executable.
 *
 * In the flat release layout tools arrive already decompressed alongside
 * gastera1n.  The .gz fallback handles the legacy case where archives were
 * not pre-decompressed at staging time.  Both paths are resolved relative
 * to g_tool_dir.
 */
static int ensure_tool(const char *name)
{
    char bin[PATH_MAX], gz[PATH_MAX];
    tool_path(name, bin);
    snprintf(gz, sizeof(gz), "%s.gz", bin);

    if (access(gz, F_OK) == 0) {
        log_info("Decompressing %s.gz", name);
        if (gunzip_file(gz, bin) != 0) {
            log_error("ensure_tool: failed to decompress '%s'\n", gz);
            return -1;
        }
    }

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

    /*
     * restored_external is a data file, not a tool.  It lives alongside the
     * other binaries in g_tool_dir (same flat layout).  Decompress from
     * g_tool_dir if not yet present there.
     */
    char re_bin[PATH_MAX], re_gz[PATH_MAX];
    snprintf(re_bin, sizeof(re_bin), "%s/restored_external",    g_tool_dir);
    snprintf(re_gz,  sizeof(re_gz),  "%s/restored_external.gz", g_tool_dir);

    if (access(re_bin, F_OK) != 0 && access(re_gz, F_OK) == 0) {
        if (gunzip_file(re_gz, re_bin) != 0) {
            log_error("stage_ensure_tools: failed to decompress restored_external.gz\n");
            return -1;
        }
    }
    return 0;
}


/* ═══════════════════════════════════════════════════════════════════════════
 * Stage: prepare
 * ═══════════════════════════════════════════════════════════════════════════ */

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
            ctx->loader   = device_loaders[i];
            ctx->ipsw_url = (char *)ctx->loader.ipsw_url;
            found = 1;
            break;
        }
    }
    free(identifier);

    if (!found) {
        log_error("stage_prepare: unsupported device\n");
        return -1;
    }

    /*
     * Derive and create the per-device cache directory now that we have
     * enough device info.  The cache dir is established here (not in
     * ctx_init) because it requires a device round-trip.
     */
    char *ecid = dfu_get_info("ecid");
    char *cpid = dfu_get_info("cpid");

    if (!ecid || !cpid) {
        log_error("stage_prepare: could not get ecid/cpid for cache dir\n");
        free(ecid); free(cpid);
        return -1;
    }

    int cret = ctx_set_cache_dir(ctx, ecid, cpid);
    free(ecid); free(cpid);
    if (cret != 0) return -1;

    /*
     * Invalidate any existing cache before we start building so that a
     * partial run (crash, power loss) doesn't leave stale payloads that
     * look valid.
     */
    cache_invalidate(ctx);

    /* Clean up any leftover staging directory then re-create it. */
    if (rm_rf(ctx->staging) != 0 && errno != ENOENT) {
        log_error("stage_prepare: failed to remove '%s'\n", ctx->staging);
        return -1;
    }
    if (mkdir_p(ctx->staging, 0755) != 0 || mkdir_p(ctx->mount, 0755) != 0) {
        log_error("stage_prepare: failed to create staging directories\n");
        return -1;
    }

    /*
     * Copy the boot image from g_tool_dir (flat layout) into staging.
     */
    char bootim_src[PATH_MAX], bootim_dst[PATH_MAX];
    snprintf(bootim_src, sizeof(bootim_src),
             "%s/bootim@750x1334.im4p", g_tool_dir);
    snprintf(bootim_dst, sizeof(bootim_dst),
             "%s/bootim@750x1334.im4p", ctx->staging);
    if (file_copy(bootim_src, bootim_dst) != 0) {
        log_error("stage_prepare: failed to copy boot image\n");
        return -1;
    }
    return 0;
}


/* ═══════════════════════════════════════════════════════════════════════════
 * Stage: download
 * ═══════════════════════════════════════════════════════════════════════════ */

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

/* Decrypt one im4p file using the built img4 tool (exec_tool, no shell). */
static int img4_decrypt(const char *img4_bin, const char *in, const char *out)
{
    return exec_tool(img4_bin, "-i", in, "-o", out, NULL);
}

static int stage_decrypt(rdsk_ctx_t *ctx)
{
    char img4_bin[PATH_MAX];
    tool_path(TOOL_IMG4, img4_bin);

    /* Kernel, trustcache, ramdisk, devicetree – decrypted via img4. */
    struct { const char *in; } plain[] = {
        { ctx->kernelcache },
        { ctx->trustcache  },
        { ctx->ramdisk     },
        { ctx->devicetree  },
        { NULL }
    };

    for (int i = 0; plain[i].in; i++) {
        char out[PATH_MAX];
        snprintf(out, sizeof(out), "%s.dec", plain[i].in);
        if (img4_decrypt(img4_bin, plain[i].in, out) != 0) {
            log_error("stage_decrypt: img4 failed on '%s'\n", plain[i].in);
            return -1;
        }
    }

    /* iBEC / iBSS – decrypted via gastera1n_decrypt (handles GID key). */
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

    if (access(shsh, F_OK) == 0) return 0;  /* already saved */

    char *ecid  = dfu_get_info("ecid");
    char *ptype = dfu_get_info("product_type");
    char *model = dfu_get_info("model");

    if (!ecid || !ptype || !model) {
        log_error("stage_get_shsh: failed to read device info\n");
        free(ecid); free(ptype); free(model);
        return -1;
    }

    char tss_bin[PATH_MAX];
    tool_path(TOOL_TSSCHECKER, tss_bin);

    /*
     * tsschecker writes .shsh2 files with non-deterministic names; we tell
     * it to save into ctx->staging then locate the newest one below.
     *
     * exec_tool is used here because tsschecker takes simple flag arguments
     * with no shell quoting concerns.
     */
    int ret = exec_tool(tss_bin,
                        "-e", ecid,
                        "-d", ptype,
                        "-B", model,
                        "-b", "-l", "-s",
                        "--save-path", ctx->staging,
                        NULL);
    free(ecid); free(ptype); free(model);

    if (ret != 0) {
        log_error("stage_get_shsh: tsschecker failed\n");
        return -1;
    }

    /* Locate the newest .shsh2 and rename it to a canonical path. */
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
 * stage_patch_kernel – patch the decrypted kernelcache using the
 * Kernel64Patcher wrapper binary, then produce a binary diff for img4.
 *
 * The wrapper automatically selects Kernel64Patcher (legacy) or
 * KPlooshFinder based on the iOS version detected in the kernelcache
 * image, and silently drops flags inappropriate for the selected tool
 * (see build_tool_argv() in the wrapper source).
 *
 * Flags passed here cover the full iOS 15/16 surface; the wrapper will
 * filter them to the correct subset at runtime:
 *
 *   -a   Patch AMFI (both iOS 15 and 16)
 *   -f   Patch AppleFirmwareUpdate img4 signature check (both)
 *   -t   Patch tfp0 (both)
 *   -d   Patch developer mode (both)
 *   -s   Patch SPUFirmwareValidation (iOS 15 only – dropped for iOS 16)
 *   -r   Patch RootVPNotAuthenticatedAfterMounting (iOS 15 only)
 *   -o   Patch could_not_authenticate_personalized_root_hash (iOS 15 only)
 *   -e   Patch root volume seal is broken (iOS 15 only)
 *   -u   Patch update_rootfs_rw (iOS 15 only)
 *
 * kerneldiff is left unchanged; it still diffs kdec → kpwn to produce
 * the binary patch file consumed by the img4 stage.
 */
static int stage_patch_kernel(rdsk_ctx_t *ctx)
{
    char k64_bin[PATH_MAX];
    tool_path(TOOL_KERNEL64PATCHER, k64_bin);

    char kdec[PATH_MAX], kpwn[PATH_MAX], diff[PATH_MAX];
    snprintf(kdec, sizeof(kdec), "%s.dec",       ctx->kernelcache);
    snprintf(kpwn, sizeof(kpwn), "%s.pwn",       ctx->kernelcache);
    snprintf(diff, sizeof(diff), "%s/kc.bpatch", ctx->staging);

    /*
     * Each flag is a separate argument to exec_tool — never pack them
     * into a single string.  The wrapper's build_tool_argv() will drop
     * any flag not valid for the detected iOS version.
     */
    if (exec_tool(k64_bin, kdec, kpwn, "-a", NULL) != 0) {
        log_error("stage_patch_kernel: Kernel64Patcher failed\n");
        return -1;
    }

    if (kerneldiff(kdec, kpwn, diff) != 0) return -1;
    return 0;
}

/*
 * patch_iboot – patch one iBoot image (iBEC or iBSS) with iBoot64Patcher.
 *
 * Extra flags for iBEC are passed as separate argv tokens, not as a single
 * pre-quoted string.  execv does not perform shell word-splitting, so each
 * flag must be its own argument.
 */
static int patch_iboot(const char *path, bool is_ibec)
{
    char iboot_bin[PATH_MAX];
    tool_path(TOOL_IBOOT64PATCHER, iboot_bin);

    char in[PATH_MAX], out[PATH_MAX];
    snprintf(in,  sizeof(in),  "%s.dec", path);
    snprintf(out, sizeof(out), "%s.pwn", path);

    if (is_ibec) {
        /* Each flag is a separate argument — never pack them into one string. */
        return exec_tool(iboot_bin, in, out, "-n", "-b", "rd=md0 -v", NULL);
    } else {
        return exec_tool(iboot_bin, in, out, NULL);
    }
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
 *
 * ldid2 -e captures entitlements to a plist, then ldid2 -M re-signs the
 * patched binary with those entitlements.  The plist capture is done via
 * shell_cmd_capture so we can write the output to a file ourselves without
 * relying on shell redirection.
 */
static int patch_restored_external_in_ramdisk(rdsk_ctx_t *ctx)
{
    char ldid2_bin[PATH_MAX];
    tool_path(TOOL_LDID2, ldid2_bin);

    char re_src[PATH_MAX];   /* source binary from tool dir */
    char hax[PATH_MAX];      /* patched copy in staging     */
    char plist[PATH_MAX];    /* captured entitlements       */
    char dst_bin[PATH_MAX];  /* target inside ramdisk       */

    snprintf(re_src,  sizeof(re_src),  "%s/restored_external",              g_tool_dir);
    snprintf(hax,     sizeof(hax),     "%s/restored_external_hax",          ctx->staging);
    snprintf(plist,   sizeof(plist),   "%s/restored_external.plist",         ctx->staging);
    snprintf(dst_bin, sizeof(dst_bin), "%s/usr/local/bin/restored_external", ctx->mount);

    if (file_copy(re_src, hax) != 0) {
        log_error("patch_restored_external: failed to copy binary\n");
        return -1;
    }

    /* Capture entitlements from the ramdisk binary. */
    char *ents = shell_cmd_capture("%s -e %s", ldid2_bin, dst_bin);
    if (!ents) {
        log_error("patch_restored_external: ldid2 -e failed\n");
        unlink(hax);
        return -1;
    }

    /* Write entitlements to plist file. */
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

    /*
     * Re-sign patched binary.  Build the -S<plist> flag as a separate
     * heap string so it can be freed after the call.
     */
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

    if (rename(hax, dst_bin) != 0) {
        log_error("patch_restored_external: rename failed: %s\n", strerror(errno));
        unlink(hax);
        return -1;
    }
    return 0;
}

static int stage_build_ramdisk(rdsk_ctx_t *ctx)
{
    log_info("Building ramdisk...");

    /*
     * ssh64.tar.gz may be split across ssh64.tar.gz.* parts.
     * Resolve both paths relative to g_tool_dir.
     */
    char ssh64_gz[PATH_MAX], ssh64_glob[PATH_MAX];
    snprintf(ssh64_gz,   sizeof(ssh64_gz),   "%s/ssh64.tar.gz",   g_tool_dir);
    snprintf(ssh64_glob, sizeof(ssh64_glob), "%s/ssh64.tar.gz_*", g_tool_dir);

    if (access(ssh64_gz, F_OK) != 0)
        if (shell_cmd("cat %s > '%s'", ssh64_glob, ssh64_gz) != 0)
            return -1;

    char rdsk_dec[PATH_MAX], rdsk_dmg[PATH_MAX];
    snprintf(rdsk_dec, sizeof(rdsk_dec), "%s.dec", ctx->ramdisk);
    snprintf(rdsk_dmg, sizeof(rdsk_dmg), "%s/rdsk.dmg", ctx->staging);

    if (file_copy(rdsk_dec, rdsk_dmg) != 0) {
        log_error("stage_build_ramdisk: failed to copy ramdisk image\n");
        return -1;
    }

    if (shell_cmd("hdiutil resize -size 180MB '%s'", rdsk_dmg) != 0)
        return -1;

    sleep(SLEEP_HDIUTIL_ATTACH);

    if (shell_cmd("hdiutil attach '%s' -mountpoint '%s'",
                  rdsk_dmg, ctx->mount) != 0)
        return -1;

    sleep(SLEEP_HDIUTIL_ATTACH);

    /* All failure paths below must detach before returning. */
    int ret = 0;

#define RDSK_FAIL(msg, ...) \
    do { log_error(msg "\n", ##__VA_ARGS__); ret = -1; goto detach; } while (0)

    /* Create required directories and write motd. */
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

    if (shell_cmd("tar -C '%s/sshd' --preserve-permissions -xf '%s'",
                  ctx->mount, ssh64_gz) != 0)
        RDSK_FAIL("stage_build_ramdisk: tar extract failed");
      
   if (shell_cmd(
            "cd '%s/sshd' && "
            "chmod 0755 bin/* usr/bin/* usr/sbin/* usr/local/bin/*",
            ctx->mount) != 0)
        RDSK_FAIL("stage_build_ramdisk: chmod failed");
   
   if (shell_cmd(
            "cd '%s/sshd' && "
            "rsync --ignore-existing -auK . '%s/'",
            ctx->mount, ctx->mount) != 0)
        RDSK_FAIL("stage_build_ramdisk: rsync failed");
   
    if (shell_cmd(
            "cd '%s' && "
            "rm -rf ./sshd "
                    "./usr/local/standalone/firmware/* "
                    "./usr/share/progressui "
                    "./usr/share/terminfo "
                    "./etc/apt "
                    "./etc/dpkg",
            ctx->mount) != 0)
        RDSK_FAIL("stage_build_ramdisk: cleanup failed");

    if (patch_restored_external_in_ramdisk(ctx) != 0)
        RDSK_FAIL("stage_build_ramdisk: patch_restored_external failed");

detach:
    shell_cmd("hdiutil detach -force '%s'", ctx->mount);
    sleep(SLEEP_HDIUTIL_DETACH);

    if (ret != 0) return ret;

    if (shell_cmd("hdiutil resize -sectors min '%s/rdsk.dmg'", ctx->staging) != 0)
        return -1;

    sleep(SLEEP_AFTER_RESIZE);

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

    /*
     * Each img4 invocation is independent; run them sequentially via
     * exec_tool so that arguments are never subject to shell splitting.
     *
     * Output paths (ctx->*_img4) now point into ctx->cache_dir so the
     * built payloads land directly in the cache — no separate copy step
     * is required.
     */
    struct {
        const char *in;
        const char *out;
        const char *patch;   /* -P argument, or NULL */
        const char *type;    /* -T argument */
        bool        apple;   /* pass -A flag */
    } steps[] = {
        { ctx->kernelcache,       ctx->kernelcache_img4, NULL /* set below */, "rkrn", false },
        { ctx->trustcache,        ctx->trustcache_img4,  NULL,                  "rtsc", false },
        { NULL /* rdsk, set below */, ctx->ramdisk_img4, NULL,                  "rdsk", true  },
        { ctx->devicetree,        ctx->devicetree_img4,  NULL,                  "rdtr", false },
        { NULL /* bootim */,      ctx->bootim_img4,      NULL,                  "rlgo", true  },
        { NULL /* ibec */,        ctx->ibec_img4,        NULL,                  "ibec", true  },
        { NULL /* ibss */,        ctx->ibss_img4,        NULL,                  "ibss", true  },
    };

    /* Fill in paths that require runtime construction. */
    char rdsk_dmg[PATH_MAX], ibec_pwn[PATH_MAX], ibss_pwn[PATH_MAX];
    char kc_patch[PATH_MAX], bootim_src[PATH_MAX];

    snprintf(rdsk_dmg,   sizeof(rdsk_dmg),   "%s/rdsk.dmg",              ctx->staging);
    snprintf(ibec_pwn,   sizeof(ibec_pwn),   "%s.pwn",                   ctx->ibec);
    snprintf(ibss_pwn,   sizeof(ibss_pwn),   "%s.pwn",                   ctx->ibss);
    snprintf(kc_patch,   sizeof(kc_patch),   "%s/kc.bpatch",             ctx->staging);
    snprintf(bootim_src, sizeof(bootim_src), "%s/bootim@750x1334.im4p",  ctx->staging);

    steps[0].patch = kc_patch;
    steps[2].in    = rdsk_dmg;
    steps[4].in    = bootim_src;
    steps[5].in    = ibec_pwn;
    steps[6].in    = ibss_pwn;

    for (size_t i = 0; i < sizeof(steps)/sizeof(steps[0]); i++) {
        int r;
        if (steps[i].patch) {
            r = steps[i].apple
                ? exec_tool(img4_bin,
                            "-i", steps[i].in,
                            "-o", steps[i].out,
                            "-P", steps[i].patch,
                            "-M", ctx->im4m,
                            "-A",
                            "-T", steps[i].type,
                            NULL)
                : exec_tool(img4_bin,
                            "-i", steps[i].in,
                            "-o", steps[i].out,
                            "-P", steps[i].patch,
                            "-M", ctx->im4m,
                            "-T", steps[i].type,
                            NULL);
        } else {
            r = steps[i].apple
                ? exec_tool(img4_bin,
                            "-i", steps[i].in,
                            "-o", steps[i].out,
                            "-M", ctx->im4m,
                            "-A",
                            "-T", steps[i].type,
                            NULL)
                : exec_tool(img4_bin,
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

    /*
     * All img4 files written successfully.  Write the manifest sentinel so
     * subsequent runs with ramdiskBootMode can use this cache directly.
     */
    if (cache_mark_complete(ctx) != 0) {
        log_error("stage_build_img4: failed to write cache manifest\n");
        return -1;
    }

    log_info("Payloads cached to %s", ctx->cache_dir);
    return 0;
}


/* ═══════════════════════════════════════════════════════════════════════════
 * Stage: patch (top-level)
 * ═══════════════════════════════════════════════════════════════════════════ */

static int stage_patch(rdsk_ctx_t *ctx)
{
    log_info("Patching images...");

    if (stage_ensure_tools()    != 0) return -1;
    if (stage_get_shsh(ctx)     != 0) return -1;
    if (stage_patch_kernel(ctx) != 0) return -1;
    if (stage_patch_iboot(ctx)  != 0) return -1;
    if (stage_build_ramdisk(ctx)!= 0) return -1;
    if (stage_build_img4(ctx)   != 0) return -1;
    return 0;
}


/* ═══════════════════════════════════════════════════════════════════════════
 * Stage: boot
 * ═══════════════════════════════════════════════════════════════════════════ */

static int stage_boot_ramdisk(rdsk_ctx_t *ctx)
{
    log_info("Waiting for DFU device...");
    if (dfu_wait_for_device() != 0) return -1;

    log_info("Booting SSH ramdisk...");
    if (gastera1n_reset() != 0) return -1;
    sleep(SLEEP_AFTER_RESET);

    if (dfu_send_file(ctx->ibss_img4)    != 0) return -1;
    sleep(SLEEP_IBSS_AFTER_SEND);

    if (dfu_send_file(ctx->ibec_img4)    != 0) return -1;
    sleep(SLEEP_IBEC_AFTER_SEND);

    if (dfu_send_cmd("go")               != 0) return -1;
    sleep(SLEEP_AFTER_GO);

    if (dfu_send_file(ctx->bootim_img4)  != 0) return -1;
    if (dfu_send_cmd("setpicture 0x1")   != 0) return -1;
    if (dfu_send_cmd("bgcolor 255 55 55")!= 0) return -1;

    if (dfu_send_file(ctx->devicetree_img4) != 0) return -1;
    if (dfu_send_cmd("devicetree")          != 0) return -1;

    if (dfu_send_file(ctx->ramdisk_img4) != 0) return -1;
    if (dfu_send_cmd("ramdisk")          != 0) return -1;

    if (dfu_send_file(ctx->trustcache_img4) != 0) return -1;
    if (dfu_send_cmd("firmware")            != 0) return -1;

    if (dfu_send_file(ctx->kernelcache_img4) != 0) return -1;
    if (dfu_send_cmd("bootx")               != 0) return -1;

    log_info("Device should be booting now.");
    return 0;
}


/* ═══════════════════════════════════════════════════════════════════════════
 * Public entry point
 * ═══════════════════════════════════════════════════════════════════════════ */

int ideviceenterramdisk_load(void)
{
    rdsk_ctx_t ctx;
    ctx_init(&ctx);

    /*
     * ramdiskBootMode fast path – check the per-device cache first.
     *
     * cache_load_for_boot() opens the device, reads ECID+CPID, constructs
     * the cache directory path, and verifies that all required img4 files
     * are present and marked complete.  If the cache is valid we go
     * straight to booting without any download/decrypt/patch work.
     *
     * If the cache is missing or incomplete we fall through to the full
     * pipeline below, which will rebuild and repopulate the cache.
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
