#include <psp2/power.h>
#include "cpu.h"

// PSP-style CPU presets for Vita
static const CpuPreset g_presets[] = {
    { "Default (333 MHz / 166 MHz)", 333, 166 },
    { "Max (444 MHz / 222 MHz)",     444, 222 },
    { "Medium (222 MHz / 111 MHz)",  222, 111 },
    { "Low (111 MHz / 55 MHz)",      111,  55 },
    { "PowerSave (41 MHz / 20 MHz)",  41,  20 },
};
static const int g_preset_count = sizeof(g_presets) / sizeof(g_presets[0]);
static int g_current_preset = 0;

int cpu_get_preset_count(void) { return g_preset_count; }

const char *cpu_get_preset_name(int idx) {
    if (idx < 0 || idx >= g_preset_count) return "???";
    return g_presets[idx].name;
}

int cpu_get_current_preset(void) { return g_current_preset; }

void cpu_apply_preset(int idx) {
    if (idx < 0 || idx >= g_preset_count) return;
    g_current_preset = idx;
    scePowerSetArmClockFrequency(g_presets[idx].arm_mhz);
    scePowerSetBusClockFrequency(g_presets[idx].bus_mhz);
}

int cpu_get_current_arm_mhz(void) {
    return scePowerGetArmClockFrequency();
}

int cpu_get_current_bus_mhz(void) {
    return scePowerGetBusClockFrequency();
}
