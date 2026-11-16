#include "debugScreen.h"
#include "display.h"
#include "rain.h"
#include <psp2/display.h>
#include <psp2/power.h>
#include <psp2/kernel/processmgr.h>
#include "compat.h"
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

/* Classic colours (the original look). theme.c replaces these at runtime. */
ThemeColors g_tc = {
    .text     = RGBA8(255,255,255,255),
    .selected = RGBA8(255,255,255,255),
    .sel_bg   = RGBA8(  0,180,  0,255),
    .dim      = RGBA8(160,160,160,255),
    .green    = RGBA8( 80,255, 80,255),
    .red      = RGBA8(255, 80, 80,255),
    .yellow   = RGBA8(255,220,  0,255),
    .title_bg = RGBA8(  0, 60,  0,255),
    .title    = RGBA8(255,255,255,255),
    .header   = RGBA8(200,200,200,255),
    .line     = RGBA8( 80,255, 80,255),
};
int g_theme_retro = 0;

void display_init(void)  { psvDebugScreenInit(); }
void display_start(void) {}
void display_end(void)   { psvDebugScreenFlip(); }

void display_clear(uint32_t color) {
    psvDebugScreenClear(color);
    rain_draw();          /* v1.2.2: Matrix rain (retro + rain on only) */
}

void display_pixel(int x, int y, uint32_t color) {
    psvDebugScreenFillRect(x, y, 1, 1, color);
}
/* Retro style has no panels or bars: dark fills (title/footer bars,
 * warning boxes, selection bars) become black, thin dark strips such as
 * scroll bar tracks become a faint line in the theme colour. Bright fills
 * (progress bars, scroll thumbs) are kept. */
static uint32_t retro_fill(int w, int h, uint32_t c) {
    if (c == 0) return 0;
    unsigned r = c & 0xFF, g = (c >> 8) & 0xFF, b = (c >> 16) & 0xFF;
    unsigned mx = r > g ? (r > b ? r : b) : (g > b ? g : b);
    if (mx >= 100) return c;
    return (w <= 6 || h <= 3) ? g_tc.line : 0;
}

void display_rect(int x, int y, int w, int h, uint32_t color) {
    if (g_theme_retro) color = retro_fill(w, h, color);
    if (color == 0 && rain_is_on()) return;    /* keep the rain behind panels */
    psvDebugScreenFillRect(x, y, w, h, color);
}
void display_hline(int x, int y, int w, uint32_t color) {
    if (g_theme_retro) color = g_tc.line;   /* all separators: faint accent */
    psvDebugScreenFillRect(x, y, w, 1, color);
}

/* Opaque text — draws fg + bg (bg = COLOR_BG by default) */
/* While the rain is on, every string gets a black backing so the rain
 * passes behind words, never through them. */
static void rain_backing(int x, int y, const char *str) {
    if (rain_is_on() && str && *str)
        psvDebugScreenFillRect(x - 3, y - 2, (int)strlen(str) * 16 + 6, 20, 0);
}

void display_text(int x, int y, uint32_t color, const char *str) {
    rain_backing(x, y, str);
    psvDebugScreenSetFgColor(color);
    psvDebugScreenSetBgColor(COLOR_BG);
    psvDebugScreenSetCoordsXY(x, y);
    psvDebugScreenPrintf("%s", str);
}

/* Transparent text — fg only, leaves whatever is behind (use over colored bars) */
void display_text_transp(int x, int y, uint32_t color, const char *str) {
    rain_backing(x, y, str);
    psvDebugScreenSetFgColor(color);
    psvDebugScreenSetBgColor(PSV_TRANSPARENT);
    psvDebugScreenSetCoordsXY(x, y);
    psvDebugScreenPrintf("%s", str);
}

void display_char(int x, int y, uint32_t color, char c) {
    char one[2] = { c, 0 };
    rain_backing(x, y, one);
    psvDebugScreenSetFgColor(color);
    psvDebugScreenSetBgColor(COLOR_BG);
    psvDebugScreenSetCoordsXY(x, y);
    psvDebugScreenPrintf("%c", c);
}

void display_textf(int x, int y, uint32_t color, const char *fmt, ...) {
    char buf[512];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    display_text(x, y, color, buf);
}

int display_text_width(const char *str) {
    return (int)strlen(str) * 16;
}

/* ── Battery (v1.2.3) ─────────────────────────────────────────────────────
 * Icon + percentage at the right of the title bar, under "By DrinkingSubset".
 * Read from the system every 2 seconds (screens redraw 60 times a second;
 * the battery does not change that fast). Hidden on PS TV (no battery). */
#define BAT_POLL_US  2000000ULL
#define BAT_Y        26                 /* second title row, like subtitles */
#define BAT_W        30                 /* body, without the nub            */
#define BAT_H        14

static int        g_bat_pct  = -1;
static int        g_bat_chg  = 0;
static SceUInt64  g_bat_time = 0;

static void battery_poll(void) {
    SceUInt64 now = sceKernelGetProcessTimeWide();
    if (g_bat_pct >= 0 && now - g_bat_time < BAT_POLL_US) return;
    g_bat_time = now;
    int p = scePowerGetBatteryLifePercent();
    g_bat_pct = (p < 0) ? -1 : (p > 100 ? 100 : p);
    g_bat_chg = scePowerIsBatteryCharging() ? 1 : 0;
}

/* Fill without the Retro "dark fill -> black" rule (display_rect). */
static void bat_rect(int x, int y, int w, int h, uint32_t c) {
    psvDebugScreenFillRect(x, y, w, h, c);
}

static void display_battery(void) {
    if (g_compat.model == MODEL_PSTV) return;
    battery_poll();
    if (g_bat_pct < 0) return;

    char txt[8];
    snprintf(txt, sizeof(txt), "%d%%", g_bat_pct);
    int right = SCREEN_W - MENU_X;                  /* lines up with the credit */
    int bx = right - BAT_W;                         /* body; 3 px nub before it */
    int by = BAT_Y + 1;
    int tx = bx - 3 - 8 - display_text_width(txt);

    /* Low = red, getting low = yellow (all themes); else green, or the
     * theme colour in Retro */
    uint32_t fill = g_bat_pct <= 10 ? COLOR_RED
                  : g_bat_pct <= 20 ? COLOR_YELLOW
                  : g_theme_retro   ? COLOR_TEXT : COLOR_GREEN;   /* Retro: theme colour */
    uint32_t edge = COLOR_TEXT;

    if (rain_is_on()) bat_rect(tx - 3, BAT_Y - 2, right - tx + 6, 20, 0);
    display_text(tx, BAT_Y, g_bat_pct <= 20 && !g_bat_chg ? fill : COLOR_TEXT, txt);

    /* outline 2 px, nub on the LEFT, and the level filled from the RIGHT
     * (like the PS Vita's own status bar: it empties toward the nub) */
    bat_rect(bx, by, BAT_W, 2, edge);
    bat_rect(bx, by + BAT_H - 2, BAT_W, 2, edge);
    bat_rect(bx, by, 2, BAT_H, edge);
    bat_rect(bx + BAT_W - 2, by, 2, BAT_H, edge);
    bat_rect(bx - 3, by + 4, 3, BAT_H - 8, edge);
    bat_rect(bx + 3, by + 3, BAT_W - 6, BAT_H - 6, 0);          /* empty inside */
    int lvl = (BAT_W - 6) * g_bat_pct / 100;
    if (lvl < 1 && g_bat_pct > 0) lvl = 1;
    bat_rect(bx + BAT_W - 3 - lvl, by + 3, lvl, BAT_H - 6, fill);

    if (g_bat_chg) {
        /* lightning bolt in the middle of the battery */
        static const char *const BOLT[] = {
            "...##", "..##.", ".###.", "#####", ".###.", ".##..", "##...",
        };
        int ox = bx + BAT_W / 2 - 5, oy = by + 0;
        for (int r = 0; r < 7; r++)
            for (int k = 0; k < 5; k++)
                if (BOLT[r][k] == '#') {
                    bat_rect(ox + k * 2, oy + r * 2, 2, 2, COLOR_YELLOW);
                }
    }
}

void display_app_title(int y) {
    display_text(MENU_X, y, COLOR_TEXT, APP_TITLE);
    int cx = SCREEN_W - MENU_X - display_text_width(APP_CREDIT);
    display_text(cx, y, COLOR_TEXT, APP_CREDIT);
    display_battery();      /* v1.2.3 */
}
