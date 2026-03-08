#pragma once

typedef struct {
    const char *name;
    int arm_mhz;
    int bus_mhz;
} CpuPreset;

int         cpu_get_preset_count(void);
const char *cpu_get_preset_name(int idx);
int         cpu_get_current_preset(void);
void        cpu_apply_preset(int idx);
int         cpu_get_current_arm_mhz(void);
int         cpu_get_current_bus_mhz(void);
