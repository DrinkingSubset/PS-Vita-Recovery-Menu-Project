/*
 * sysinfo.c — System Information Screen
 *
 * Called every frame from menu_run(). scroll_y shifts all content upward.
 * Scroll is managed by menu.c (Up/Down d-pad), clamped to SYSINFO_MAX_SCROLL.
 */

#include <psp2/power.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/modulemgr.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "sysinfo.h"
#include "compat.h"
#include "draw.h"
#include "menu.h"

/* ── Content area ────────────────────────────────────────────────────── */
#define CONTENT_TOP  (TITLE_H + 8)
#define CONTENT_BOT  FOOTER_Y          /* 514 */
#define CONTENT_H    (CONTENT_BOT - CONTENT_TOP)   /* 460px */
#define SCROLL_STEP  LINE_H            /* 24px per d-pad press */

/* ── Layout macros (all use local `y` which is pre-offset) ───────────── */

#define ROW(label, val, col) do { \
    if (y >= CONTENT_TOP && y < CONTENT_BOT) { \
        draw_text(MENU_X, y, COLOR_DIM, label); \
        draw_text(VAL_X,  y, col,       val);   \
    } \
    y += LINE_H; \
} while(0)

#define ROWF(label, col, ...) do { \
    char _rb[128]; \
    snprintf(_rb, sizeof(_rb), __VA_ARGS__); \
    ROW(label, _rb, col); \
} while(0)

#define ROWTEXT(text, col) do { \
    if (y >= CONTENT_TOP && y < CONTENT_BOT) \
        draw_text(MENU_X, y, col, text); \
    y += LINE_H; \
} while(0)

#define ROWTEXTINDENT(text, col) do { \
    if (y >= CONTENT_TOP && y < CONTENT_BOT) \
        draw_text(MENU_X + 16, y, col, text); \
    y += LINE_H; \
} while(0)

#define DIVIDER() do { \
    y += 4; \
    if (y >= CONTENT_TOP && y < CONTENT_BOT) \
        display_hline(MENU_X, y, SCREEN_W - MENU_X * 2, RGBA8(60,60,60,255)); \
    y += 8; \
} while(0)

#define SECTION(label) do { \
    if (y >= CONTENT_TOP && y < CONTENT_BOT) \
        draw_text(MENU_X, y, COLOR_DIM, label); \
    y += LINE_H; \
} while(0)

/* ── Battery lifetime formatter ──────────────────────────────────────── */
static void fmt_lifetime(char *out, int out_sz) {
    int mins = scePowerGetBatteryLifeTime();
    if (mins < 0) { snprintf(out, out_sz, "--:--"); return; }
    snprintf(out, out_sz, "%02dh %02dm", mins / 60, mins % 60);
}

/* ── Main draw ───────────────────────────────────────────────────────── */
void sysinfo_draw(void *font, int scroll_y) {
    draw_init(font);
    display_clear(COLOR_BG);

    /* ── Title bar (never scrolls) ── */
    display_rect(0, 0, SCREEN_W, TITLE_LINE, COLOR_TITLE_BG);
    draw_text(MENU_X, 6,  COLOR_TEXT, "PS Vita Recovery Menu v1.0");
    draw_text(MENU_X, 26, COLOR_DIM,  "System Information");
    display_hline(0, TITLE_LINE, SCREEN_W, COLOR_GREEN);

    /* ── Clip region: mask above content top and below footer ── */
    /* We achieve clipping by only drawing rows whose y is within bounds.
     * The ROW/SECTION/DIVIDER macros all guard on (y >= CONTENT_TOP). */

    /* y starts at content top minus scroll offset */
    int y = CONTENT_TOP - scroll_y;

    /* ── SYSTEM ────────────────────────────────────────────────────── */

    ROW("Model:", compat_model_name(), COLOR_TEXT);

    /* Firmware — real from partition, flag if syscall differs */
    {
        char fwbuf[64];
        SceKernelFwInfo sp;
        sp.size = sizeof(sp);
        sceKernelGetSystemSwVersion(&sp);
        if (sp.versionString[0] &&
            strncmp(sp.versionString, g_compat.fw_string,
                    sizeof(g_compat.fw_string) - 1) != 0)
            snprintf(fwbuf, sizeof(fwbuf), "%s  (syscall: %s)",
                     g_compat.fw_display, sp.versionString);
        else
            snprintf(fwbuf, sizeof(fwbuf), "%s", g_compat.fw_display);
        ROW("Firmware:", fwbuf, COLOR_TEXT);
    }

    /* Exploit */
    {
        uint32_t col = COLOR_GREEN;
        if (g_compat.fw_tier == FW_367_HENCORE  ||
            g_compat.fw_tier == FW_368_HENCORE  ||
            g_compat.fw_tier == FW_373_HENCORE2 ||
            g_compat.fw_tier == FW_374_HENCORE2)
            col = COLOR_YELLOW;
        if (g_compat.fw_tier == FW_UNKNOWN)
            col = COLOR_RED;
        ROW("Exploit:", compat_fw_tier_name(), col);
    }

    /* Ensō */
    {
        const char *s;  uint32_t col;
        if (g_compat.has_enso) {
            s = "INSTALLED";               col = COLOR_GREEN;
        } else if (g_compat.enso_capable) {
            s = "Not installed";           col = COLOR_YELLOW;
        } else {
            s = "Not supported on this FW"; col = COLOR_DIM;
        }
        ROW("Enso:", s, col);
    }

    /* Motherboard */
    ROW("Motherboard:", g_compat.motherboard, COLOR_TEXT);

    DIVIDER();

    /* ── TAI CONFIG ────────────────────────────────────────────────── */

    ROWTEXT("Active tai config:", COLOR_DIM);
    ROWTEXTINDENT(g_compat.active_tai_config, COLOR_GREEN);

    if (g_compat.tai_config_count > 1) {
        ROWTEXT("WARNING: multiple tai configs detected:", COLOR_YELLOW);
        for (int i = 1; i < g_compat.tai_config_count; i++) {
            char line[80];
            snprintf(line, sizeof(line), "  %s", g_compat.tai_configs[i]);
            ROWTEXT(line, COLOR_DIM);
        }
    }

    if (g_compat.has_imc0)
        ROWTEXT("Note: imc0:tai/ loads before ur0:tai/ on 2000/PSTV",
                COLOR_YELLOW);

    DIVIDER();

    /* ── CLOCKS (live) ─────────────────────────────────────────────── */

    SECTION("-- Clocks --");
    ROWF("ARM CPU:", COLOR_TEXT, "%d MHz", scePowerGetArmClockFrequency());
    ROWF("Bus:",     COLOR_TEXT, "%d MHz", scePowerGetBusClockFrequency());
    {
        int gpu = scePowerGetGpuClockFrequency();
        if (gpu <= 0) gpu = scePowerGetGpuXbarClockFrequency();
        ROWF("GPU:", COLOR_TEXT, "%d MHz", gpu);
    }

    DIVIDER();

    /* ── BATTERY (live) ────────────────────────────────────────────── */

    SECTION("-- Battery --");
    {
        int pct  = scePowerGetBatteryLifePercent();
        int chg  = scePowerIsBatteryCharging();
        int mv   = scePowerGetBatteryVolt();
        int temp = scePowerGetBatteryTemp() / 10;
        ROWF("Battery:", pct > 20 ? COLOR_GREEN : COLOR_RED,
             "%d%%%s  %dmV  %dC", pct, chg ? " [CHG]" : "", mv, temp);
    }
    {
        char lt[16]; fmt_lifetime(lt, sizeof(lt));
        ROWF("Lifetime:", COLOR_TEXT, "%s remaining", lt);
    }
    ROWF("Cycle count:", COLOR_TEXT, "%d", scePowerGetBatteryCycleCount());
    {
        int soh = scePowerGetBatterySOH();
        uint32_t col = soh >= 80 ? COLOR_GREEN :
                       soh >= 50 ? COLOR_YELLOW : COLOR_RED;
        ROWF("Health:", col, "%d%%", soh);
    }

    DIVIDER();

    /* ── MEMORY (live) ─────────────────────────────────────────────── */

    SECTION("-- Memory --");
    {
        SceKernelFreeMemorySizeInfo mem;
        mem.size = sizeof(mem);
        sceKernelGetFreeMemorySize(&mem);
        ROWF("User RAM free:", COLOR_TEXT, "%d KB", mem.size_user  / 1024);
        ROWF("CDRAM free:",    COLOR_TEXT, "%d KB", mem.size_cdram / 1024);
    }

    /* No-Ensō warning box */
    if (g_compat.fw_tier == FW_367_HENCORE  ||
        g_compat.fw_tier == FW_368_HENCORE  ||
        g_compat.fw_tier == FW_373_HENCORE2 ||
        g_compat.fw_tier == FW_374_HENCORE2) {
        if (y >= CONTENT_TOP && y < CONTENT_BOT) {
            y += 4;
            display_rect(MENU_X - 6, y - 2,
                         SCREEN_W - (MENU_X - 6) * 2, LINE_H + 4,
                         RGBA8(60, 30, 0, 255));
            draw_text(MENU_X, y, COLOR_YELLOW,
                      "Re-run exploit after every reboot (no Enso).");
        }
    }

    /* ── Scroll bar ────────────────────────────────────────────────── */
    if (scroll_y > 0 || scroll_y < SYSINFO_MAX_SCROLL) {
        int total_h = CONTENT_H + SYSINFO_MAX_SCROLL;
        int bar_h   = CONTENT_H * CONTENT_H / total_h;
        if (bar_h < 16) bar_h = 16;
        int bar_y = CONTENT_TOP +
                    (scroll_y * (CONTENT_H - bar_h)) / SYSINFO_MAX_SCROLL;
        display_rect(SCREEN_W - 5, CONTENT_TOP, 3, CONTENT_H,
                     RGBA8(30, 30, 30, 255));
        display_rect(SCREEN_W - 5, bar_y, 3, bar_h, COLOR_GREEN);
    }

#undef ROW
#undef ROWF
#undef ROWTEXT
#undef ROWTEXTINDENT
#undef DIVIDER
#undef SECTION

    /* ── Footer (never scrolls) ── */
    display_rect(0, FOOTER_Y, SCREEN_W, SCREEN_H - FOOTER_Y, COLOR_TITLE_BG);
    display_hline(0, FOOTER_LINE, SCREEN_W, COLOR_GREEN);
    draw_text(MENU_X, FOOTER_Y + 7, COLOR_DIM,
              "[Up/Down] Scroll   [O] Back");
}
