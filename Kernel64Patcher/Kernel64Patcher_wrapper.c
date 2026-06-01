#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>

#include "Kernel64Patcher_legacy.h"
#include "KPlooshFinder.h"

extern unsigned char Kernel64Patcher_legacy[];
extern unsigned int Kernel64Patcher_legacy_len;

extern unsigned char KPlooshFinder[];
extern unsigned int KPlooshFinder_len;

/*
 * Write an embedded binary to a temp file and return its path.
 * Returns 0 on success, -1 on any I/O error.
 */
static int write_temp_exec(const char *name,
                           unsigned char *data,
                           unsigned int size,
                           char *out_path,
                           size_t out_path_size)
{
    snprintf(out_path, out_path_size, "/tmp/%s_%d", name, getpid());

    int fd = open(out_path, O_CREAT | O_WRONLY | O_TRUNC, 0700);
    if (fd < 0) {
        perror("open temp file");
        return -1;
    }

    ssize_t written = write(fd, data, size);
    close(fd);

    if (written < 0 || (unsigned int)written != size) {
        perror("write temp file");
        unlink(out_path);
        return -1;
    }

    if (chmod(out_path, 0755) != 0) {
        perror("chmod temp file");
        unlink(out_path);
        return -1;
    }

    return 0;
}

/*
 * Detect the iOS major version embedded in an XNU kernelcache image.
 *
 * XNU kernels carry a string of the form:
 *   "Darwin Kernel Version XX.Y.Z; ... root:xnu-.../RELEASE_ARM64"
 *
 * The Darwin → iOS major version mapping used by palera1n tooling:
 *   Darwin 20 → iOS 14
 *   Darwin 21 → iOS 15
 *   Darwin 22 → iOS 16
 *   Darwin 23 → iOS 17
 *   Darwin 24 → iOS 18
 *   (general formula: iOS major = Darwin major - 6)
 *
 * KPlooshFinder is required for iOS 16+ (Darwin 22+).
 * Kernel64Patcher (legacy) handles iOS 15 and below (Darwin 21 and below).
 *
 * Returns the iOS major version on success, or -1 on failure.
 */
#define SCAN_CHUNK  (1024 * 1024)   /* 1 MiB per read               */
#define SCAN_MAX    (64 * 1024 * 1024) /* give up after 64 MiB       */

static int get_ios_major(const char *kernel_path)
{
    FILE *f = fopen(kernel_path, "rb");
    if (!f) { perror("open kernelcache"); return -1; }

    const char *needle = "Darwin Kernel Version ";
    const size_t nlen  = strlen(needle);

    /*
     * We read in overlapping chunks so the needle is never split across
     * a chunk boundary.  Each new chunk starts (nlen - 1) bytes before
     * the end of the previous chunk.
     */
    char *buf = malloc(SCAN_CHUNK + nlen);
    if (!buf) { fclose(f); fprintf(stderr, "OOM\n"); return -1; }

    size_t total   = 0;
    size_t overlap = 0;          /* bytes carried over from last chunk */
    int    ios     = -1;

    while (total < SCAN_MAX) {
        size_t want = SCAN_CHUNK;
        size_t got  = fread(buf + overlap, 1, want, f);

        if (got == 0) break;     /* EOF or error */

        size_t window = overlap + got;
        char *p = memmem(buf, window, needle, nlen);
        if (p) {
            int darwin_major = atoi(p + nlen);
            if (darwin_major >= 14) {
                ios = darwin_major - 6;
                printf("Detected kernelcache: Darwin %d → iOS %d\n",
                       darwin_major, ios);
            } else {
                fprintf(stderr, "Unexpected Darwin major: %d\n", darwin_major);
            }
            break;
        }

        /* Carry the tail forward so we don't miss a split needle */
        overlap = (window >= nlen - 1) ? nlen - 1 : window;
        memmove(buf, buf + window - overlap, overlap);
        total += got;
    }

    free(buf);
    fclose(f);

    if (ios < 0)
        fprintf(stderr,
                "Darwin version string not found in kernelcache "
                "(is it still IMG4/lzfse compressed?)\n");
    return ios;
}

/*
 * Wrapper usage:
 *   Kernel64Patcher <kernel_in> <kernel_out> [flags...]
 *
 * Flags understood by each tool:
 *
 *   Kernel64Patcher (legacy, iOS < 15): <kernel_in> <kernel_out>
 *     -a  Patch AMFI
 *     -f  Patch AppleFirmwareUpdate img4 signature check
 *     -s  Patch SPUFirmwareValidation          (iOS 15 only)
 *     -r  Patch RootVPNotAuthenticatedAfterMounting (iOS 15 only)
 *     -o  Patch could_not_authenticate_personalized_root_hash (iOS 15 only)
 *     -e  Patch root volume seal is broken     (iOS 15 only)
 *     -u  Patch update_rootfs_rw               (iOS 15 only)
 *     -p  Patch AMFIInitializeLocalSigningPublicKey (iOS 15 only)
 *     -h  Patch is_root_hash_authentication_required_ios (iOS 16 – handled
 *         by legacy tool when targeting iOS 15 devices with that flag)
 *     -l  Patch launchd path
 *     -t  Patch tfp0
 *     -d  Patch developer mode
 *
 *   KPlooshFinder (iOS >= 15): <kernel_in> <kernel_out>
 *
 * The wrapper inspects the detected iOS version and builds a fresh argv[]
 * containing only the flags that the selected tool accepts.
 */

int main(int argc, char *argv[])
{
    if (argc < 3) {
        fprintf(stderr,
                "Usage: %s <kernel_in> <kernel_out> [flags...]\n", argv[0]);
        return 1;
    }

    const char *kernel_in = argv[1];

    int ios = get_ios_major(kernel_in);
    if (ios < 0) {
        fprintf(stderr, "Failed to detect iOS version from kernelcache\n");
        return 1;
    }

    char tool_path[256];
    int rc;

    /*
     * KPlooshFinder handles iOS 16+ (palera1n supports A8–A11, iOS 15–16).
     * Kernel64Patcher (legacy) handles iOS 15 and below.
     */
    if (ios >= 15) {
        printf("iOS %d >= 15: using embedded KPlooshFinder\n", ios);
        rc = write_temp_exec("kpf",
                             KPlooshFinder, KPlooshFinder_len,
                             tool_path, sizeof(tool_path));
    } else {
        printf("iOS %d < 15: using embedded Kernel64Patcher\n", ios);
        rc = write_temp_exec("k64",
                             Kernel64Patcher_legacy, Kernel64Patcher_legacy_len,
                             tool_path, sizeof(tool_path));
    }

    if (rc != 0) {
        fprintf(stderr, "Failed to extract embedded tool\n");
        return 1;
    }

    char **tool_argv = calloc((size_t)(argc + 1), sizeof(char *));
    if (!tool_argv) {
        fprintf(stderr, "Out of memory building argv\n");
        return NULL;
    }

    int out = 0;
    tool_argv[out++] = (char *)tool_path;  /* argv[0] = tool being exec'd    */
    tool_argv[out++] = argv[1];            /* kernel_in  (positional arg 1)  */
    tool_argv[out++] = argv[2];            /* kernel_out (positional arg 2)  */

    if (ios < 15) {
        tool_argv[out++] = "-a"; 
    }

    tool_argv[out] = NULL;

    execv(tool_path, tool_argv);

    /* execv only returns on error */
    perror("execv");
    free(tool_argv);
    unlink(tool_path);
    return 1;
}
