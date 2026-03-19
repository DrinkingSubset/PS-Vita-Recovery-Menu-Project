/*
 * plugin_fix.c -- Plugin Fix Mode for PS Vita Recovery Menu
 *
 * Purpose: dedicated recovery screen for fixing boot loops caused by
 *          bad or conflicting plugins in taiHEN config.txt.
 *
 * Options:
 *   Safe Mode       -- disable all non-essential plugins in one press
 *   View & Toggle   -- interactive per-plugin enable/disable list
 *   Re-enable All   -- undo Safe Mode, turn every non-essential plugin on
 *   Reset Config    -- write a bare minimal config (henkaku only)
 */

#include <psp2/ctrl.h>
#include <psp2/power.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "plugin_fix.h"
#include "plugins.h"
#include "draw.h"
#include "display.h"
#include "menu.h"

/* ── Layout constants (match global display.h) ───────────────────────── */
#define PF_TITLE_H    46
#define PF_TITLE_LINE 44
#define PF_FOOTER_Y   514
#define PF_FOOTER_LINE 512
#define PF_LIST_Y     (PF_TITLE_H + 6)
#define PF_ROW_H      26
#define PF_LIST_H     (PF_FOOTER_Y - PF_LIST_Y)
#define PF_MAX_ROWS   ((PF_LIST_H) / PF_ROW_H)

/* ── Helpers ──────────────────────────────────────────────────────────── */
static int pf_btn(SceCtrlData *n, SceCtrlData *o, unsigned int b) {
    return (n->buttons & b) && !(o->buttons & b);
}

static void pf_drain(SceCtrlData *old) {
    do { sceCtrlPeekBufferPositive(0, old, 1); } while (old->buttons);
}

static void pf_titlebar(const char *subtitle) {
    display_rect(0, 0, SCREEN_W, PF_TITLE_LINE, COLOR_TITLE_BG);
    display_text(MENU_X, 6,  COLOR_TEXT, "PS Vita Recovery Menu v1.0");
    display_text(MENU_X, 26, COLOR_YELLOW, subtitle[0] ? subtitle : "Plugin Fix Mode");
    display_hline(0, PF_TITLE_LINE, SCREEN_W, COLOR_GREEN);
}

static void pf_footer(const char *hint) {
    display_rect(0, PF_FOOTER_Y, SCREEN_W, SCREEN_H - PF_FOOTER_Y, COLOR_TITLE_BG);
    display_hline(0, PF_FOOTER_LINE, SCREEN_W, COLOR_GREEN);
    display_text(MENU_X, PF_FOOTER_Y + 7, COLOR_DIM, hint);
}

/* Simple full-screen message, press O to dismiss */
static void pf_message(const char *line1, const char *line2, uint32_t col) {
    SceCtrlData old, ctrl;
    memset(&old, 0, sizeof(old));
    pf_drain(&old);
    while (1) {
        display_start(); display_clear(COLOR_BG);
        pf_titlebar("Plugin Fix Mode");
        display_text(MENU_X, PF_LIST_Y + 8,  col,       line1);
        if (line2 && line2[0])
            display_text(MENU_X, PF_LIST_Y + 36, COLOR_DIM, line2);
        display_text(MENU_X, PF_LIST_Y + 80, COLOR_DIM, "Press O to continue.");
        pf_footer("[O] OK");
        display_end();
        sceCtrlPeekBufferPositive(0, &ctrl, 1);
        if (pf_btn(&ctrl, &old, SCE_CTRL_CIRCLE)) break;
        old = ctrl;
    }
}

/* Yes/No confirm. Returns 1=yes, 0=no */
static int pf_confirm(const char *q) {
    SceCtrlData old, ctrl;
    memset(&old, 0, sizeof(old));
    pf_drain(&old);
    while (1) {
        display_start(); display_clear(COLOR_BG);
        pf_titlebar("Plugin Fix Mode -- Confirm");
        int y = PF_LIST_Y + 8;
        display_text(MENU_X, y,                COLOR_YELLOW, "!! CONFIRM !!");
        display_text(MENU_X, y + PF_ROW_H,    COLOR_TEXT,   q);
        display_text(MENU_X, y + PF_ROW_H*3,  COLOR_GREEN,  "[X] Confirm");
        display_text(MENU_X, y + PF_ROW_H*4,  COLOR_DIM,    "[O] Cancel");
        pf_footer("[X] Confirm    [O] Cancel");
        display_end();
        sceCtrlPeekBufferPositive(0, &ctrl, 1);
        if (pf_btn(&ctrl, &old, SCE_CTRL_CROSS))  return 1;
        if (pf_btn(&ctrl, &old, SCE_CTRL_CIRCLE)) return 0;
        old = ctrl;
    }
}

/* ── Safe Mode: disable all non-essential plugins ──────────────────────── */
static void pf_safe_mode(void) {
    if (!pf_confirm("Disable all non-essential plugins?")) return;
    int n = plugins_get_count();
    if (n <= 0) {
        pf_message("No plugins found in config.", "Is the config path correct?", COLOR_YELLOW);
        return;
    }
    int changed = 0;
    for (int i = 0; i < n; i++) {
        if (!plugins_is_essential(i) && plugins_is_enabled(i)) {
            plugins_toggle(i);
            changed++;
        }
    }
    int r = plugins_save();
    if (r < 0) {
        pf_message("ERROR: Could not save config.txt",
                   "Check that the tai folder exists.", COLOR_RED);
        return;
    }
    char msg[64];
    snprintf(msg, sizeof(msg), "Disabled %d plugin%s.", changed, changed == 1 ? "" : "s");
    pf_message(msg, "Reboot for kernel plugin changes to take effect.", COLOR_GREEN);
}

/* ── Re-enable All: undo safe mode ─────────────────────────────────────── */
static void pf_reenable_all(void) {
    if (!pf_confirm("Re-enable ALL non-essential plugins?")) return;
    int n = plugins_get_count();
    if (n <= 0) {
        pf_message("No plugins found in config.", "", COLOR_YELLOW);
        return;
    }
    int changed = 0;
    for (int i = 0; i < n; i++) {
        if (!plugins_is_essential(i) && !plugins_is_enabled(i)) {
            plugins_toggle(i);
            changed++;
        }
    }
    int r = plugins_save();
    if (r < 0) {
        pf_message("ERROR: Could not save config.txt", "", COLOR_RED);
        return;
    }
    char msg[64];
    snprintf(msg, sizeof(msg), "Re-enabled %d plugin%s.", changed, changed == 1 ? "" : "s");
    pf_message(msg, "Reboot for kernel plugin changes to take effect.", COLOR_GREEN);
}

/* ── Reset to Minimal Config ────────────────────────────────────────────── */
/* Writes the bare minimum config needed for HENkaku to boot.              */
/* WARNING: this overwrites config.txt completely.                          */
/* ── Backup / Restore config ────────────────────────────────────────────── */
#define PF_BACKUP_PATH "ur0:tai/config_backup.txt"
#define PF_COPY_BUF    (16 * 1024)   /* 16KB — plenty for any config.txt */

static int pf_backup_exists(void) {
    SceIoStat st;
    return (sceIoGetstat(PF_BACKUP_PATH, &st) >= 0);
}

/* Copy src -> dst atomically (write to .tmp then rename) */
static int pf_copy_file(const char *src, const char *dst) {
    SceUID fdin = sceIoOpen(src, SCE_O_RDONLY, 0);
    if (fdin < 0) return -1;
    int sz = (int)sceIoLseek(fdin, 0, SCE_SEEK_END);
    sceIoLseek(fdin, 0, SCE_SEEK_SET);
    if (sz <= 0 || sz > PF_COPY_BUF) { sceIoClose(fdin); return -2; }
    char *buf = malloc(sz + 1);
    if (!buf) { sceIoClose(fdin); return -3; }
    int rd = sceIoRead(fdin, buf, sz);
    sceIoClose(fdin);
    if (rd != sz) { free(buf); return -4; }
    /* Write atomically via .tmp */
    char tmp[256];
    snprintf(tmp, sizeof(tmp), "%s.tmp", dst);
    SceUID fdout = sceIoOpen(tmp, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
    if (fdout < 0) { free(buf); return -5; }
    int wr = sceIoWrite(fdout, buf, sz);
    sceIoClose(fdout);
    free(buf);
    if (wr != sz) { sceIoRemove(tmp); return -6; }
    sceIoRemove(dst);
    return sceIoRename(tmp, dst);
}

static void pf_backup_config(void) {
    const char *src = plugins_get_config_path();
    if (!src || src[0] == '(') {
        pf_message("ERROR: No active config path found.", "", COLOR_RED);
        return;
    }
    /* Warn if overwriting existing backup */
    if (pf_backup_exists()) {
        if (!pf_confirm("Overwrite existing backup?")) return;
    }
    int r = pf_copy_file(src, PF_BACKUP_PATH);
    if (r >= 0)
        pf_message("Backup saved to:", PF_BACKUP_PATH, COLOR_GREEN);
    else {
        char msg[64];
        snprintf(msg, sizeof(msg), "Backup failed (err %d).", r);
        pf_message(msg, "Check ur0:tai/ exists.", COLOR_RED);
    }
}

static void pf_restore_config(void) {
    if (!pf_backup_exists()) {
        pf_message("No backup found.", PF_BACKUP_PATH " does not exist.", COLOR_YELLOW);
        return;
    }
    const char *dst = plugins_get_config_path();
    if (!dst || dst[0] == '(') {
        pf_message("ERROR: No active config path found.", "", COLOR_RED);
        return;
    }
    if (!pf_confirm("Restore backup over current config.txt?")) return;
    int r = pf_copy_file(PF_BACKUP_PATH, dst);
    if (r >= 0) {
        pf_message("Config restored from backup.",
                   "Reboot for kernel plugin changes.", COLOR_GREEN);
        plugins_load();
    } else {
        char msg[64];
        snprintf(msg, sizeof(msg), "Restore failed (err %d).", r);
        pf_message(msg, "Backup file may be corrupted.", COLOR_RED);
    }
}

static void pf_reset_config(void) {
    if (!pf_confirm("OVERWRITE config.txt with minimal HENkaku config?"))
        return;

    const char *config_path = plugins_get_config_path();
    if (!config_path || config_path[0] == '(') {
        pf_message("ERROR: No active config path found.", "", COLOR_RED);
        return;
    }

    /* Minimal config — just what HENkaku needs to boot */
    static const char MINIMAL[] =
        "# PSVita taiHEN config - reset by PS Vita Recovery Menu\n"
        "# henkaku.skprx is always loaded by taiHEN and not listed here\n"
        "# Kernel plugins require reboot to take effect\n"
        "\n"
        "*KERNEL\n"
        "\n"
        "*main\n"
        "ur0:tai/henkaku.suprx\n"
        "\n"
        "*NPXS10015\n"
        "ur0:tai/henkaku.suprx\n"
        "\n"
        "*NPXS10016\n"
        "ur0:tai/henkaku.suprx\n";

    /* Write atomically: tmp file then rename */
    char tmp[256];
    snprintf(tmp, sizeof(tmp), "%s.tmp", config_path);
    int len = (int)strlen(MINIMAL);
    SceUID fd = sceIoOpen(tmp, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
    if (fd < 0) {
        pf_message("ERROR: Cannot write config file.", "Check permissions.", COLOR_RED);
        return;
    }
    int wr = sceIoWrite(fd, MINIMAL, len);
    sceIoClose(fd);
    if (wr != len) {
        sceIoRemove(tmp);
        pf_message("Write error -- file unchanged.", "", COLOR_RED);
        return;
    }
    sceIoRemove(config_path);
    int mv = sceIoRename(tmp, config_path);
    if (mv < 0) {
        pf_message("Rename failed -- check *.tmp file.", "", COLOR_RED);
        return;
    }
    pf_message("Config reset to minimal HENkaku config.",
               "Reboot to apply. Re-add your plugins manually.", COLOR_GREEN);
    /* Reload plugin list to reflect new state */
    plugins_load();
}

/* ── View & Toggle: interactive per-plugin list ────────────────────────── */
/*
 * Shows all plugins grouped by section. Section headers are shown in yellow
 * and are not selectable. Plugin rows show [ON]/[OFF] on the right.
 * Essential plugins show [ON*] in cyan and cannot be toggled.
 *
 * Controls:
 *   Up/Down  -- navigate plugins (skips section headers)
 *   Triangle -- toggle selected plugin on/off
 *   Square   -- save changes to config.txt immediately
 *   O        -- exit (prompts if unsaved changes)
 */

/* Flat display entry: either a section header or a plugin row */
typedef struct {
    int is_header;      /* 1 = section header (not selectable) */
    int plugin_idx;     /* index into plugins_get_* API, -1 for headers */
    char label[128];    /* display text */
} PfRow;

#define PF_MAX_ROWS_LIST 128
static PfRow pf_rows[PF_MAX_ROWS_LIST];
static int   pf_row_count = 0;

static void pf_build_rows(void) {
    pf_row_count = 0;
    int n = plugins_get_count();
    if (n <= 0) return;

    /* Collect unique section names in order */
    char sections[32][64];
    int  section_count = 0;
    for (int i = 0; i < n; i++) {
        const char *sec = plugins_get_section(i);
        int found = 0;
        for (int s = 0; s < section_count; s++)
            if (strcmp(sections[s], sec) == 0) { found = 1; break; }
        if (!found && section_count < 32)
            snprintf(sections[section_count++], 64, "%s", sec);
    }

    /* Build display rows: header then plugins for each section */
    for (int s = 0; s < section_count && pf_row_count < PF_MAX_ROWS_LIST - 1; s++) {
        /* Section header row */
        PfRow *hr = &pf_rows[pf_row_count++];
        hr->is_header  = 1;
        hr->plugin_idx = -1;
        snprintf(hr->label, sizeof(hr->label), "* %s", sections[s]);

        for (int i = 0; i < n && pf_row_count < PF_MAX_ROWS_LIST; i++) {
            if (strcmp(plugins_get_section(i), sections[s]) != 0) continue;
            PfRow *pr = &pf_rows[pf_row_count++];
            pr->is_header  = 0;
            pr->plugin_idx = i;
            snprintf(pr->label, sizeof(pr->label), "  %s", plugins_get_name(i));
        }
    }
}

/* Return the next non-header row index, wrapping. Direction: +1 or -1 */
static int pf_next_plugin_row(int cur, int dir) {
    if (pf_row_count == 0) return 0;
    int next = cur + dir;
    for (int tries = 0; tries < pf_row_count; tries++) {
        if (next < 0) next = pf_row_count - 1;
        if (next >= pf_row_count) next = 0;
        if (!pf_rows[next].is_header) return next;
        next += dir;
    }
    return cur; /* fallback — all headers, shouldn't happen */
}

static void pf_view_toggle(void) {
    plugins_load(); /* always reload from disk before editing */
    pf_build_rows();
    if (pf_row_count == 0) {
        pf_message("No plugins found in config.", "Is the config loaded?", COLOR_YELLOW);
        return;
    }

    /* Start cursor on first plugin row (skip any leading header) */
    int sel = pf_next_plugin_row(-1, 1);
    int top = 0;
    int dirty = 0;
    SceCtrlData old, ctrl;
    memset(&old, 0, sizeof(old));
    pf_drain(&old);

    while (1) {
        display_start(); display_clear(COLOR_BG);

        /* Title */
        display_rect(0, 0, SCREEN_W, PF_TITLE_LINE, COLOR_TITLE_BG);
        display_text(MENU_X, 6,  COLOR_TEXT,
                     dirty ? "Plugin Fix -- View & Toggle  [unsaved]"
                           : "Plugin Fix -- View & Toggle");
        display_text(MENU_X, 26, COLOR_DIM, plugins_get_config_path());
        display_hline(0, PF_TITLE_LINE, SCREEN_W, COLOR_GREEN);

        /* Rows */
        int vis = PF_LIST_H / PF_ROW_H;
        /* Keep selected row visible */
        if (sel < top) top = sel;
        if (sel >= top + vis) top = sel - vis + 1;
        /* If top points at a header, nudge it back */
        while (top > 0 && pf_rows[top].is_header) top--;

        for (int i = 0; i < vis && top + i < pf_row_count; i++) {
            int idx = top + i;
            PfRow *row = &pf_rows[idx];
            int y = PF_LIST_Y + i * PF_ROW_H;

            if (row->is_header) {
                /* Section header — yellow, no highlight */
                display_text(MENU_X, y, COLOR_YELLOW, row->label);
                continue;
            }

            int pi      = row->plugin_idx;
            int is_sel  = (idx == sel);
            int enabled = plugins_is_enabled(pi);
            int essen   = plugins_is_essential(pi);

            if (is_sel)
                display_rect(0, y - 1, SCREEN_W - 8, PF_ROW_H + 1, COLOR_SEL_BG);

            uint32_t tc = is_sel ? COLOR_SELECTED : COLOR_TEXT;
            if (is_sel) display_text_transp(MENU_X, y, tc, row->label);
            else        display_text(MENU_X, y, tc, row->label);

            /* Status badge on the right */
            if (essen) {
                uint32_t bc = is_sel ? COLOR_SELECTED : RGBA8(0,220,220,255);
                if (is_sel) display_text_transp(SCREEN_W - 88, y, bc, "[ON*]");
                else        display_text(SCREEN_W - 88, y, bc, "[ON*]");
            } else {
                char badge[8];
                snprintf(badge, sizeof(badge), "[%s]", enabled ? "ON " : "OFF");
                uint32_t bc = is_sel ? COLOR_SELECTED
                              : (enabled ? COLOR_GREEN : COLOR_RED);
                if (is_sel) display_text_transp(SCREEN_W - 88, y, bc, badge);
                else        display_text(SCREEN_W - 88, y, bc, badge);
            }
        }

        /* Scrollbar counter */
        char sb[16];
        snprintf(sb, sizeof(sb), "%d/%d", sel + 1, pf_row_count);
        display_text(SCREEN_W - 80, PF_LIST_Y, COLOR_DIM, sb);

        pf_footer("[Tri] Toggle   [Sq] Save   [O] Exit");
        display_end();

        sceCtrlPeekBufferPositive(0, &ctrl, 1);

        if (pf_btn(&ctrl, &old, SCE_CTRL_DOWN))
            sel = pf_next_plugin_row(sel, 1);
        if (pf_btn(&ctrl, &old, SCE_CTRL_UP))
            sel = pf_next_plugin_row(sel, -1);

        /* Triangle — toggle */
        if (pf_btn(&ctrl, &old, SCE_CTRL_TRIANGLE)) {
            int pi = pf_rows[sel].plugin_idx;
            if (pi >= 0 && !plugins_is_essential(pi)) {
                plugins_toggle(pi);
                dirty = 1;
            }
        }

        /* Square — save immediately */
        if (pf_btn(&ctrl, &old, SCE_CTRL_SQUARE)) {
            int r = plugins_save();
            if (r == 0) {
                dirty = 0;
                pf_message("Saved successfully.",
                           "Kernel plugins require reboot.", COLOR_GREEN);
            } else {
                pf_message("ERROR: Could not save config.txt.", "", COLOR_RED);
            }
            pf_drain(&old);
        }

        /* O — exit (prompt if dirty) */
        if (pf_btn(&ctrl, &old, SCE_CTRL_CIRCLE)) {
            if (!dirty) break;
            if (pf_confirm("Exit without saving changes?")) break;
            pf_drain(&old);
        }

        old = ctrl;
    }
}

/* ── Main Plugin Fix Mode screen ────────────────────────────────────────── */

typedef struct {
    const char *label;
    const char *desc;
} PfOption;

static const PfOption PF_OPTIONS[] = {
    { "Safe Mode",        "Disable all non-essential plugins (keeps henkaku)" },
    { "View & Toggle",    "Browse and toggle individual plugins"               },
    { "Re-enable All",    "Turn all non-essential plugins back on"             },
    { "Reset to Minimal", "Overwrite config with bare HENkaku minimum"         },
    { "Backup Config",    "Save current config.txt to ur0:tai/config_backup.txt" },
    { "Restore Backup",   "Restore config from ur0:tai/config_backup.txt"        },
};
#define PF_OPTION_COUNT 6

void plugin_fix_run(void *font) {
    (void)font;

    /* Load plugins from disk at entry */
    plugins_load();

    int sel = 0;
    SceCtrlData old, ctrl;
    memset(&old, 0, sizeof(old));
    pf_drain(&old);

#define PF_WARN_Y    (PF_TITLE_H + 2)
#define PF_BANNER_H  (LINE_H * 3 + 8)
#define PF_SEP_Y     (PF_WARN_Y + PF_BANNER_H + 2)
#define PF_OPT_Y     (PF_SEP_Y + 6)
#define PF_OPT_ROW_H (LINE_H * 2 + 6)

    while (1) {
        display_start();
        display_clear(COLOR_BG);

        /* Title */
        display_rect(0, 0, SCREEN_W, PF_TITLE_LINE, COLOR_TITLE_BG);
        display_text(MENU_X, 6,  COLOR_TEXT,   "PS Vita Recovery Menu v1.0");
        display_text(MENU_X, 26, COLOR_YELLOW, "Plugin Fix Mode");
        display_hline(0, PF_TITLE_LINE, SCREEN_W, COLOR_GREEN);

        /* Config path banner */
        display_rect(0, PF_WARN_Y, SCREEN_W, PF_BANNER_H, RGBA8(20, 40, 0, 255));
        char conf_line[128];
        snprintf(conf_line, sizeof(conf_line), "Config: %s", plugins_get_config_path());
        display_text(MENU_X, PF_WARN_Y + 4,            COLOR_DIM,    conf_line);
        char count_line[64];
        snprintf(count_line, sizeof(count_line),
                 "%d plugin%s loaded.", plugins_get_count(),
                 plugins_get_count() == 1 ? "" : "s");
        display_text(MENU_X, PF_WARN_Y + 4 + LINE_H,   COLOR_TEXT,   count_line);
        /* Backup status */
        if (pf_backup_exists())
            display_text(MENU_X, PF_WARN_Y + 4 + LINE_H * 2, COLOR_GREEN,
                         "Backup: " PF_BACKUP_PATH);
        else
            display_text(MENU_X, PF_WARN_Y + 4 + LINE_H * 2, COLOR_DIM,
                         "Backup: none");

        display_hline(0, PF_SEP_Y, SCREEN_W, RGBA8(50, 50, 50, 255));

        /* Option rows */
        for (int i = 0; i < PF_OPTION_COUNT; i++) {
            int y = PF_OPT_Y + i * PF_OPT_ROW_H;
            if (y + PF_OPT_ROW_H > PF_FOOTER_Y) break;

            int is_sel = (i == sel);
            if (is_sel)
                display_rect(0, y - 1, SCREEN_W - 8, PF_OPT_ROW_H, COLOR_SEL_BG);

            if (is_sel) {
                display_text_transp(MENU_X, y,           COLOR_SELECTED, PF_OPTIONS[i].label);
                display_text_transp(MENU_X, y + LINE_H,  COLOR_SELECTED, PF_OPTIONS[i].desc);
            } else {
                display_text(MENU_X, y,           COLOR_TEXT, PF_OPTIONS[i].label);
                display_text(MENU_X, y + LINE_H,  COLOR_DIM,  PF_OPTIONS[i].desc);
            }
        }

        pf_footer("[X] Execute    [Up/Down] Select    [O] Back");
        display_end();

        sceCtrlPeekBufferPositive(0, &ctrl, 1);

        if (pf_btn(&ctrl, &old, SCE_CTRL_DOWN))
            sel = (sel + 1) % PF_OPTION_COUNT;
        if (pf_btn(&ctrl, &old, SCE_CTRL_UP))
            sel = (sel - 1 + PF_OPTION_COUNT) % PF_OPTION_COUNT;

        if (pf_btn(&ctrl, &old, SCE_CTRL_CROSS)) {
            switch (sel) {
                case 0: pf_safe_mode();    plugins_load(); break;
                case 1: pf_view_toggle();  plugins_load(); break;
                case 2: pf_reenable_all(); plugins_load(); break;
                case 3: pf_reset_config(); break;
                case 4: pf_backup_config();  break;
                case 5: pf_restore_config(); plugins_load(); break;
            }
            pf_drain(&old);
        }

        if (pf_btn(&ctrl, &old, SCE_CTRL_CIRCLE)) break;

        old = ctrl;
    }
}
