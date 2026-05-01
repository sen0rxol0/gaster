#ifndef _PATCHES_SBOPS_H
#define _PATCHES_SBOPS_H
#include <stdint.h>
#include <stdlib.h>

extern bool found_sbops;
extern uint64_t *sbops;

void sbops_patch(void *real_buf, void *data_const_buf, size_t data_const_len, uint64_t string_addr);

#endif