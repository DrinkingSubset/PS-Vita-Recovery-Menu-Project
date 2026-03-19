#include "settings.h"
#include "display.h"
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <stdio.h>
#include <string.h>

#define SETTINGS_DIR  "ux0:data/vita_recovery"
#define SETTINGS_FILE "ux0:data/vita_recovery/settings.cfg"

const ColorPreset HIGHLIGHT_COLORS[NUM_HIGHLIGHT_COLORS] = {
    { "Green",   RGBA8(  0, 180,   0, 255) },
    { "Blue",    RGBA8(  0,  80, 220, 255) },
    { "Purple",  RGBA8(120,   0, 200, 255) },
    { "Cyan",    RGBA8(  0, 180, 180, 255) },
    { "Red",     RGBA8(200,   0,   0, 255) },
    { "Orange",  RGBA8(220, 120,   0, 255) },
    { "White",   RGBA8(180, 180, 180, 255) },
};

const ColorPreset STAR_COLORS[NUM_STAR_COLORS] = {
    { "Green",   RGBA8(  0, 220,   0, 255) },
    { "Blue",    RGBA8(  0, 120, 255, 255) },
    { "Purple",  RGBA8(160,   0, 255, 255) },
    { "Cyan",    RGBA8(  0, 220, 220, 255) },
    { "Red",     RGBA8(220,   0,   0, 255) },
    { "Orange",  RGBA8(220,  80,  20, 255) },
    { "White",   RGBA8(220, 220, 220, 255) },
};

AppSettings g_settings = {
    .highlight_idx = 0,   /* Green */
    .star_idx      = 0,   /* Green */
};

void settings_load(void) {
    SceUID fd = sceIoOpen(SETTINGS_FILE, SCE_O_RDONLY, 0);
    if (fd < 0) return;
    char buf[64];
    int n = sceIoRead(fd, buf, sizeof(buf) - 1);
    sceIoClose(fd);
    if (n <= 0) return;
    buf[n] = '\0';

    int hi = 0, si = 0;
    /* Parse "highlight=N\nstars=N\n" */
    char *p = buf;
    while (*p) {
        if (strncmp(p, "highlight=", 10) == 0)
            hi = p[10] - '0';
        else if (strncmp(p, "stars=", 6) == 0)
            si = p[6] - '0';
        while (*p && *p != '\n') p++;
        if (*p == '\n') p++;
    }

    if (hi >= 0 && hi < NUM_HIGHLIGHT_COLORS) g_settings.highlight_idx = hi;
    if (si >= 0 && si < NUM_STAR_COLORS)      g_settings.star_idx      = si;
}

void settings_save(void) {
    sceIoMkdir(SETTINGS_DIR, 0777);
    SceUID fd = sceIoOpen(SETTINGS_FILE,
                          SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
    if (fd < 0) return;
    char buf[64];
    int n = snprintf(buf, sizeof(buf), "highlight=%d\nstars=%d\n",
                     g_settings.highlight_idx, g_settings.star_idx);
    sceIoWrite(fd, buf, n);
    sceIoClose(fd);
}
