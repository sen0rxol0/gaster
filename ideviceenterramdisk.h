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
extern bool ramdiskBootMode;

/*
 * ideviceenterramdisk_set_tool_dir – set the directory that contains all
 * companion tool binaries and bundled resources (img4, ldid2, bootim, …).
 *
 * The path is resolved to an absolute path via realpath() so it remains
 * valid regardless of any subsequent working-directory changes.
 *
 * Must be called before ideviceenterramdisk_load().
 * Returns 0 on success, -1 on failure (path does not exist).
 */
int ideviceenterramdisk_set_tool_dir(const char *path);

/*
 * ideviceenterramdisk_load – run the full ramdisk boot flow:
 *   prepare → download → decrypt → patch → boot
 *
 * If ramdiskBootMode is true and a valid per-device cache exists the
 * prepare/download/patch stages are skipped and the device is booted
 * directly from the cached payloads.
 *
 * cache_dir_override – when non-NULL and ramdiskBootMode is true, this
 * path is used as the cache directory directly instead of the default
 * per-device path derived from ECID and CPID.  Ignored when
 * ramdiskBootMode is false.
 *
 * Returns 0 on success, -1 on any failure.
 */
int ideviceenterramdisk_load(const char *cache_dir_override);

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
 * timeout_secs elapses.
 *
 * Returns 0 if the device responds, -1 on timeout.
 */
int dfu_wait_ready(unsigned int initial_delay_ms,
                   unsigned int timeout_secs);

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
