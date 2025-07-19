/*
* Copyright 2020, @Ralph0045
* gcc Kernel64Patcher.c -o Kernel64Patcher
*
*
* source: https://github.com/Ralph0045/Kernel64Patcher
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "kernel64patcher_patchfinder64.c"

#define GET_OFFSET(kernel_len, x) (x - (uintptr_t) kernel_buf)

int get_amfi_out_of_my_way_patch(void* kernel_buf,size_t kernel_len) {

    printf("%s: Entering ...\n",__FUNCTION__);

    void* xnu = memmem(kernel_buf,kernel_len,"root:xnu-",9);
    int kernel_vers = atoi(xnu+9);
    printf("%s: Kernel-%d inputted\n",__FUNCTION__, kernel_vers);
    char amfiString[33] = "entitlements too small";
    int stringLen = 22;
    if (kernel_vers >= 7938) { // Using "entitlements too small" fails on iOS 15 Kernels
        strncpy(amfiString, "Internal Error: No cdhash found.", 33);
        stringLen = 32;
    }
    void* ent_loc = memmem(kernel_buf,kernel_len,amfiString,stringLen);
    if(!ent_loc) {
        printf("%s: Could not find %s string\n",__FUNCTION__, amfiString);
        return -1;
    }
    printf("%s: Found %s str loc at %p\n",__FUNCTION__,amfiString,GET_OFFSET(kernel_len,ent_loc));
    addr_t ent_ref = xref64(kernel_buf,0,kernel_len,(addr_t)GET_OFFSET(kernel_len, ent_loc));
    if(!ent_ref) {
        printf("%s: Could not find %s xref\n",__FUNCTION__,amfiString);
        return -1;
    }
    printf("%s: Found %s str ref at %p\n",__FUNCTION__,amfiString,(void*)ent_ref);
    addr_t next_bl = step64(kernel_buf, ent_ref, 100, INSN_CALL);
    if(!next_bl) {
        printf("%s: Could not find next bl\n",__FUNCTION__);
        return -1;
    }
    next_bl = step64(kernel_buf, next_bl+0x4, 200, INSN_CALL);
    if(!next_bl) {
        printf("%s: Could not find next bl\n",__FUNCTION__);
        return -1;
    }
    if(kernel_vers>3789) {
        next_bl = step64(kernel_buf, next_bl+0x4, 200, INSN_CALL);
        if(!next_bl) {
            printf("%s: Could not find next bl\n",__FUNCTION__);
            return -1;
        }
    }
    addr_t function = follow_call64(kernel_buf, next_bl);
    if(!function) {
        printf("%s: Could not find function bl\n",__FUNCTION__);
        return -1;
    }
    printf("%s: Patching AMFI at %p\n",__FUNCTION__,(void*)function);
    *(uint32_t *)(kernel_buf + function) = 0x320003E0;
    *(uint32_t *)(kernel_buf + function + 0x4) = 0xD65F03C0;
    return 0;
}

int
kernel64patcher_amfi(char *kernel_in, char *kernel_out) {

    printf("%s: Starting Kernel64Patcher...\n", __FUNCTION__);

    void* kernel_buf;
    size_t kernel_len;

    FILE* fp = NULL;
    fp = fopen(kernel_in, "rb");
    if(!fp) {
        printf("%s: Error opening %s!\n", __FUNCTION__, kernel_in);
        return -1;
    }

    fseek(fp, 0, SEEK_END);
    kernel_len = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    kernel_buf = (void*)malloc(kernel_len);
    if(!kernel_buf) {
        printf("%s: Out of memory!\n", __FUNCTION__);
        fclose(fp);
        return -1;
    }

    fread(kernel_buf, 1, kernel_len, fp);
    fclose(fp);

    if(memmem(kernel_buf,kernel_len,"KernelCacheBuilder",18)) {
        printf("%s: Detected IMG4/IM4P, you have to unpack and decompress it!\n",__FUNCTION__);
        return -1;
    }

    int is_fat = 0;
    void* fat_buf;
    if (*(uint32_t*)kernel_buf == 0xbebafeca) {
        printf("%s: Detected fat macho kernel\n",__FUNCTION__);

        is_fat = 1;
        fat_buf = (void*)malloc(28);
        if(!fat_buf) {
            printf("%s: Out of memory!\n", __FUNCTION__);
            free(kernel_buf);
            return -1;
        }
        memcpy(fat_buf, kernel_buf, 28);

        memmove(kernel_buf,kernel_buf+28,kernel_len);
    }

    printf("Kernel64Patcher: Adding AMFI_get_out_of_my_way patch...\n");
    get_amfi_out_of_my_way_patch(kernel_buf,kernel_len);

    /* Write patched kernel */
    printf("%s: Writing out patched file to %s...\n", __FUNCTION__, kernel_out);

    fp = fopen(kernel_out, "wb+");
    if(!fp) {
        printf("%s: Unable to open %s!\n", __FUNCTION__, kernel_out);
        free(kernel_buf);
        return -1;
    }

    if (is_fat == 1) {
        memmove(kernel_buf, kernel_buf - 28, kernel_len);
        memcpy(kernel_buf, fat_buf, 28);
        free(fat_buf);
    }

    fwrite(kernel_buf, 1, kernel_len, fp);
    fflush(fp);
    fclose(fp);

    free(kernel_buf);

    printf("%s: Quitting Kernel64Patcher...\n", __FUNCTION__);

    return 0;
}
