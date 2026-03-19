#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/io/dirent.h>
#include <psp2/kernel/modulemgr.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/appmgr.h>
#include <psp2/vshbridge.h>
#include <stdio.h>
#include <string.h>
#include <psp2/registrymgr.h>
#include "compat.h"

extern int vshSysconHasWWAN(void);
extern int _vshSysconGetHardwareInfo(unsigned char *info);
/* No SDK header declares this but it exists in userland SceKernel */
extern int sceKernelGetModelForCDialog(void);

VitaCompat g_compat;

/* ── Helpers ─────────────────────────────────────────────────────────── */

static int path_exists(const char *p) {
    SceIoStat s;
    return (sceIoGetstat(p, &s) >= 0);
}

/*
 * dir_exists() — try to open a path as a directory.
 * More reliable than sceIoGetstat() for mount point roots on some firmware.
 */
static int dir_exists(const char *p) {
    SceUID dd = sceIoDopen(p);
    if (dd >= 0) {
        sceIoDclose(dd);
        return 1;
    }
    return 0;
}

/*
 * drive_exists() — check a drive using both stat and dopen.
 * Returns 1 if either method succeeds.
 */
static int drive_exists(const char *p) {
    return path_exists(p) || dir_exists(p);
}

/*
 * Read real firmware version directly from the active OS partition.
 * Cannot be spoofed by plugins — sceKernelGetSystemSwVersion() can be.
 *
 * Technique from PSVident / TheFloW:
 *   sdstor0:int-lp-act-os, seek 0xB000, read 0x100 bytes,
 *   version word at offset +0x92.
 *   Example: 0x03650000 -> "3.65"
 *
 * Returns 0 on success, negative on failure.
 */
static int read_fw_from_partition(char *out, int out_sz,
                                  unsigned int *ver_out)
{
    SceUID fd = sceIoOpen("sdstor0:int-lp-act-os", SCE_O_RDONLY, 0);
    if (fd < 0) return -1;

    char buf[0x100];
    sceIoLseek(fd, 0xB000, SCE_SEEK_SET);
    int r = sceIoRead(fd, buf, sizeof(buf));
    sceIoClose(fd);
    if (r < 0x100) return -2;

    unsigned int ver = *(unsigned int *)(buf + 0x92);
    if (ver == 0 || ver == 0xFFFFFFFF) return -3;

    /* Decode nibbles: 0x03650000 -> a=3, b=6, c=5, d=0 */
    /* Extract as bytes: 0x03650000 -> a=0x03, b=0x65 -> "3.65" */
    unsigned int a = (ver >> 24) & 0xFF;
    unsigned int b = (ver >> 16) & 0xFF;
    unsigned int hi = b / 16;
    unsigned int lo = b % 16;

    if (lo)
        snprintf(out, out_sz, "%u.%u%u%u", a, hi, lo, 0);
    else
        snprintf(out, out_sz, "%u.%u%u",   a, hi, lo);

    if (ver_out) *ver_out = ver;
    return 0;
}

/*
 * build_motherboard_from_hwinfo() — parse raw syscon hardware info bytes.
 * hwinfo[2] = board code, hwinfo[0] bit 0–3 = variant (0x02 = 3G).
 * Only succeeds when _vshSysconGetHardwareInfo works (VSH/kernel context).
 *
 * Board code reference (from PSVident / Vita hardware documentation):
 *   0x10 IRS-001  PCH-1000 rev1      0x31 IRT-001  PCH-1100 3G rev1
 *   0x40 IRS-002  PCH-1000 rev2      0x41 IRT-002  PCH-1100 3G rev2
 *   0x60 IRS-1001 PCH-1000 late      0x80 USS-1001 PCH-2000 rev1
 *   0x82 USS-1002 PCH-2000 rev2      0x70 DOL-1001 VTE-1000 rev1
 *   0x72 DOL-1002 VTE-1000 rev2
 */
static int build_motherboard_from_hwinfo(char *out, int out_sz,
                                          unsigned char hwinfo[4])
{
    const char *board = NULL;
    switch (hwinfo[2]) {
        case 0x10: board = "IRS-001";  break;   /* PCH-1000 rev1     */
        case 0x31: board = "IRT-001";  break;   /* PCH-1100 3G rev1  */
        case 0x40: board = "IRS-002";  break;   /* PCH-1000 rev2     */
        case 0x41: board = "IRT-002";  break;   /* PCH-1100 3G rev2  */
        case 0x60: board = "IRS-1001"; break;   /* PCH-1000 late     */
        case 0x70: board = "DOL-1001"; break;   /* VTE-1000 rev1     */
        case 0x72: board = "DOL-1002"; break;   /* VTE-1000 rev2     */
        case 0x80: board = "USS-1001"; break;   /* PCH-2000 rev1     */
        case 0x82: board = "USS-1002"; break;   /* PCH-2000 rev2     */
    }
    if (!board) return -1;   /* unknown code — caller should fall back */

    int is_3g = ((hwinfo[0] & 0x0F) == 0x02);
    if (is_3g)
        snprintf(out, out_sz, "%s (3G)", board);
    else
        snprintf(out, out_sz, "%s", board);
    return 0;
}

/*
 * derive_motherboard_from_model() — model-based fallback.
 *
 * _vshSysconGetHardwareInfo() always fails from bubble app context.
 * We derive a useful board string from model + wwan which ARE reliable.
 *
 * Each model maps to a known board series:
 *   PCH-1000  → IRS series  (IRS-001, IRS-002, IRS-1001)
 *   PCH-1100  → IRT series  (IRT-001, IRT-002)  — 3G variant of 1000
 *   PCH-2000  → USS series  (USS-1001, USS-1002)
 *   VTE-1000  → DOL series  (DOL-1001, DOL-1002)
 *
 * We cannot distinguish revisions (001 vs 002) without hwinfo,
 * so we show the series name and PCH number.
 */
static void derive_motherboard_from_model(char *out, int out_sz,
                                           VitaModel model, int wwan)
{
    switch (model) {
        case MODEL_VITA_1000:
            snprintf(out, out_sz, "IRS series  (PCH-1000)");
            break;
        case MODEL_VITA_1000_3G:
            snprintf(out, out_sz, "IRT series  (PCH-1100 3G)");
            break;
        case MODEL_VITA_2000:
            snprintf(out, out_sz, "USS series  (PCH-2000)");
            break;
        case MODEL_PSTV:
            snprintf(out, out_sz, "DOL series  (VTE-1000)");
            break;
        default:
            /* Last resort — at least show what we know about WWAN */
            snprintf(out, out_sz, wwan ? "Unknown (3G detected)"
                                        : "Unknown");
            break;
    }
}

/*
 * read_model_from_registry() — read model_type from /CONFIG/SYSTEM.
 *
 * This is the most reliable userland model detection method.
 * The registry is always accessible from bubble app context.
 *
 * Known values (from Vita registry documentation):
 *   0x00 = PCH-1000  (Vita 1000 WiFi only, OLED)
 *   0x01 = PCH-1100  (Vita 1000 3G + WiFi, OLED)  ← PCH-1101 also returns 1
 *   0x02 = PCH-2000  (Vita 2000 Slim, LCD)
 *   0x03 = VTE-1000  (PlayStation TV)
 *
 * Returns the raw model_type value, or -1 on failure.
 */
static int read_model_from_registry(void)
{
    int val = 0;
    int r = sceRegMgrGetKeyInt("/CONFIG/SYSTEM", "model_type", &val);
    if (r >= 0) return val;
    return -1;
}

/* ── Public API ──────────────────────────────────────────────────────── */

void compat_init(void) {
    VitaCompat *c = &g_compat;
    memset(c, 0, sizeof(*c));

    /* ── 1. Firmware detection ──────────────────────────────────────────────
     *
     * PRIMARY: sceKernelGetSystemSwVersion() — kernel syscall, always accurate.
     * FALLBACK: partition read from sdstor0:int-lp-act-os at offset 0xB092.
     *   The partition offset is not consistent across all models/firmware, so
     *   the partition result is ONLY used when the syscall fails AND the decoded
     *   version falls within a known PS Vita firmware range (3.00 - 3.74).
     *   This prevents garbage reads (e.g. 0x03070000 → "3.07") being displayed.
     *
     * Version string format from syscall: "03.650.011" (major.minor*10.build)
     * We normalise both "03.650.011" and plain "3.65" formats correctly.
     */

    /* ── Helper: parse versionString → display string + ver_num (e.g. 365) */
    /* ver_num = major*100 + minor, where minor is 2-digit (65 for 3.65)     */
    {
        int fw_found = 0;
        unsigned int ver_num = 0;

        /* Try syscall first */
        SceKernelFwInfo fw;
        memset(&fw, 0, sizeof(fw));
        fw.size = sizeof(fw);
        if (sceKernelGetSystemSwVersion(&fw) >= 0 && fw.versionString[0]) {
            unsigned int maj = 0, min_raw = 0;
            if (sscanf(fw.versionString, "%u.%u", &maj, &min_raw) == 2 && maj > 0) {
                /* Handle both "03.650.011" (min_raw=650) and "3.65" (min_raw=65) */
                unsigned int minor = (min_raw >= 100) ? min_raw / 10 : min_raw;
                ver_num = maj * 100 + minor;
                snprintf(c->fw_display, sizeof(c->fw_display),
                         "%u.%02u", maj, minor);
                snprintf(c->fw_string,  sizeof(c->fw_string),
                         "%s", fw.versionString);
                fw_found = 1;
            }
        }

        /* Fallback: partition read — only accept result in valid FW range */
        if (!fw_found) {
            char part_disp[16];
            unsigned int part_ver = 0;
            if (read_fw_from_partition(part_disp, sizeof(part_disp), &part_ver) == 0) {
                unsigned int a = (part_ver >> 24) & 0xFF;
                unsigned int b = (part_ver >> 16) & 0xFF;
                unsigned int pver = a * 100 + b / 10;
                /* Only trust if within known Vita firmware range 3.00 – 3.74 */
                if (a == 3 && pver >= 300 && pver <= 374) {
                    ver_num = pver;
                    snprintf(c->fw_display, sizeof(c->fw_display), "%s", part_disp);
                    snprintf(c->fw_string,  sizeof(c->fw_string),  "%s", part_disp);
                    fw_found = 1;
                }
            }
        }

        if (!fw_found) {
            snprintf(c->fw_string,  sizeof(c->fw_string),  "unknown");
            snprintf(c->fw_display, sizeof(c->fw_display), "?.??");
            ver_num = 0;
        }

        /* Map ver_num to fw_tier — covers all known CFW-capable firmwares */
        switch (ver_num) {
            case 360: case 361: case 363:
                c->fw_tier = FW_360_HENKAKU;  break;
            case 365:
                c->fw_tier = FW_365_HENCORE;  break;
            case 367:
                c->fw_tier = FW_367_HENCORE;  break;
            case 368:
                c->fw_tier = FW_368_HENCORE;  break;
            case 369: case 370: case 371:
            case 372: case 373:
                c->fw_tier = FW_373_HENCORE2; break;
            case 374:
                c->fw_tier = FW_374_HENCORE2; break;
            default:
                c->fw_tier = FW_UNKNOWN;      break;
        }
    }

    /* ── 2. Ensō capability & detection ── */
    c->enso_capable = (c->fw_tier == FW_360_HENKAKU ||
                       c->fw_tier == FW_365_HENCORE);
    c->has_enso = path_exists("ur0:tai/boot_config.txt") ||
                  path_exists("ur0:tai/enso/");

    /* ── 3. Model + motherboard ──────────────────────────────────────────
     *
     * All VSH/PsCode calls fail from bubble app context. Detection is
     * purely filesystem-based. PCH-2000 and PSTV have imc0: internal
     * storage; PCH-1000/1100 do not.
     *
     * drive_exists() tries both sceIoGetstat() and sceIoDopen() since
     * some firmware versions only respond to one or the other on root paths.
     * Retry loop gives up to 1 second for the partition to mount.
     */
    unsigned char hwinfo[4] = {0};
    int hw_ok = _vshSysconGetHardwareInfo(hwinfo);

    /* ── Model detection — three methods, best-wins priority ────────────
     *
     * 1. Registry /CONFIG/SYSTEM model_type — most reliable, distinguishes
     *    all variants including PCH-1100 3G. Always works from userland.
     * 2. sceKernelGetModelForCDialog() — works from userland but does NOT
     *    distinguish PCH-1000 from PCH-1100 (both return 0). Used as
     *    fallback when registry fails.
     * 3. imc0 filesystem probe — last resort. Distinguishes 1000/1100
     *    (no imc0) from 2000/PSTV (has imc0).
     *
     * vshSysconHasWWAN() ALWAYS fails from bubble app context (returns 0).
     * Do NOT use it for 3G detection — registry method_type handles it.
     */
    int reg_model = read_model_from_registry();
    int cdlg      = sceKernelGetModelForCDialog();

    /* Store raw debug values */
    g_compat.dbg_imc0_stat  = reg_model;   /* registry result */
    g_compat.dbg_wwan       = cdlg;        /* cdlg result     */
    g_compat.dbg_imc0_dopen = 0;

    /* imc0 probe with retry */
    c->has_imc0 = drive_exists("imc0:") || drive_exists("int:");
    for (int i = 0; i < 3 && !c->has_imc0; i++) {
        sceKernelDelayThread(200000);
        c->has_imc0 = drive_exists("imc0:") || drive_exists("int:");
    }
    g_compat.dbg_imc0_dopen = c->has_imc0;

    /* Method 1: registry model_type — authoritative */
    if (reg_model >= 0) {
        switch (reg_model) {
            case 0: c->model = MODEL_VITA_1000;    break;  /* PCH-1000 WiFi  */
            case 1: c->model = MODEL_VITA_1000_3G; break;  /* PCH-1100/1101  */
            case 2: c->model = MODEL_VITA_2000;    break;  /* PCH-2000 Slim  */
            case 3: c->model = MODEL_PSTV;         break;  /* VTE-1000 PSTV  */
            default:
                /* Unknown registry value — fall through to method 2 */
                reg_model = -1;
                break;
        }
    }

    /* Method 2: sceKernelGetModelForCDialog — fallback if registry failed */
    if (reg_model < 0) {
        switch (cdlg) {
            case 0:
                /* cdlg cannot distinguish PCH-1000 vs PCH-1100;
                 * use imc0 to distinguish 2000/PSTV from 1000/1100 */
                if (c->has_imc0)
                    c->model = drive_exists("psglobal:") ? MODEL_PSTV : MODEL_VITA_2000;
                else
                    c->model = MODEL_VITA_1000;
                break;
            case 1: c->model = MODEL_VITA_2000; break;
            case 2: c->model = MODEL_PSTV;      break;
            default:
                c->model = c->has_imc0 ?
                    (drive_exists("psglobal:") ? MODEL_PSTV : MODEL_VITA_2000)
                    : MODEL_VITA_1000;
                break;
        }
    }

    /* Final override: if model still shows as 1000 but imc0 exists,
     * this must be a 2000 or PSTV — registry returned wrong value */
    if (c->model == MODEL_VITA_1000 && c->has_imc0) {
        c->model = drive_exists("psglobal:") ? MODEL_PSTV : MODEL_VITA_2000;
    }

    /* Try hwinfo first (works in VSH/kernel context), else derive from model */
    if (hw_ok == 0 &&
        build_motherboard_from_hwinfo(c->motherboard,
                                       sizeof(c->motherboard), hwinfo) == 0) {
        /* hwinfo succeeded — exact board code known */
    } else {
        /* hwinfo failed (expected from bubble app) — derive from model */
        derive_motherboard_from_model(c->motherboard, sizeof(c->motherboard),
                                      c->model, g_compat.dbg_wwan);
    }

    /* ── 4. tai config priority ──
     * ur0: first  — canonical location (AutoPlugin2)
     * imc0: second — PCH-2000/PSTV internal storage
     * ux0: last fallback
     */
    c->tai_config_count = 0;
    if (path_exists("ur0:tai/config.txt"))
        snprintf(c->tai_configs[c->tai_config_count++], 64,
                 "ur0:tai/config.txt");
    if (c->has_imc0 && path_exists("imc0:tai/config.txt"))
        snprintf(c->tai_configs[c->tai_config_count++], 64,
                 "imc0:tai/config.txt");
    if (path_exists("ux0:tai/config.txt"))
        snprintf(c->tai_configs[c->tai_config_count++], 64,
                 "ux0:tai/config.txt");

    if (c->tai_config_count > 0)
        snprintf(c->active_tai_config, sizeof(c->active_tai_config),
                 "%.63s", c->tai_configs[0]);
    else
        snprintf(c->active_tai_config, sizeof(c->active_tai_config),
                 "(none found)");
}

const char *compat_model_name(void) {
    switch (g_compat.model) {
        case MODEL_VITA_1000:    return "PS Vita 1000 (OLED)";
        case MODEL_VITA_1000_3G: return "PS Vita 1100 (3G OLED)";
        case MODEL_VITA_2000:    return "PS Vita 2000 (Slim LCD)";
        case MODEL_PSTV:         return "PlayStation TV";
        default:                 return "Unknown";
    }
}

const char *compat_fw_tier_name(void) {
    switch (g_compat.fw_tier) {
        case FW_360_HENKAKU:  return "HENkaku (Enso capable)";
        case FW_365_HENCORE:  return "h-encore (Enso capable)";
        case FW_367_HENCORE:  return "h-encore (no Enso)";
        case FW_368_HENCORE:  return "h-encore (no Enso)";
        case FW_373_HENCORE2: return "h-encore2 (no Enso)";
        case FW_374_HENCORE2: return "h-encore2 / Hackcockoo";
        default:              return "Unknown exploit";
    }
}

void compat_get_summary(char *out, int out_size) {
    snprintf(out, out_size, "%s | FW %s | %s | Enso: %s",
             compat_model_name(),
             g_compat.fw_display,
             compat_fw_tier_name(),
             g_compat.has_enso     ? "YES" :
             g_compat.enso_capable ? "No"  : "N/A");
}
