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

extern char *optarg;

int main(int argc, char **argv)
{
#ifdef __APPLE__
    system("killall -9 iTunesHelper 2>/dev/null");
    system("killall -9 iTunes 2>/dev/null");
    system("kill -STOP $(pgrep AMPDeviceDiscoveryAgent) 2>/dev/null");
#endif

    puts("--==== gastera1n v2.0 ====--");
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
    puts("//\thttps://github.com/Ralph0045/Kernel64Patcher");
    puts("//\thttps://github.com/palera1n/Kernel64Patcher");
    puts("//\thttps://github.com/palera1n/KPlooshFinder");
    puts("//\thttps://github.com/verygenericname/kerneldiff_C");
    puts("//\thttps://github.com/synackuk/belladonna");
    puts("//\thttps://github.com/libimobiledevice/libirecovery");
    puts("//\thttps://github.com/libimobiledevice/libplist");
    puts("//\thttps://github.com/ProcursusTeam/ldid");
    puts("//\thttps://github.com/realnp/ibootim");
    puts("//\thttps://github.com/1Conan/tsschecker");
    puts("");

    const char *cache_dir_override = NULL;
    
    int opt;
    while ((opt = getopt(argc, argv, "hdptc:")) != -1) {
        switch (opt) {
        case 'h':
            puts("gastera1n v2.0\n"
                 "Optional arguments:\n"
                 "  -t  Boot with files from cache directory\n"
                 "  -c <dir>  Set custom cache directory\n"
                 "  -d  Enable debug logging\n"
                 "  -p  Run gaster");
            return 0;
        case 'd':
            DEBUG_ENABLED = true;
            log_warn("%s\n", "Debug is enabled!");
            break;
        case 'p':
            pwnDFUMode = true;
            log_warn("%s\n", "Entering pwned DFU mode with gaster!");
            break;
        case 't':
            ramdiskBootMode = true;
            log_warn("%s\n", "Booting directly, skipping downloading and patching!");
            break;
        case 'c':
            cache_dir_override = optarg;
            log_warn("Using custom cache directory: %s\n", cache_dir_override);
            break;
        default:
            break;
        }
    }

    if (gastera1n() != 0) {
        log_error("Failed to put device in pwned DFU\n");
        return -1;
    }

    log_info("Device is now in pwned DFU mode.");
        
    if(!pwnDFUMode && ideviceenterramdisk_load(cache_dir_override) != 0) {
        log_error("Failed to boot device into SSH ramdisk\n");
        return -1;
    }

    puts("DONE!");

    return 0;
}
