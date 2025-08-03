//
//  main.c
//  gastera1n
//
//

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>

#include "log.h"
//
#include "idevicedfu.h"
#include "ideviceenterrecovery.h"
#include "ideviceenterramdisk.h"
#include "gastera1n.h"

extern bool DEBUG_ENABLED;
extern unsigned int ramdiskBootMode;
int pwnDFUMode;

static int
idevicepwn() {

	if (idevicedfu_find() != 0) {
		return -1;
	}

	sleep(1);

	return gastera1n();
}

static int
idevicepwn_ramdisk() {
	return ideviceenterramdisk_load();
}


int
main(int argc, char **argv) {

#ifdef __APPLE__
	system("killall -9 iTunesHelper 2> /dev/null");
	system("killall -9 iTunes 2> /dev/null");
	system("kill -STOP $(pgrep AMPDeviceDiscoveryAgent) 2> /dev/null"); // TY Siguza
#endif
	printf("%s\n", "--==== gastera1n ====--\n");
	printf("%s\n", "// Super thanks to:");
	printf("%s\n", "//\thttps://github.com/0x7ff/gaster");
	printf("%s\n", "//\thttps://github.com/mineek/openra1n");
	printf("%s\n", "// Credits:");
	printf("%s\n", "//\thttps://github.com/xerub/sshrd");
	printf("%s\n", "//\thttps://github.com/dayt0n/restored-external-hax");
	printf("%s\n", "//\thttps://github.com/xerub/img4lib");
	printf("%s\n", "//\thttps://github.com/tihmstar/libfragmentzip");
	printf("%s\n", "//\thttps://github.com/tihmstar/iBoot64Patcher");
	printf("%s\n", "//\thttps://github.com/Cryptiiiic/iBoot64Patcher");
	printf("%s\n", "//\thttps://github.com//Ralph0045/Kernel64Patcher");
	printf("%s\n", "//\thttps://github.com//verygenericname/Kernel64Patcher");
	printf("%s\n", "//\thttps://github.com//verygenericname/kerneldiff_C");
	printf("%s\n", "//\thttps://github.com/synackuk/belladonna");
	printf("%s\n", "//\thttps://github.com/libimobiledevice/libirecovery");
	printf("%s\n", "//\thttps://github.com/libimobiledevice/libplist");
	printf("%s\n", "//\thttps://github.com//ProcursusTeam/ldid");
	printf("%s\n", "//\thttps://github.com/realnp/ibootim");
	printf("%s\n", "//\thttps://github.com/1Conan/tsschecker");
	printf("\n\n");

	DEBUG_ENABLED = false;
	ramdiskBootMode = 0;
	pwnDFUMode = 0;
	int opt;

	while ((opt = getopt(argc, argv, "htdp")) != -1) {
	    switch (opt) {
				case 'h':
					printf("%s\n", "Optional arguments:\n\
						-t Boot files from tmp directory\n\
						-d Enable debug\n\
						-p Run gaster\n");
					return 0;
				case 't':
					log_warn("%s\n", "Booting directly, skipping downloading and patching!");
					ramdiskBootMode = 1;
					break;
				case 'd':
					log_warn("%s\n", "Debug is enabled!");
					DEBUG_ENABLED = true;
					break;
				case 'p':
					log_warn("%s\n", "Entering pwned DFU mode with gaster!");
					pwnDFUMode = 1;
					break;
				default:
					break;
     	}
	}

	int ret = EXIT_SUCCESS;
	ret = idevicepwn();

	if (ret != 0) {
		ret = ideviceenterrecovery();

		if (ret == 0) {
			sleep(9);
			ret = idevicepwn();
		}
	}

	if (pwnDFUMode == 0 && ret == 0) {
			log_info("Device reached pwned DFU mode.");
			ret = idevicepwn_ramdisk();
	}

	if (ret == 0) {
		printf("DONE !");
	}

	return ret;
}
