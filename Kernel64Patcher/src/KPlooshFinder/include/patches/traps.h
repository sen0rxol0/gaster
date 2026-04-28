#ifndef _PATCHES_TRAPS_H
#define _PATCHES_TRAPS_H
#include <stdint.h>
#include <stdlib.h>

extern bool found_mach_traps;

void patch_mach_traps(void *real_buf, void *text_buf, size_t text_len);

#endif