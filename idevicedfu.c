#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>
//
#include "log.h"

#include <libirecovery.h>

static void
step(int time, char *text) {
    for (int i = 0; i < time; i++) {
        printf("\r\e[K%s (%d)", text, time - i);
        fflush(stdout);

        sleep(1);
    }
    printf("\r%s (%d)\n", text, 0);
}

static int
ideviceenterdfu(unsigned int cpid) {
    char *s1, *s2;

    if ((cpid == 0x8010 || cpid == 0x8015)) {
        s1 = "Hold volume down + side button";
        s2 = "Release side button, but keep holding volume down";
    } else {
        s1 = "Hold home + power button";
        s2 = "Release power button, but keep holding home button";
    }

    log_info("Press any key when ready for DFU mode\n");
    getchar();
    step(3, "Get ready");
    step(8, s1);
    step(7, s2);

	return 0;
}

static const char*
idevicedfu_mode(irecv_client_t client) {
	int ret, mode;
	const char* str_mode = "";
	ret = irecv_get_mode(client, &mode);
	if (ret == IRECV_E_SUCCESS) {
		switch (mode) {
			case IRECV_K_RECOVERY_MODE_1:
			case IRECV_K_RECOVERY_MODE_2:
			case IRECV_K_RECOVERY_MODE_3:
			case IRECV_K_RECOVERY_MODE_4:
				str_mode = "Recovery";
				break;
			case IRECV_K_DFU_MODE:
				str_mode = "DFU";
				break;
			case IRECV_K_PORT_DFU_MODE:
				str_mode = "Port DFU";
				break;
			case IRECV_K_WTF_MODE:
				str_mode = "WTF";
				break;
			default:
				str_mode = "Unknown";
				break;
		}
	}

	return str_mode;
}

static irecv_client_t
idevicedfu_open_client() {
    uint64_t ecid = 0;
	irecv_client_t client = NULL;
	int i;
	for (i = 0; i <= 5; i++) {
		log_debug("Attempting to connect...\n");

		irecv_error_t err = irecv_open_with_ecid(&client, ecid);
		if (err == IRECV_E_UNSUPPORTED) {
			log_error("%s\n", irecv_strerror(err));
            return NULL;
			// return -1;
		}
		else if (err != IRECV_E_SUCCESS)
			sleep(1);
		else
			break;

		if (i == 5) {
			log_error("%s\n", irecv_strerror(err));
            return NULL;
			// return -1;
		}
	}

    return client;
}

char*
idevicedfu_info(char *t) {
    log_debug("Getting device info: %s", t);
    char *info = NULL;
    irecv_client_t client = idevicedfu_open_client();
	const struct irecv_device_info *devinfo = irecv_get_device_info(client);
    irecv_device_t device = NULL;
    irecv_devices_get_device_by_client(client, &device);

    if (strcmp(t, "ecid") == 0) {
        char buffer[100];
        sprintf(buffer, "0x%016llX", devinfo->ecid);
        info = buffer;
    } else if (strcmp(t, "cpid") == 0) {
        char buffer[20];
        sprintf(buffer, "0x%04X", devinfo->cpid);
        info = buffer;
    } else if (strcmp(t, "product_type") == 0) {
        info = device->product_type;
    } else if (strcmp(t, "model") == 0) {
        info = device->hardware_model;
    }

    irecv_close(client);
    log_debug("%s\n", info);
    return info;
}

int
idevicedfu_find() {
	log_info("Searching for DFU mode device...\n");

	int ret = 0;
	irecv_device_t device = NULL;
    irecv_client_t client = idevicedfu_open_client();

    if (client == NULL) {
        return -1;
    }

    irecv_devices_get_device_by_client(client, &device);

    if (device) {
        const char *mode = idevicedfu_mode(client);
        unsigned int chip_id = device->chip_id;
        log_debug("PRODUCT: %s\n", device->product_type);
        log_debug("MODEL: %s\n", device->hardware_model);
        log_debug("NAME: %s\n", device->display_name);
        log_debug("MODE: %s\n", mode);

        irecv_error_t error = 0;
        error = irecv_setenv(client, "auto-boot", "true");
        if (error != IRECV_E_SUCCESS) {
            log_error("%s\n", irecv_strerror(error));
        }

        error = irecv_saveenv(client);
        if (error != IRECV_E_SUCCESS) {
            log_error("%s\n", irecv_strerror(error));
        }

        irecv_close(client);

        sleep(1);

        if (strcmp(mode, "DFU") == 0) {
            log_info("Found device in DFU mode.\n");
            ret = 0;
        } else if (strcmp(mode, "Recovery") == 0) {
            log_info("Found device in Recovery mode.\n");
            ret = ideviceenterdfu(chip_id);

            if (ret == 0) {
                return idevicedfu_find();
            }
        }
    } else {
        return 1;
    }

	return ret;
}

int progress_cb(irecv_client_t client, const irecv_event_t* event);

int
progress_cb(irecv_client_t client, const irecv_event_t* event) {
	if (event->type == IRECV_PROGRESS) {

        double progress = event->progress;
        int i = 0;

        if (progress < 0) {
            return 0;
        }

        if (progress > 100) {
            progress = 100;
        }

        printf("\r[");

        for (i = 0; i < 50; i++) {
            if (i < progress / 2) {
                printf("=");
            } else {
                printf(" ");
            }
        }

        printf("] %3.1f%%", progress);

        fflush(stdout);

        if (progress == 100) {
            printf("\n");
        }
	}

	return 0;
}

int
idevicedfu_sendfile(const char* filepath) {

    irecv_error_t error = IRECV_E_SUCCESS;
    irecv_client_t client = idevicedfu_open_client();
    irecv_event_subscribe(client, IRECV_PROGRESS, &progress_cb, NULL);
	error = irecv_send_file(client, filepath, IRECV_SEND_OPT_DFU_NOTIFY_FINISH);
	log_debug("%s\n", irecv_strerror(error));
    irecv_close(client);

    if (error != IRECV_E_SUCCESS) {
        return -1;
    }

    return 0;
}

void
idevicedfu_sendcommand(char* command) {
    irecv_client_t client = idevicedfu_open_client();
    irecv_send_command(client, command);
    irecv_close(client);
}
