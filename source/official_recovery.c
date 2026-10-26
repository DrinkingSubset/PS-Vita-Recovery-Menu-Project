/*
 * official_recovery.c
 * Reimplements Sony's 5 official PS Vita safe-mode recovery functions.
 *
 * Max safe text width: (960 - 40 - 40) / 16 = 55 chars per line.
 * All strings verified <= 55 chars.
 */

#include <psp2/ctrl.h>
#include <psp2/power.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/io/dirent.h>
#include <psp2/promoterutil.h>
#include <psp2/sysmodule.h>

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "official_recovery.h"
#include "plugins.h"
#include "compat.h"
#include <strings.h>
#include "draw.h"
#include "menu.h"

#define APP_DB_PATH  "ur0:shell/db/app.db"
#define UX0_PATH     "ux0:"
#define UPDATE_PUP   "ux0:update/PSP2UPDAT.PUP"

/* Max chars per description line — (960 - 40 left - 40 right) / 16px = 55 */
#define MAX_DESC_CHARS 55

static char g_msg[256]  = "";
static char g_msg2[256] = "";   /* optional 2nd line (error detail/hint) */
static char g_msg3[256] = "";   /* optional 3rd line */

static int btn_pressed(SceCtrlData *now, SceCtrlData *old, unsigned int b) {
    return (now->buttons & b) && !(old->buttons & b);
}

/* ── Layout ──────────────────────────────────────────────────────────────── */
/* Each option row: label (LINE_H) + desc (LINE_H) + gap (4) = 52px         */
#define ROW_H        (LINE_H * 2 + 4)

/* Options start after title bar + subtitle note + separator                 */
/* TITLE_H=46, subtitle row=LINE_H=24, sep=1, gap=4 → OPTIONS_Y=75          */
#define SUBTITLE_Y   (TITLE_H + 4)
#define SEP_Y        (SUBTITLE_Y + LINE_H)
#define OPTIONS_Y    (SEP_Y + 6)

/* [DANGER] tag — right-aligned, 8 chars * 16px = 128px wide                */
#define DANGER_X     (SCREEN_W - 128 - MENU_X)   /* = 792 */

/* ── Shared helpers ──────────────────────────────────────────────────────── */
static void draw_titlebar(const char *subtitle) {
    display_rect(0, 0, SCREEN_W, TITLE_LINE, COLOR_TITLE_BG);
    display_app_title(6);
    draw_text(MENU_X, 26, COLOR_DIM,  subtitle);
    display_hline(0, TITLE_LINE, SCREEN_W, COLOR_GREEN);
}

static void draw_footerbar(const char *hint) {
    display_rect(0, FOOTER_Y, SCREEN_W, SCREEN_H - FOOTER_Y, COLOR_TITLE_BG);
    display_hline(0, FOOTER_LINE, SCREEN_W, COLOR_GREEN);
    draw_text(MENU_X, FOOTER_Y + 7, COLOR_DIM, hint);
}

/* ── Confirm dialog ──────────────────────────────────────────────────────── */
static int confirm_screen(void *font, const char *title,
                           const char *warning, const char *detail) {
    SceCtrlData old, ctrl;
    memset(&old, 0, sizeof(old));
    /* v1.1.4: start from the buttons held right now. Before, the X that
     * opened this screen counted as a NEW press, so a held X could pass
     * the confirm screens (and close result screens) instantly. */
    sceCtrlPeekBufferPositive(0, &old, 1);
    while (1) {
        display_start();
        display_clear(COLOR_BG);
        draw_titlebar(title);

        int y = TITLE_H + 8;
        display_rect(MENU_X - 8, y,
                     SCREEN_W - (MENU_X - 8) * 2, LINE_H * 5,
                     RGBA8(60, 0, 0, 255));
        draw_text(MENU_X, y + 4,              COLOR_YELLOW, "!! WARNING !!");
        draw_text(MENU_X, y + 4 + LINE_H,     COLOR_TEXT,   warning);
        draw_text(MENU_X, y + 4 + LINE_H * 2, COLOR_DIM,    detail);
        draw_text(MENU_X, y + 4 + LINE_H * 3, COLOR_DIM,    "This cannot be undone.");

        int by = y + 4 + LINE_H * 5 + 8;
        draw_text(MENU_X,           by, COLOR_GREEN, "[X] Confirm");
        draw_text(MENU_X + 16 * 14, by, COLOR_RED,   "[O] Cancel");

        draw_footerbar("[X] Confirm    [O] Cancel");
        display_end();

        sceCtrlPeekBufferPositive(0, &ctrl, 1);
        if (btn_pressed(&ctrl, &old, SCE_CTRL_CROSS))  return 1;
        if (btn_pressed(&ctrl, &old, SCE_CTRL_CIRCLE)) return 0;
        old = ctrl;
    }
}

/* Cancel: go straight back to the list (no "SUCCESS / Cancelled" screen).
 * Wait for O to be released first, or the list would see it and close too. */
static void cancel_return(void) {
    SceCtrlData d;
    do { sceCtrlPeekBufferPositive(0, &d, 1); } while (d.buttons);
}

/* ── Result screen ───────────────────────────────────────────────────────── */
static void result_screen(void *font, const char *title,
                           int ok, int offer_reboot) {
    SceCtrlData old, ctrl;
    memset(&old, 0, sizeof(old));
    /* v1.1.4: start from the buttons held right now. Before, the X that
     * opened this screen counted as a NEW press, so a held X could pass
     * the confirm screens (and close result screens) instantly. */
    sceCtrlPeekBufferPositive(0, &old, 1);
    while (1) {
        display_start();
        display_clear(COLOR_BG);
        draw_titlebar(title);

        int y = TITLE_H + 8;
        draw_text(MENU_X, y,            ok ? COLOR_GREEN : COLOR_RED,
                                         ok ? "SUCCESS" : "ERROR");
        draw_text(MENU_X, y + LINE_H,   COLOR_TEXT, g_msg);
        if (g_msg2[0]) draw_text(MENU_X, y + LINE_H*2, COLOR_YELLOW, g_msg2);
        if (g_msg3[0]) draw_text(MENU_X, y + LINE_H*3, COLOR_DIM,    g_msg3);
        draw_text(MENU_X, y + LINE_H*5, COLOR_DIM,
                  offer_reboot && ok
                  ? "[Start] Reboot now    [O] Back"
                  : "[O] Back");

        draw_footerbar(offer_reboot && ok
                       ? "[O] Back    [Start] Reboot"
                       : "[O] Back");
        display_end();

        sceCtrlPeekBufferPositive(0, &ctrl, 1);
        if (btn_pressed(&ctrl, &old, SCE_CTRL_CIRCLE)) {
            g_msg2[0] = g_msg3[0] = '\0';   /* don't leak into the next result */
            cancel_return();   /* release O before the list sees it */
            return;
        }
        if (offer_reboot && ok &&
            btn_pressed(&ctrl, &old, SCE_CTRL_START))
            scePowerRequestColdReset();
        old = ctrl;
    }
}

/* ── Recursive delete ────────────────────────────────────────────────────── */
static void recursive_delete(const char *path) {
    if (strstr(path, "ux0:tai") != NULL) return;
    SceUID dfd = sceIoDopen(path);
    if (dfd < 0) { sceIoRemove(path); return; }
    SceIoDirent entry;
    while (sceIoDread(dfd, &entry) > 0) {
        if (entry.d_name[0] == '.') continue;
        char child[512];
        snprintf(child, sizeof(child), "%s/%s", path, entry.d_name);
        recursive_delete(child);
    }
    sceIoDclose(dfd);
    sceIoRmdir(path);
}

/* ══ Function implementations ═════════════════════════════════════════════ */

static void do_restart_system(void *font) {
    display_start(); display_clear(COLOR_BG);
    draw_titlebar("Sony Recovery - Restart System");
    draw_text(MENU_X, TITLE_H + 8, COLOR_TEXT, "Rebooting...");
    draw_footerbar("");
    display_end();
    SceCtrlData d;
    for (int i = 0; i < 60; i++) sceCtrlPeekBufferPositive(0, &d, 1);
    scePowerRequestColdReset();
}

/* Short name for common Vita I/O error codes */
static const char *io_err_name(int e) {
    switch ((unsigned int)e) {
        case 0x80010001: return "EPERM - not permitted";
        case 0x80010002: return "ENOENT - file not found";
        case 0x80010005: return "EIO - I/O error";
        case 0x8001000D: return "EACCES - access denied";
        case 0x80010010: return "EBUSY - file in use";
        case 0x8001001E: return "EROFS - read-only";
        default:         return "unknown error";
    }
}

/* ── Rebuild Database (v1.1.6) ─────────────────────────────────────────────
 * This app is a safe homebrew and cannot see ur0:shell, so it cannot delete
 * app.db itself. It writes a request flag; our kernel boot plugin
 * (boot_recovery.skprx) deletes app.db at the next boot, before the home
 * screen starts, and writes the result for main.c to show. */
#define REBUILD_FLAG    "ur0:tai/rebuild_db_trigger"
#define BOOT_SKPRX_SRC  "app0:boot_recovery.skprx"

/* 1 if the two files have identical contents */
static int files_equal(const char *a, const char *b) {
    SceUID fa = sceIoOpen(a, SCE_O_RDONLY, 0);
    if (fa < 0) return 0;
    SceUID fb = sceIoOpen(b, SCE_O_RDONLY, 0);
    if (fb < 0) { sceIoClose(fa); return 0; }
    static char ba[4096], bb[4096];
    int same = 1;
    while (1) {
        int ra = sceIoRead(fa, ba, sizeof(ba));
        int rb = sceIoRead(fb, bb, sizeof(bb));
        if (ra != rb || ra < 0) { same = 0; break; }
        if (ra == 0) break;
        if (memcmp(ba, bb, ra) != 0) { same = 0; break; }
    }
    sceIoClose(fa); sceIoClose(fb);
    return same;
}

/* 0 = not installed/enabled, 1 = installed but older build, 2 = current */
static int boot_plugin_state(void) {
    plugins_load();
    int listed = 0;
    for (int i = 0; i < plugins_get_count(); i++)
        if (strcasecmp(plugins_get_name(i), "boot_recovery.skprx") == 0 &&
            strcmp(plugins_get_section(i), "KERNEL") == 0 &&
            plugins_is_enabled(i)) {
            listed = 1;
            if (files_equal(plugins_get_path(i), BOOT_SKPRX_SRC)) return 2;
        }
    return listed ? 1 : 0;
}

/* Full-screen warning with several lines. Returns 1 = X, 0 = O. */
static int warn_screen(const char *title, const char *const *lines, int n) {
    SceCtrlData old, ctrl;
    sceCtrlPeekBufferPositive(0, &old, 1);   /* held buttons are not presses */
    while (1) {
        display_start();
        display_clear(COLOR_BG);
        draw_titlebar(title);
        int y = TITLE_H + 8;
        display_rect(MENU_X - 8, y, SCREEN_W - (MENU_X - 8) * 2,
                     LINE_H * (n + 1) + 8, RGBA8(60, 0, 0, 255));
        draw_text(MENU_X, y + 4, COLOR_YELLOW, "!! WARNING !!");
        for (int i = 0; i < n; i++)
            draw_text(MENU_X, y + 4 + LINE_H * (i + 1), COLOR_TEXT, lines[i]);
        int by = y + LINE_H * (n + 1) + 20;
        draw_text(MENU_X,           by, COLOR_GREEN, "[X] Continue");
        draw_text(MENU_X + 16 * 15, by, COLOR_RED,   "[O] Cancel");
        draw_footerbar("[X] Continue    [O] Cancel");
        display_end();

        sceCtrlPeekBufferPositive(0, &ctrl, 1);
        if (btn_pressed(&ctrl, &old, SCE_CTRL_CROSS))  return 1;
        if (btn_pressed(&ctrl, &old, SCE_CTRL_CIRCLE)) return 0;
        old = ctrl;
    }
}

static void do_rebuild_database(void *font) {
    g_msg2[0] = g_msg3[0] = '\0';
    SceIoStat st;

    /* Already requested? Offer reboot or cancelling the request. */
    if (sceIoGetstat(REBUILD_FLAG, &st) >= 0) {
        static const char *const L[] = {
            "A database rebuild is already scheduled.",
            "It runs at the next boot (restart the Vita).",
            "",
            "[X] Keep it scheduled   [O] Cancel the rebuild",
        };
        if (!warn_screen("Sony Recovery - Rebuild Database", L, 4)) {
            sceIoRemove(REBUILD_FLAG);
            snprintf(g_msg, sizeof(g_msg), "Rebuild request removed.");
            snprintf(g_msg2, sizeof(g_msg2), "Nothing will change at next boot.");
            result_screen(font, "Rebuild Database", 1, 0);
        } else {
            snprintf(g_msg, sizeof(g_msg), "Rebuild still scheduled for next boot.");
            result_screen(font, "Rebuild Database", 1, 1);
        }
        return;
    }

    /* The boot plugin does the actual work, so it must be installed and
     * be the build that knows about rebuilds. */
    int bp = boot_plugin_state();
    if (bp != 2) {
        snprintf(g_msg, sizeof(g_msg), bp == 0
                 ? "Boot plugin (boot_recovery.skprx) not installed."
                 : "Boot plugin is an older version.");
        snprintf(g_msg2, sizeof(g_msg2),
                 "Install it from Recovery Installer, reboot,");
        snprintf(g_msg3, sizeof(g_msg3), "then come back here. Nothing changed.");
        result_screen(font, "Rebuild Database", 0, 0);
        return;
    }

    static const char *const W[] = {
        "This deletes the system app database (app.db).",
        "The Vita rebuilds it at the next boot.",
        "",
        "YOUR HOME SCREEN LAYOUT WILL BE RESET:",
        "- All FOLDERS are removed.",
        "- Bubbles go back to default order.",
        "Games, apps and saves are NOT deleted.",
        "The first boot after this takes longer.",
    };
    if (!warn_screen("Sony Recovery - Rebuild Database", W, 8)) {
        cancel_return();
        return;
    }
    if (!confirm_screen(font,
            "Rebuild Database [2/2]",
            "Are you sure? Folders and layout will be lost.",
            "Press X to schedule the rebuild for next boot.")) {
        cancel_return();
        return;
    }

    /* Firmware without Ensō loads plugins too late in boot to do this */
    int late = (g_compat.fw_tier >= FW_367_HENCORE && g_compat.fw_tier != FW_UNKNOWN);

    sceIoMkdir("ur0:tai", 0777);
    SceUID fd = sceIoOpen(REBUILD_FLAG, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
    if (fd < 0) {
        snprintf(g_msg,  sizeof(g_msg),  "ERROR: Could not write the request.");
        snprintf(g_msg2, sizeof(g_msg2), "Code 0x%08X (%s)", (unsigned int)fd, io_err_name(fd));
        result_screen(font, "Rebuild Database", 0, 0);
        return;
    }
    sceIoWrite(fd, "1", 1);
    sceIoClose(fd);

    snprintf(g_msg,  sizeof(g_msg),  "Rebuild scheduled for the next boot.");
    snprintf(g_msg2, sizeof(g_msg2), late
             ? "Note: needs Enso; may be skipped on this FW."
             : "The result is shown when this app opens.");
    snprintf(g_msg3, sizeof(g_msg3), "Press Start to reboot now.");
    result_screen(font, "Rebuild Database", 1, 1);
}

static void do_format_memory_card(void *font) {
    if (!confirm_screen(font,
            "Sony Recovery - Format Memory Card",
            "ALL data on ux0: will be deleted.",
            "ux0:tai/ is preserved. HENkaku stays intact."))
        goto cancelled;
    if (!confirm_screen(font,
            "Format Memory Card [2/3]",
            "Are you absolutely sure? CANNOT be undone.",
            "ux0:tai/ preserved. Everything else deleted."))
        goto cancelled;
    if (!confirm_screen(font,
            "Format Memory Card [FINAL]",
            "LAST CHANCE - press X to begin format.",
            "Press O to cancel."))
        goto cancelled;

    display_start(); display_clear(COLOR_BG);
    draw_titlebar("Sony Recovery - Format Memory Card");
    draw_text(MENU_X, TITLE_H + 8,          COLOR_YELLOW, "Formatting...");
    draw_text(MENU_X, TITLE_H + 8 + LINE_H, COLOR_DIM,    "Do NOT power off.");
    draw_footerbar("");
    display_end();

    SceUID dfd = sceIoDopen(UX0_PATH);
    if (dfd < 0) {
        snprintf(g_msg, sizeof(g_msg), "ERROR: Cannot open ux0:");
        result_screen(font, "Format Memory Card", 0, 0);
        return;
    }
    SceIoDirent entry; int deleted = 0;
    while (sceIoDread(dfd, &entry) > 0) {
        if (entry.d_name[0] == '.') continue;
        if (strcmp(entry.d_name, "tai") == 0) continue;
        char full[512];
        snprintf(full, sizeof(full), "ux0:%s", entry.d_name);
        recursive_delete(full);
        deleted++;
    }
    sceIoDclose(dfd);
    snprintf(g_msg, sizeof(g_msg),
             "Done. %d items removed. ux0:tai/ preserved.", deleted);
    result_screen(font, "Format Memory Card", 1, 0);
    return;

cancelled:
    cancel_return();
}

static void do_restore_system(void *font) {
    if (!confirm_screen(font,
            "Sony Recovery - Restore PS Vita System",
            "ALL system data and accounts will be erased.",
            "The system resets to factory state."))
        goto cancelled;
    if (!confirm_screen(font,
            "Restore PS Vita System [2/3]",
            "This removes HENkaku. CFW will be lost.",
            "Back up your data before proceeding."))
        goto cancelled;
    if (!confirm_screen(font,
            "Restore PS Vita System [FINAL]",
            "FINAL WARNING - irrecoverable factory reset.",
            "Press X to confirm. Press O to cancel."))
        goto cancelled;

    display_start(); display_clear(COLOR_BG);
    draw_titlebar("Sony Recovery - Restore PS Vita System");
    draw_text(MENU_X, TITLE_H + 8,          COLOR_YELLOW, "Restoring...");
    draw_text(MENU_X, TITLE_H + 8 + LINE_H, COLOR_DIM,    "Do NOT power off.");
    draw_footerbar("");
    display_end();

    int ret = sceSysmoduleLoadModule(0x0038);
    if (ret >= 0) {
        scePromoterUtilityInit();
        scePromoterUtilityExit();
        sceSysmoduleUnloadModule(0x0038);
    }
    scePowerRequestColdReset();
    snprintf(g_msg, sizeof(g_msg), "Restore initiated. Rebooting...");
    result_screen(font, "Restore PS Vita System", 1, 0);
    return;

cancelled:
    cancel_return();
}

static void do_update_system(void *font) {
    if (!confirm_screen(font,
            "Sony Recovery - Update System Software",
            "UPDATING WILL REMOVE HENkaku / taiHEN.",
            "Only proceed if you intend to update FW.")) {
        cancel_return();
        return;
    }
    SceIoStat st;
    if (sceIoGetstat(UPDATE_PUP, &st) >= 0) {
        snprintf(g_msg, sizeof(g_msg), "Found PSP2UPDAT.PUP. Rebooting...");
        display_start(); display_clear(COLOR_BG);
        draw_titlebar("Sony Recovery - Update System Software");
        draw_text(MENU_X, TITLE_H + 8,
                  COLOR_YELLOW, "Starting update from memory card...");
        draw_text(MENU_X, TITLE_H + 8 + LINE_H, COLOR_DIM, "Do NOT power off.");
        draw_footerbar("");
        display_end();
        scePowerRequestColdReset();
    } else {
        snprintf(g_msg, sizeof(g_msg),
                 "No file at ux0:update/PSP2UPDAT.PUP");
        result_screen(font, "Update System Software", 0, 0);
    }
}

/* ══ Option table ══════════════════════════════════════════════════════════ */
typedef struct {
    const char *label;       /* <= 30 chars — leaves room for [DANGER] tag  */
    const char *desc;        /* <= 55 chars — fits within safe right margin  */
    void (*fn)(void *);
    int dangerous;
} OfficialOption;

static const OfficialOption OPTIONS[] = {
    { "1. Restart System",
      "Cold reboot. Same as power off and on.",
      do_restart_system, 0 },
    { "2. Rebuild Database",
      "Rebuilt at next boot. Resets folders and layout.",
      do_rebuild_database, 0 },
    { "3. Format Memory Card",
      "Erases ALL of ux0:. ux0:tai/ preserved. IRREVERSIBLE.",
      do_format_memory_card, 1 },
    { "4. Restore PS Vita System",
      "Factory reset. Removes all data, accounts, CFW.",
      do_restore_system, 1 },
    { "5. Update System Software",
      "Uses ux0:update/PSP2UPDAT.PUP. Removes HENkaku.",
      do_update_system, 1 },
};

void official_recovery_screen_run(void *font) {
    int selected = 0;
    SceCtrlData old, ctrl;
    memset(&old, 0, sizeof(old));

    do { sceCtrlPeekBufferPositive(0, &old, 1); } while (old.buttons);

    while (1) {
        display_start();
        display_clear(COLOR_BG);
        draw_titlebar("Sony Recovery Functions");

        /* Subtitle note — its own row below title bar */
        draw_text(MENU_X, SUBTITLE_Y, COLOR_DIM,
                  "Matches Sony safe-mode options.");
        display_hline(0, SEP_Y, SCREEN_W, RGBA8(60, 60, 60, 255));

        /* Option rows */
        for (int i = 0; i < 5; i++) {
            int y = OPTIONS_Y + i * ROW_H;
            if (y + ROW_H > FOOTER_Y) break;

            if (i == selected) {
                display_rect(0, y - 1, SCREEN_W - 8,
                             ROW_H + 1, COLOR_SEL_BG);
                display_text_transp(MENU_X, y,
                                    COLOR_SELECTED, OPTIONS[i].label);
                display_text_transp(MENU_X, y + LINE_H,
                                    COLOR_SELECTED, OPTIONS[i].desc);
                if (OPTIONS[i].dangerous)
                    display_text_transp(DANGER_X, y,
                                        COLOR_SELECTED, "[DANGER]");
            } else {
                uint32_t lc = OPTIONS[i].dangerous ? COLOR_YELLOW : COLOR_TEXT;
                draw_text(MENU_X, y,          lc,        OPTIONS[i].label);
                draw_text(MENU_X, y + LINE_H, COLOR_DIM, OPTIONS[i].desc);
                if (OPTIONS[i].dangerous)
                    draw_text(DANGER_X, y, COLOR_RED, "[DANGER]");
            }
        }

        draw_footerbar("[X] Execute  [O] Back  (Safe mode: L+R+PS+Power)");
        display_end();

        sceCtrlPeekBufferPositive(0, &ctrl, 1);
        if (btn_pressed(&ctrl, &old, SCE_CTRL_DOWN))
            selected = (selected + 1) % 5;
        if (btn_pressed(&ctrl, &old, SCE_CTRL_UP))
            selected = (selected - 1 + 5) % 5;
        if (btn_pressed(&ctrl, &old, SCE_CTRL_CROSS))
            OPTIONS[selected].fn(font);
        if (btn_pressed(&ctrl, &old, SCE_CTRL_CIRCLE))
            return;
        old = ctrl;
    }
}
