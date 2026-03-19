#pragma once
/*
 * compat.h — Hardware & Firmware Compatibility Layer
 *
 * Abstracts every difference between:
 *   - PS Vita 1000 (PCH-10xx) — OLED, no 3G, no imc0:
 *   - PS Vita 1100 (PCH-11xx) — OLED + 3G, no imc0:
 *   - PS Vita 2000 (PCH-20xx) — Slim LCD, has imc0: (1 GB)
 *   - PS TV / Vita TV          — TV box, has imc0: + USB
 *
 * Firmware / exploit tiers supported:
 *   3.60  — HENkaku (browser), Ensō available
 *   3.65  — h-encore, Ensō available
 *   3.67  — h-encore only, NO Ensō
 *   3.68  — h-encore only, NO Ensō
 *   3.73  — h-encore2 only, NO Ensō
 *   3.74  — Hackcockoo / h-encore2, NO Ensō
 */

#include <psp2/io/stat.h>
#include <psp2/kernel/modulemgr.h>

/* ── Model identifiers ───────────────────────────────────────────────── */
typedef enum {
    MODEL_VITA_1000   = 0,  /* PCH-10xx OLED, WiFi only              */
    MODEL_VITA_1000_3G= 1,  /* PCH-11xx OLED + 3G                    */
    MODEL_VITA_2000   = 2,  /* PCH-20xx Slim LCD, has imc0:          */
    MODEL_PSTV        = 3,  /* CEM-3000 PS TV, has imc0: + USB       */
    MODEL_UNKNOWN     = 4
} VitaModel;

/* ── Firmware / exploit tier ─────────────────────────────────────────── */
typedef enum {
    FW_360_HENKAKU  = 0,   /* 3.60 — HENkaku, Ensō capable          */
    FW_365_HENCORE  = 1,   /* 3.65 — h-encore, Ensō capable         */
    FW_367_HENCORE  = 2,   /* 3.67 — h-encore, NO Ensō              */
    FW_368_HENCORE  = 3,   /* 3.68 — h-encore, NO Ensō              */
    FW_373_HENCORE2 = 4,   /* 3.73 — h-encore2, NO Ensō             */
    FW_374_HENCORE2 = 5,   /* 3.74 — Hackcockoo / h-encore2, NO Ensō*/
    FW_UNKNOWN      = 6
} FwTier;

/* ── Detected device state (populated once by compat_init) ──────────── */
typedef struct {
    VitaModel model;
    FwTier    fw_tier;
    char      fw_string[16];     /* raw version e.g. "3.65"           */
    char      fw_display[16];    /* display version e.g. "3.65"       */
    char      motherboard[32];   /* e.g. "IRS-1001 (3G)"             */
    int       has_imc0;          /* 1 if imc0: accessible             */
    int       has_enso;          /* 1 if Ensō boot_config present     */
    int       enso_capable;      /* 1 if this FW supports Ensō        */
    /* tai config paths */
    char      active_tai_config[64];
    char      tai_configs[3][64];
    int       tai_config_count;
    /* debug — raw detection values, remove once model confirmed on all hw */
    int       dbg_imc0_stat;   /* sceIoGetstat("imc0:")  result           */
    int       dbg_imc0_dopen;  /* sceIoDopen("imc0:")    result           */
    int       dbg_wwan;        /* vshSysconHasWWAN()     result           */
} VitaCompat;

/* ── Global instance ─────────────────────────────────────────────────── */
extern VitaCompat g_compat;

/* ── Init — call once at startup before drawing anything ─────────────── */
void compat_init(void);

/* ── Accessors ───────────────────────────────────────────────────────── */
static inline int       compat_has_imc0(void)     { return g_compat.has_imc0; }
static inline int       compat_has_enso(void)     { return g_compat.has_enso; }
static inline int       compat_enso_capable(void) { return g_compat.enso_capable; }
static inline VitaModel compat_model(void)        { return g_compat.model; }
static inline FwTier    compat_fw_tier(void)      { return g_compat.fw_tier; }

const char *compat_model_name(void);
const char *compat_fw_tier_name(void);
void        compat_get_summary(char *out, int out_size);
