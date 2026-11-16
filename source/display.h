#pragma once
#include <stdint.h>
#include "debugScreen.h"
#include "version.h"

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
#define COLOR_BG        RGBA8(  0,  0,  0,255)   /* always black (0 = transparent text bg) */

/* v1.2.2 (themes test): colours come from the active theme at runtime.
 * Classic = the original green look. Retro = M33-style: black screen,
 * PSP font, text in the theme colour, selected text in white, no bars. */
typedef struct {
    uint32_t text, selected, sel_bg, dim, green, red, yellow,
             title_bg, title, header, line;
} ThemeColors;
extern ThemeColors g_tc;
extern int g_theme_retro;          /* 1 = retro style active */

#define COLOR_TEXT      (g_tc.text)
#define COLOR_SELECTED  (g_tc.selected)
#define COLOR_SEL_BG    (g_tc.sel_bg)
#define COLOR_DIM       (g_tc.dim)
#define COLOR_GREEN     (g_tc.green)
#define COLOR_RED       (g_tc.red)
#define COLOR_YELLOW    (g_tc.yellow)
#define COLOR_TITLE_BG  (g_tc.title_bg)
#define COLOR_TITLE     (g_tc.title)
#define COLOR_HEADER    (g_tc.header)

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

/* Draws "PS Vita Recovery Menu vX.Y.Z" at the left and "By DrinkingSubset"
 * at the far right of the title bar, both on row y. */
void display_app_title(int y);
