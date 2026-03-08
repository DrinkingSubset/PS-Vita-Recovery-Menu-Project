#pragma once
#include <stdint.h>

/* Color presets for the selection highlight and star row */
typedef struct {
    const char *name;
    uint32_t    color;   /* RGBA8(r,g,b,255) format used by display layer */
} ColorPreset;

#define NUM_HIGHLIGHT_COLORS  7
#define NUM_STAR_COLORS       7

extern const ColorPreset HIGHLIGHT_COLORS[NUM_HIGHLIGHT_COLORS];
extern const ColorPreset STAR_COLORS[NUM_STAR_COLORS];

typedef struct {
    int highlight_idx;   /* index into HIGHLIGHT_COLORS */
    int star_idx;        /* index into STAR_COLORS      */
} AppSettings;

extern AppSettings g_settings;

void settings_load(void);
void settings_save(void);
