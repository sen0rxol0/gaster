#ifndef _PATCHES_SHELLCODE_H
#define _PATCHES_SHELLCODE_H
#include <stdint.h>

extern uint32_t _sandbox_shellcode[4];
extern uint32_t _sandbox_shellcode_setuid_patch[4];
extern uint64_t orig[1];

#define _sandbox_shellcode_ptrs orig

extern uint32_t _nvram_shc[4];
#define _sandbox_shellcode_end _nvram_shc
#define _nvram_shc_end _nvram_shc + 4

#endif