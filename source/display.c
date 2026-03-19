#include "debugScreen.h"
#include "display.h"
#include <psp2/display.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

void display_init(void)  { psvDebugScreenInit(); }
void display_start(void) {}
void display_end(void)   { psvDebugScreenFlip(); }

void display_clear(uint32_t color) { psvDebugScreenClear(color); }

void display_pixel(int x, int y, uint32_t color) {
    psvDebugScreenFillRect(x, y, 1, 1, color);
}
void display_rect(int x, int y, int w, int h, uint32_t color) {
    psvDebugScreenFillRect(x, y, w, h, color);
}
void display_hline(int x, int y, int w, uint32_t color) {
    psvDebugScreenFillRect(x, y, w, 1, color);
}

/* Opaque text — draws fg + bg (bg = COLOR_BG by default) */
void display_text(int x, int y, uint32_t color, const char *str) {
    psvDebugScreenSetFgColor(color);
    psvDebugScreenSetBgColor(COLOR_BG);
    psvDebugScreenSetCoordsXY(x, y);
    psvDebugScreenPrintf("%s", str);
}

/* Transparent text — fg only, leaves whatever is behind (use over colored bars) */
void display_text_transp(int x, int y, uint32_t color, const char *str) {
    psvDebugScreenSetFgColor(color);
    psvDebugScreenSetBgColor(PSV_TRANSPARENT);
    psvDebugScreenSetCoordsXY(x, y);
    psvDebugScreenPrintf("%s", str);
}

void display_char(int x, int y, uint32_t color, char c) {
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
