#include <stdint.h>
#include <stdio.h>
#include <stdbool.h>
#include "formats/macho.h"
#include "plooshfinder.h"
#include "plooshfinder64.h"
#include "patches/sbops.h"

bool found_sbops = false;
uint64_t *sbops;
void *sbops_rbuf;
uint64_t sbops_string_addr;

bool patch_sbops(struct pf_patch_t *patch, uint64_t *stream) {
    printf("%s: Found sbops\n", __FUNCTION__);

    sbops = macho_va_to_ptr(sbops_rbuf, macho_xnu_untag_va(stream[3]));
    found_sbops = true;
    pf_disable_patch(patch);
    return true;
}

void sbops_patch(void *real_buf, void *data_const_buf, size_t data_const_len, uint64_t string_addr) {
    sbops_rbuf = real_buf;
    sbops_string_addr = string_addr;

    uint64_t sbops_match[] = {
        string_addr
    };
    uint64_t sbops_mask[] = {
        0xffffffffffffffff
    };

    struct pf_patch_t sbops = pf_construct_patch(sbops_match, sbops_mask, sizeof(sbops_match) / sizeof(uint64_t), (void *) patch_sbops);

    struct pf_patch_t patches[] = {
        sbops
    };

    struct pf_patchset_t patchset = pf_construct_patchset(patches, sizeof(patches) / sizeof(struct pf_patch_t), (void *) pf_find_maskmatch64);

    pf_patchset_emit(data_const_buf, data_const_len, patchset);
}