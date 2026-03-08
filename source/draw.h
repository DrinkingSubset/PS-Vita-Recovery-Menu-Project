#pragma once
#include "display.h"
#include "menu.h"
#include <stdarg.h>
#include <stdio.h>

static inline void draw_init(void *unused) {}

static inline void draw_text(int x, int y, unsigned int color, const char *text) {
    display_text(x, y, color, text);
}
static inline void draw_textf(int x, int y, unsigned int color, const char *fmt, ...) {
    char buf[256];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    display_text(x, y, color, buf);
}
static inline void draw_fill_rect(int x, int y, int w, int h, unsigned int color) {
    display_rect(x, y, w, h, color);
}
static inline void draw_hline(int x, int y, int w, unsigned int color) {
    display_hline(x, y, w, color);
}

void draw_menu(void *unused, Menu *menu);
