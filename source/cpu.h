#pragma once

/* ── Individual clock domain arrays ──────────────────────────────────────── */

/* ARM CPU — 8 steps */
extern const int CPU_ARM_FREQS[];
extern const int CPU_ARM_COUNT;

/* GPU (ES4) — 5 steps */
extern const int CPU_ES4_FREQS[];
extern const int CPU_ES4_COUNT;

/* BUS — 5 steps */
extern const int CPU_BUS_FREQS[];
extern const int CPU_BUS_COUNT;

/* XBR (crossbar) — 3 steps */
extern const int CPU_XBR_FREQS[];
extern const int CPU_XBR_COUNT;

/* ── Current selected indices (into the arrays above) ────────────────────── */
extern int g_cpu_arm_idx;
extern int g_cpu_es4_idx;
extern int g_cpu_bus_idx;
extern int g_cpu_xbr_idx;

/* ── API ──────────────────────────────────────────────────────────────────── */

/* Call once at startup — sets indices to match default 333/166 clocks */
void        cpu_init(void);

/* Apply all four domains immediately (called after any index change) */
void        cpu_apply(void);

/* Apply default preset: ARM=333, BUS=166, ES4=222, XBR=166 */
void        cpu_apply_default(void);

/* Apply powersave preset: all domains at minimum */
void        cpu_apply_powersave(void);

/* Save current indices to ux0:data/VitaRecovery/cpu_clocks.cfg */
void        cpu_save(void);

/* Load saved indices from file and apply them (call once at startup) */
void        cpu_load_and_apply(void);

/* Live readback from hardware */
int         cpu_get_arm_mhz(void);
int         cpu_get_bus_mhz(void);
int         cpu_get_gpu_mhz(void);

/* Self-contained CPU speed screen — call from menu */
void        cpu_screen_run(void);

/* Legacy preset API kept for compatibility */
int         cpu_get_preset_count(void);
const char *cpu_get_preset_name(int idx);
int         cpu_get_current_preset(void);
void        cpu_apply_preset(int idx);
int         cpu_get_current_arm_mhz(void);
int         cpu_get_current_bus_mhz(void);
