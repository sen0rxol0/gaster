#ifndef _PATCHES_KEXT_H
#define _PATCHES_KEXT_H
#include <stdint.h>
#include <stdlib.h>

void patch_all_kexts(void *real_buf, void *kext_buf, size_t kext_len);

#endif