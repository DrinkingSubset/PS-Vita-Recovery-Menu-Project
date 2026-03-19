/*
 * cheat_manager.c — Unified cheat & trainer manager
 *
 * No live memory patching. All edits are written to cheat files on disk.
 * The respective plugins (VitaCheat / CWCheat) pick them up on next boot.
 */

#include <psp2/ctrl.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/io/dirent.h>

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "cheat_manager.h"
#include "draw.h"
#include "menu.h"

/* ── Paths ──────────────────────────────────────────────────────────────── */
#define VITA_CHEAT_DIR  "ux0:data/vitacheat/cheats"
#define CW_DB_PATH      "ux0:pspemu/seplugins/cwcheat/CHEAT.db"

/* ── File helpers ───────────────────────────────────────────────────────── */
static int btn_pressed(SceCtrlData *n, SceCtrlData *o, unsigned int b) {
    return (n->buttons & b) && !(o->buttons & b);
}
static int file_exists(const char *p) {
    SceIoStat s; return sceIoGetstat(p, &s) >= 0;
}

/* Read file into a heap buffer. Caller must free(). Returns NULL on failure. */
static char *read_alloc(const char *path, int *out_len) {
    SceUID fd = sceIoOpen(path, SCE_O_RDONLY, 0);
    if (fd < 0) return NULL;
    int sz = (int)sceIoLseek(fd, 0, SCE_SEEK_END);
    sceIoLseek(fd, 0, SCE_SEEK_SET);
    if (sz <= 0) { sceIoClose(fd); return NULL; }
    char *buf = malloc(sz + 2);
    if (!buf)  { sceIoClose(fd); return NULL; }
    sceIoRead(fd, buf, sz);
    sceIoClose(fd);
    buf[sz] = '\n'; buf[sz+1] = '\0';
    if (out_len) *out_len = sz;
    return buf;
}

static int write_text(const char *path, const char *data) {
    SceUID fd = sceIoOpen(path,
                          SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
    if (fd < 0) return -1;
    sceIoWrite(fd, data, strlen(data));
    sceIoClose(fd);
    return 0;
}

/* ── Common UI ──────────────────────────────────────────────────────────── */
static void title_bar(const char *t) {
    display_rect(0, 0, SCREEN_W, TITLE_LINE, COLOR_TITLE_BG);
    draw_text(MENU_X, 6,  COLOR_TEXT, "PS Vita Recovery Menu v1.0");
    draw_text(MENU_X, 26, COLOR_DIM,  t);
    display_hline(0, TITLE_LINE, SCREEN_W, COLOR_GREEN);
}
static void footer(const char *t) {
    display_rect(0, FOOTER_Y, SCREEN_W, SCREEN_H - FOOTER_Y, COLOR_TITLE_BG);
    display_hline(0, FOOTER_LINE, SCREEN_W, COLOR_GREEN);
    draw_text(MENU_X, FOOTER_Y + 7, COLOR_DIM, t);
}

/* ════════════════════════════════════════════════════════════════
 * PART A — VITA NATIVE CHEAT MANAGER  (.psv files)
 *
 * File format:
 *   [PCSE00123]
 *   $Infinite Health
 *   V0 20123456 000F423F    <- V0=off, V1=on
 *   V0 20123458 000F423F
 *   $Infinite Ammo
 *   V1 2012ABCD 00000063
 *
 * We parse the file into cheat entries and raw lines.
 * Toggling flips V0<->V1 on all code lines for that cheat,
 * then we reserialise the raw lines back to disk.
 * ════════════════════════════════════════════════════════════════ */

#define MAX_VITA_CHEATS  64
#define MAX_RAW_LINES    512

typedef struct {
    char name[64];
    int  enabled;        /* 1 = first code line starts with V1 */
    int  first_code_raw; /* index in g_vraw[] of first Vx line */
    int  code_count;
} VitaCheat;

typedef struct {
    char raw[128];
    int  is_code;        /* 1 if this is a Vx data line */
    int  cheat_idx;      /* which VitaCheat owns this code line */
} RawLine;

static VitaCheat  g_vcheats[MAX_VITA_CHEATS];
static int        g_vcheat_count;
static RawLine    g_vraw[MAX_RAW_LINES];
static int        g_vraw_count;
static char       g_v_titleid[32];
static char       g_v_path[128];

static void vita_parse(const char *path) {
    g_vcheat_count = g_vraw_count = 0;
    g_v_titleid[0] = g_v_path[0] = '\0';

    char *buf = read_alloc(path, NULL);
    if (!buf) return;

    snprintf(g_v_path, sizeof(g_v_path), "%s", path);

    char *line = strtok(buf, "\n");
    int   cur  = -1;

    while (line) {
        int ll = strlen(line);
        if (ll > 0 && line[ll-1] == '\r') line[--ll] = '\0';

        if (g_vraw_count < MAX_RAW_LINES) {
            snprintf(g_vraw[g_vraw_count].raw,
                     sizeof(g_vraw[0].raw), "%s", line);
            g_vraw[g_vraw_count].is_code    = 0;
            g_vraw[g_vraw_count].cheat_idx  = cur;
            g_vraw_count++;
        }

        if (line[0] == '[') {
            /* Title ID header e.g. [PCSE00123] */
            int tl = ll - 2;
            if (tl > 0 && tl < 30) { strncpy(g_v_titleid, line+1, tl); g_v_titleid[tl]='\0'; }
        } else if (line[0] == '$' && g_vcheat_count < MAX_VITA_CHEATS) {
            cur = g_vcheat_count++;
            VitaCheat *ch = &g_vcheats[cur];
            snprintf(ch->name, sizeof(ch->name), "%s", line+1);
            ch->enabled        = 0;
            ch->first_code_raw = -1;
            ch->code_count     = 0;
            g_vraw[g_vraw_count-1].cheat_idx = cur;
        } else if ((line[0]=='V'||line[0]=='v') && cur >= 0) {
            int ri = g_vraw_count - 1;
            g_vraw[ri].is_code   = 1;
            g_vraw[ri].cheat_idx = cur;
            VitaCheat *ch = &g_vcheats[cur];
            if (ch->first_code_raw < 0) {
                ch->first_code_raw = ri;
                ch->enabled        = (line[1] == '1');
            }
            ch->code_count++;
        }
        line = strtok(NULL, "\n");
    }
    free(buf);
}

static void vita_save(void) {
    if (!g_v_path[0]) return;
    /* Update V0/V1 on every code line based on its cheat's enabled flag */
    for (int i = 0; i < g_vraw_count; i++) {
        if (g_vraw[i].is_code) {
            int ci = g_vraw[i].cheat_idx;
            if (ci >= 0 && ci < g_vcheat_count)
                g_vraw[i].raw[1] = g_vcheats[ci].enabled ? '1' : '0';
        }
    }
    char *out = malloc(MAX_RAW_LINES * 130);
    if (!out) return;
    int pos = 0;
    for (int i = 0; i < g_vraw_count; i++)
        pos += snprintf(out+pos, MAX_RAW_LINES*130 - pos,
                        "%s\n", g_vraw[i].raw);
    write_text(g_v_path, out);
    free(out);
}

static void vita_editor(void *font, const char *path) {
    vita_parse(path);

    if (g_vcheat_count == 0) {
        SceCtrlData old, ctrl; memset(&old, 0, sizeof(old));
        while (1) {
            display_start(); display_clear(COLOR_BG);
            title_bar("Vita Cheat Editor");
            draw_text(MENU_X, MENU_Y, COLOR_DIM, "No cheats found in this .psv file.");
            footer("[O] Back");
            display_end(); 
            sceCtrlPeekBufferPositive(0, &ctrl, 1);
            if (btn_pressed(&ctrl, &old, SCE_CTRL_CIRCLE)) { do { sceCtrlPeekBufferPositive(0, &old, 1); } while (old.buttons); return; }
            old = ctrl;
        }
    }

    int sel = 0, scroll = 0, dirty = 0;
    const int VIS = 9;
    SceCtrlData old, ctrl; memset(&old, 0, sizeof(old));

    while (1) {
        display_start(); display_clear(COLOR_BG);
        char th[64];
        snprintf(th, sizeof(th), "Vita Cheats — %s", g_v_titleid);
        title_bar(th);
        draw_text(MENU_X, MENU_Y, COLOR_DIM,
                  dirty ? "* unsaved — press Triangle to save *" : "");

        for (int i = scroll; i < g_vcheat_count && i < scroll+VIS; i++) {
            int y = MENU_Y + LINE_H + (i-scroll)*(LINE_H+2);
            int  en  = g_vcheats[i].enabled;
            char label[80];
            snprintf(label, sizeof(label), "%s %s",
                     en ? "[ON] " : "[OFF]", g_vcheats[i].name);
            if (i == sel) {
                draw_fill_rect(MENU_X-4, y-1,
                               SCREEN_W-(MENU_X-4)*2, LINE_H+2, COLOR_SEL_BG);
                draw_text(MENU_X, y, COLOR_SELECTED, label);
            } else {
                draw_text(MENU_X, y, en ? COLOR_GREEN : COLOR_DIM, label);
            }
        }
        footer("[X] Toggle  [L] All ON  [R] All OFF  [Tri] Save  [O] Back");
        display_end(); 

        sceCtrlPeekBufferPositive(0, &ctrl, 1);
        if (btn_pressed(&ctrl, &old, SCE_CTRL_DOWN) && sel < g_vcheat_count-1) {
            sel++; if (sel >= scroll+VIS) scroll++;
        }
        if (btn_pressed(&ctrl, &old, SCE_CTRL_UP) && sel > 0) {
            sel--; if (sel < scroll) scroll--;
        }
        if (btn_pressed(&ctrl, &old, SCE_CTRL_CROSS)) {
            g_vcheats[sel].enabled ^= 1; dirty = 1;
        }
        if (btn_pressed(&ctrl, &old, SCE_CTRL_LTRIGGER)) {
            for (int i = 0; i < g_vcheat_count; i++) g_vcheats[i].enabled = 1;
            dirty = 1;
        }
        if (btn_pressed(&ctrl, &old, SCE_CTRL_RTRIGGER)) {
            for (int i = 0; i < g_vcheat_count; i++) g_vcheats[i].enabled = 0;
            dirty = 1;
        }
        if (btn_pressed(&ctrl, &old, SCE_CTRL_TRIANGLE) && dirty) {
            vita_save(); dirty = 0;
        }
        if (btn_pressed(&ctrl, &old, SCE_CTRL_CIRCLE)) { do { sceCtrlPeekBufferPositive(0, &old, 1); } while (old.buttons); return; }
        old = ctrl;
    }
}

static void vita_cheat_picker(void *font) {
#define MAX_PSV 64
    char files[MAX_PSV][64]; int count = 0;
    sceIoMkdir(VITA_CHEAT_DIR, 0777);
    SceUID dfd = sceIoDopen(VITA_CHEAT_DIR);
    if (dfd >= 0) {
        SceIoDirent e;
        while (sceIoDread(dfd, &e) > 0 && count < MAX_PSV) {
            if (e.d_name[0] == '.') continue;
            int nl = strlen(e.d_name);
            if (nl > 4 && strcmp(e.d_name+nl-4, ".psv") == 0) {
                snprintf(files[count], 64, "%s", e.d_name);
                count++;
            }
        }
        sceIoDclose(dfd);
    }

    int sel = 0;
    SceCtrlData old, ctrl; memset(&old, 0, sizeof(old));

    while (1) {
        display_start(); display_clear(COLOR_BG);
        title_bar("Vita Cheat Manager — Select Game");

        if (count == 0) {
            draw_text(MENU_X, MENU_Y,          COLOR_DIM, "No .psv files found in:");
            draw_text(MENU_X, MENU_Y+LINE_H,   COLOR_DIM, VITA_CHEAT_DIR);
            draw_text(MENU_X, MENU_Y+LINE_H*3, COLOR_DIM,
                      "Copy TITLEID.psv cheat files there.");
            draw_text(MENU_X, MENU_Y+LINE_H*4, COLOR_DIM,
                      "Get them from: github.com/r0ah/vitacheat");
        } else {
            for (int i = 0; i < count; i++) {
                int y = MENU_Y + i*(LINE_H+2);
                if (i == sel) {
                    draw_fill_rect(MENU_X-4, y-1,
                                   SCREEN_W-(MENU_X-4)*2, LINE_H+2, COLOR_SEL_BG);
                    draw_text(MENU_X, y, COLOR_SELECTED, files[i]);
                } else {
                    draw_text(MENU_X, y, COLOR_TEXT, files[i]);
                }
            }
        }
        footer("[X] Open   [O] Back");
        display_end(); 

        sceCtrlPeekBufferPositive(0, &ctrl, 1);
        if (btn_pressed(&ctrl, &old, SCE_CTRL_DOWN) && sel < count-1) sel++;
        if (btn_pressed(&ctrl, &old, SCE_CTRL_UP)   && sel > 0)       sel--;
        if (btn_pressed(&ctrl, &old, SCE_CTRL_CROSS) && count > 0) {
            char path[128];
            snprintf(path, sizeof(path), "%s/%s", VITA_CHEAT_DIR, files[sel]);
            vita_editor(font, path);
        }
        if (btn_pressed(&ctrl, &old, SCE_CTRL_CIRCLE)) { do { sceCtrlPeekBufferPositive(0, &old, 1); } while (old.buttons); return; }
        old = ctrl;
    }
#undef MAX_PSV
}

/* ════════════════════════════════════════════════════════════════
 * PART B — PSP CWCHEAT .db MANAGER
 *
 * CHEAT.db format:
 *   _S ULUS-10000
 *   _G Game Title
 *   _C0 Cheat Name       <- _C0=disabled, _C1=enabled
 *   _L 0xADDRESS 0xVALUE
 *
 * We load the entire file as raw lines, index game starts,
 * parse per-game cheats on demand, toggle _C0<->_C1 in-place,
 * and write the full file back. All other games untouched.
 * ════════════════════════════════════════════════════════════════ */

#define MAX_CW_GAMES   256
#define MAX_CW_CHEATS  128
#define MAX_DB_LINES   65536

typedef struct {
    char id[32];
    char title[64];
    int  line_start; /* index in g_db_lines where _S appears */
} CWGame;

typedef struct {
    char name[64];
    int  enabled;
    int  line_idx; /* index in g_db_lines for the _Cx line */
} CWCheat;

static char  **g_db_lines  = NULL;
static int     g_db_lcount = 0;
static CWGame  g_cw_games[MAX_CW_GAMES];
static int     g_cw_gcount = 0;

static void cw_free_db(void) {
    if (g_db_lines) {
        for (int i = 0; i < g_db_lcount; i++) free(g_db_lines[i]);
        free(g_db_lines);
        g_db_lines = NULL;
    }
    g_db_lcount = g_cw_gcount = 0;
}

static int cw_load_db(void) {
    cw_free_db();
    char *buf = read_alloc(CW_DB_PATH, NULL);
    if (!buf) return -1;

    g_db_lines = malloc(MAX_DB_LINES * sizeof(char *));
    if (!g_db_lines) { free(buf); return -2; }

    char *tok = strtok(buf, "\n");
    while (tok && g_db_lcount < MAX_DB_LINES) {
        int ll = strlen(tok);
        if (ll > 0 && tok[ll-1] == '\r') tok[--ll] = '\0';
        g_db_lines[g_db_lcount] = malloc(ll + 2);
        if (!g_db_lines[g_db_lcount]) break;   /* OOM — stop parsing */
        snprintf(g_db_lines[g_db_lcount], ll+2, "%s", tok);

        if (strncmp(tok, "_S ", 3) == 0 && g_cw_gcount < MAX_CW_GAMES) {
            CWGame *g = &g_cw_games[g_cw_gcount++];
            snprintf(g->id,    sizeof(g->id),    "%s", tok+3);
            g->title[0]  = '\0';
            g->line_start = g_db_lcount;
        } else if (strncmp(tok, "_G ", 3) == 0 && g_cw_gcount > 0) {
            snprintf(g_cw_games[g_cw_gcount-1].title,
                     sizeof(g_cw_games[0].title), "%s", tok+3);
        }
        g_db_lcount++;
        tok = strtok(NULL, "\n");
    }
    free(buf);
    return 0;
}

static void cw_save_db(void) {
    if (!g_db_lines) return;
    int bufsz = g_db_lcount * 100 + 16;
    char *out = malloc(bufsz);
    if (!out) return;
    int pos = 0;
    for (int i = 0; i < g_db_lcount; i++)
        pos += snprintf(out+pos, bufsz-pos, "%s\n", g_db_lines[i]);
    write_text(CW_DB_PATH, out);
    free(out);
}

static void cw_edit_game(void *font, CWGame *game) {
    CWCheat cheats[MAX_CW_CHEATS]; int cc = 0;
    int in_game = 0;
    for (int i = game->line_start; i < g_db_lcount && cc < MAX_CW_CHEATS; i++) {
        char *l = g_db_lines[i];
        if (strncmp(l, "_S ", 3) == 0) {
            if (in_game) break;
            in_game = 1; continue;
        }
        if (!in_game) continue;
        if (strncmp(l, "_C", 2) == 0 && (l[2]=='0' || l[2]=='1')) {
            CWCheat *ch = &cheats[cc++];
            snprintf(ch->name, sizeof(ch->name), "%s", l+3);
            ch->enabled  = (l[2] == '1');
            ch->line_idx = i;
        }
    }

    int sel = 0, scroll = 0, dirty = 0;
    const int VIS = 9;
    SceCtrlData old, ctrl; memset(&old, 0, sizeof(old));

    while (1) {
        display_start(); display_clear(COLOR_BG);
        char th[96];
        snprintf(th, sizeof(th), "CWCheat — %s",
                 game->title[0] ? game->title : game->id);
        title_bar(th);
        draw_text(MENU_X, MENU_Y, COLOR_DIM,
                  dirty ? "* unsaved — press Triangle to save *" : "");

        if (cc == 0) {
            draw_text(MENU_X, MENU_Y+LINE_H, COLOR_DIM,
                      "(no cheats found for this game ID)");
        }
        for (int i = scroll; i < cc && i < scroll+VIS; i++) {
            int y = MENU_Y + LINE_H + (i-scroll)*(LINE_H+2);
            int  en = cheats[i].enabled;
            char label[80];
            snprintf(label, sizeof(label), "%s %s",
                     en ? "[ON] " : "[OFF]", cheats[i].name);
            if (i == sel) {
                draw_fill_rect(MENU_X-4, y-1,
                               SCREEN_W-(MENU_X-4)*2, LINE_H+2, COLOR_SEL_BG);
                draw_text(MENU_X, y, COLOR_SELECTED, label);
            } else {
                draw_text(MENU_X, y, en ? COLOR_GREEN : COLOR_DIM, label);
            }
        }
        footer("[X] Toggle  [L] All ON  [R] All OFF  [Tri] Save  [O] Back");
        display_end(); 

        sceCtrlPeekBufferPositive(0, &ctrl, 1);
        if (btn_pressed(&ctrl, &old, SCE_CTRL_DOWN) && sel < cc-1) {
            sel++; if (sel >= scroll+VIS) scroll++;
        }
        if (btn_pressed(&ctrl, &old, SCE_CTRL_UP) && sel > 0) {
            sel--; if (sel < scroll) scroll--;
        }
        if (btn_pressed(&ctrl, &old, SCE_CTRL_CROSS) && cc > 0) {
            cheats[sel].enabled ^= 1;
            g_db_lines[cheats[sel].line_idx][2] =
                cheats[sel].enabled ? '1' : '0';
            dirty = 1;
        }
        if (btn_pressed(&ctrl, &old, SCE_CTRL_LTRIGGER)) {
            for (int i = 0; i < cc; i++) {
                cheats[i].enabled = 1;
                g_db_lines[cheats[i].line_idx][2] = '1';
            }
            dirty = 1;
        }
        if (btn_pressed(&ctrl, &old, SCE_CTRL_RTRIGGER)) {
            for (int i = 0; i < cc; i++) {
                cheats[i].enabled = 0;
                g_db_lines[cheats[i].line_idx][2] = '0';
            }
            dirty = 1;
        }
        if (btn_pressed(&ctrl, &old, SCE_CTRL_TRIANGLE) && dirty) {
            cw_save_db(); dirty = 0;
        }
        if (btn_pressed(&ctrl, &old, SCE_CTRL_CIRCLE)) { do { sceCtrlPeekBufferPositive(0, &old, 1); } while (old.buttons); return; }
        old = ctrl;
    }
}

static void cwcheat_manager(void *font) {
    if (!file_exists(CW_DB_PATH)) {
        SceCtrlData old, ctrl; memset(&old, 0, sizeof(old));
        while (1) {
            display_start(); display_clear(COLOR_BG);
            title_bar("CWCheat DB Manager");
            draw_text(MENU_X, MENU_Y,          COLOR_RED,  "CHEAT.db not found.");
            draw_text(MENU_X, MENU_Y+LINE_H,   COLOR_DIM,  CW_DB_PATH);
            draw_text(MENU_X, MENU_Y+LINE_H*2, COLOR_DIM,
                      "Copy CHEAT.db there via VitaShell / FTP.");
            footer("[O] Back");
            display_end(); 
            sceCtrlPeekBufferPositive(0, &ctrl, 1);
            if (btn_pressed(&ctrl, &old, SCE_CTRL_CIRCLE)) { do { sceCtrlPeekBufferPositive(0, &old, 1); } while (old.buttons); return; }
            old = ctrl;
        }
    }

    if (cw_load_db() < 0) {
        SceCtrlData old, ctrl; memset(&old, 0, sizeof(old));
        while (1) {
            display_start(); display_clear(COLOR_BG);
            title_bar("CWCheat DB Manager");
            draw_text(MENU_X, MENU_Y, COLOR_RED, "Failed to load CHEAT.db.");
            footer("[O] Back");
            display_end(); 
            sceCtrlPeekBufferPositive(0, &ctrl, 1);
            if (btn_pressed(&ctrl, &old, SCE_CTRL_CIRCLE)) { cw_free_db(); do { sceCtrlPeekBufferPositive(0, &old, 1); } while (old.buttons); return; }
            old = ctrl;
        }
    }

    int sel = 0, scroll = 0;
    const int VIS = 10;
    SceCtrlData old, ctrl; memset(&old, 0, sizeof(old));

    while (1) {
        display_start(); display_clear(COLOR_BG);
        title_bar("CWCheat DB Manager — Select Game");
        char hdr[64];
        snprintf(hdr, sizeof(hdr), "%d games in CHEAT.db:", g_cw_gcount);
        draw_text(MENU_X, MENU_Y, COLOR_DIM, hdr);

        for (int i = scroll; i < g_cw_gcount && i < scroll+VIS; i++) {
            int y = MENU_Y + LINE_H + (i-scroll)*(LINE_H+2);
            char label[96];
            snprintf(label, sizeof(label), "[%s]  %s",
                     g_cw_games[i].id,
                     g_cw_games[i].title[0] ? g_cw_games[i].title : "");
            if (i == sel) {
                draw_fill_rect(MENU_X-4, y-1,
                               SCREEN_W-(MENU_X-4)*2, LINE_H+2, COLOR_SEL_BG);
                draw_text(MENU_X, y, COLOR_SELECTED, label);
            } else {
                draw_text(MENU_X, y, COLOR_TEXT, label);
            }
        }
        footer("[X] Edit Cheats   [O] Back");
        display_end(); 

        sceCtrlPeekBufferPositive(0, &ctrl, 1);
        if (btn_pressed(&ctrl, &old, SCE_CTRL_DOWN) && sel < g_cw_gcount-1) {
            sel++; if (sel >= scroll+VIS) scroll++;
        }
        if (btn_pressed(&ctrl, &old, SCE_CTRL_UP) && sel > 0) {
            sel--; if (sel < scroll) scroll--;
        }
        if (btn_pressed(&ctrl, &old, SCE_CTRL_CROSS))
            cw_edit_game(font, &g_cw_games[sel]);
        if (btn_pressed(&ctrl, &old, SCE_CTRL_CIRCLE)) {
            cw_free_db(); return;
        }
        old = ctrl;
    }
}

/* ════════════════════════════════════════════════════════════════
 * Main cheat manager screen
 * ════════════════════════════════════════════════════════════════ */
void cheat_manager_screen_run(void *font) {
    draw_init(font);   /* ensure g_font is set before any draw call */
    typedef struct { const char *label; const char *desc;
                     void(*fn)(void *); } Opt;
    static const Opt OPTS[] = {
        { "Vita Native Cheats  (.psv)",
          "Toggle PS Vita game cheats via VitaCheat .psv files.",
          vita_cheat_picker },
        { "PSP CWCheat Manager  (.db)",
          "Toggle cheats in CHEAT.db for PSP games via CWCheat.",
          cwcheat_manager },
    };
    int sel = 0;
    SceCtrlData old, ctrl; memset(&old, 0, sizeof(old));

    /* Drain any buttons held from the menu (e.g. X that opened this screen) */
    do { sceCtrlPeekBufferPositive(0, &old, 1); } while (old.buttons);

    while (1) {
        display_start(); display_clear(COLOR_BG);
        display_rect(0, 0, SCREEN_W, TITLE_LINE, COLOR_TITLE_BG);
        draw_text(MENU_X, 6,  COLOR_TEXT, "PS Vita Recovery Menu v1.0");
        draw_text(MENU_X, 26, COLOR_DIM,  "Cheat & Trainer Manager");
        display_hline(0, TITLE_LINE, SCREEN_W, COLOR_GREEN);

        draw_text(MENU_X, MENU_Y, COLOR_DIM,
                  "Changes written to disk — applied on next game launch.");

        for (int i = 0; i < 2; i++) {
            int y = MENU_Y + LINE_H*2 + i*(LINE_H*2+8);
            if (i == sel) {
                display_rect(0, y - 1, SCREEN_W - 8, LINE_H*2+2, COLOR_SEL_BG);
                display_text_transp(MENU_X, y,        COLOR_SELECTED, OPTS[i].label);
                display_text_transp(MENU_X, y+LINE_H, COLOR_SELECTED, OPTS[i].desc);
            } else {
                draw_text(MENU_X, y,        COLOR_TEXT, OPTS[i].label);
                draw_text(MENU_X, y+LINE_H, COLOR_DIM,  OPTS[i].desc);
            }
        }
        footer("[X] Open   [O] Back to Main Menu");
        display_end(); 

        sceCtrlPeekBufferPositive(0, &ctrl, 1);
        if (btn_pressed(&ctrl, &old, SCE_CTRL_DOWN)) sel = (sel+1)%2;
        if (btn_pressed(&ctrl, &old, SCE_CTRL_UP))   sel = (sel+1)%2;
        if (btn_pressed(&ctrl, &old, SCE_CTRL_CROSS)) {
            OPTS[sel].fn(font);
            /* Drain held buttons after sub-screen returns so O/X don't
             * bleed into this screen and cascade back to main menu. */
            do { sceCtrlPeekBufferPositive(0, &old, 1); } while (old.buttons);
        }
        if (btn_pressed(&ctrl, &old, SCE_CTRL_CIRCLE)) { do { sceCtrlPeekBufferPositive(0, &old, 1); } while (old.buttons); return; }
        old = ctrl;
    }
}
