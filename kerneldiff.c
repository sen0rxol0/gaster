/*
 * kerneldiff.c
 *
 * Produce a bpatch-format diff between two kernel cache files.
 * Source: https://github.com/verygenericname/kerneldiff_C
 *
 * Fixes applied vs. original:
 *   • stat() call passed &st correctly (was passing &&st via pointer arg)
 *   • fread() return value now checked
 *   • Size mismatch between original and patched is now an error
 *   • diff table moved from stack (~1 MB) to heap
 *   • fopen/fwrite return values checked for the output file
 *   • All string arguments are const-correct per the header declaration
 */

#include "kerneldiff.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define MAX_DIFF          16384
#define DIFF_COMP_MAX_SZ  20

/* Header written at the top of every bpatch file. */
static const char kerneldiff_amfi[] = "#AMFI\n\n";

/* One bpatch record: offset, original byte, patched byte – all as hex strings. */
typedef struct {
    char offset[DIFF_COMP_MAX_SZ];
    char orig  [DIFF_COMP_MAX_SZ];
    char patch [DIFF_COMP_MAX_SZ];
} diff_entry_t;

/* Read an entire file into a heap buffer.  Fills *out_size with the file
   size.  Returns the buffer on success; caller must free().
   Returns NULL on any error. */
static char *read_file(const char *path, size_t *out_size)
{
    struct stat st;
    if (stat(path, &st) != 0) {
        perror("kerneldiff: stat");
        return NULL;
    }

    FILE *fp = fopen(path, "rb");
    if (!fp) {
        perror("kerneldiff: fopen");
        return NULL;
    }

    size_t size = (size_t)st.st_size;
    char *data = malloc(size);
    if (!data) {
        perror("kerneldiff: malloc");
        fclose(fp);
        return NULL;
    }

    if (fread(data, 1, size, fp) != size) {
        fprintf(stderr, "kerneldiff: short read on '%s'\n", path);
        free(data);
        fclose(fp);
        return NULL;
    }

    fclose(fp);
    *out_size = size;
    return data;
}

int kerneldiff(const char *kc_original, const char *kc_patched, const char *kc_diff)
{
    size_t orig_size = 0, patch_size = 0;
    char *o = read_file(kc_original, &orig_size);
    char *p = read_file(kc_patched,  &patch_size);

    if (!o || !p) {
        free(o);
        free(p);
        return -1;
    }

    if (orig_size != patch_size) {
        fprintf(stderr,
                "kerneldiff: size mismatch: '%s' (%zu bytes) vs '%s' (%zu bytes)\n",
                kc_original, orig_size, kc_patched, patch_size);
        free(o);
        free(p);
        return -1;
    }

    /* Heap-allocate the diff table to avoid ~1 MB of stack usage. */
    diff_entry_t *diff = calloc(MAX_DIFF, sizeof(diff_entry_t));
    if (!diff) {
        perror("kerneldiff: calloc");
        free(o);
        free(p);
        return -1;
    }

    int diff_idx = 0;
    for (size_t i = 0; i < orig_size; i++) {
        if (o[i] == p[i]) continue;

        if (diff_idx >= MAX_DIFF) {
            fprintf(stderr,
                    "kerneldiff: too many differences (max %d supported)\n",
                    MAX_DIFF);
            free(diff);
            free(o);
            free(p);
            return -1;
        }

        snprintf(diff[diff_idx].offset, DIFF_COMP_MAX_SZ, "0x%zx", i);
        snprintf(diff[diff_idx].orig,   DIFF_COMP_MAX_SZ, "0x%x",  (unsigned char)o[i]);
        snprintf(diff[diff_idx].patch,  DIFF_COMP_MAX_SZ, "0x%x",  (unsigned char)p[i]);
        diff_idx++;
    }

    free(o);
    free(p);

    FILE *fp = fopen(kc_diff, "w+");
    if (!fp) {
        perror("kerneldiff: fopen output");
        free(diff);
        return -1;
    }

    if (fwrite(kerneldiff_amfi, 1, sizeof(kerneldiff_amfi) - 1, fp)
            != sizeof(kerneldiff_amfi) - 1) {
        fprintf(stderr, "kerneldiff: failed to write header to '%s'\n", kc_diff);
        fclose(fp);
        free(diff);
        return -1;
    }

    for (int i = 0; i < diff_idx; i++) {
        printf("kerneldiff: %s %s %s\n",
               diff[i].offset, diff[i].orig, diff[i].patch);
        if (fprintf(fp, "%s %s %s\n",
                    diff[i].offset, diff[i].orig, diff[i].patch) < 0) {
            fprintf(stderr, "kerneldiff: write error on '%s'\n", kc_diff);
            fclose(fp);
            free(diff);
            return -1;
        }
    }

    fclose(fp);
    free(diff);
    return 0;
}