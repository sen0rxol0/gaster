#ifndef KERNELDIFF_H
#define KERNELDIFF_H

/*
 * kerneldiff – produce a bpatch-format diff between two kernel cache files.
 *
 * kc_original  path to the original (unpatched) kernelcache
 * kc_patched   path to the patched kernelcache  (must be the same size)
 * kc_diff      path to write the output bpatch file
 *
 * Returns 0 on success, -1 on any error (file I/O, size mismatch, or
 * more differences than MAX_DIFF).
 *
 * Source: https://github.com/verygenericname/kerneldiff_C
 */
int kerneldiff(const char *kc_original, const char *kc_patched, const char *kc_diff);

#endif /* KERNELDIFF_H */