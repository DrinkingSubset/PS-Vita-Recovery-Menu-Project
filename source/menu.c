#include <psp2/ctrl.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/power.h>
#include <psp2/appmgr.h>

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "menu.h"
#include "draw.h"
#include "plugins.h"
#include "sysinfo.h"
#include "cpu.h"
#include "registry.h"
#include "restore_screen.h"
#include "official_recovery.h"
#include "sd2vita.h"
#include "cheat_manager.h"
#include "filemanager.h"
#include "plugin_fix.h"
#include "diag.h"
#include "recovery_installer.h"
#include "compat.h"

// ── Forward declarations ──────────────────────────────────────────────────────
static Menu *build_main_menu(void);
static Menu *build_plugins_menu(void);
static Menu *build_advanced_menu(void);
static Menu *build_registry_menu(void);
static void build_plugins_menu_impl(Menu *parent);

static MenuItem g_main_items[16];
static Menu     g_main_menu;

static MenuItem g_advanced_items[14];
static Menu     g_advanced_menu;

// Plugins menu is dynamic (up to 64 plugins + 2 extra items)
static MenuItem  g_plugin_items[MAX_PLUGINS + 16];
static Menu      g_plugin_menu;

static MenuItem  g_registry_items[16];
static Menu      g_registry_menu;


static int g_show_sysinfo        = 0;
static int g_sysinfo_scroll       = 0;
static int g_show_restore        = 0;
static int g_show_official_rec   = 0;
static int g_show_sd2vita        = 0;
static int g_show_cheat_manager  = 0;
static int g_show_filemanager    = 0;
static int g_show_plugin_fix     = 0;
static int g_dev_unlock          = 0;   /* System Write Mode */
static int g_show_diag           = 0;
int g_safemode_applied = 0;   /* set by main.c if L-trigger safe mode ran */
static int g_show_ri             = 0;
static int g_exit_requested      = 0;

// ── Action callbacks ──────────────────────────────────────────────────────────
/* Show a simple red error message and wait for O press */
static void menu_show_error(const char *msg, int err_code) {
    char errbuf[48];
    snprintf(errbuf, sizeof(errbuf), "Error code: 0x%08X", (unsigned int)err_code);
    SceCtrlData old, ctrl;
    memset(&old, 0, sizeof(old));
    do { sceCtrlPeekBufferPositive(0, &old, 1); } while (old.buttons);
    while (1) {
        display_start();
        display_clear(COLOR_BG);
        display_rect(0, 0, SCREEN_W, TITLE_LINE, COLOR_TITLE_BG);
        display_text(MENU_X, 4,  COLOR_TEXT, "PS Vita Recovery Menu v1.0");
        display_hline(0, TITLE_LINE, SCREEN_W, COLOR_GREEN);
        display_text(MENU_X, TITLE_H + 20, COLOR_RED,  msg);
        display_text(MENU_X, TITLE_H + 44, COLOR_DIM,  errbuf);
        display_text(MENU_X, TITLE_H + 80, COLOR_TEXT, "Press O to continue.");
        display_end();
        sceCtrlPeekBufferPositive(0, &ctrl, 1);
        if ((ctrl.buttons & SCE_CTRL_CIRCLE) && !(old.buttons & SCE_CTRL_CIRCLE)) break;
        old = ctrl;
    }
}

static void action_continue(void)          { g_exit_requested = 1; }
static void action_reboot(void)            { scePowerRequestColdReset(); }
static void action_poweroff(void)          { scePowerRequestStandby(); }
static void action_show_sysinfo(void)      { g_show_sysinfo = 1; }
static void action_show_restore(void)      { g_show_restore = 1; }
static void action_show_official_rec(void) { g_show_official_rec = 1; }
static void action_show_sd2vita(void)      { g_show_sd2vita = 1; }
static void action_show_cheats(void)       { g_show_cheat_manager = 1; }
static void action_show_filemanager(void)  { g_show_filemanager = 1; }
static void action_show_plugin_fix(void)    { g_show_plugin_fix  = 1; }
int menu_dev_unlock_enabled(void)           { return g_dev_unlock; }
static void action_show_diag(void)          { g_show_diag = 1; }
static void action_show_ri(void)            { g_show_ri   = 1; }
static void action_clean_config(void) {
    // Show warning first
    display_start();
    display_clear(COLOR_BG);
    display_rect(0, 0, SCREEN_W, 44, COLOR_TITLE_BG);
    display_text(MENU_X, 10, COLOR_TEXT, "Clean Config");
    display_hline(0, 44, SCREEN_W, COLOR_GREEN);
    display_text(MENU_X, 80, COLOR_YELLOW, "This will:");
    display_text(MENU_X, 104, COLOR_TEXT, "  - Remove ALL duplicate plugin entries");
    display_text(MENU_X, 124, COLOR_TEXT, "  - Re-enable plugins whose files exist on disk");
    display_text(MENU_X, 144, COLOR_TEXT, "  - Rewrite config.txt cleanly");
    display_text(MENU_X, 174, COLOR_GREEN, "Press X to continue, O to cancel.");
    display_end();
    SceCtrlData pad;
    while (1) { sceCtrlReadBufferPositive(0, &pad, 1); if (pad.buttons) break; sceKernelDelayThread(50000); }
    int confirmed = (pad.buttons & SCE_CTRL_CROSS);
    while (1) { sceCtrlReadBufferPositive(0, &pad, 1); if (!pad.buttons) break; sceKernelDelayThread(50000); }
    if (!confirmed) return;

    int r = plugins_clean_config();
    display_start();
    display_clear(COLOR_BG);
    display_rect(0, 0, SCREEN_W, 44, COLOR_TITLE_BG);
    display_text(MENU_X, 10, COLOR_TEXT, "Clean Config");
    display_hline(0, 44, SCREEN_W, COLOR_GREEN);
    if (r < 0) {
        display_text(MENU_X, 140, COLOR_RED, "ERROR: Could not read/write config.txt");
    } else {
        display_text(MENU_X, 110, COLOR_GREEN, "Config cleaned successfully.");
        display_text(MENU_X, 134, COLOR_TEXT, "Duplicates removed, real plugins re-enabled.");
        display_text(MENU_X, 158, COLOR_YELLOW, "Reboot for kernel plugin changes.");
    }
    display_text(MENU_X, 210, COLOR_DIM, "Press any button to continue.");
    display_end();
    while (1) { sceCtrlReadBufferPositive(0, &pad, 1); if (pad.buttons) break; sceKernelDelayThread(50000); }
    while (1) { sceCtrlReadBufferPositive(0, &pad, 1); if (!pad.buttons) break; sceKernelDelayThread(50000); }
    build_plugins_menu_impl(g_plugin_menu.parent);
}

static void action_remove_duplicate_plugins(void) {
    int r = plugins_remove_duplicates();
    display_start();
    display_clear(COLOR_BG);
    display_rect(0, 0, SCREEN_W, 44, COLOR_TITLE_BG);
    display_text(MENU_X, 10, COLOR_TEXT, "Plugin Manager");
    display_hline(0, 44, SCREEN_W, COLOR_GREEN);
    if (r < 0) {
        display_text(MENU_X, 140, COLOR_RED, "ERROR: Could not read config.txt");
    } else if (r == 0) {
        display_text(MENU_X, 140, COLOR_GREEN, "No duplicates found - config is clean.");
    } else {
        char msg[64];
        snprintf(msg, sizeof(msg), "Removed %d duplicate entr%s from config.txt",
                 r, r == 1 ? "y" : "ies");
        display_text(MENU_X, 140, COLOR_GREEN, msg);
        display_text(MENU_X, 168, COLOR_TEXT, "Reload this menu to see changes.");
    }
    display_text(MENU_X, 220, COLOR_DIM, "Press any button to continue.");
    display_end();
    SceCtrlData pad;
    while (1) { sceCtrlReadBufferPositive(0, &pad, 1); if (pad.buttons) break; sceKernelDelayThread(50000); }
    while (1) { sceCtrlReadBufferPositive(0, &pad, 1); if (!pad.buttons) break; sceKernelDelayThread(50000); }
    // Reload plugin list after removing duplicates
    build_plugins_menu_impl(g_plugin_menu.parent);
}

static void action_save_plugins(void) {
    int r = plugins_save();
    // Show result on screen briefly
    display_start();
    display_clear(COLOR_BG);
    display_rect(0, 0, SCREEN_W, 34, COLOR_TITLE_BG);
    display_text(MENU_X, 10, COLOR_TEXT, "Plugin Manager");
    display_hline(0, 34, SCREEN_W, COLOR_GREEN);
    if (r == 0) {
        display_text(MENU_X, 120, COLOR_GREEN, "Changes saved to config.txt");
        display_text(MENU_X, 148, COLOR_TEXT, "Kernel plugins require reboot.");
        display_text(MENU_X, 176, COLOR_DIM, "Press any button to continue.");
    } else {
        display_text(MENU_X, 120, COLOR_RED, "ERROR: Could not save config.txt");
        display_text(MENU_X, 148, COLOR_TEXT, "Check that ur0:tai/ exists.");
        display_text(MENU_X, 176, COLOR_DIM, "Press any button to continue.");
    }
    display_end();
    SceCtrlData pad;
    while (1) {
        sceCtrlReadBufferPositive(0, &pad, 1);
        if (pad.buttons) break;
        sceKernelDelayThread(50000);
    }
    while (1) {
        sceCtrlReadBufferPositive(0, &pad, 1);
        if (!pad.buttons) break;
        sceKernelDelayThread(50000);
    }
}
static void action_shutdown(void)          { scePowerRequestStandby(); }
static void action_suspend(void)           { scePowerRequestSuspend(); }
static void action_reset_vsh(void) {
    /* Relaunch SceShell (LiveArea) without a full reboot.
     * "main" is the special taiHEN title ID for SceShell.
     * On failure the user can still reboot manually. */
    sceAppMgrLaunchAppByName(0, "main", NULL);
}

/* ── Dev unlock warning dialog ──────────────────────────────────────────── */
static void action_toggle_dev_unlock(void) {
    if (g_dev_unlock) {
        /* Already ON — turn off immediately, no prompt needed */
        g_dev_unlock = 0;
        return;
    }
    /* Turning ON — show full-screen warning and require confirmation */
    SceCtrlData old, ctrl;
    memset(&old, 0, sizeof(old));
    do { sceCtrlPeekBufferPositive(0, &old, 1); } while (old.buttons);
    while (1) {
        display_start();
        display_clear(COLOR_BG);
        display_rect(0, 0, SCREEN_W, TITLE_LINE, COLOR_TITLE_BG);
        display_text(MENU_X, 4,  COLOR_TEXT,   "PS Vita Recovery Menu v1.0");
        display_text(MENU_X, 26, COLOR_RED,    "Advanced -- System Write Mode");
        display_hline(0, TITLE_LINE, SCREEN_W, COLOR_GREEN);
        /* Warning box */
        int bx = MENU_X - 8;
        int by = TITLE_H + 20;
        int bw = SCREEN_W - bx * 2;
        int bh = LINE_H * 11;
        display_rect(bx, by, bw, bh, RGBA8(60, 0, 0, 255));
        display_rect(bx, by, bw, 3,  RGBA8(200, 0, 0, 255));
        int ty = by + 10;
        display_text(MENU_X, ty,                COLOR_YELLOW, "!! WARNING: SYSTEM WRITE MODE !!");
        display_text(MENU_X, ty + LINE_H * 2,  COLOR_TEXT,   "This enables write access to system");
        display_text(MENU_X, ty + LINE_H * 3,  COLOR_TEXT,   "partitions: os0, vs0, sa0, tm0, pd0");
        display_text(MENU_X, ty + LINE_H * 5,  COLOR_TEXT,   "Deleting or overwriting files on");
        display_text(MENU_X, ty + LINE_H * 6,  COLOR_TEXT,   "these partitions CAN BRICK your Vita.");
        display_text(MENU_X, ty + LINE_H * 8,  COLOR_GREEN,  "[X] I understand -- Enable");
        display_text(MENU_X, ty + LINE_H * 9,  COLOR_DIM,    "[O] Cancel");
        display_rect(0, FOOTER_Y, SCREEN_W, SCREEN_H - FOOTER_Y, COLOR_TITLE_BG);
        display_hline(0, FOOTER_Y, SCREEN_W, COLOR_GREEN);
        display_text(MENU_X, FOOTER_Y + 8, COLOR_DIM,
                     "[X] Enable System Write Mode    [O] Cancel");
        display_end();
        sceCtrlPeekBufferPositive(0, &ctrl, 1);
        if ((ctrl.buttons & SCE_CTRL_CROSS)  && !(old.buttons & SCE_CTRL_CROSS))  { g_dev_unlock = 1; break; }
        if ((ctrl.buttons & SCE_CTRL_CIRCLE) && !(old.buttons & SCE_CTRL_CIRCLE)) break;
        old = ctrl;
    }
}

// ── Menu builders ─────────────────────────────────────────────────────────────



static void action_cpu_screen(void) { cpu_screen_run(); }

// Storage for section header labels (static so pointers stay valid)
static char g_section_labels[8][32];
static int  g_section_label_count = 0;

static void build_plugins_menu_impl(Menu *parent) {
    plugins_load();
    int count = plugins_get_count();
    int n = 0;
    char last_section[64] = "";
    g_section_label_count = 0;

    for (int i = 0; i < count && n < MAX_PLUGINS + 16; i++) {
        const char *sec = plugins_get_section(i);
        // Insert section header when section changes
        if (strcmp(sec, last_section) != 0) {
            snprintf(last_section, sizeof(last_section), "%s", sec);
            if (g_section_label_count < 8) {
                snprintf(g_section_labels[g_section_label_count], 32, "*%s", sec);
                g_plugin_items[n].label   = g_section_labels[g_section_label_count++];
                g_plugin_items[n].type    = ITEM_HEADER;
                g_plugin_items[n].action  = NULL;
                g_plugin_items[n].submenu = NULL;
                n++;
            }
        }
        g_plugin_items[n].label   = plugins_get_name(i);
        /* Essential plugins shown as ITEM_ACTION (not toggleable) */
        g_plugin_items[n].type    = plugins_is_essential(i) ? ITEM_ACTION : ITEM_TOGGLE;
        g_plugin_items[n].enabled = plugins_is_enabled(i);
        g_plugin_items[n].action  = NULL;
        g_plugin_items[n].submenu = NULL;
        n++;
    }
    if (count == 0) {
        g_plugin_items[n].label   = "(No plugins found)";
        g_plugin_items[n].type    = ITEM_ACTION;
        g_plugin_items[n].action  = NULL;
        g_plugin_items[n].submenu = NULL;
        n++;
    }

    // Separator before actions
    g_plugin_items[n].label   = "";
    g_plugin_items[n].type    = ITEM_SEPARATOR;
    g_plugin_items[n].action  = NULL;
    g_plugin_items[n].submenu = NULL;
    n++;

    g_plugin_items[n].label   = "Clean Config (Fix AutoPlugin2)";
    g_plugin_items[n].type    = ITEM_ACTION;
    g_plugin_items[n].action  = action_clean_config;
    g_plugin_items[n].submenu = NULL;
    n++;

    g_plugin_items[n].label   = "Remove Duplicates";
    g_plugin_items[n].type    = ITEM_ACTION;
    g_plugin_items[n].action  = action_remove_duplicate_plugins;
    g_plugin_items[n].submenu = NULL;
    n++;

    g_plugin_items[n].label   = "Save Changes";
    g_plugin_items[n].type    = ITEM_ACTION;
    g_plugin_items[n].action  = action_save_plugins;
    g_plugin_items[n].submenu = NULL;
    n++;

    g_plugin_items[n].label   = "< Back";
    g_plugin_items[n].type    = ITEM_BACK;
    g_plugin_items[n].action  = NULL;
    g_plugin_items[n].submenu = NULL;
    n++;

    g_plugin_menu.title    = plugins_get_config_path();
    g_plugin_menu.subtitle = "Plugins";
    g_plugin_menu.items    = g_plugin_items;
    g_plugin_menu.count    = n;
    g_plugin_menu.selected = 0;
    g_plugin_menu.scroll   = 0;
    g_plugin_menu.parent   = parent;
}

static void build_registry_menu_impl(Menu *parent) {
    registry_load_all();
    int count = registry_get_count();
    int n = 0;

    for (int i = 0; i < count && n < 14; i++) {
        RegHack *h = registry_get(i);
        g_registry_items[n].label   = h->label;
        g_registry_items[n].type    = ITEM_TOGGLE;
        g_registry_items[n].enabled = (h->current_val == 1);
        g_registry_items[n].action  = NULL;
        g_registry_items[n].submenu = NULL;
        n++;
    }

    g_registry_items[n].label   = "< Back";
    g_registry_items[n].type    = ITEM_BACK;
    g_registry_items[n].action  = NULL;
    g_registry_items[n].submenu = NULL;
    n++;

    g_registry_menu.title    = "Registry Hacks";
    g_registry_menu.items    = g_registry_items;
    g_registry_menu.count    = n;
    g_registry_menu.selected = 0;
    g_registry_menu.parent   = parent;
}

static void build_advanced_menu_impl(Menu *main) {
    build_registry_menu_impl(&g_advanced_menu);

    int n = 0;
    g_advanced_items[n++] = (MenuItem){ "CPU Speed",       ITEM_ACTION,    0, action_cpu_screen, NULL             };
    g_advanced_items[n++] = (MenuItem){ "Registry Hacks",  ITEM_SUBMENU,   0, NULL,              &g_registry_menu };
    g_advanced_items[n++] = (MenuItem){ "",                ITEM_SEPARATOR, 0, NULL,              NULL             };
    g_advanced_items[n++] = (MenuItem){ "Reset VSH",       ITEM_ACTION,    0, action_reset_vsh,  NULL             };
    g_advanced_items[n++] = (MenuItem){ "Suspend Device",  ITEM_ACTION,    0, action_suspend,    NULL             };
    g_advanced_items[n++] = (MenuItem){ "Shut Down",       ITEM_ACTION,    0, action_shutdown,   NULL             };
    g_advanced_items[n++] = (MenuItem){ "Reset Device",    ITEM_ACTION,    0, action_reboot,     NULL             };
    g_advanced_items[n++] = (MenuItem){ "",                ITEM_SEPARATOR, 0, NULL,              NULL             };
    g_advanced_items[n++] = (MenuItem){ "Boot Diagnostics",  ITEM_ACTION,  0, action_show_diag,         NULL      };
    g_advanced_items[n++] = (MenuItem){ "Boot Recovery",     ITEM_ACTION,  0, action_show_ri,           NULL      };
    g_advanced_items[n++] = (MenuItem){ "System Write Mode", ITEM_TOGGLE,  0, action_toggle_dev_unlock, NULL      };
    g_advanced_items[n++] = (MenuItem){ "< Back",          ITEM_BACK,      0, NULL,              NULL             };

    g_advanced_menu.title    = "Advanced";
    g_advanced_menu.items    = g_advanced_items;
    g_advanced_menu.count    = n;
    g_advanced_menu.selected = 0;
    g_advanced_menu.parent   = main;
}

static void build_main_menu_impl(void) {
    build_advanced_menu_impl(&g_main_menu);
    build_plugins_menu_impl(&g_main_menu);

    int n = 0;
    g_main_items[n++] = (MenuItem){ "Exit to LiveArea",      ITEM_ACTION,   0, action_continue,          NULL };
    g_main_items[n++] = (MenuItem){ "",                     ITEM_SEPARATOR,0, NULL,                     NULL };
    g_main_items[n++] = (MenuItem){ "Plugins",              ITEM_SUBMENU,  0, NULL,                     &g_plugin_menu };
    g_main_items[n++] = (MenuItem){ "Advanced",             ITEM_SUBMENU,  0, NULL,                     &g_advanced_menu };
    g_main_items[n++] = (MenuItem){ "System Info",          ITEM_ACTION,   0, action_show_sysinfo,      NULL };
    g_main_items[n++] = (MenuItem){ "Restore / Unbrick",    ITEM_ACTION,   0, action_show_restore,      NULL };
    g_main_items[n++] = (MenuItem){ "Plugin Fix Mode",       ITEM_ACTION,   0, action_show_plugin_fix,   NULL };
    g_main_items[n++] = (MenuItem){ "Sony Recovery",        ITEM_ACTION,   0, action_show_official_rec, NULL };
    g_main_items[n++] = (MenuItem){ "Storage Manager",      ITEM_ACTION,   0, action_show_sd2vita,      NULL };
    g_main_items[n++] = (MenuItem){ "File Manager",     ITEM_ACTION,   0, action_show_filemanager,  NULL };
    g_main_items[n++] = (MenuItem){ "Cheat Manager",    ITEM_ACTION,   0, action_show_cheats,       NULL };
    g_main_items[n++] = (MenuItem){ "",                     ITEM_SEPARATOR,0, NULL,                     NULL };
    g_main_items[n++] = (MenuItem){ "Reboot",               ITEM_ACTION,   0, action_reboot,            NULL };
    g_main_items[n++] = (MenuItem){ "Power Off",            ITEM_ACTION,   0, action_poweroff,          NULL };

    g_main_menu.title    = "Main Menu";
    g_main_menu.items    = g_main_items;
    g_main_menu.count    = n;
    g_main_menu.selected = 0;
    g_main_menu.parent   = NULL;
}

// ── Input ─────────────────────────────────────────────────────────────────────

static int pressed(SceCtrlData *now, SceCtrlData *old, unsigned int btn) {
    return (now->buttons & btn) && !(old->buttons & btn);
}

void menu_handle_input(Menu **current, SceCtrlData *old) {
    SceCtrlData ctrl;
    sceCtrlPeekBufferPositive(0, &ctrl, 1);

    Menu *m = *current;

    /* Match draw.c exactly: H_ITEM=LINE_H=24, CONTENT_TOP=46, CONTENT_BOT=514 */
    #define _H_ITEM 24
    #define _H_SEP   8
    #define _CBOT   514
    #define _CTOP    46
    #define _CH     (_CBOT - _CTOP)   /* 468px visible area */

    if (pressed(&ctrl, old, SCE_CTRL_DOWN)) {
        int guard = 0;
        do {
            m->selected = (m->selected + 1) % m->count;
        } while (++guard < m->count &&
                 (m->items[m->selected].type == ITEM_SEPARATOR ||
                  m->items[m->selected].type == ITEM_HEADER));
        // Scroll down: ensure selected item is visible
        // Calculate pixel offset of selected item from scroll
        while (1) {
            int y = 0;
            for (int i = m->scroll; i < m->selected; i++)
                y += (m->items[i].type == ITEM_SEPARATOR) ? _H_SEP : _H_ITEM;
            if (y + _H_ITEM <= _CH) break;
            m->scroll++;
        }
    }

    if (pressed(&ctrl, old, SCE_CTRL_UP)) {
        int guard = 0;
        do {
            m->selected = (m->selected - 1 + m->count) % m->count;
        } while (++guard < m->count &&
                 (m->items[m->selected].type == ITEM_SEPARATOR ||
                  m->items[m->selected].type == ITEM_HEADER));
        // Scroll up: if selected is above scroll, move scroll up
        if (m->selected < m->scroll)
            m->scroll = m->selected;
        // Also handle wrap-around: jumped from bottom to top
        if (m->selected == 0)
            m->scroll = 0;
    }

    if (pressed(&ctrl, old, SCE_CTRL_CROSS)) {
        MenuItem *item = &m->items[m->selected];

        switch (item->type) {
            case ITEM_ACTION:
                if (item->action) item->action();
                break;

            case ITEM_TOGGLE:
                // Check if this is a plugin item
                if (m == &g_plugin_menu) {
                    int plugin_idx = m->selected;
                    plugins_toggle(plugin_idx);
                    g_plugin_items[plugin_idx].enabled ^= 1;
                }
                // Check if this is a registry item
                else if (m == &g_registry_menu) {
                    int rr = registry_toggle(m->selected);
                    if (rr >= 0)
                        g_registry_items[m->selected].enabled ^= 1;
                    else
                        menu_show_error("Registry write failed", rr);
                }
                // Dev unlock toggle in advanced menu
                else if (m == &g_advanced_menu) {
                    MenuItem *it = &m->items[m->selected];
                    if (it->action) {
                        it->action();  /* warning dialog handles its own logic */
                        it->enabled = g_dev_unlock;
                    }
                }
                break;

            case ITEM_SUBMENU:
                if (item->submenu) {
                    item->submenu->parent = m;
                    *current = item->submenu;
                }
                break;

            case ITEM_BACK:
                if (m->parent) *current = m->parent;
                break;

            default: break;
        }
    }

    // O = back
    if (pressed(&ctrl, old, SCE_CTRL_CIRCLE)) {
        if (m->parent) *current = m->parent;
    }

    // Triangle = exit
    if (pressed(&ctrl, old, SCE_CTRL_TRIANGLE)) {
        g_exit_requested = 1;
    }

    *old = ctrl;
}

// ── Main loop ─────────────────────────────────────────────────────────────────

void menu_run(void) {

    build_main_menu_impl();

    Menu *current = &g_main_menu;
    SceCtrlData old_ctrl;
    memset(&old_ctrl, 0, sizeof(old_ctrl));

    while (!g_exit_requested) {
        /* Hand off to full-screen overlays BEFORE starting a new frame */
        if (g_show_restore) {
            g_show_restore = 0;
            restore_screen_run(NULL);
            memset(&old_ctrl, 0, sizeof(old_ctrl));
            continue;
        }
        if (g_show_official_rec) {
            g_show_official_rec = 0;
            official_recovery_screen_run(NULL);
            memset(&old_ctrl, 0, sizeof(old_ctrl));
            continue;
        }
        if (g_show_sd2vita) {
            g_show_sd2vita = 0;
            sd2vita_screen_run(NULL);
            memset(&old_ctrl, 0, sizeof(old_ctrl));
            continue;
        }
        if (g_show_cheat_manager) {
            g_show_cheat_manager = 0;
            cheat_manager_screen_run(NULL);
            memset(&old_ctrl, 0, sizeof(old_ctrl));
            continue;
        }
        if (g_show_filemanager) {
            g_show_filemanager = 0;
            filemanager_screen_run(NULL);
            memset(&old_ctrl, 0, sizeof(old_ctrl));
            continue;
        }
        if (g_show_plugin_fix) {
            g_show_plugin_fix = 0;
            plugin_fix_run(NULL);
            memset(&old_ctrl, 0, sizeof(old_ctrl));
            continue;
        }
        if (g_show_diag) {
            g_show_diag = 0;
            diag_run();
            /* Drain: wait for all buttons released so O doesn't fire back handler */
            do { sceCtrlPeekBufferPositive(0, &old_ctrl, 1); } while (old_ctrl.buttons);
            continue;
        }
        if (g_show_ri) {
            g_show_ri = 0;
            recovery_installer_run();
            do { sceCtrlPeekBufferPositive(0, &old_ctrl, 1); } while (old_ctrl.buttons);
            continue;
        }

        display_start();

        if (g_show_sysinfo) {
            sysinfo_draw(NULL, g_sysinfo_scroll);
            SceCtrlData ctrl;
            sceCtrlPeekBufferPositive(0, &ctrl, 1);
            if ((ctrl.buttons & SCE_CTRL_CIRCLE) && !(old_ctrl.buttons & SCE_CTRL_CIRCLE)) {
                g_show_sysinfo = 0;
                g_sysinfo_scroll = 0;
            }
            if ((ctrl.buttons & SCE_CTRL_DOWN) && !(old_ctrl.buttons & SCE_CTRL_DOWN))
                if (g_sysinfo_scroll < SYSINFO_MAX_SCROLL) g_sysinfo_scroll += 24;
            if ((ctrl.buttons & SCE_CTRL_UP) && !(old_ctrl.buttons & SCE_CTRL_UP))
                if (g_sysinfo_scroll > 0) g_sysinfo_scroll -= 24;
            old_ctrl = ctrl;
        } else {
            draw_menu(NULL, current);

            /* Safe Mode boot banner — drawn after draw_menu so it overlays
             * the last content row rather than being wiped by display_clear */
            if (g_safemode_applied && current == &g_main_menu) {
                display_rect(0, FOOTER_Y - LINE_H - 4, SCREEN_W, LINE_H + 4,
                             RGBA8(60, 30, 0, 255));
                display_hline(0, FOOTER_Y - LINE_H - 4, SCREEN_W, COLOR_YELLOW);
                display_text(MENU_X, FOOTER_Y - LINE_H + 4,
                             COLOR_YELLOW,
                             "L-Trigger boot: Safe Mode applied. Plugins disabled.");
            }

            menu_handle_input(&current, &old_ctrl);
        }

        display_end();
    
    }
}
