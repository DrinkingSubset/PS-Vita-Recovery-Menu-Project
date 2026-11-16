/*
 * theme.c - Classic / Retro themes (v1.2.2).
 *
 * Classic: the original look (green bars, bold font).
 * Retro:   M33 recovery style - black screen, PSP debug font, every text
 *          in the theme colour, the selected row in white, no bars.
 *
 * Every screen already draws with the shared COLOR_* names and draws the
 * selected row with COLOR_SELECTED, so switching the values in g_tc (plus
 * the retro rules in display.c) restyles all screens at once.
 *
 * OK / warning / error colours (COLOR_GREEN / COLOR_YELLOW / COLOR_RED)
 * keep their meaning in every theme.
 */

#include <psp2/ctrl.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <stdio.h>
#include <string.h>

#include "theme.h"
#include "display.h"
#include "rain.h"

#define THEME_DIR   "ux0:data/vita_recovery"
#define THEME_FILE  "ux0:data/vita_recovery/theme.cfg"

typedef struct { const char *name; unsigned char r, g, b; } RetroColour;

/* Bright enough to read on black (pure blue is not) */
static const RetroColour RETRO[] = {
    { "Red",     255,   0,   0 }, { "Orange",  255, 128,   0 },
    { "Amber",   255, 191,   0 }, { "Yellow",  255, 255,   0 },
    { "Lime",    160, 255,   0 }, { "Green",     0, 230,   0 },
    { "Mint",      0, 255, 160 }, { "Cyan",      0, 230, 255 },
    { "Blue",     70, 130, 255 }, { "Purple",  170,  90, 255 },
    { "Magenta", 255,   0, 255 }, { "Pink",    255, 110, 190 },
    { "Silver",  150, 150, 150 },
};
#define N_RETRO ((int)(sizeof(RETRO) / sizeof(RETRO[0])))
#define N_THEMES (1 + N_RETRO)          /* 0 = Classic, 1.. = Retro colours */

static int g_saved = 0;                 /* theme index stored on disk */
static int g_rain  = 0;                 /* Matrix rain on/off (saved)   */

static uint32_t scale(const RetroColour *c, int pct) {
    return RGBA8(c->r * pct / 100, c->g * pct / 100, c->b * pct / 100, 255);
}

static void apply(int idx) {
    if (idx <= 0 || idx >= N_THEMES) {
        /* Classic: exactly the original colours */
        g_theme_retro = 0;
        g_tc.text     = RGBA8(255,255,255,255);
        g_tc.selected = RGBA8(255,255,255,255);
        g_tc.sel_bg   = RGBA8(  0,180,  0,255);
        g_tc.dim      = RGBA8(160,160,160,255);
        g_tc.green    = RGBA8( 80,255, 80,255);
        g_tc.red      = RGBA8(255, 80, 80,255);
        g_tc.yellow   = RGBA8(255,220,  0,255);
        g_tc.title_bg = RGBA8(  0, 60,  0,255);
        g_tc.title    = RGBA8(255,255,255,255);
        g_tc.header   = RGBA8(200,200,200,255);
        g_tc.line     = RGBA8( 80,255, 80,255);
        psvDebugScreenSetFont(0);
        return;
    }
    const RetroColour *c = &RETRO[idx - 1];
    g_theme_retro = 1;
    g_tc.text     = scale(c, 100);
    g_tc.selected = RGBA8(255,255,255,255);
    g_tc.sel_bg   = RGBA8(0,0,0,255);           /* no selection bar */
    g_tc.dim      = scale(c, 60);
    g_tc.green    = RGBA8( 80,255, 80,255);     /* meanings stay the same */
    g_tc.red      = RGBA8(255, 80, 80,255);
    g_tc.yellow   = RGBA8(255,220,  0,255);
    g_tc.title_bg = RGBA8(0,0,0,255);           /* no title / footer bars */
    g_tc.title    = scale(c, 100);
    g_tc.header   = scale(c, 80);
    g_tc.line     = scale(c, 30);               /* faint separators */
    psvDebugScreenSetFont(1);
}

static const char *theme_name(int idx, char *buf, int len) {
    if (idx <= 0) snprintf(buf, len, "Classic (green bars)");
    else          snprintf(buf, len, "Retro: %s", RETRO[idx - 1].name);
    return buf;
}

void theme_load(void) {
    int idx = 0;
    SceUID fd = sceIoOpen(THEME_FILE, SCE_O_RDONLY, 0);
    if (fd >= 0) {
        char buf[32] = {0};
        sceIoRead(fd, buf, sizeof(buf) - 1);
        sceIoClose(fd);
        if (sscanf(buf, "theme=%d", &idx) != 1) idx = 0;
        const char *rp = strstr(buf, "rain=");
        g_rain = (rp && rp[5] == '1');
    }
    if (idx < 0 || idx >= N_THEMES) idx = 0;
    g_saved = idx;
    rain_set(g_rain);
    apply(idx);
}

static int save(int idx, int rain) {
    sceIoMkdir("ux0:data", 0777);
    sceIoMkdir(THEME_DIR, 0777);
    SceUID fd = sceIoOpen(THEME_FILE, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
    if (fd < 0) return fd;
    char buf[32];
    int n = snprintf(buf, sizeof(buf), "theme=%d\nrain=%d\n", idx, rain ? 1 : 0);
    sceIoWrite(fd, buf, n);
    sceIoClose(fd);
    return 0;
}

static int pressed(SceCtrlData *now, SceCtrlData *old, unsigned int b) {
    return (now->buttons & b) && !(old->buttons & b);
}

static void drain(void) {
    SceCtrlData d;
    do { sceCtrlPeekBufferPositive(0, &d, 1); } while (d.buttons);
}

void theme_screen_run(void) {
    int sel = g_saved, scroll = 0, rain = g_rain;
    const int rows = 15;                 /* visible rows */
    SceCtrlData old, now;
    drain();
    sceCtrlPeekBufferPositive(0, &old, 1);

    while (1) {
        apply(sel);                      /* live preview */
        rain_set(rain);
        display_start();
        display_clear(COLOR_BG);
        display_rect(0, 0, SCREEN_W, TITLE_LINE, COLOR_TITLE_BG);
        display_app_title(6);
        display_text(MENU_X, 26, COLOR_DIM, "Theme");
        display_hline(0, TITLE_LINE, SCREEN_W, COLOR_GREEN);

        if (sel < scroll) scroll = sel;
        if (sel >= scroll + rows) scroll = sel - rows + 1;
        int y = TITLE_H + 8;
        char name[40], row[64];
        for (int i = scroll; i < N_THEMES && i < scroll + rows; i++) {
            snprintf(row, sizeof(row), "%s%s", theme_name(i, name, sizeof(name)),
                     i == g_saved ? "   (saved)" : "");
            if (i == sel) {
                display_rect(0, y - 1, SCREEN_W - 8, LINE_H + 1, COLOR_SEL_BG);
                display_text_transp(MENU_X, y, COLOR_SELECTED, row);
            } else {
                display_text(MENU_X, y, COLOR_TEXT, row);
            }
            y += LINE_H;
        }
        display_text(MENU_X, y + 8, COLOR_DIM, "The whole app changes as you move.");
        display_text(MENU_X, y + 8 + LINE_H, sel > 0 ? COLOR_TEXT : COLOR_DIM,
                     sel > 0 ? (rain ? "Matrix rain: ON   [Square] to turn off"
                                     : "Matrix rain: OFF  [Square] to turn on")
                             : "Matrix rain: Retro themes only");

        display_rect(0, FOOTER_Y, SCREEN_W, SCREEN_H - FOOTER_Y, COLOR_TITLE_BG);
        display_hline(0, FOOTER_LINE, SCREEN_W, COLOR_GREEN);
        display_text(MENU_X, FOOTER_Y + 7, COLOR_DIM, "[X] Save [Up/Dn] Preview [Sq] Rain [O] Cancel");
        display_end();

        sceCtrlPeekBufferPositive(0, &now, 1);
        if (pressed(&now, &old, SCE_CTRL_DOWN)) sel = (sel + 1) % N_THEMES;
        if (pressed(&now, &old, SCE_CTRL_UP))   sel = (sel + N_THEMES - 1) % N_THEMES;
        if (pressed(&now, &old, SCE_CTRL_SQUARE)) rain = !rain;
        if (pressed(&now, &old, SCE_CTRL_CROSS)) {
            if (save(sel, rain) >= 0) { g_saved = sel; g_rain = rain; }
            rain_set(g_rain);
            apply(g_saved);
            drain();
            return;
        }
        if (pressed(&now, &old, SCE_CTRL_CIRCLE)) {
            rain_set(g_rain);
            apply(g_saved);              /* undo the preview */
            drain();
            return;
        }
        old = now;
    }
}
