//
//  main.c
//  gastera1n
//

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>

#include "log.h"
#include "ideviceenterramdisk.h"
#include "gastera1n.h"


bool ramdiskBootMode = false;
bool pwnDFUMode = false;
bool DEBUG_ENABLED = false;

int main(int argc, char **argv)
{
#ifdef __APPLE__
    system("killall -9 iTunesHelper 2>/dev/null");
    system("killall -9 iTunes 2>/dev/null");
    system("kill -STOP $(pgrep AMPDeviceDiscoveryAgent) 2>/dev/null");
#endif

    // FIX: the original code did `printf("%s\n", "text\n")` throughout,
    // which printed two newlines (one from the literal, one from the format).
    // Use puts() for plain string lines — it appends exactly one newline.
    puts("--==== gastera1n ====--");
    puts("// Super thanks to:");
    puts("//\thttps://github.com/0x7ff/gaster");
    puts("//\thttps://github.com/mineek/openra1n");
    puts("// Credits:");
    puts("//\thttps://github.com/xerub/sshrd");
    puts("//\thttps://github.com/dayt0n/restored-external-hax");
    puts("//\thttps://github.com/xerub/img4lib");
    puts("//\thttps://github.com/tihmstar/libfragmentzip");
    puts("//\thttps://github.com/tihmstar/iBoot64Patcher");
    puts("//\thttps://github.com/Cryptiiiic/iBoot64Patcher");
    // FIX: corrected double-slash in the following URLs
    puts("//\thttps://github.com/Ralph0045/Kernel64Patcher");
    puts("//\thttps://github.com/palera1n/Kernel64Patcher");
    puts("//\thttps://github.com/palera1n/KPlooshFinder");
    puts("//\thttps://github.com/verygenericname/kerneldiff_C");
    puts("//\thttps://github.com/synackuk/belladonna");
    puts("//\thttps://github.com/libimobiledevice/libirecovery");
    puts("//\thttps://github.com/libimobiledevice/libplist");
    // FIX: corrected double-slash in the following URLs
    puts("//\thttps://github.com/ProcursusTeam/ldid");
    puts("//\thttps://github.com/realnp/ibootim");
    puts("//\thttps://github.com/1Conan/tsschecker");
    puts("");

    int opt;
    while ((opt = getopt(argc, argv, "hdpt")) != -1) {
        switch (opt) {
        case 'h':
            puts("Optional arguments:\n"
                 "  -t  Boot with files from cache directory\n"
                 "  -d  Enable debug logging\n"
                 "  -p  Run gaster");
            return 0;
        case 'd':
            log_warn("%s\n", "Debug is enabled!");
            DEBUG_ENABLED = true;
            break;
        case 'p':
            log_warn("%s\n", "Entering pwned DFU mode with gaster!");
            pwnDFUMode = true;
            break;
        case 't':
            log_warn("%s\n", "Booting directly, skipping downloading and patching!");
            ramdiskBootMode = true;
            break;
        default:
            break;
        }
    }

    int ret = 0;
    
    if (pwnDFUMode) {
        ret = gastera1n();
        log_info("Device reached pwned DFU mode.");
    } else {
        ret = ideviceenterramdisk_load();
        log_info("Device reached SSH ramdisk mode.");
    }

    if (ret == 0)
        puts("DONE!");

    return ret;
}
