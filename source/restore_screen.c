/*
 * restore_screen.c
 * Draws the restore/unbrick menu and handles its input loop.
 *
 * Layout (960x544, font=16px wide, LINE_H=24):
 *
 *  y=0               ┌──────────────────────────────────────┐
 *                    │  Dark green title bar                 │
 *  y=44 (TITLE_LINE) ├────────────── green line ────────────┤
 *  y=46              │  Row 1: WARNING text          (24px) │
 *  y=70              │  Row 2: [Backup: Found/None]  (24px) │
 *  y=94              ├────────────── dim separator ─────────┤
 *  y=98              │  Option 1  label + desc      (52px)  │
 *  y=150             │  Option 2  ...                        │
 *  y=202             │  Option 3  ...                        │
 *  y=254             │  Option 4  ...                        │
 *  y=306             │  Option 5  ...                        │
 *  y=358             │  (empty)                              │
 *  y=514 (FOOTER_Y)  ├────────────── green line ─────────────┤
 *                    │  Dark green footer                    │
 *  y=543             └──────────────────────────────────────┘
 *
 * Max safe text width: (960 - 40 - 40) / 16 = 55 chars per line.
 * All description strings are trimmed to <= 55 chars.
 */

#include <psp2/ctrl.h>
#include <psp2/power.h>
#include <string.h>
#include <stdio.h>
#include "restore_screen.h"
#include "restore.h"
#include "draw.h"
#include "menu.h"

/* ── Layout ──────────────────────────────────────────────────────────────── */
#define WARN_Y       TITLE_H               /* 46 — warning row 1 y           */
#define BAKSTAT_Y    (WARN_Y + LINE_H)     /* 70 — backup status row y       */
#define BANNER_H     (LINE_H * 2)          /* 48px — two-row red banner      */
#define SEP_Y        (WARN_Y + BANNER_H)   /* 94 — dim separator             */
#define OPTIONS_Y    (SEP_Y + 4)           /* 98 — first option row          */
#define ROW_H        (LINE_H * 2 + 4)      /* 52px — label + desc + gap      */
#define RIGHT_MARGIN (SCREEN_W - MENU_X)   /* 920 — descriptions stop here   */

/* ── Option list ─────────────────────────────────────────────────────────── */
typedef struct {
    const char *label;
    const char *desc;
    RestoreStatus (*fn)(void);
} RestoreOption;

static const RestoreOption OPTIONS[] = {
    { "1. Safe Mode Boot",
      "Disables ALL plugins so Vita can boot cleanly.",
      restore_safe_mode },
    { "2. Reset taiHEN Config",
      "Writes safe defaults to config.txt. Old saved as .bak",
      restore_reset_tai_config },
    { "3. Backup  ux0:tai/",
      "Copies tai/ folder to vita_recovery/tai_backup/",
      restore_backup_tai },
    { "4. Restore ux0:tai/",
      "Restores tai/ from tai_backup/. Backup required first.",
      restore_restore_tai },
    { "5. Rebuild LiveArea Database",
      "Deletes app.db so firmware rebuilds it on next reboot.",
      restore_rebuild_livearea },
};
static const int OPTION_COUNT = sizeof(OPTIONS) / sizeof(OPTIONS[0]);

/* ── Shared draw helpers ─────────────────────────────────────────────────── */
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
static int confirm_dialog(void *font, const char *action_name) {
    SceCtrlData old, ctrl;
    memset(&old, 0, sizeof(old));
    /* Drain buttons — the X press that opened this dialog must be released
     * before we start reading input, otherwise it fires instantly. */
    do { sceCtrlPeekBufferPositive(0, &old, 1); } while (old.buttons);
    while (1) {
        display_start();
        display_clear(COLOR_BG);
        draw_titlebar("Restore / Unbrick — Confirm");

        int y = TITLE_H + 12;
        display_rect(MENU_X - 8, y,
                     SCREEN_W - (MENU_X - 8) * 2, LINE_H * 5,
                     RGBA8(30, 30, 30, 255));
        draw_text(MENU_X, y + 4,              COLOR_YELLOW, "!! CONFIRM ACTION !!");
        draw_text(MENU_X, y + 4 + LINE_H,     COLOR_TEXT,   action_name);
        draw_text(MENU_X, y + 4 + LINE_H * 2, COLOR_DIM,    "This will modify system files.");
        draw_text(MENU_X, y + 4 + LINE_H * 3, COLOR_DIM,    "Make sure you have a backup.");

        int by = y + 4 + LINE_H * 5 + 8;
        draw_text(MENU_X,           by, COLOR_GREEN, "[X] Confirm");
        draw_text(MENU_X + 16 * 14, by, COLOR_RED,   "[O] Cancel");

        draw_footerbar("[X] Confirm    [O] Cancel");
        display_end();

        sceCtrlPeekBufferPositive(0, &ctrl, 1);
        if ((ctrl.buttons & SCE_CTRL_CROSS)  && !(old.buttons & SCE_CTRL_CROSS))  return 1;
        if ((ctrl.buttons & SCE_CTRL_CIRCLE) && !(old.buttons & SCE_CTRL_CIRCLE)) return 0;
        old = ctrl;
    }
}

/* ── Result screen ───────────────────────────────────────────────────────── */
static void result_screen(void *font, RestoreStatus status) {
    SceCtrlData old, ctrl;
    memset(&old, 0, sizeof(old));
    uint32_t    col  = (status == RESTORE_OK) ? COLOR_GREEN : COLOR_RED;
    const char *head = (status == RESTORE_OK) ? "SUCCESS" : "ERROR";

    while (1) {
        display_start();
        display_clear(COLOR_BG);
        draw_titlebar("Restore / Unbrick — Result");

        int y = TITLE_H + 8;
        draw_text(MENU_X, y,              col,        head);
        draw_text(MENU_X, y + LINE_H,     COLOR_TEXT, restore_get_status_msg());
        draw_text(MENU_X, y + LINE_H * 3, COLOR_DIM,
                  status == RESTORE_OK
                  ? "Tip: Reboot for changes to take effect."
                  : "Check ux0:data/vita_recovery/restore.log");

        draw_footerbar(status == RESTORE_OK
                       ? "[O] Back    [Start] Reboot now"
                       : "[O] Back");
        display_end();

        sceCtrlPeekBufferPositive(0, &ctrl, 1);
        if ((ctrl.buttons & SCE_CTRL_CIRCLE) && !(old.buttons & SCE_CTRL_CIRCLE)) return;
        if ((ctrl.buttons & SCE_CTRL_START)  && !(old.buttons & SCE_CTRL_START))
            scePowerRequestColdReset();
        old = ctrl;
    }
}

/* ── Main restore screen ─────────────────────────────────────────────────── */
void restore_screen_run(void *font) {
    draw_init(font);
    int selected = 0;
    SceCtrlData old, ctrl;
    memset(&old, 0, sizeof(old));

    do { sceCtrlPeekBufferPositive(0, &old, 1); } while (old.buttons);

    restore_ensure_dirs();
    int backup_exists = restore_backup_exists();

    while (1) {
        display_start();
        display_clear(COLOR_BG);

        /* ── Title bar ── */
        draw_titlebar("Restore / Unbrick");

        /* ── Two-row red banner — WARNING on row 1, backup status on row 2 ── */
        display_rect(0, WARN_Y, SCREEN_W, BANNER_H, RGBA8(80, 0, 0, 255));

        /* Row 1: warning message — full width, no competition */
        draw_text(MENU_X, WARN_Y + 4,
                  COLOR_YELLOW, "WARNING: These operations modify system files.");

        /* Row 2: backup status — its own dedicated line */
        if (backup_exists)
            draw_text(MENU_X, BAKSTAT_Y + 4, COLOR_GREEN,  "Backup: Found");
        else
            draw_text(MENU_X, BAKSTAT_Y + 4, COLOR_RED,    "Backup: None — backup before restoring!");

        /* Dim separator below banner */
        display_hline(0, SEP_Y, SCREEN_W, RGBA8(60, 60, 60, 255));

        /* ── Option rows ── */
        for (int i = 0; i < OPTION_COUNT; i++) {
            int y = OPTIONS_Y + i * ROW_H;
            if (y + ROW_H > FOOTER_Y) break;

            if (i == selected) {
                display_rect(0, y - 1, SCREEN_W - 8, ROW_H + 1, COLOR_SEL_BG);
                display_text_transp(MENU_X, y,          COLOR_SELECTED, OPTIONS[i].label);
                display_text_transp(MENU_X, y + LINE_H, COLOR_SELECTED, OPTIONS[i].desc);
            } else {
                draw_text(MENU_X, y,          COLOR_TEXT, OPTIONS[i].label);
                draw_text(MENU_X, y + LINE_H, COLOR_DIM,  OPTIONS[i].desc);
            }
        }

        /* ── Footer ── */
        draw_footerbar("[X] Execute   [O] Back   [Sq] Quick Backup tai/");
        display_end();

        /* ── Input ── */
        sceCtrlPeekBufferPositive(0, &ctrl, 1);

        if ((ctrl.buttons & SCE_CTRL_DOWN) && !(old.buttons & SCE_CTRL_DOWN))
            selected = (selected + 1) % OPTION_COUNT;
        if ((ctrl.buttons & SCE_CTRL_UP) && !(old.buttons & SCE_CTRL_UP))
            selected = (selected - 1 + OPTION_COUNT) % OPTION_COUNT;

        if ((ctrl.buttons & SCE_CTRL_CROSS) && !(old.buttons & SCE_CTRL_CROSS)) {
            if (confirm_dialog(font, OPTIONS[selected].label)) {
                RestoreStatus st = OPTIONS[selected].fn();
                backup_exists = restore_backup_exists();
                result_screen(font, st);
            }
            /* Drain: prevent lingering O/X from leaking into outer loop */
            do { sceCtrlPeekBufferPositive(0, &old, 1); } while (old.buttons);
        }
        if ((ctrl.buttons & SCE_CTRL_SQUARE) && !(old.buttons & SCE_CTRL_SQUARE)) {
            if (confirm_dialog(font, "Backup ux0:tai/")) {
                RestoreStatus st = restore_backup_tai();
                backup_exists = restore_backup_exists();
                result_screen(font, st);
            }
            /* Drain: prevent lingering O/X from leaking into outer loop */
            do { sceCtrlPeekBufferPositive(0, &old, 1); } while (old.buttons);
        }
        if ((ctrl.buttons & SCE_CTRL_CIRCLE) && !(old.buttons & SCE_CTRL_CIRCLE))
            return;

        old = ctrl;
    }
}
