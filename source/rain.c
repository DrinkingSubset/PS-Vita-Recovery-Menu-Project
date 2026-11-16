/*
 * rain.c - Matrix-style rain behind the retro theme (v1.2.2).
 *
 * Drawn by display_clear(), which every screen calls at the start of each
 * frame, so the rain keeps falling on every screen. It moves by elapsed
 * time, not per frame, so its speed is the same everywhere. (Screens that
 * are busy working draw no new frames, so the rain pauses there.)
 *
 * Characters: letters, digits and coding symbols, PSP-font extras
 * (Greek / maths signs, card suits, box lines), and whole "code words"
 * that fall down one column (sceIoOpen, 0x80010002, *KERNEL ...).
 *
 * Text stays readable: display_text() puts a black backing behind every
 * string while the rain is on, so drops pass behind words, never through.
 */

#include <psp2/kernel/processmgr.h>
#include <string.h>

#include "rain.h"
#include "display.h"

#define CELL     16                       /* one character cell, px  */
#define COLS     (SCREEN_W / CELL)        /* 60 */
#define ROWS     (SCREEN_H / CELL)        /* 34 */

static int g_on = 0;

typedef struct {
    int   y;          /* head position in 1/256 cells        */
    int   speed;      /* 1/256 cells per millisecond         */
    int   len;        /* trail length in cells               */
    int   word;       /* index into WORDS, or -1 = random    */
} Drop;

static Drop          g_drop[COLS];
static unsigned char g_cell[COLS][ROWS];
static SceUInt64     g_last = 0;
static unsigned int  g_seed = 0x1234567u;
static int           g_ready = 0;

/* Coding words that fall as a whole, top to bottom */
static const char *const WORDS[] = {
    "sceIoOpen", "0x80010002", "*KERNEL", "ur0:tai", "taiHEN", "HENkaku",
    "#include", "int main()", "return 0;", "while(1)", "0xFFFFFFFF",
    "if(r<0)", "{...}", "Enso", "app.db", "*main", "ksceIoRemove",
    "0xDEADBEEF", "sceCtrl", "skprx", "suprx", "NULL", "uint32_t", "!=",
};
#define N_WORDS ((int)(sizeof(WORDS) / sizeof(WORDS[0])))

/* Extra PSP-font glyphs: suits and symbols, currency, Greek / maths */
static const unsigned char EXTRA[] = {
    1, 2, 3, 4, 5, 6, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27,
    155, 156, 157, 158, 159,
    224, 225, 226, 227, 228, 229, 230, 231, 232, 233, 234, 235, 236, 237, 238,
    239, 240, 241, 242, 243, 244, 245, 246, 247, 248, 249, 250, 251, 252, 253,
};

/* Coding symbols */
static const char SYMS[] = "{}[]()<>;:=+-*/&|!?#$%^~_\\@";

static unsigned int rnd(void) {
    g_seed = g_seed * 1103515245u + 12345u;
    return (g_seed >> 8) & 0xFFFFFF;
}

static unsigned char rand_char(void) {
    unsigned int r = rnd() % 100;
    if (r < 30) return (unsigned char)EXTRA[rnd() % sizeof(EXTRA)];
    if (r < 55) return (unsigned char)SYMS[rnd() % (sizeof(SYMS) - 1)];
    return (unsigned char)(33 + rnd() % 94);             /* any ASCII */
}

static void new_drop(int c, int top) {
    Drop *d = &g_drop[c];
    d->len   = 6 + (int)(rnd() % 14);
    d->speed = 2 + (int)(rnd() % 5);                     /* ~8-25 cells/s */
    d->y     = top ? -(int)(rnd() % (ROWS * 256)) : (int)(rnd() % (ROWS * 256));
    d->word  = (rnd() % 100 < 22) ? (int)(rnd() % N_WORDS) : -1;
    if (d->word >= 0) d->len = (int)strlen(WORDS[d->word]) + 3;
}

static void init(void) {
    g_seed ^= (unsigned int)sceKernelGetProcessTimeWide();
    for (int c = 0; c < COLS; c++) {
        for (int r = 0; r < ROWS; r++) g_cell[c][r] = rand_char();
        new_drop(c, 0);
    }
    g_last = sceKernelGetProcessTimeWide();
    g_ready = 1;
}

void rain_set(int on) { g_on = on; }
int  rain_is_on(void) { return g_on && g_theme_retro; }

static uint32_t shade(uint32_t c, int pct) {
    unsigned r = (c & 0xFF) * pct / 100, g = ((c >> 8) & 0xFF) * pct / 100,
             b = ((c >> 16) & 0xFF) * pct / 100;
    return RGBA8(r, g, b, 255);
}

void rain_draw(void) {
    if (!rain_is_on()) return;
    if (!g_ready) init();

    SceUInt64 now = sceKernelGetProcessTimeWide();
    int ms = (int)((now - g_last) / 1000);
    if (ms > 100) ms = 100;                 /* after a pause, don't jump */
    if (ms < 0) ms = 0;
    g_last = now;

    uint32_t accent = g_tc.text;
    for (int c = 0; c < COLS; c++) {
        Drop *d = &g_drop[c];
        d->y += d->speed * ms;
        int head = d->y / 256;
        if (head - d->len > ROWS) { new_drop(c, 1); head = d->y / 256; }

        for (int t = 0; t < d->len; t++) {
            int r = head - t;
            if (r < 0 || r >= ROWS) continue;
            /* random columns flicker a little; word columns stay readable */
            unsigned char ch;
            if (d->word >= 0 && t < (int)strlen(WORDS[d->word])) {
                /* the word falls with the drop, reading top to bottom */
                const char *w = WORDS[d->word];
                ch = (unsigned char)w[strlen(w) - 1 - t];
            } else {
                if ((rnd() % 100) < 3) g_cell[c][r] = rand_char();   /* flicker */
                ch = g_cell[c][r];
            }
            int pct = (t == 0) ? 60 : 10 + 30 * (d->len - t) / d->len;
            psvDebugScreenDrawGlyph(c * CELL, r * CELL, ch, shade(accent, pct));
        }
    }
}
