#ifndef IDEVICELOADERS_H
#define IDEVICELOADERS_H

#include <stdlib.h>
#include <string.h>

/* Firmware version entry: one per supported iOS version per device */
typedef struct loader_version {
    char* version;           /* e.g. "14.8" */
    char* ibss_ivkey;
    char* ibss_path;
    char* ibec_ivkey;
    char* ibec_path;
    char* devicetree_path;
    char* trustcache_path;
    char* kernelcache_path;
    char* ramdisk_path;
    char* ipsw_url;
} loader_version;

typedef struct device_loader {
    char*                 identifier;
    loader_version versions[8]; /* up to 7 version entries + 1 sentinel (version == NULL) */
} device_loader;

/*
 * device_loader_select_version – find the version entry for a given iOS
 * version string (e.g. "14.8") within a device_loader.
 *
 * Returns a pointer to the matching device_version_entry on success, or
 * NULL if the version is not found or loader is NULL.
 *
 * The returned pointer is valid for the lifetime of the device_loaders[]
 * array (i.e. the duration of the process) and must not be freed.
 */
static inline const loader_version *
device_loader_select_version(const device_loader *loader, const char *version)
{
    if (!loader || !version) return NULL;
    for (int i = 0; i < 8; i++) {
        if (!loader->versions[i].version) break;          /* sentinel */
        if (strcmp(loader->versions[i].version, version) == 0)
            return &loader->versions[i];
    }
    return NULL;
}

static inline bool
device_loader_has_versions(const device_loader *loader)
{
    return loader && loader->versions[0].version != NULL;
}

/*
 * device_loader_find – look up a device_loader by product type string
 * (e.g. "iPhone9,1") in the global device_loaders[] table.
 *
 * Returns a pointer to the matching entry, or NULL if not found.
 */
static inline const device_loader *
device_loader_find(const char *product_type);   /* defined after table */

/* ================================================================== *
 * Board-level firmware paths                                         *
 * Each board family has its own iBSS/iBEC/DeviceTree/kernelcache.   *
 * Trustcache + ramdisk filenames come from the IPSW BuildManifest.  *
 * Lines marked TODO need confirming against the IPSW manifest.      *
 * ================================================================== */

/* ------------------------------------------------------------------
 * n71  –  iPhone 8          (iPhone8,1)   A9   4.7"
 * Paths confirmed from BuildManifest (n71ap board)
 * ------------------------------------------------------------------ */
#define N71_IBSS_IVKEY      ""
#define N71_IBSS_PATH       "Firmware/dfu/iBSS.n71.RELEASE.im4p"
#define N71_IBEC_IVKEY      ""
#define N71_IBEC_PATH       "Firmware/dfu/iBEC.n71.RELEASE.im4p"
#define N71_DEVICETREE      "Firmware/all_flash/DeviceTree.n71ap.im4p"
#define N71_KERNELCACHE     "kernelcache.release.n71"

/* ------------------------------------------------------------------
 * n66  –  iPhone 8 Plus     (iPhone8,2)   A9   5.5"
 * Paths confirmed from BuildManifest (n66ap board)
 * ------------------------------------------------------------------ */
#define N66_IBSS_IVKEY      ""
#define N66_IBSS_PATH       "Firmware/dfu/iBSS.n66.RELEASE.im4p"
#define N66_IBEC_IVKEY      ""
#define N66_IBEC_PATH       "Firmware/dfu/iBEC.n66.RELEASE.im4p"
#define N66_DEVICETREE      "Firmware/all_flash/DeviceTree.n66ap.im4p"
#define N66_KERNELCACHE     "kernelcache.release.n66"

/* ------------------------------------------------------------------
 * n69  –  iPhone SE 1st gen (iPhone8,4)   A9   4.0"
 * Paths confirmed from BuildManifest (n69ap board)
 * ------------------------------------------------------------------ */
#define N69_IBSS_IVKEY      ""
#define N69_IBSS_PATH       "Firmware/dfu/iBSS.n69.RELEASE.im4p"
#define N69_IBEC_IVKEY      ""
#define N69_IBEC_PATH       "Firmware/dfu/iBEC.n69.RELEASE.im4p"
#define N69_DEVICETREE      "Firmware/all_flash/DeviceTree.n69ap.im4p"
#define N69_KERNELCACHE     "kernelcache.release.iphone8b"

/* ------------------------------------------------------------------
 * d10  –  iPhone 7 / 9,1 / 9,3 / 10,1 / 10,4   A10  4.7"
 * Paths confirmed from BuildManifest (d101ap primary board)
 * ------------------------------------------------------------------ */
#define D10_IBSS_IVKEY      ""
#define D10_IBSS_PATH       "Firmware/dfu/iBSS.d10.RELEASE.im4p"
#define D10_IBEC_IVKEY      ""
#define D10_IBEC_PATH       "Firmware/dfu/iBEC.d10.RELEASE.im4p"
#define D10_DEVICETREE      "Firmware/all_flash/DeviceTree.d101ap.im4p"
#define D10_KERNELCACHE     "kernelcache.release.iphone9"

/* ------------------------------------------------------------------
 * d11  –  iPhone 7 Plus / 9,2 / 9,4 / 10,2 / 10,5   A10  5.5"
 * Paths confirmed from BuildManifest (d111ap primary board)
 * ------------------------------------------------------------------ */
#define D11_IBSS_IVKEY      ""
#define D11_IBSS_PATH       "Firmware/dfu/iBSS.d11.RELEASE.im4p"
#define D11_IBEC_IVKEY      ""
#define D11_IBEC_PATH       "Firmware/dfu/iBEC.d11.RELEASE.im4p"
#define D11_DEVICETREE      "Firmware/all_flash/DeviceTree.d111ap.im4p"
#define D11_KERNELCACHE     "kernelcache.release.iphone9"

/* ------------------------------------------------------------------
 * d22  –  iPhone X / 10,3 / 10,6   A11  5.8" OLED
 * Paths confirmed from BuildManifest (d221ap primary board)
 * ------------------------------------------------------------------ */
#define D22_IBSS_IVKEY      ""
#define D22_IBSS_PATH       "Firmware/dfu/iBSS.d22.RELEASE.im4p"
#define D22_IBEC_IVKEY      ""
#define D22_IBEC_PATH       "Firmware/dfu/iBEC.d22.RELEASE.im4p"
#define D22_DEVICETREE      "Firmware/all_flash/DeviceTree.d221ap.im4p"
#define D22_KERNELCACHE     "kernelcache.release.iphone10b"

/* ================================================================== *
 * iOS 14.8 (18H17) – ramdisk/trustcache confirmed from BuildManifest *
 * Primary RestoreRamDisk across all boards: 018-61747-017.dmg        *
 * (018-61899-017.dmg is the secondary ramdisk variant in same IPSW)  *
 * ================================================================== */

/* iPhone8,1  –  071-97932 */
#define N71_148_TRUSTCACHE  "Firmware/018-61747-017.dmg.trustcache"
#define N71_148_RAMDISK     "018-61747-017.dmg"
#define N71_148_IPSW_URL    "https://updates.cdn-apple.com/2021FallFCS/fullrestores/071-97932/" \
                            "2C2C8127-289D-44D6-93D6-2BA03D0D6E0D/iPhone_4.7_14.8_18H17_Restore.ipsw"

/* iPhone8,2  –  071-98082 */
#define N66_148_TRUSTCACHE  "Firmware/018-61747-017.dmg.trustcache"
#define N66_148_RAMDISK     "018-61747-017.dmg"
#define N66_148_IPSW_URL    "https://updates.cdn-apple.com/2021FallFCS/fullrestores/071-98082/" \
                            "ACD0236B-A5B2-4BA2-B3A3-C7039AAD5B7B/iPhone_5.5_14.8_18H17_Restore.ipsw"

/* iPhone8,4  –  071-98012 */
#define N69_148_TRUSTCACHE  "Firmware/018-61747-017.dmg.trustcache"
#define N69_148_RAMDISK     "018-61747-017.dmg"
#define N69_148_IPSW_URL    "https://updates.cdn-apple.com/2021FallFCS/fullrestores/071-98012/" \
                            "0DFDF326-8C56-4C26-BFE9-EF5CF29EF8E1/iPhone_4.0_64bit_14.8_18H17_Restore.ipsw"

/* iPhone9,1 / 9,3 / 10,1 / 10,4  –  071-98010 */
#define D10_148_TRUSTCACHE  "Firmware/018-61747-017.dmg.trustcache"
#define D10_148_RAMDISK     "018-61747-017.dmg"
#define D10_148_IPSW_URL    "https://updates.cdn-apple.com/2021FallFCS/fullrestores/071-98010/" \
                            "39CE5096-991E-4C25-87BF-7FF9181C07CA/iPhone_4.7_P3_14.8_18H17_Restore.ipsw"

/* iPhone9,2 / 9,4 / 10,2 / 10,5  –  071-97875 */
#define D11_148_TRUSTCACHE  "Firmware/018-61747-017.dmg.trustcache"
#define D11_148_RAMDISK     "018-61747-017.dmg"
#define D11_148_IPSW_URL    "https://updates.cdn-apple.com/2021FallFCS/fullrestores/071-97875/" \
                            "CA80DE3C-608D-480B-94F1-38BC6452074A/iPhone_5.5_P3_14.8_18H17_Restore.ipsw"

/* iPhone10,3 / 10,6  –  071-98144 */
#define D22_148_TRUSTCACHE  "Firmware/018-61747-017.dmg.trustcache"
#define D22_148_RAMDISK     "018-61747-017.dmg"
#define D22_148_IPSW_URL    "https://updates.cdn-apple.com/2021FallFCS/fullrestores/071-98144/" \
                            "45903C19-6F00-4014-88BF-5DA2A16D13CF/iPhone10,3,iPhone10,6_14.8_18H17_Restore.ipsw"

/* ================================================================== *
 * iOS 15.7 (19H12) – ramdisk/trustcache confirmed from BuildManifest *
 * All boards in these IPSWs share ramdisk 078-69817-013.dmg          *
 * ================================================================== */

/* iPhone8,1  –  012-39051 */
#define N71_157_TRUSTCACHE  "Firmware/078-69817-013.dmg.trustcache"
#define N71_157_RAMDISK     "078-69817-013.dmg"
#define N71_157_IPSW_URL    "https://updates.cdn-apple.com/2022FallFCS/fullrestores/012-39051/" \
                            "60D8E589-3C30-4416-BEA9-2156217142BC/iPhone_4.7_15.7_19H12_Restore.ipsw"

/* iPhone8,2  –  012-39102 */
#define N66_157_TRUSTCACHE  "Firmware/078-69817-013.dmg.trustcache"
#define N66_157_RAMDISK     "078-69817-013.dmg"
#define N66_157_IPSW_URL    "https://updates.cdn-apple.com/2022FallFCS/fullrestores/012-39102/" \
                            "8B9B6001-92E2-49EC-969C-1E2BADF5602D/iPhone_5.5_15.7_19H12_Restore.ipsw"

/* iPhone8,4  –  012-39072 */
#define N69_157_TRUSTCACHE  "Firmware/078-69817-013.dmg.trustcache"
#define N69_157_RAMDISK     "078-69817-013.dmg"
#define N69_157_IPSW_URL    "https://updates.cdn-apple.com/2022FallFCS/fullrestores/012-39072/" \
                            "BFDAFD43-D3FF-4811-B210-6A104D91A201/iPhone_4.0_64bit_15.7_19H12_Restore.ipsw"

/* iPhone9,1 / 9,3 / 10,1 / 10,4  –  012-38914 */
#define D10_157_TRUSTCACHE  "Firmware/078-69817-013.dmg.trustcache"
#define D10_157_RAMDISK     "078-69817-013.dmg"
#define D10_157_IPSW_URL    "https://updates.cdn-apple.com/2022FallFCS/fullrestores/012-38914/" \
                            "C7764173-5CC4-4D58-8F8B-F093F9A060F0/iPhone_4.7_P3_15.7_19H12_Restore.ipsw"

/* iPhone9,2 / 9,4 / 10,2 / 10,5  –  012-38894 */
#define D11_157_TRUSTCACHE  "Firmware/078-69817-013.dmg.trustcache"
#define D11_157_RAMDISK     "078-69817-013.dmg"
#define D11_157_IPSW_URL    "https://updates.cdn-apple.com/2022FallFCS/fullrestores/012-38894/" \
                            "94B29539-E9B4-4288-B79B-957D8A074982/iPhone_5.5_P3_15.7_19H12_Restore.ipsw"

/* iPhone10,3 / 10,6  –  012-39067 */
#define D22_157_TRUSTCACHE  "Firmware/078-69817-013.dmg.trustcache"
#define D22_157_RAMDISK     "078-69817-013.dmg"
#define D22_157_IPSW_URL    "https://updates.cdn-apple.com/2022FallFCS/fullrestores/012-39067/" \
                            "20ACC1A5-8816-484B-A176-DB3A54B16299/iPhone10,3,iPhone10,6_15.7_19H12_Restore.ipsw"

/* ================================================================== *
 * Convenience macros: V_<version>_<board>                            *
 * ================================================================== */

/* --- 14.8 --- */
#define V_148_N71 { "14.8", N71_IBSS_IVKEY, N71_IBSS_PATH, N71_IBEC_IVKEY, N71_IBEC_PATH, \
                    N71_DEVICETREE, N71_148_TRUSTCACHE, N71_KERNELCACHE, N71_148_RAMDISK, N71_148_IPSW_URL }

#define V_148_N66 { "14.8", N66_IBSS_IVKEY, N66_IBSS_PATH, N66_IBEC_IVKEY, N66_IBEC_PATH, \
                    N66_DEVICETREE, N66_148_TRUSTCACHE, N66_KERNELCACHE, N66_148_RAMDISK, N66_148_IPSW_URL }

#define V_148_N69 { "14.8", N69_IBSS_IVKEY, N69_IBSS_PATH, N69_IBEC_IVKEY, N69_IBEC_PATH, \
                    N69_DEVICETREE, N69_148_TRUSTCACHE, N69_KERNELCACHE, N69_148_RAMDISK, N69_148_IPSW_URL }

#define V_148_D10 { "14.8", D10_IBSS_IVKEY, D10_IBSS_PATH, D10_IBEC_IVKEY, D10_IBEC_PATH, \
                    D10_DEVICETREE, D10_148_TRUSTCACHE, D10_KERNELCACHE, D10_148_RAMDISK, D10_148_IPSW_URL }

#define V_148_D11 { "14.8", D11_IBSS_IVKEY, D11_IBSS_PATH, D11_IBEC_IVKEY, D11_IBEC_PATH, \
                    D11_DEVICETREE, D11_148_TRUSTCACHE, D11_KERNELCACHE, D11_148_RAMDISK, D11_148_IPSW_URL }

#define V_148_D22 { "14.8", D22_IBSS_IVKEY, D22_IBSS_PATH, D22_IBEC_IVKEY, D22_IBEC_PATH, \
                    D22_DEVICETREE, D22_148_TRUSTCACHE, D22_KERNELCACHE, D22_148_RAMDISK, D22_148_IPSW_URL }

/* --- 15.7 --- */
#define V_157_N71 { "15.7", N71_IBSS_IVKEY, N71_IBSS_PATH, N71_IBEC_IVKEY, N71_IBEC_PATH, \
                    N71_DEVICETREE, N71_157_TRUSTCACHE, N71_KERNELCACHE, N71_157_RAMDISK, N71_157_IPSW_URL }

#define V_157_N66 { "15.7", N66_IBSS_IVKEY, N66_IBSS_PATH, N66_IBEC_IVKEY, N66_IBEC_PATH, \
                    N66_DEVICETREE, N66_157_TRUSTCACHE, N66_KERNELCACHE, N66_157_RAMDISK, N66_157_IPSW_URL }

#define V_157_N69 { "15.7", N69_IBSS_IVKEY, N69_IBSS_PATH, N69_IBEC_IVKEY, N69_IBEC_PATH, \
                    N69_DEVICETREE, N69_157_TRUSTCACHE, N69_KERNELCACHE, N69_157_RAMDISK, N69_157_IPSW_URL }

#define V_157_D10 { "15.7", D10_IBSS_IVKEY, D10_IBSS_PATH, D10_IBEC_IVKEY, D10_IBEC_PATH, \
                    D10_DEVICETREE, D10_157_TRUSTCACHE, D10_KERNELCACHE, D10_157_RAMDISK, D10_157_IPSW_URL }

#define V_157_D11 { "15.7", D11_IBSS_IVKEY, D11_IBSS_PATH, D11_IBEC_IVKEY, D11_IBEC_PATH, \
                    D11_DEVICETREE, D11_157_TRUSTCACHE, D11_KERNELCACHE, D11_157_RAMDISK, D11_157_IPSW_URL }

#define V_157_D22 { "15.7", D22_IBSS_IVKEY, D22_IBSS_PATH, D22_IBEC_IVKEY, D22_IBEC_PATH, \
                    D22_DEVICETREE, D22_157_TRUSTCACHE, D22_KERNELCACHE, D22_157_RAMDISK, D22_157_IPSW_URL }

/* Sentinel entry marking the end of a versions array */
#define V_END  { NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL }

/* ------------------------------------------------------------------ *
 * Device table                                                        *
 * ------------------------------------------------------------------ */
static const device_loader device_loaders[] = {

    /* ---- iPhone ---- */
    { "iPhone5,1",  { V_END } },
    { "iPhone5,2",  { V_END } },
    { "iPhone5,3",  { V_END } },
    { "iPhone5,4",  { V_END } },
    { "iPhone6,1",  { V_END } },
    { "iPhone6,2",  { V_END } },
    { "iPhone7,1",  { V_END } },
    { "iPhone7,2",  { V_END } },
    { "iPhone8,1",  { V_148_N71, V_157_N71, V_END } },
    { "iPhone8,2",  { V_148_N66, V_157_N66, V_END } },
    { "iPhone8,4",  { V_148_N69, V_157_N69, V_END } },
    { "iPhone9,1",  { V_148_D10, V_157_D10, V_END } },
    { "iPhone9,2",  { V_148_D11, V_157_D11, V_END } },
    { "iPhone9,3",  { V_148_D10, V_157_D10, V_END } },
    { "iPhone9,4",  { V_148_D11, V_157_D11, V_END } },
    { "iPhone10,1", { V_148_D10, V_157_D10, V_END } },
    { "iPhone10,2", { V_148_D11, V_157_D11, V_END } },
    { "iPhone10,3", { V_148_D22, V_157_D22, V_END } },
    { "iPhone10,4", { V_148_D10, V_157_D10, V_END } },
    { "iPhone10,5", { V_148_D11, V_157_D11, V_END } },
    { "iPhone10,6", { V_148_D22, V_157_D22, V_END } },

    /* ---- iPad ---- */
    { "iPad1,1",    { V_END } },
    { "iPad2,1",    { V_END } },
    { "iPad2,2",    { V_END } },
    { "iPad2,3",    { V_END } },
    { "iPad2,4",    { V_END } },
    { "iPad2,5",    { V_END } },
    { "iPad2,6",    { V_END } },
    { "iPad2,7",    { V_END } },
    { "iPad3,1",    { V_END } },
    { "iPad3,2",    { V_END } },
    { "iPad3,3",    { V_END } },
    { "iPad3,4",    { V_END } },
    { "iPad3,5",    { V_END } },
    { "iPad3,6",    { V_END } },
    { "iPad4,1",    { V_END } },
    { "iPad4,2",    { V_END } },
    { "iPad4,4",    { V_END } },
    { "iPad4,5",    { V_END } },
    { "iPad4,6",    { V_END } },
    { "iPad4,7",    { V_END } },
    { "iPad4,8",    { V_END } },
    { "iPad4,9",    { V_END } },
    { "iPad5,1",    { V_END } },
    { "iPad5,2",    { V_END } },
    { "iPad5,3",    { V_END } },
    { "iPad5,4",    { V_END } },
    { "iPad6,3",    { V_END } },
    { "iPad6,4",    { V_END } },
    { "iPad6,7",    { V_END } },
    { "iPad6,8",    { V_END } },
    { "iPad6,11",   { V_END } },
    { "iPad6,12",   { V_END } },
    { "iPad7,1",    { V_END } },
    { "iPad7,2",    { V_END } },
    { "iPad7,3",    { V_END } },
    { "iPad7,4",    { V_END } },
    { "iPad7,5",    { V_END } },
    { "iPad7,6",    { V_END } },
    { "iPad7,11",   { V_END } },
    { "iPad7,12",   { V_END } },
    { "iPad8,1",    { V_END } },
    { "iPad8,2",    { V_END } },
    { "iPad8,3",    { V_END } },
    { "iPad8,4",    { V_END } },
    { "iPad8,5",    { V_END } },
    { "iPad8,6",    { V_END } },
    { "iPad8,7",    { V_END } },
    { "iPad8,8",    { V_END } },
    { "iPad11,1",   { V_END } },
    { "iPad11,2",   { V_END } },
    { "iPad11,3",   { V_END } },
    { "iPad11,4",   { V_END } },

	/* ---- sentinel ---- */
    { NULL,         { V_END } }
};

/* ------------------------------------------------------------------ *
 * device_loader_find – defined here (after device_loaders[]) so the  *
 * forward declaration above can reference the table.                 *
 * ------------------------------------------------------------------ */
static inline const device_loader *
device_loader_find(const char *product_type)
{
    if (!product_type) return NULL;
    for (int i = 0; device_loaders[i].identifier; i++) {
        if (strcmp(device_loaders[i].identifier, product_type) == 0)
            return &device_loaders[i];
    }
    return NULL;
}

/* Replace the strcmp-based comparison in device_loader_lowest_version() */
static inline int compare_version(const char *a, const char *b)
{
    int a_maj, a_min, b_maj, b_min;
    a_maj = a_min = b_maj = b_min = 0;
    sscanf(a, "%d.%d", &a_maj, &a_min);
    sscanf(b, "%d.%d", &b_maj, &b_min);
    if (a_maj != b_maj) return a_maj - b_maj;
    return a_min - b_min;
}

static inline const loader_version *
device_loader_lowest_version(const device_loader *loader)
{
    if (!loader) return NULL;
    const loader_version *best = NULL;
    for (int i = 0; i < 8; i++) {
        if (!loader->versions[i].version) break;
        if (!best || compare_version(loader->versions[i].version, best->version) < 0)
            best = &loader->versions[i];
    }
    return best;
}

#endif /* IDEVICELOADERS_H */
