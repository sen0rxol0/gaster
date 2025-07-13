/*
 * ideviceenterrecovery.c
 * Simple utility to make a device in normal mode enter recovery mode.
 *
 * Copyright (c) 2009 Martin Szulecki All Rights Reserved.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include <libimobiledevice/libimobiledevice.h>
#include <libimobiledevice/lockdown.h>

#include "ideviceenterrecovery.h"
#include "log.h"

#define TOOL_NAME "ideviceenterrecovery"

int
ideviceenterrecovery()
{
    log_info("Searching for Normal mode device");


    idevice_t device = NULL;
    const char* udid = NULL;
    idevice_error_t ret = IDEVICE_E_UNKNOWN_ERROR;

    ret = idevice_new_with_options(&device, udid, IDEVICE_LOOKUP_USBMUX);
	if (ret != IDEVICE_E_SUCCESS) {
        log_error("No device found!\n");
		return -1;
	}

    lockdownd_client_t client = NULL;
    lockdownd_error_t ldret = LOCKDOWN_E_UNKNOWN_ERROR;

	if (LOCKDOWN_E_SUCCESS != (ldret = lockdownd_client_new(device, &client, TOOL_NAME))) {
        log_error("ERROR: Could not connect to lockdownd: %s!\n", lockdownd_strerror(ldret));
		idevice_free(device);
		return 1;
	}

	int res = 0;
    log_info("Telling device with udid %s to enter recovery mode.\n", udid);
	ldret = lockdownd_enter_recovery(client);
	if (ldret == LOCKDOWN_E_SESSION_INACTIVE) {
		lockdownd_client_free(client);
		client = NULL;
		if (LOCKDOWN_E_SUCCESS != (ldret = lockdownd_client_new_with_handshake(device, &client, TOOL_NAME))) {
			log_error("Could not connect to lockdownd: %s (%d)\n", lockdownd_strerror(ldret));
			idevice_free(device);
			return 1;
		}
		ldret = lockdownd_enter_recovery(client);
	}
	if (ldret != LOCKDOWN_E_SUCCESS) {
		log_warn("Failed to enter recovery mode.\n");
		res = 1;
	} else {
        log_info("Device is successfully switching to recovery mode.\n");
	}

	lockdownd_client_free(client);
	idevice_free(device);

	return res;
}
