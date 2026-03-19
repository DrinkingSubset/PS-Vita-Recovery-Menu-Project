/*
 * cpu.c — Individual CPU/GPU/BUS/XBR clock control
 */

#include <psp2/power.h>
#include <psp2/ctrl.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "cpu.h"
#include "display.h"
#include "draw.h"
#include "menu.h"

/* ── Frequency tables ──────────────────────────────────────────────────── */
const int CPU_ARM_FREQS[] = { 41, 83, 111, 166, 222, 333, 444, 500 };
const int CPU_ARM_COUNT   = 8;

const int CPU_ES4_FREQS[] = { 41, 83, 111, 166, 222 };
const int CPU_ES4_COUNT   = 5;

const int CPU_BUS_FREQS[] = { 55, 83, 111, 166, 222 };
const int CPU_BUS_COUNT   = 5;

const int CPU_XBR_FREQS[] = { 83, 111, 166 };
const int CPU_XBR_COUNT   = 3;

/* ── Current indices ───────────────────────────────────────────────────── */
int g_cpu_arm_idx = 5;   /* 333 MHz */
int g_cpu_es4_idx = 4;   /* 222 MHz */
int g_cpu_bus_idx = 3;   /* 166 MHz */
int g_cpu_xbr_idx = 2;   /* 166 MHz */

/* ── Init ──────────────────────────────────────────────────────────────── */
void cpu_init(void) {
    int arm = scePowerGetArmClockFrequency();
    int bus = scePowerGetBusClockFrequency();
    int best, diff, d, i;

    best = 0; diff = 9999;
    for (i = 0; i < CPU_ARM_COUNT; i++) {
        d = CPU_ARM_FREQS[i] - arm; if (d < 0) d = -d;
        if (d < diff) { diff = d; best = i; }
    }
    g_cpu_arm_idx = best;

    best = 0; diff = 9999;
    for (i = 0; i < CPU_BUS_COUNT; i++) {
        d = CPU_BUS_FREQS[i] - bus; if (d < 0) d = -d;
        if (d < diff) { diff = d; best = i; }
    }
    g_cpu_bus_idx = best;

    g_cpu_es4_idx = CPU_ES4_COUNT - 1;
    g_cpu_xbr_idx = CPU_XBR_COUNT - 1;
}

/* ── Apply ─────────────────────────────────────────────────────────────── */
void cpu_apply(void) {
    scePowerSetArmClockFrequency (CPU_ARM_FREQS[g_cpu_arm_idx]);
    scePowerSetBusClockFrequency (CPU_BUS_FREQS[g_cpu_bus_idx]);
    scePowerSetGpuClockFrequency (CPU_ES4_FREQS[g_cpu_es4_idx]);
    scePowerSetGpuXbarClockFrequency(CPU_XBR_FREQS[g_cpu_xbr_idx]);
}

void cpu_apply_default(void) {
    g_cpu_arm_idx = 5;
    g_cpu_es4_idx = 4;
    g_cpu_bus_idx = 3;
    g_cpu_xbr_idx = 2;
    cpu_apply();
}

void cpu_apply_powersave(void) {
    g_cpu_arm_idx = 0;   /* 41 MHz  */
    g_cpu_es4_idx = 0;   /* 41 MHz  */
    g_cpu_bus_idx = 0;   /* 55 MHz  */
    g_cpu_xbr_idx = 0;   /* 83 MHz  */
    cpu_apply();
}

/* ── Persist clock settings ───────────────────────────────────────────── */
#define CPU_SAVE_DIR  "ux0:data/VitaRecovery"
#define CPU_SAVE_FILE "ux0:data/VitaRecovery/cpu_clocks.cfg"

void cpu_save(void) {
    /* Ensure directory exists */
    sceIoMkdir(CPU_SAVE_DIR, 0777);
    SceUID fd = sceIoOpen(CPU_SAVE_FILE,
                          SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
    if (fd < 0) return;
    char buf[64];
    int n = snprintf(buf, sizeof(buf), "arm=%d\nes4=%d\nbus=%d\nxbr=%d\n",
                     g_cpu_arm_idx, g_cpu_es4_idx,
                     g_cpu_bus_idx, g_cpu_xbr_idx);
    sceIoWrite(fd, buf, n);
    sceIoClose(fd);
}

void cpu_load_and_apply(void) {
    SceUID fd = sceIoOpen(CPU_SAVE_FILE, SCE_O_RDONLY, 0);
    if (fd < 0) {
        /* No saved file — apply default and save it */
        cpu_apply_default();
        cpu_save();
        return;
    }
    char buf[64];
    int n = sceIoRead(fd, buf, sizeof(buf) - 1);
    sceIoClose(fd);
    if (n <= 0) return;
    buf[n] = '\0';

    int arm = -1, es4 = -1, bus = -1, xbr = -1;
    char *p = buf;
    while (*p) {
        if      (strncmp(p, "arm=", 4) == 0) arm = atoi(p + 4);
        else if (strncmp(p, "es4=", 4) == 0) es4 = atoi(p + 4);
        else if (strncmp(p, "bus=", 4) == 0) bus = atoi(p + 4);
        else if (strncmp(p, "xbr=", 4) == 0) xbr = atoi(p + 4);
        while (*p && *p != '\n') p++;
        if (*p == '\n') p++;
    }

    /* Validate and clamp indices */
    if (arm >= 0 && arm < CPU_ARM_COUNT) g_cpu_arm_idx = arm;
    if (es4 >= 0 && es4 < CPU_ES4_COUNT) g_cpu_es4_idx = es4;
    if (bus >= 0 && bus < CPU_BUS_COUNT) g_cpu_bus_idx = bus;
    if (xbr >= 0 && xbr < CPU_XBR_COUNT) g_cpu_xbr_idx = xbr;

    cpu_apply();
}

/* ── Live readback ─────────────────────────────────────────────────────── */
int cpu_get_arm_mhz(void) { return scePowerGetArmClockFrequency(); }
int cpu_get_bus_mhz(void) { return scePowerGetBusClockFrequency(); }
int cpu_get_gpu_mhz(void) {
    int g = scePowerGetGpuClockFrequency();
    if (g <= 0) g = scePowerGetGpuXbarClockFrequency();
    return g;
}

/* ── Input helpers ─────────────────────────────────────────────────────── */
static int btn(SceCtrlData *now, SceCtrlData *old, unsigned int b) {
    return (now->buttons & b) && !(old->buttons & b);
}
static void drain(SceCtrlData *old) {
    do { sceCtrlPeekBufferPositive(0, old, 1); } while (old->buttons);
}

/* ── Row layout constants (all in pixels, FONT_W=16px per char) ─────────
 *
 *  x+  0  "ARM CPU:"   label    (9 chars = 144px)
 *  x+160  "<"          dec btn  (1 char  =  16px)
 *  x+192  "333 MHz"    value    (7 chars = 112px, ends at x+304)
 *  x+336  ">"          inc btn  (1 char, starts 32px after value end)
 *  x+368  "(live: 333 MHz)"     (rest of line)
 */
#define ROW_LABEL_X   0
#define ROW_DEC_X     160
#define ROW_VAL_X     192
#define ROW_INC_X     336
#define ROW_LIVE_X    368

static void draw_clock_row(int base_x, int y, const char *label,
                            int live_mhz, int sel_mhz,
                            int selected, int can_dec, int can_inc)
{
    uint32_t lbl_col = selected ? COLOR_SELECTED : COLOR_TEXT;
    uint32_t val_col = selected ? COLOR_SELECTED : COLOR_GREEN;
    uint32_t btn_col = val_col;
    uint32_t dim_col = COLOR_DIM;

    if (selected)
        display_rect(0, y - 2, SCREEN_W - 8, LINE_H + 4, COLOR_SEL_BG);

    /* Label */
    display_text(base_x + ROW_LABEL_X, y, lbl_col, label);

    /* < button */
    display_text(base_x + ROW_DEC_X, y,
                 can_dec ? btn_col : dim_col, "<");

    /* Value — always 7 chars wide ("%3d MHz") so INC_X never overlaps */
    char val[12];
    snprintf(val, sizeof(val), "%3d MHz", sel_mhz);
    display_text(base_x + ROW_VAL_X, y, val_col, val);

    /* > button — 32px past the end of the widest possible value */
    display_text(base_x + ROW_INC_X, y,
                 can_inc ? btn_col : dim_col, ">");

    /* Live readback */
    char live[24];
    snprintf(live, sizeof(live), "(live: %d MHz)", live_mhz);
    display_text(base_x + ROW_LIVE_X, y, dim_col, live);
}

/* ── Main CPU screen ───────────────────────────────────────────────────── */
void cpu_screen_run(void) {
    /* Indices are already set correctly by cpu_load_and_apply() at startup.
     * Do NOT call cpu_init() here — it reads the hardware clock which caps
     * at 444 and would incorrectly snap a 500MHz setting back to 444. */
    SceCtrlData old, ctrl;
    memset(&old, 0, sizeof(old));
    drain(&old);

    int sel = 0;

    const char *labels[]  = { "ARM CPU:", "GPU ES4:", "BUS:    ", "XBR:    " };
    const int  *freqs[]   = { CPU_ARM_FREQS, CPU_ES4_FREQS,
                               CPU_BUS_FREQS, CPU_XBR_FREQS };
    const int   counts[]  = { CPU_ARM_COUNT, CPU_ES4_COUNT,
                               CPU_BUS_COUNT, CPU_XBR_COUNT };
    int        *indices[] = { &g_cpu_arm_idx, &g_cpu_es4_idx,
                               &g_cpu_bus_idx, &g_cpu_xbr_idx };

    while (1) {
        /* scePowerGet* caps at 444 even when set to 500 — use our
         * applied index values so the display always reflects what we set. */
        int live[4] = {
            CPU_ARM_FREQS[g_cpu_arm_idx],
            CPU_ES4_FREQS[g_cpu_es4_idx],
            CPU_BUS_FREQS[g_cpu_bus_idx],
            CPU_XBR_FREQS[g_cpu_xbr_idx]
        };

        display_start();
        display_clear(COLOR_BG);

        /* Title bar */
        display_rect(0, 0, SCREEN_W, TITLE_LINE, COLOR_TITLE_BG);
        display_text(MENU_X, 6,  COLOR_TEXT,   "PS Vita Recovery Menu v1.0");
        display_text(MENU_X, 26, COLOR_YELLOW, "CPU Speed Control");
        display_hline(0, TITLE_LINE, SCREEN_W, COLOR_GREEN);

        /* Hint — two short lines, each under 45 chars */
        int hy = TITLE_H + 6;
        display_text(MENU_X, hy,          COLOR_DIM, "[Up/Down] Select   [L/R] Change value");
        display_text(MENU_X, hy + LINE_H, COLOR_DIM, "[Sq] Default   [Tri] PowerSave   [O] Back");
        display_hline(0, hy + LINE_H * 2 + 4, SCREEN_W, RGBA8(40, 40, 40, 255));

        /* Clock domain rows */
        int row_y = hy + LINE_H * 2 + 14;
        for (int i = 0; i < 4; i++) {
            draw_clock_row(
                MENU_X,
                row_y + i * (LINE_H + 10),
                labels[i],
                live[i],
                freqs[i][*indices[i]],
                (i == sel),
                (*indices[i] > 0),
                (*indices[i] < counts[i] - 1)
            );
        }

        /* Default note */
        int dy = row_y + 4 * (LINE_H + 10) + 8;
        display_hline(0, dy, SCREEN_W, RGBA8(40, 40, 40, 255));
        display_text(MENU_X, dy + 8, COLOR_DIM,
                     "Default: ARM 333 / ES4 222 / BUS 166 / XBR 166 MHz");

        if (CPU_ARM_FREQS[g_cpu_arm_idx] > 333)
            display_text(MENU_X, dy + 8 + LINE_H, COLOR_YELLOW,
                         "WARNING: Overclocking may cause instability.");

        /* Footer */
        display_rect(0, FOOTER_Y, SCREEN_W, SCREEN_H - FOOTER_Y, COLOR_TITLE_BG);
        display_hline(0, FOOTER_Y, SCREEN_W, COLOR_GREEN);
        display_text(MENU_X, FOOTER_Y + 7, COLOR_DIM,
                     "[L/R] Change   [Sq] Default   [Tri] PowerSave   [O] Back");
        display_end();

        sceCtrlPeekBufferPositive(0, &ctrl, 1);

        if (btn(&ctrl, &old, SCE_CTRL_UP))
            sel = (sel - 1 + 4) % 4;
        if (btn(&ctrl, &old, SCE_CTRL_DOWN))
            sel = (sel + 1) % 4;

        if (btn(&ctrl, &old, SCE_CTRL_LEFT)) {
            if (*indices[sel] > 0) {
                (*indices[sel])--;
                cpu_apply();
                cpu_save();
            }
        }
        if (btn(&ctrl, &old, SCE_CTRL_RIGHT)) {
            if (*indices[sel] < counts[sel] - 1) {
                (*indices[sel])++;
                cpu_apply();
                cpu_save();
            }
        }
        if (btn(&ctrl, &old, SCE_CTRL_SQUARE)) {
            cpu_apply_default();
            cpu_save();
        }
        if (btn(&ctrl, &old, SCE_CTRL_TRIANGLE)) {
            cpu_apply_powersave();
            cpu_save();
        }
        if (btn(&ctrl, &old, SCE_CTRL_CIRCLE))
            break;

        old = ctrl;
    }
    drain(&old);
}

/* ── Legacy preset API ─────────────────────────────────────────────────── */
typedef struct { const char *name; int arm; int bus; } CpuPreset;
static const CpuPreset g_presets[] = {
    { "Default (333 MHz / 166 MHz)", 333, 166 },
    { "Max (444 MHz / 222 MHz)",     444, 222 },
    { "Medium (222 MHz / 111 MHz)",  222, 111 },
    { "Low (111 MHz / 55 MHz)",      111,  55 },
    { "PowerSave (41 MHz / 20 MHz)",  41,  20 },
};
static const int g_preset_count = 5;
static int g_current_preset = 0;

int         cpu_get_preset_count(void)    { return g_preset_count; }
const char *cpu_get_preset_name(int i)    { return (i>=0&&i<g_preset_count)?g_presets[i].name:"???"; }
int         cpu_get_current_preset(void)  { return g_current_preset; }
int         cpu_get_current_arm_mhz(void) { return scePowerGetArmClockFrequency(); }
int         cpu_get_current_bus_mhz(void) { return scePowerGetBusClockFrequency(); }
void cpu_apply_preset(int idx) {
    if (idx < 0 || idx >= g_preset_count) return;
    g_current_preset = idx;
    scePowerSetArmClockFrequency(g_presets[idx].arm);
    scePowerSetBusClockFrequency(g_presets[idx].bus);
}
