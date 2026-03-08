#pragma once
#include <stdint.h>
#include "debugScreen.h"

#define SCREEN_W     960
#define SCREEN_H     544
#define FONT_W        16    /* 8px font at 2x scale = 16px wide  */
#define FONT_H        16    /* 8px font at 2x scale = 16px tall  */
#define LINE_H        24    /* item row height (font + spacing)  */
#define MENU_X        40    /* left indent for all text          */
#define MENU_Y        80    /* first content row on full screens */
#define VAL_X        340    /* value column x (right of labels)  */

/* Shared layout — used consistently across ALL screens */
#define TITLE_H       46    /* title bar height (0 .. TITLE_H-1) */
#define TITLE_LINE   (TITLE_H - 2)   /* green separator y         */
#define FOOTER_Y     (SCREEN_H - 30) /* bottom bar y = 514        */
#define FOOTER_LINE  FOOTER_Y        /* green separator y         */

/* Colors — psvDebugScreen pixel format: 0x00BBGGRR */
#define RGBA8(r,g,b,a)  (((b)<<16)|((g)<<8)|(r))
#define COLOR_BG        RGBA8(  0,  0,  0,255)
#define COLOR_TEXT      RGBA8(255,255,255,255)
#define COLOR_SELECTED  RGBA8(255,255,255,255)
#define COLOR_SEL_BG    RGBA8(  0,180,  0,255)
#define COLOR_DIM       RGBA8(160,160,160,255)
#define COLOR_GREEN     RGBA8( 80,255, 80,255)
#define COLOR_RED       RGBA8(255, 80, 80,255)
#define COLOR_YELLOW    RGBA8(255,220,  0,255)
#define COLOR_TITLE_BG  RGBA8(  0, 60,  0,255)
#define COLOR_TITLE     RGBA8(255,255,255,255)
#define COLOR_HEADER    RGBA8(200,200,200,255)

void display_init(void);
void display_start(void);
void display_end(void);
void display_clear(uint32_t color);
void display_pixel(int x, int y, uint32_t color);
void display_rect(int x, int y, int w, int h, uint32_t color);
void display_hline(int x, int y, int w, uint32_t color);
void display_char(int x, int y, uint32_t color, char c);
void display_text(int x, int y, uint32_t color, const char *str);
void display_text_transp(int x, int y, uint32_t color, const char *str);
void display_textf(int x, int y, uint32_t color, const char *fmt, ...);
int  display_text_width(const char *str);
