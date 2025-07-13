#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
//
#include "log.h"
#include "ideviceenterrecovery.h"
#include "idevicepwndfu.h"

int
main(int argc, char **argv) {
#ifdef __APPLE__
	system("killall -9 iTunesHelper 2> /dev/null");
	system("killall -9 iTunes 2> /dev/null");
	system("kill -STOP $(pgrep AMPDeviceDiscoveryAgent) 2> /dev/null"); // TY Siguza
#endif
	printf("%s\n", "--==== gastera1n ====--\n");
	printf("%s\n", "// thanks to gaster and openra1n");

	int ret = 0;
	ret = idevicepwndfu();

	if (ret != 0) {
		ret = ideviceenterrecovery();

		if (ret == 0) {
			sleep(9);
			ret = idevicepwndfu();
		}
	}

	return ret;
}
