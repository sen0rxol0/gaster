#include <stdint.h>
#include <stdio.h>
#include <stdbool.h>
#include "formats/macho.h"
#include "plooshfinder.h"
#include "plooshfinder64.h"
#include "patches/traps.h"

bool found_mach_traps = false;
void *traps_rbuf;

bool mach_traps_patch(uint32_t *tfp) {
    if(found_mach_traps) {
        return false;
    }
    printf("%s: Found mach traps\n", __FUNCTION__);

    // for the task for pid routine we only need to patch the first branch that checks if the pid == 0
    // we just replace it with a nop
    // see vm_unix.c in xnu
    uint32_t* tfp0check = pf_find_prev(tfp, 0x20, 0x34000000, 0xff000000);
    if(!tfp0check)
    {
        return false;
    }

    tfp0check[0] = nop;
    printf("%s: Found tfp0\n", __FUNCTION__);
    found_mach_traps = true;

    return true;
}

bool mach_traps_callback(struct pf_patch_t *patch, uint64_t *stream) {
    return mach_traps_patch(macho_va_to_ptr(traps_rbuf, macho_xnu_untag_va(stream[45 * 4 + 1])));
}

bool mach_traps_callback_alt(struct pf_patch_t *patch, uint64_t *stream) {
    return mach_traps_patch(macho_va_to_ptr(traps_rbuf, macho_xnu_untag_va(stream[45 * 3 + 1])));
}

void patch_mach_traps(void *real_buf, void *text_buf, size_t text_len) {
    traps_rbuf = real_buf;

    uint64_t traps_match[] = {
        0x0000000000000000, 0, 0x0000000000000000, 0x0000000000000000,
        0x0000000000000000, 0, 0x0000000000000000, 0x0000000000000000,
        0x0000000000000000, 0, 0x0000000000000000, 0x0000000000000000,
        0x0000000000000000, 0, 0x0000000000000000, 0x0000000000000000,
        0x0000000000000000, 0, 0x0000000000000000, 0x0000000000000000,
        0x0000000000000000, 0, 0x0000000000000000, 0x0000000000000000,
        0x0000000000000000, 0, 0x0000000000000000, 0x0000000000000000,
        0x0000000000000000, 0, 0x0000000000000000, 0x0000000000000000,
        0x0000000000000000, 0, 0x0000000000000000, 0x0000000000000000,
        0x0000000000000000, 0, 0x0000000000000000, 0x0000000000000000,
        0x0000000000000004, 0, 0x0000000000000000, 0x0000000000000005,
    };
    uint64_t traps_mask[] = {
        0xffffffffffffffff, 0, 0xffffffffffffffff, 0xffffffffffffffff,
        0xffffffffffffffff, 0, 0xffffffffffffffff, 0xffffffffffffffff,
        0xffffffffffffffff, 0, 0xffffffffffffffff, 0xffffffffffffffff,
        0xffffffffffffffff, 0, 0xffffffffffffffff, 0xffffffffffffffff,
        0xffffffffffffffff, 0, 0xffffffffffffffff, 0xffffffffffffffff,
        0xffffffffffffffff, 0, 0xffffffffffffffff, 0xffffffffffffffff,
        0xffffffffffffffff, 0, 0xffffffffffffffff, 0xffffffffffffffff,
        0xffffffffffffffff, 0, 0xffffffffffffffff, 0xffffffffffffffff,
        0xffffffffffffffff, 0, 0xffffffffffffffff, 0xffffffffffffffff,
        0xffffffffffffffff, 0, 0xffffffffffffffff, 0xffffffffffffffff,
        0xffffffffffffffff, 0, 0x0000000000000000, 0xffffffffffffffff,
    };

    struct pf_patch_t mach_traps_patch = pf_construct_patch(traps_match, traps_mask, sizeof(traps_match) / sizeof(uint64_t), (void *) mach_traps_callback);

    uint64_t traps_match_alt[] = {
        0x0000000000000000, 0, 0x0000000000000000,
        0x0000000000000000, 0, 0x0000000000000000,
        0x0000000000000000, 0, 0x0000000000000000,
        0x0000000000000000, 0, 0x0000000000000000,
        0x0000000000000000, 0, 0x0000000000000000,
        0x0000000000000000, 0, 0x0000000000000000,
        0x0000000000000000, 0, 0x0000000000000000,
        0x0000000000000000, 0, 0x0000000000000000,
        0x0000000000000000, 0, 0x0000000000000000,
        0x0000000000000000, 0, 0x0000000000000000,
        0x0000000000000504, 0, 0x0000000000000000,
    };
    uint64_t traps_mask_alt[] = {
        0xffffffffffffffff, 0, 0xffffffffffffffff,
        0xffffffffffffffff, 0, 0xffffffffffffffff,
        0xffffffffffffffff, 0, 0xffffffffffffffff,
        0xffffffffffffffff, 0, 0xffffffffffffffff,
        0xffffffffffffffff, 0, 0xffffffffffffffff,
        0xffffffffffffffff, 0, 0xffffffffffffffff,
        0xffffffffffffffff, 0, 0xffffffffffffffff,
        0xffffffffffffffff, 0, 0xffffffffffffffff,
        0xffffffffffffffff, 0, 0xffffffffffffffff,
        0xffffffffffffffff, 0, 0xffffffffffffffff,
        0xffffffffffffffff, 0, 0x0000000000000000,
    };

    struct pf_patch_t mach_traps_patch_alt = pf_construct_patch(traps_match_alt, traps_mask_alt, sizeof(traps_match_alt) / sizeof(uint64_t), (void *) mach_traps_callback_alt);

    struct pf_patch_t patches[] = {
        mach_traps_patch,
        mach_traps_patch_alt
    };

    struct pf_patchset_t patchset = pf_construct_patchset(patches, sizeof(patches) / sizeof(struct pf_patch_t), (void *) pf_find_maskmatch64);

    pf_patchset_emit(text_buf, text_len, patchset);
}