#ifndef IDEVICEENTERRAMDISK_H
#define IDEVICEENTERRAMDISK_H

#include <stdbool.h>
#include <libirecovery.h>

/*
 * ramdiskBootMode – when true, ideviceenterramdisk_load() skips the
 * prepare / download / patch pipeline and boots directly from the
 * per-device cache.  Set this before calling ideviceenterramdisk_load().
 *
 * Value is set in main.c.
 */
bool ramdiskBootMode;

/*
 * ideviceenterramdisk_set_tool_dir – set the directory containing the
 * tool binaries (img4, ldid2, iBoot64Patcher, tsschecker, Kernel64Patcher).
 * Must be called before ideviceenterramdisk_load().
 */
void ideviceenterramdisk_set_tool_dir(const char *dir);

/*
 * ideviceenterramdisk_load – run the full ramdisk boot flow:
 *   prepare → download → decrypt → patch → boot
 *
 * If ramdiskBootMode is true and a valid per-device cache exists the
 * prepare/download/patch stages are skipped and the device is booted
 * directly from the cached payloads.
 *
 * Returns 0 on success, -1 on any failure.
 */
int ideviceenterramdisk_load(void);

/* ── DFU / irecovery helpers ──────────────────────────────────────────── */

/*
 * dfu_wait_for_device – block until a DFU-mode or recovery-mode device
 * appears, set auto-boot=true in NVRAM, and return.
 *
 * Note: writing auto-boot=true is a persistent NVRAM side-effect.
 *
 * Returns 0 on success.  Never returns -1 — loops indefinitely until a
 * device is found.
 */
int dfu_wait_for_device(void);

/*
 * dfu_wait_ready – poll until the DFU/recovery client is reachable or
 * max_wait_secs elapses.  Used after iBSS/iBEC sends to absorb USB
 * re-enumeration delay.  context is a short label used in log messages.
 *
 * Returns 0 if the device responds, -1 on timeout.
 */
int dfu_wait_ready(unsigned int max_wait_secs, const char *context);

/*
 * dfu_get_info – query a named property from the connected DFU device.
 *
 * Recognised keys: "ecid", "cpid", "product_type", "model"
 *
 * Returns a heap-allocated string; caller must free().
 * Returns NULL if the device cannot be opened or the key is unknown.
 */
char *dfu_get_info(const char *key);

/*
 * dfu_send_file – upload a file to the device in DFU / recovery mode.
 * Returns 0 on success, -1 on error.
 */
int dfu_send_file(const char *filepath);

/*
 * dfu_send_cmd – send a recovery-mode command string.
 * Returns 0 on success, -1 on error.
 */
int dfu_send_cmd(const char *command);

#endif /* IDEVICEENTERRAMDISK_H */
