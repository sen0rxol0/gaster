#ifndef IDEVICEENTERRAMDISK_H
#define IDEVICEENTERRAMDISK_H

#include <stdint.h>

/* Defined in ideviceenterramdisk.c — extern here to avoid multiple
   definitions when the header is included by more than one translation unit. */
extern unsigned int ramdiskBootMode;

/* Also defined in ideviceenterramdisk.c and shared with main.c. */
extern unsigned int pwnDFUMode;

int ideviceenterramdisk_load(void);

/* DFU helpers exposed for use by other modules. */
int          dfu_wait_for_device(void);
char        *dfu_get_info(const char *key);  /* caller must free() result */
int          dfu_send_file(const char *filepath);
int          dfu_send_cmd(const char *command);

#endif /* IDEVICEENTERRAMDISK_H */