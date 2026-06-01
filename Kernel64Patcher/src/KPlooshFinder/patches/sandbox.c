#include <stdint.h>
#include <stdio.h>
#include <stdbool.h>
#include "plooshfinder.h"
#include "plooshfinder32.h"
#include "patches/sandbox.h"

uint32_t *vnode_lookup;
uint32_t *vnode_put;
uint32_t *vfs_context_current;
void *sandbox_rbuf = 0;

bool patch_vnode_lookup(struct pf_patch_t *patch, uint32_t *stream) {
    if(vnode_lookup) {
        return false;
    }

    uint32_t *try = &stream[8] + ((stream[8] >> 5) & 0xfff);
    if (!pf_maskmatch32(try[0], 0xaa0003e0, 0xffe0ffff) ||   // MOV x0, Xn
        !pf_maskmatch32(try[1], 0x94000000, 0xfc000000) ||    // BL _sfree
        !pf_maskmatch32(try[3], 0xb4000000, 0xff000000) ||    // CBZ
        !pf_maskmatch32(try[4], 0x94000000, 0xfc000000)) {   // BL _vnode_put
        return false;
    }

    printf("%s: Found vnode_lookup\n", __FUNCTION__);
    vfs_context_current = pf_follow_branch(sandbox_rbuf, &stream[1]);
    vnode_lookup = pf_follow_branch(sandbox_rbuf, &stream[6]);
    vnode_put = pf_follow_branch(sandbox_rbuf, &try[4]);
    pf_disable_patch(patch);
    return true;
}

void patch_sandbox_kext(void *real_buf, void *sandbox_buf, size_t sandbox_len) {
    sandbox_rbuf = real_buf;

    uint32_t matches[] = {
        0x35000000, // CBNZ
        0x94000000, // BL _vfs_context_current
        0xAA0003E0, // MOV Xn, X0
        0xD1006002, // SUB
        0x00000000, // MOV X0, Xn || MOV W1, #0
        0x00000000, // MOV X0, Xn || MOV W1, #0
        0x94000000, // BL _vnode_lookup
        0xAA0003E0, // MOV Xn, X0
        0x35000000  // CBNZ
    };
    uint32_t masks[] = {
        0xFF000000,
        0xFC000000,
        0xFFFFFFE0,
        0xFFFFE01F,
        0x00000000,
        0x00000000,
        0xFC000000,
        0xFFFFFFE0,
        0xFF000000
    };
    struct pf_patch_t vnode_lookup_patch = pf_construct_patch(matches, masks, sizeof(matches) / sizeof(uint32_t), (void *) patch_vnode_lookup);

    struct pf_patch_t patches[] = {
        vnode_lookup_patch
    };

    struct pf_patchset_t patchset = pf_construct_patchset(patches, sizeof(patches) / sizeof(struct pf_patch_t), (void *) pf_find_maskmatch32);

    pf_patchset_emit(sandbox_buf, sandbox_len, patchset);
}