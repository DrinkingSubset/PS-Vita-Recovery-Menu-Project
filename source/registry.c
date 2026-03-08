#include <psp2/registrymgr.h>
#include <string.h>
#include "registry.h"

// Registry hacks table
static RegHack g_hacks[] = {
    {
        "/CONFIG/DISPLAY",
        "brightness",
        REG_TYPE_INT,
        "Max Display Brightness",
        0
    },
    {
        "/CONFIG/ACCESSIBILITY",
        "large_text",
        REG_TYPE_INT,
        "Large Text Mode",
        0
    },
    {
        "/CONFIG/NP",
        "env",
        REG_TYPE_INT,
        "NP Environment (0=prod)",
        0
    },
    {
        "/CONFIG/SYSTEM",
        "button_assign",
        REG_TYPE_INT,
        "Swap X/O Buttons",
        0
    },
};
static const int g_hack_count = sizeof(g_hacks) / sizeof(g_hacks[0]);

void registry_load_all(void) {
    for (int i = 0; i < g_hack_count; i++) {
        RegHack *h = &g_hacks[i];
        if (h->type == REG_TYPE_INT) {
            int val = 0;
            int size = sizeof(val);
            int ret = sceRegMgrGetKeyInt(h->reg_path, h->reg_key, &val);
            h->current_val = (ret >= 0) ? val : -1;
        }
    }
}

int registry_get_count(void) { return g_hack_count; }

RegHack *registry_get(int idx) {
    if (idx < 0 || idx >= g_hack_count) return NULL;
    return &g_hacks[idx];
}

/* Returns 0 on success, negative SCE error code on failure.
 * current_val is only updated if the write actually succeeded — prevents
 * the UI showing a toggled state when the registry write silently failed. */
int registry_toggle(int idx) {
    if (idx < 0 || idx >= g_hack_count) return -1;
    RegHack *h = &g_hacks[idx];
    int new_val = (h->current_val == 0) ? 1 : 0;
    int ret = sceRegMgrSetKeyInt(h->reg_path, h->reg_key, new_val);
    if (ret >= 0)
        h->current_val = new_val;
    return ret;
}
