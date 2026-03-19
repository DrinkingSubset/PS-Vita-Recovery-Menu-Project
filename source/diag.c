/*
 * diag.c — Boot Diagnostic Mode for PS Vita Recovery Menu
 *
 * Runs a one-shot scan of every common soft-brick cause and displays
 * colour-coded results with a suggested-fix section at the bottom.
 *
 * Checks performed:
 *   1. Kernel / HENkaku running         (we are running = kernel OK)
 *   2. Mount points: ux0, ur0, imc0, uma0, grw0, xmc0
 *   3. tai config: which file is active, multiple-config warning
 *   4. HENkaku key files: henkaku.suprx, henkaku.skprx, taihen.skprx
 *   5. Storage plugin in config (StorageMgr / YAMT / gamesd / usbmc)
 *   6. storage_config.txt present when storage plugin detected
 *   7. Plugin count sanity (warn if > 10)
 *   8. Enso status
 *   9. Backup config present
 */

#include <psp2/ctrl.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/io/dirent.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "diag.h"
#include "compat.h"
#include "plugins.h"
#include "draw.h"
#include "display.h"
#include "menu.h"

/* ── Severity ────────────────────────────────────────────────────────────── */
typedef enum {
    SEV_OK   = 0,
    SEV_INFO = 1,
    SEV_WARN = 2,
    SEV_FAIL = 3
} DiagSev;

static const char *SEV_TAG[]   = { "[ OK ]", "[INFO]", "[WARN]", "[FAIL]" };
static const uint32_t SEV_COL[] = {
    RGBA8( 80,255, 80,255),   /* OK   — green  */
    RGBA8(160,160,160,255),   /* INFO — dim    */
    RGBA8(255,220,  0,255),   /* WARN — yellow */
    RGBA8(255, 80, 80,255),   /* FAIL — red    */
};

/* ── Result entry ─────────────────────────────────────────────────────────── */
#define DIAG_MAX       48
#define DIAG_MSG_LEN   72    /* fits on screen at FONT_W=16, MENU_X=40 */
#define DIAG_FIX_MAX   16
#define DIAG_FIX_LEN   72

typedef struct {
    DiagSev  sev;
    char     msg[DIAG_MSG_LEN];
    int      is_section;     /* 1 = section header row (dim, no tag) */
} DiagResult;

static DiagResult g_results[DIAG_MAX];
static int        g_result_count = 0;
static int        g_warn_count   = 0;
static int        g_fail_count   = 0;

/* Suggested fixes — filled during scan */
static char g_fixes[DIAG_FIX_MAX][DIAG_FIX_LEN];
static int  g_fix_count = 0;

/* ── Emit helpers ─────────────────────────────────────────────────────────── */
static void emit(DiagSev sev, const char *msg) {
    if (g_result_count >= DIAG_MAX) return;
    g_results[g_result_count].sev        = sev;
    g_results[g_result_count].is_section = 0;
    snprintf(g_results[g_result_count].msg,
             DIAG_MSG_LEN, "%s", msg);
    g_result_count++;
    if (sev == SEV_WARN) g_warn_count++;
    if (sev == SEV_FAIL) g_fail_count++;
}

static void emitf(DiagSev sev, const char *fmt, ...) {
    char buf[DIAG_MSG_LEN];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    emit(sev, buf);
}

static void section(const char *label) {
    if (g_result_count >= DIAG_MAX) return;
    g_results[g_result_count].sev        = SEV_INFO;
    g_results[g_result_count].is_section = 1;
    snprintf(g_results[g_result_count].msg, DIAG_MSG_LEN, "-- %s --", label);
    g_result_count++;
}

static void fix(const char *msg) {
    if (g_fix_count >= DIAG_FIX_MAX) return;
    snprintf(g_fixes[g_fix_count], DIAG_FIX_LEN, "%s", msg);
    g_fix_count++;
}

/* ── Low-level probe helpers ──────────────────────────────────────────────── */
static int path_exists(const char *p) {
    SceIoStat st;
    return sceIoGetstat(p, &st) >= 0;
}

/* Probe a mount point by trying to open it as a directory */
static int mount_ok(const char *p) {
    SceUID d = sceIoDopen(p);
    if (d >= 0) { sceIoDclose(d); return 1; }
    return 0;
}

/* Scan the active tai config text for a substring (plugin filename) */
static int config_contains(const char *needle) {
    const char *cp = plugins_get_config_path();
    if (!cp || cp[0] == '(' || cp[0] == '\0') return 0;
    SceUID fd = sceIoOpen(cp, SCE_O_RDONLY, 0);
    if (fd < 0) return 0;
    int sz = (int)sceIoLseek(fd, 0, SCE_SEEK_END);
    sceIoLseek(fd, 0, SCE_SEEK_SET);
    if (sz <= 0 || sz > 32768) { sceIoClose(fd); return 0; }
    char *buf = malloc(sz + 1);
    if (!buf) { sceIoClose(fd); return 0; }
    int rd = sceIoRead(fd, buf, sz);
    sceIoClose(fd);
    if (rd != sz) { free(buf); return 0; }
    buf[sz] = '\0';
    int found = (strstr(buf, needle) != NULL);
    free(buf);
    return found;
}

/* ── Individual checks ────────────────────────────────────────────────────── */

static void check_kernel(void) {
    section("Kernel / Exploit");
    /* If we are executing, kernel userland is working */
    emit(SEV_OK, "Kernel userland: RUNNING");

    /* HENkaku — check henkaku.suprx is accessible */
    if (path_exists("ur0:tai/henkaku.suprx"))
        emit(SEV_OK, "HENkaku: ACTIVE  (ur0:tai/henkaku.suprx)");
    else if (path_exists("ux0:tai/henkaku.suprx"))
        emit(SEV_OK, "HENkaku: ACTIVE  (ux0:tai/henkaku.suprx)");
    else {
        emit(SEV_WARN, "HENkaku: henkaku.suprx NOT FOUND");
        fix("Reinstall h-encore/HENkaku to restore henkaku.suprx");
    }

    /* taiHEN kernel plugin */
    if (path_exists("ur0:tai/henkaku.skprx"))
        emit(SEV_OK, "taiHEN kernel plugin: FOUND");
    else {
        emit(SEV_WARN, "henkaku.skprx NOT FOUND in ur0:tai/");
        fix("Reinstall HENkaku to restore kernel plugin files");
    }

    /* Enso */
    if (g_compat.has_enso)
        emit(SEV_OK,   "Enso: INSTALLED (auto-CFW on boot)");
    else if (g_compat.enso_capable)
        emit(SEV_INFO, "Enso: NOT installed (exploit needed each boot)");
    else
        emit(SEV_INFO, "Enso: Not supported on this firmware");
}

static void check_mounts(void) {
    section("Mount Points");

    /* ux0: is the most important — SD2Vita or Sony card */
    if (mount_ok("ux0:"))
        emit(SEV_OK,   "ux0:  MOUNTED");
    else {
        emit(SEV_FAIL, "ux0:  NOT MOUNTED");
        fix("ux0: failed - check SD2Vita card / StorageMgr config");
    }

    /* ur0: internal 1GB flash — always present on HENkaku */
    if (mount_ok("ur0:"))
        emit(SEV_OK,   "ur0:  MOUNTED");
    else {
        emit(SEV_FAIL, "ur0:  NOT MOUNTED  (critical!)");
        fix("ur0: failed - internal flash, try full reinstall");
    }

    /* imc0: only on Vita 2000 / PS TV */
    if (g_compat.has_imc0) {
        if (mount_ok("imc0:"))
            emit(SEV_OK,   "imc0: MOUNTED  (2000/PSTV internal)");
        else {
            emit(SEV_WARN, "imc0: NOT MOUNTED  (expected on this model)");
            fix("imc0: missing - check StorageMgr INT= config entry");
        }
    } else {
        emit(SEV_INFO, "imc0: N/A  (Vita 1000/1100 - no internal flash)");
    }

    /* uma0: USB/extra storage — optional */
    if (mount_ok("uma0:"))
        emit(SEV_INFO, "uma0: MOUNTED  (USB / extra storage)");
    else
        emit(SEV_INFO, "uma0: not present  (normal if no USB storage)");

    /* grw0: SD2Vita remapped partition — optional */
    if (mount_ok("grw0:"))
        emit(SEV_INFO, "grw0: MOUNTED  (remapped storage)");
    else
        emit(SEV_INFO, "grw0: not present");

    /* xmc0: Sony Memory Stick / MC slot */
    if (mount_ok("xmc0:"))
        emit(SEV_INFO, "xmc0: MOUNTED  (Sony Memory Card)");
    else
        emit(SEV_INFO, "xmc0: not present");
}

static void check_tai_config(void) {
    section("tai Config");

    /* Check each possible config location */
    int found_imc0 = path_exists("imc0:tai/config.txt");
    int found_ux0  = path_exists("ux0:tai/config.txt");
    int found_ur0  = path_exists("ur0:tai/config.txt");

    if (!found_imc0 && !found_ux0 && !found_ur0) {
        emit(SEV_FAIL, "No tai config.txt found anywhere!");
        fix("Plugin Fix Mode -> Reset to Minimal to fix config");
        fix("Or copy a working config.txt to ur0:tai/config.txt");
        return;
    }

    /* Show active config from compat */
    const char *active = g_compat.active_tai_config;
    if (active && active[0])
        emitf(SEV_OK, "Active config: %s", active);
    else
        emit(SEV_WARN, "Active config: could not determine");

    /* Multiple config warning — taiHEN loads highest-priority one only */
    int config_count = found_imc0 + found_ux0 + found_ur0;
    if (config_count > 1) {
        emit(SEV_WARN, "Multiple config.txt files detected!");
        if (found_imc0) emit(SEV_INFO, "  imc0:tai/config.txt  [HIGHEST priority]");
        if (found_ux0)  emit(SEV_INFO, "  ux0:tai/config.txt");
        if (found_ur0)  emit(SEV_INFO, "  ur0:tai/config.txt   [lowest priority]");
        fix("Remove unused tai configs to avoid confusion");
    }

    /* Plugin count sanity */
    int pc = plugins_get_count();
    if (pc == 0)
        emit(SEV_WARN, "Config loaded but 0 plugins found - config may be empty");
    else if (pc > 10) {
        emitf(SEV_WARN, "High plugin count: %d  (boot loop risk)", pc);
        fix("Too many plugins: Plugin Fix Mode -> Safe Mode");
    } else {
        emitf(SEV_OK, "Plugin count: %d", pc);
    }

    /* Backup config */
    if (path_exists("ur0:tai/config_backup.txt"))
        emit(SEV_OK,   "Config backup: PRESENT  (ur0:tai/config_backup.txt)");
    else
        emit(SEV_INFO, "Config backup: none  (use Plugin Fix Mode to create one)");
}

static void check_henkaku_files(void) {
    section("HENkaku Files");

    static const struct { const char *path; const char *label; int critical; } FILES[] = {
        { "ur0:tai/henkaku.suprx",    "henkaku.suprx  (userland)",   1 },
        { "ur0:tai/henkaku.skprx",    "henkaku.skprx  (kernel)",     1 },
        { "ur0:tai/taihen.skprx",     "taihen.skprx",                0 },
        { "ur0:tai/bootstrap.self",   "bootstrap.self",               0 },
    };

    for (int i = 0; i < (int)(sizeof(FILES)/sizeof(FILES[0])); i++) {
        if (path_exists(FILES[i].path))
            emitf(SEV_OK,   "FOUND    %s", FILES[i].label);
        else if (FILES[i].critical) {
            emitf(SEV_FAIL, "MISSING  %s", FILES[i].label);
            fix("Critical HENkaku file missing: reinstall CFW");
        } else {
            emitf(SEV_INFO, "absent   %s  (optional)", FILES[i].label);
        }
    }
}

static void check_storage_plugin(void) {
    section("Storage Plugin");

    /* Known storage plugins and their display names */
    static const struct { const char *name; const char *display; } PLUGINS[] = {
        { "storagemgr.skprx", "StorageMgr" },
        { "yamt.skprx",       "YAMT"       },
        { "gamesd.skprx",     "gamesd"     },
        { "usbmc.skprx",      "usbmc"      },
        { "sdgamesd.skprx",   "sdgamesd"   },
    };

    int found_plugin = 0;
    char found_name[32] = {0};

    for (int i = 0; i < (int)(sizeof(PLUGINS)/sizeof(PLUGINS[0])); i++) {
        if (config_contains(PLUGINS[i].name)) {
            found_plugin = 1;
            snprintf(found_name, sizeof(found_name), "%s", PLUGINS[i].display);
            break;
        }
    }

    if (found_plugin) {
        emitf(SEV_OK, "Storage plugin in config: %s", found_name);

        /* Check storage_config.txt — required by StorageMgr */
        int has_cfg_ur0 = path_exists("ur0:tai/storage_config.txt");
        int has_cfg_ux0 = path_exists("ux0:tai/storage_config.txt");
        if (has_cfg_ur0)
            emit(SEV_OK,   "storage_config.txt: FOUND  (ur0:tai/)");
        else if (has_cfg_ux0)
            emit(SEV_OK,   "storage_config.txt: FOUND  (ux0:tai/)");
        else {
            emit(SEV_WARN, "storage_config.txt: NOT FOUND");
            fix("StorageMgr detected but storage_config.txt missing");
            fix("Go to Storage Manager -> Install StorageMgr to fix");
        }

        /* Also verify the .skprx file itself is on disk */
        char skprx_ur0[80], skprx_ux0[80];
        /* Find which one was matched */
        for (int i = 0; i < (int)(sizeof(PLUGINS)/sizeof(PLUGINS[0])); i++) {
            if (config_contains(PLUGINS[i].name)) {
                snprintf(skprx_ur0, sizeof(skprx_ur0),
                         "ur0:tai/%s", PLUGINS[i].name);
                snprintf(skprx_ux0, sizeof(skprx_ux0),
                         "ux0:tai/%s", PLUGINS[i].name);
                if (path_exists(skprx_ur0) || path_exists(skprx_ux0))
                    emitf(SEV_OK,   "Plugin file on disk: %s", PLUGINS[i].name);
                else {
                    emitf(SEV_WARN, "Plugin listed in config but .skprx NOT FOUND");
                    fix("Storage plugin in config but file missing on disk");
                    fix("Go to Storage Manager -> Install StorageMgr");
                }
                break;
            }
        }
    } else {
        emit(SEV_INFO, "No storage plugin found in config");
        emit(SEV_INFO, "ux0: = built-in Sony Memory Card slot");
        if (!mount_ok("ux0:"))
            fix("No storage plugin + ux0 failed: install StorageMgr");
    }
}

static void check_model_specific(void) {
    section("Model / Firmware");

    emitf(SEV_INFO, "Model: %s", compat_model_name());
    emitf(SEV_INFO, "Firmware: %s  (%s)",
          g_compat.fw_display, compat_fw_tier_name());

    /* Warn on firmwares that lose CFW on reboot */
    if (g_compat.fw_tier == FW_367_HENCORE  ||
        g_compat.fw_tier == FW_368_HENCORE  ||
        g_compat.fw_tier == FW_373_HENCORE2 ||
        g_compat.fw_tier == FW_374_HENCORE2) {
        emit(SEV_WARN, "This FW has no Enso - re-run exploit after every reboot");
    }

    /* 2000/PSTV imc0 load order */
    if (g_compat.has_imc0) {
        if (path_exists("imc0:tai/config.txt"))
            emit(SEV_WARN,
                 "imc0:tai/config.txt loads BEFORE ux0:/ur0: on this model");
    }
}

/* ── Full scan ─────────────────────────────────────────────────────────────── */
static void diag_scan(void) {
    g_result_count = 0;
    g_warn_count   = 0;
    g_fail_count   = 0;
    g_fix_count    = 0;

    /* Reload plugins from disk before scanning */
    plugins_load();

    check_kernel();
    check_mounts();
    check_tai_config();
    check_henkaku_files();
    check_storage_plugin();
    check_model_specific();
}

/* ── Layout constants ─────────────────────────────────────────────────────── */
#define DG_TITLE_H    TITLE_H
#define DG_TITLE_LINE TITLE_LINE
#define DG_FOOTER_Y   FOOTER_Y
#define DG_TAG_X      MENU_X
#define DG_MSG_X      (MENU_X + 104)   /* 6-char tag @ FONT_W=16 = 96px + 8px gap */
#define DG_ITEM_H     LINE_H
#define DG_CONTENT_T  (DG_TITLE_H + 4)

/* Reserve bottom area for summary + fixes */
#define DG_SUMMARY_ROWS_MAX  10
#define DG_SUMMARY_H         (LINE_H * DG_SUMMARY_ROWS_MAX + 8)
#define DG_CONTENT_B         (DG_FOOTER_Y - DG_SUMMARY_H - 8)
#define DG_CONTENT_H         (DG_CONTENT_B - DG_CONTENT_T)

static int g_scroll = 0;

static void diag_draw(void) {
    display_start();
    display_clear(COLOR_BG);

    /* ── Title ── */
    display_rect(0, 0, SCREEN_W, DG_TITLE_LINE, COLOR_TITLE_BG);
    display_text(MENU_X, 6,  COLOR_TEXT,   "PS Vita Recovery Menu v1.0");
    display_text(MENU_X, 26, COLOR_YELLOW, "Boot Diagnostics");
    display_hline(0, DG_TITLE_LINE, SCREEN_W, COLOR_GREEN);

    /* ── Scrollable results ── */
    int y = DG_CONTENT_T - g_scroll;

    for (int i = 0; i < g_result_count; i++) {
        DiagResult *r = &g_results[i];
        int row_y = y;
        y += DG_ITEM_H;

        /* Clip to content area */
        if (row_y + DG_ITEM_H <= DG_CONTENT_T) continue;
        if (row_y >= DG_CONTENT_B) break;

        if (r->is_section) {
            /* Section header — full-width dim line */
            display_text(DG_TAG_X, row_y, COLOR_DIM, r->msg);
        } else {
            /* Severity tag */
            display_text(DG_TAG_X, row_y, SEV_COL[r->sev], SEV_TAG[r->sev]);
            /* Message */
            uint32_t msg_col = (r->sev == SEV_OK)   ? COLOR_TEXT   :
                               (r->sev == SEV_INFO)  ? COLOR_DIM    :
                               (r->sev == SEV_WARN)  ? COLOR_YELLOW :
                                                       COLOR_RED;
            display_text(DG_MSG_X, row_y, msg_col, r->msg);
        }
    }

    /* Clip mask — overdraw content outside bounds */
    display_rect(0, 0,         SCREEN_W, DG_CONTENT_T, COLOR_BG);
    display_rect(0, DG_CONTENT_B, SCREEN_W, SCREEN_H,  COLOR_BG);
    /* Restore title bar over the mask */
    display_rect(0, 0, SCREEN_W, DG_TITLE_LINE, COLOR_TITLE_BG);
    display_text(MENU_X, 6,  COLOR_TEXT,   "PS Vita Recovery Menu v1.0");
    display_text(MENU_X, 26, COLOR_YELLOW, "Boot Diagnostics");
    display_hline(0, DG_TITLE_LINE, SCREEN_W, COLOR_GREEN);

    /* Scroll bar */
    int total_content = g_result_count * DG_ITEM_H;
    if (total_content > DG_CONTENT_H) {
        int bar_h = DG_CONTENT_H * DG_CONTENT_H / total_content;
        if (bar_h < 16) bar_h = 16;
        int max_scroll = total_content - DG_CONTENT_H;
        int bar_y = DG_CONTENT_T +
            (g_scroll * (DG_CONTENT_H - bar_h)) / (max_scroll > 0 ? max_scroll : 1);
        display_rect(SCREEN_W - 5, DG_CONTENT_T, 3, DG_CONTENT_H,
                     RGBA8(30, 30, 30, 255));
        display_rect(SCREEN_W - 5, bar_y, 3, bar_h, COLOR_GREEN);
    }

    /* Separator above summary */
    display_hline(0, DG_CONTENT_B + 2, SCREEN_W, RGBA8(50, 50, 50, 255));

    /* ── Summary box ── */
    int sy = DG_CONTENT_B + 6;

    if (g_fail_count == 0 && g_warn_count == 0) {
        display_rect(MENU_X - 8, sy - 2,
                     SCREEN_W - (MENU_X - 8) * 2, LINE_H + 4,
                     RGBA8(0, 40, 0, 255));
        display_text(MENU_X, sy, COLOR_GREEN,
                     "All checks passed. System looks healthy.");
        sy += LINE_H + 6;
    } else {
        char summary[80];
        snprintf(summary, sizeof(summary),
                 "%d failure%s, %d warning%s",
                 g_fail_count, g_fail_count == 1 ? "" : "s",
                 g_warn_count, g_warn_count == 1 ? "" : "s");
        display_rect(MENU_X - 8, sy - 2,
                     SCREEN_W - (MENU_X - 8) * 2, LINE_H + 4,
                     RGBA8(50, 20, 0, 255));
        display_text(MENU_X, sy,
                     g_fail_count > 0 ? COLOR_RED : COLOR_YELLOW,
                     summary);
        sy += LINE_H + 4;

        /* Suggested fixes */
        display_text(MENU_X, sy, COLOR_DIM, "Suggested:");
        sy += LINE_H;
        for (int i = 0; i < g_fix_count && sy + LINE_H < DG_FOOTER_Y - 2; i++) {
            display_text(MENU_X + 8, sy, COLOR_YELLOW, g_fixes[i]);
            sy += LINE_H;
        }
    }

    /* ── Footer ── */
    display_rect(0, DG_FOOTER_Y, SCREEN_W, SCREEN_H - DG_FOOTER_Y,
                 COLOR_TITLE_BG);
    display_hline(0, DG_FOOTER_Y, SCREEN_W, COLOR_GREEN);
    display_text(MENU_X, DG_FOOTER_Y + 7, COLOR_DIM,
                 "[Up/Down] Scroll   [Square] Re-scan   [O] Back");

    display_end();
}

/* ── Input / main loop ─────────────────────────────────────────────────────── */

static int diag_btn(SceCtrlData *now, SceCtrlData *old, unsigned int b) {
    return (now->buttons & b) && !(old->buttons & b);
}

static void diag_drain(SceCtrlData *old) {
    SceCtrlData c;
    do { sceCtrlPeekBufferPositive(0, old, 1); } while (old->buttons);
    (void)c;
}

void diag_run(void) {
    g_scroll = 0;
    diag_scan();

    SceCtrlData old;
    memset(&old, 0, sizeof(old));
    diag_drain(&old);

    while (1) {
        diag_draw();

        SceCtrlData ctrl;
        sceCtrlPeekBufferPositive(0, &ctrl, 1);

        /* Scroll */
        int max_scroll = g_result_count * DG_ITEM_H - DG_CONTENT_H;
        if (max_scroll < 0) max_scroll = 0;

        if (diag_btn(&ctrl, &old, SCE_CTRL_DOWN)) {
            g_scroll += DG_ITEM_H;
            if (g_scroll > max_scroll) g_scroll = max_scroll;
        }
        if (diag_btn(&ctrl, &old, SCE_CTRL_UP)) {
            g_scroll -= DG_ITEM_H;
            if (g_scroll < 0) g_scroll = 0;
        }

        /* Re-scan */
        if (diag_btn(&ctrl, &old, SCE_CTRL_SQUARE)) {
            g_scroll = 0;
            diag_scan();
            diag_drain(&old);
            continue;
        }

        /* Back */
        if (diag_btn(&ctrl, &old, SCE_CTRL_CIRCLE)) break;

        old = ctrl;
    }
}
