#ifndef IDEVICEENTERRAMDISK_H
#define IDEVICEENTERRAMDISK_H

#include <stdbool.h>
#include <libirecovery.h>

bool ramdiskBootMode; /* true  → skip prepare/download/patch, boot directly */

/*
 * ideviceenterramdisk_set_tool_dir – must be called before any other
 * function in this module.  dir is the directory that contains the tool
 * binaries (img4, ldid2, iBoot64Patcher, tsschecker).
 */
void ideviceenterramdisk_set_tool_dir(const char *dir);

/*
 * ideviceenterramdisk_load – run the full ramdisk boot flow.
 * Returns 0 on success, -1 on any failure.
 */
int ideviceenterramdisk_load(void);

/* ── DFU / irecovery helpers ──────────────────────────────────────────── */

/*
 * dfu_wait_for_device – block until a DFU-mode device appears, configure
 * auto-boot, and return.
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
 * dfu_send_file – upload a file to the device in DFU mode.
 * Returns 0 on success, -1 on error.
 */
int dfu_send_file(const char *filepath);

/*
 * dfu_send_cmd – send a recovery-mode command string.
 * Returns 0 on success, -1 on error.
 */
int dfu_send_cmd(const char *command);

/*
 * dfu_progress_cb – irecovery IRECV_PROGRESS event callback.
 * Registered via irecv_event_subscribe(); signature must match
 * irecv_event_cb_t.  Delegates rendering to the internal render_progress().
 */
int dfu_progress_cb(irecv_client_t client, const irecv_event_t *event);

#endif /* IDEVICEENTERRAMDISK_H */
