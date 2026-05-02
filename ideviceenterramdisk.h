#ifndef IDEVICEENTERRAMDISK_H
#define IDEVICEENTERRAMDISK_H

#include <stdbool.h>
#include <libirecovery.h>

/*
 * ramdiskBootMode – when true, ideviceenterramdisk_load() skips the
 * prepare / download / patch pipeline and boots directly from the
 * per-device cache.  Set this before calling ideviceenterramdisk_load().
 *
 * Defined in ideviceenterramdisk.c.
 */
extern bool ramdiskBootMode;   /* FIX: declaration only — definition lives in .c */

/*
 * ideviceenterramdisk_set_tool_dir – set the directory that contains the
 * tool binaries (img4, ldid2, iBoot64Patcher, tsschecker, Kernel64Patcher).
 *
 * Must be called before ideviceenterramdisk_load().
 */
void ideviceenterramdisk_set_tool_dir(const char *dir);

/*
 * ideviceenterramdisk_load – run the full ramdisk boot flow:
 *   prepare → download → decrypt → patch → boot
 *
 * If ramdiskBootMode is true and a valid per-device cache exists, the
 * prepare/download/patch stages are skipped and the device is booted
 * directly from the cached payloads.
 *
 * Returns 0 on success, -1 on any failure.
 */
int ideviceenterramdisk_load(void);

/* ── DFU / irecovery helpers ──────────────────────────────────────────── */

/*
 * dfu_wait_for_device – block until a DFU-mode or recovery-mode device
 * appears, set auto-boot=true in NVRAM (persistent side effect), and return.
 *
 * Returns 0 on success, -1 on error.
 */
int dfu_wait_for_device(void);

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

/*
 * dfu_wait_ready – sleep for delay_secs then verify the DFU client is
 * reachable.  Used after iBSS/iBEC sends to absorb USB re-enumeration
 * delay.  context is a short label used in error messages.
 *
 * Returns 0 if the device responds, -1 on timeout / connection failure.
 */
int dfu_wait_ready(unsigned int delay_secs, const char *context);

/*
 * dfu_progress_cb – irecovery IRECV_PROGRESS event callback.
 * Registered via irecv_event_subscribe(); signature must match
 * irecv_event_cb_t.  Renders a progress bar to stdout.
 */
int dfu_progress_cb(irecv_client_t client, const irecv_event_t *event);

#endif /* IDEVICEENTERRAMDISK_H */
