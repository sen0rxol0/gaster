#ifdef __gnu_linux__
#define _GNU_SOURCE 
#endif

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "formats/macho.h"
#include "plooshfinder.h"
#include "patches/amfi.h"
#include "patches/sandbox.h"
#include "patches/sbops.h"
#include "patches/shellcode.h"
#include "patches/text.h"
#include "mac.h"

void *kernel_buf;
size_t kernel_len;
int platform = 0;

#define addr_to_ptr(addr) macho_va_to_ptr(kernel_buf, macho_xnu_untag_va(addr))
#define patch(function, addr, size, ...) function(kernel_buf, addr_to_ptr(addr), size, ##__VA_ARGS__);
#define find_str_in_region(str, addr, size) memmem(addr, size, str, sizeof(str));
#define find_partial_str_in_region(str, addr, size) memmem(addr, size, str, sizeof(str) - 1);
#define patch_sbop(ops, op, val)       \
    if (ops->op) {                     \
        ops->op &= 0xFFFFFFFF00000000; \
        ops->op |= val;                \
    }

void patch_kernel() {
    printf("Starting KPlooshFinder\n");

    struct section_64 *data_const = macho_find_section(kernel_buf, "__DATA_CONST", "__const");
    if (!data_const) {
        printf("Unable to find data const!\n");
        return;
    }

    struct section_64 *cstring = macho_find_section(kernel_buf, "__TEXT", "__cstring");
    if (!cstring) {
        printf("Unable to find cstring!\n");
        return;
    }

    struct section_64 *text = macho_find_section(kernel_buf, "__TEXT_EXEC", "__text");
    if (!text) {
        printf("Unable to find text!\n");
        return;
    }


    const char rootvp_string[] = "rootvp not authenticated after mounting";
    const char *rootvp_string_match = find_partial_str_in_region(rootvp_string, kernel_buf + cstring->offset, cstring->size);
    const char constraints_string[] = "mac_proc_check_launch_constraints";
    const char *constraints_string_match = find_str_in_region(constraints_string, kernel_buf + cstring->offset, cstring->size);
    const char cryptex_string[] = "/private/preboot/Cryptexes";
    const char *cryptex_string_match = find_str_in_region(cryptex_string, kernel_buf + cstring->offset, cstring->size);
    const char kmap_port_string[] = "userspace has control access to a"; // iOS 14 had broken panic strings
    const char *kmap_port_string_match = find_partial_str_in_region(kmap_port_string, kernel_buf + cstring->offset, cstring->size);

    struct mach_header_64 *apfs_kext = macho_find_kext(kernel_buf, "com.apple.filesystems.apfs");
    if (!apfs_kext) {
        printf("Unable to find APFS kext!\n");
        return;
    }

    struct mach_header_64 *amfi_kext = macho_find_kext(kernel_buf, "com.apple.driver.AppleMobileFileIntegrity");
    if (!amfi_kext) {
        printf("Unable to find AMFI kext!\n");
        return;
    }

    struct section_64 *amfi_text = macho_find_section(amfi_kext, "__TEXT_EXEC", "__text");
    if (!amfi_text) {
        printf("Unable to find AMFI text!\n");
        return;
    }

    struct section_64 *amfi_cstring = macho_find_section(amfi_kext, "__TEXT", "__cstring");

    struct section_64 *devmode_cstring = amfi_cstring ? amfi_cstring : cstring;
    void *devmode_straddr = amfi_cstring ? addr_to_ptr(amfi_cstring->addr) : kernel_buf + cstring->offset;

    const char dev_mode_string[] = "AMFI: developer mode is force enabled\n";
    const char *dev_mode_string_match = find_str_in_region(dev_mode_string, devmode_straddr, devmode_cstring->size);

    patch(patch_amfi_kext, amfi_text->addr, amfi_text->size, constraints_string_match != NULL, dev_mode_string_match != NULL);

    struct mach_header_64 *sandbox_kext = macho_find_kext(kernel_buf, "com.apple.security.sandbox");
    if (!sandbox_kext) {
        printf("Unable to find sandbox kext!\n");
        return;
    }

    struct section_64 *sandbox_text = macho_find_section(sandbox_kext, "__TEXT_EXEC", "__text");
    if (!sandbox_text) {
        printf("Unable to find sandbox text!\n");
        return;
    }

    patch(patch_sandbox_kext, sandbox_text->addr, sandbox_text->size);

    // sbops useful shit
    /*
    fffffff006f33d98  data_fffffff006f33d98:
fffffff006f33d98                          ad b9 63 05 f0 ff ff ff          ..c.....
fffffff006f33da0  14 9f 63 05 f0 ff ff ff f0 3d f3 06 f0 ff ff ff  ..c......=......
fffffff006f33db0  01 00 00 00 00 00 00 00 f8 3d f3 06 f0 ff ff ff  .........=......
fffffff006f33dc0  00 00 00 00 00 00 00 00 94 3d f3 06 f0 ff ff ff  .........=......
fffffff006f33dd0  00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00  ................
fffffff006f33de0  00 00 00 00 00 00 00 00                          ........
fffffff006f33de8  data_fffffff006f33de8:
fffffff006f33de8                          00 00 00 00 00 00 00 00          ........
fffffff006f33df0  2c 9f 63 05 f0 ff ff ff 00 00 00 00 00 00 00 00  ,.c.............
fffffff006f33e00  00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00  ................
fffffff006f33e10  00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00  ................
fffffff006f33e20  00 00 00 00 00 00 00 00 a8 b7 68 06 f0 ff ff ff  ..........h.....
fffffff006f33e30  9c 1f 67 06 f0 ff ff ff 00 00 00 00 00 00 00 00  ..g.............

*/

    struct section_64 *sandbox_cstring = macho_find_section(sandbox_kext, "__TEXT", "__cstring");
    struct section_64 *sbops_cstring = sandbox_cstring ? sandbox_cstring : cstring;
    void *sbops_cstring_addr = sandbox_cstring ? addr_to_ptr(sandbox_cstring->addr) : kernel_buf + cstring->offset;

    const char sbops_string[] = "Seatbelt sandbox policy";
    const char *sbops_string_match = find_str_in_region(sbops_string, sbops_cstring_addr, sbops_cstring->size);

    if (!sbops_string_match) {
        printf("Unable to find sbops string!\n");
        return;
    }

    struct section_64 *sandbox_data_const = macho_find_section(sandbox_kext, "__DATA_CONST", "__const");
    struct section_64 *sbops_data_const = sandbox_cstring ? sandbox_data_const : data_const;
    
    uint64_t sbops_string_addr = sbops_cstring->addr + (uint64_t) ((void *) sbops_string_match - sbops_cstring_addr);

    patch(sbops_patch, sbops_data_const->addr, sbops_data_const->size, sbops_string_addr);

    patch(text_exec_patches, text->addr, text->size, text->addr, rootvp_string_match != NULL, cryptex_string_match != NULL, kmap_port_string_match != NULL);

    if (!rootvp_string_match) {
        const char *snapshot = "com.apple.os.update-";
        struct section_64 *apfs_cstring = macho_find_section(apfs_kext, "__TEXT", "__cstring");
        struct section_64 *snapshot_cstring = apfs_cstring ? apfs_cstring : cstring;
        void *snapshot_cstring_addr = apfs_cstring ? addr_to_ptr(apfs_cstring->addr) : kernel_buf + cstring->offset;

        char *snapshotStr = find_str_in_region(snapshot, snapshot_cstring_addr, snapshot_cstring->size);

        if (snapshotStr) {
            *snapshotStr = 'x';
            printf("%s: Disabled snapshot temporarily\n", __FUNCTION__);
        }
    }

    printf("Patching completed successfully.\n");
}

int main(int argc, char **argv) {
    FILE *fp = NULL;

    if (argc < 3) {
        printf("Usage: %s <input kernel> <patched kernel>\n", argv[0]);
        return 0;
    }

    fp = fopen(argv[1], "rb");
    if (!fp) {
        printf("Failed to open kernel!\n");
        return -1;
    }

    fseek(fp, 0, SEEK_END);
    kernel_len = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    kernel_buf = (void *) malloc(kernel_len);
    if (!kernel_buf) {
        printf("Out of memory while allocating region for kernel!\n");
        fclose(fp);
        return -1;
    }

    fread(kernel_buf, 1, kernel_len, fp);
    fclose(fp);

    uint32_t magic = macho_get_magic(kernel_buf);

    if (!magic) {
        free(kernel_buf);
        return 1;
    }

    void *orig_kernel_buf = kernel_buf;
    if (magic == 0xbebafeca) {
        kernel_buf = macho_find_arch(kernel_buf, CPU_TYPE_ARM64);
        if (!kernel_buf) {
            free(orig_kernel_buf);
            return 1;
        }
    }

    platform = macho_get_platform(kernel_buf);
    if (platform == 0) {
        free(orig_kernel_buf);
        return 1;
    }

    patch_kernel();

    fp = fopen(argv[2], "wb");
    if(!fp) {
        printf("Failed to open output file!\n");
        free(orig_kernel_buf);
        return -1;
    }
    
    fwrite(orig_kernel_buf, 1, kernel_len, fp);
    fflush(fp);
    fclose(fp);

    free(orig_kernel_buf);

    return 0;
}
