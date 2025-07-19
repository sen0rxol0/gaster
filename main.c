#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>
//
#include "log.h"
#include "idevicedfu.h"
#include "ideviceenterrecovery.h"
#include "ideviceenterramdisk.h"
#include "gastera1n.h"

static int
idevicepwn() {

	if (idevicedfu_find() != 0) {
		return -1;
	}

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
	printf("%s\n", "//\thttps://github.com/tihmstar/img4tool");
	printf("%s\n", "//\thttps://github.com/xerub/img4lib");
	printf("%s\n", "//\thttps://github.com/tihmstar/libfragmentzip");
	printf("%s\n", "//\thttps://github.com/tihmstar/tsschecker");
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

	printf("\n\n");

	int ret = EXIT_SUCCESS;
	ret = idevicepwn();

	if (ret != 0) {
		ret = ideviceenterrecovery();

		if (ret == 0) {
			sleep(9);
			ret = idevicepwn();
		}
	}

	if (ret == 0) {
		log_info("Device reached pwned DFU mode.");
		ret = idevicepwn_ramdisk();
	}

	if (ret == 0) {
		printf("DONE !");
	}

	return ret;
}
