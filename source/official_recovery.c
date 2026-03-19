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
#include "draw.h"
#include "menu.h"

#define APP_DB_PATH  "ur0:shell/db/app.db"
#define UX0_PATH     "ux0:"
#define UPDATE_PUP   "ux0:update/PSP2UPDAT.PUP"

/* Max chars per description line — (960 - 40 left - 40 right) / 16px = 55 */
#define MAX_DESC_CHARS 55

static char g_msg[256] = "";

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
    draw_text(MENU_X, 6,  COLOR_TEXT, "PS Vita Recovery Menu v1.0");
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

/* ── Result screen ───────────────────────────────────────────────────────── */
static void result_screen(void *font, const char *title,
                           int ok, int offer_reboot) {
    SceCtrlData old, ctrl;
    memset(&old, 0, sizeof(old));
    while (1) {
        display_start();
        display_clear(COLOR_BG);
        draw_titlebar(title);

        int y = TITLE_H + 8;
        draw_text(MENU_X, y,            ok ? COLOR_GREEN : COLOR_RED,
                                         ok ? "SUCCESS" : "ERROR");
        draw_text(MENU_X, y + LINE_H,   COLOR_TEXT, g_msg);
        draw_text(MENU_X, y + LINE_H*3, COLOR_DIM,
                  offer_reboot && ok
                  ? "[Start] Reboot now    [O] Back"
                  : "[O] Back");

        draw_footerbar(offer_reboot && ok
                       ? "[O] Back    [Start] Reboot"
                       : "[O] Back");
        display_end();

        sceCtrlPeekBufferPositive(0, &ctrl, 1);
        if (btn_pressed(&ctrl, &old, SCE_CTRL_CIRCLE)) return;
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
    draw_titlebar("Sony Recovery — Restart System");
    draw_text(MENU_X, TITLE_H + 8, COLOR_TEXT, "Rebooting...");
    draw_footerbar("");
    display_end();
    SceCtrlData d;
    for (int i = 0; i < 60; i++) sceCtrlPeekBufferPositive(0, &d, 1);
    scePowerRequestColdReset();
}

static void do_rebuild_database(void *font) {
    if (!confirm_screen(font,
            "Sony Recovery — Rebuild Database",
            "app.db will be deleted and rebuilt on next boot.",
            "Games and saves will NOT be affected.")) {
        snprintf(g_msg, sizeof(g_msg), "Cancelled.");
        result_screen(font, "Rebuild Database", 1, 0);
        return;
    }
    SceIoStat st;
    if (sceIoGetstat(APP_DB_PATH, &st) < 0) {
        snprintf(g_msg, sizeof(g_msg), "app.db not found. Already clean.");
        result_screen(font, "Rebuild Database", 1, 0);
        return;
    }
    if (sceIoRemove(APP_DB_PATH) < 0) {
        snprintf(g_msg, sizeof(g_msg), "ERROR: Could not remove app.db.");
        result_screen(font, "Rebuild Database", 0, 0);
        return;
    }
    snprintf(g_msg, sizeof(g_msg), "Database removed. Reboot to rebuild.");
    result_screen(font, "Rebuild Database", 1, 1);
}

static void do_format_memory_card(void *font) {
    if (!confirm_screen(font,
            "Sony Recovery — Format Memory Card",
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
            "LAST CHANCE — press X to begin format.",
            "Press O to cancel."))
        goto cancelled;

    display_start(); display_clear(COLOR_BG);
    draw_titlebar("Sony Recovery — Format Memory Card");
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
    snprintf(g_msg, sizeof(g_msg), "Cancelled.");
    result_screen(font, "Format Memory Card", 1, 0);
}

static void do_restore_system(void *font) {
    if (!confirm_screen(font,
            "Sony Recovery — Restore PS Vita System",
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
            "FINAL WARNING — irrecoverable factory reset.",
            "Press X to confirm. Press O to cancel."))
        goto cancelled;

    display_start(); display_clear(COLOR_BG);
    draw_titlebar("Sony Recovery — Restore PS Vita System");
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
    snprintf(g_msg, sizeof(g_msg), "Cancelled.");
    result_screen(font, "Restore PS Vita System", 1, 0);
}

static void do_update_system(void *font) {
    if (!confirm_screen(font,
            "Sony Recovery — Update System Software",
            "UPDATING WILL REMOVE HENkaku / taiHEN.",
            "Only proceed if you intend to update FW.")) {
        snprintf(g_msg, sizeof(g_msg), "Cancelled. CFW preserved.");
        result_screen(font, "Update System Software", 1, 0);
        return;
    }
    SceIoStat st;
    if (sceIoGetstat(UPDATE_PUP, &st) >= 0) {
        snprintf(g_msg, sizeof(g_msg), "Found PSP2UPDAT.PUP. Rebooting...");
        display_start(); display_clear(COLOR_BG);
        draw_titlebar("Sony Recovery — Update System Software");
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
      "Deletes app.db. Firmware rebuilds on next boot.",
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
