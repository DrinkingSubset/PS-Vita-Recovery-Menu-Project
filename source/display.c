#include "debugScreen.h"
#include "display.h"
#include "rain.h"
#include <psp2/display.h>
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

void display_app_title(int y) {
    display_text(MENU_X, y, COLOR_TEXT, APP_TITLE);
    int cx = SCREEN_W - MENU_X - display_text_width(APP_CREDIT);
    display_text(cx, y, COLOR_TEXT, APP_CREDIT);
}
