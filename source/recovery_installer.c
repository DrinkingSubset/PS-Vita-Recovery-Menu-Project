/*
 * recovery_installer.c — R-Trigger Boot Recovery Installer
 *
 * Installs or removes the boot_recovery.skprx kernel plugin that lets
 * the user hold R during power-on to enter the Recovery Menu directly.
 *
 * Install steps:
 *   1. Create ur0:recovery/ directory
 *   2. Copy boot_recovery.skprx from app0: to ur0:recovery/
 *   3. Back up active tai config to ur0:tai/config_backup_bootrecov.txt
 *   4. Add "ur0:recovery/boot_recovery.skprx" under *KERNEL in tai config
 *
 * Uninstall steps:
 *   1. Remove "ur0:recovery/boot_recovery.skprx" line from tai config
 *   2. Delete ur0:recovery/boot_recovery.skprx
 *
 * Safety rules:
 *   - Config is always backed up before modification
 *   - Atomic write (tmp → rename) for config changes
 *   - Plugin line is verified to exist before uninstalling
 *   - Never touches vs0:, os0:, or any system partition
 */

#include <psp2/ctrl.h>
#include <psp2/power.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "recovery_installer.h"
#include "compat.h"
#include "display.h"
#include "draw.h"
#include "menu.h"

/* ── Paths ───────────────────────────────────────────────────────────────── */
#define RI_PLUGIN_SRC   "app0:boot_recovery.skprx"
#define RI_PLUGIN_DST   "ux0:tai/boot_recovery.skprx"
#define RI_PLUGIN_DIR   "ux0:tai"
#define RI_PLUGIN_LINE  "ux0:tai/boot_recovery.skprx"
#define RI_BACKUP_PATH  "ur0:tai/config_backup_bootrecov.txt"
#define RI_COPY_BUF     (32 * 1024)

/* ── Layout ──────────────────────────────────────────────────────────────── */
#define RI_TITLE_LINE   TITLE_LINE
#define RI_FOOTER_Y     FOOTER_Y

/* ── Helpers ─────────────────────────────────────────────────────────────── */
static int ri_btn(SceCtrlData *now, SceCtrlData *old, unsigned int b) {
    return (now->buttons & b) && !(old->buttons & b);
}
static void ri_drain(SceCtrlData *old) {
    do { sceCtrlPeekBufferPositive(0, old, 1); } while (old->buttons);
}
static int ri_path_exists(const char *p) {
    SceIoStat st;
    return sceIoGetstat(p, &st) >= 0;
}

/* ── Message dialog ──────────────────────────────────────────────────────── */
static void ri_message(const char *line1, const char *line2, uint32_t col) {
    SceCtrlData old, ctrl;
    ri_drain(&old);
    while (1) {
        display_start();
        display_clear(COLOR_BG);
        display_rect(0, 0, SCREEN_W, RI_TITLE_LINE, COLOR_TITLE_BG);
        display_text(MENU_X, 6,  COLOR_TEXT,   "PS Vita Recovery Menu v1.0");
        display_text(MENU_X, 26, COLOR_YELLOW, "Boot Recovery Installer");
        display_hline(0, RI_TITLE_LINE, SCREEN_W, COLOR_GREEN);

        int bx = MENU_X - 8, by = TITLE_H + 30;
        int bw = SCREEN_W - bx * 2, bh = LINE_H * 5;
        display_rect(bx, by, bw, bh, RGBA8(20, 20, 20, 255));
        display_rect(bx, by, bw, 2,  col);
        display_text(MENU_X, by + 10,           col,        line1);
        if (line2 && line2[0])
            display_text(MENU_X, by + 10 + LINE_H, COLOR_DIM, line2);
        display_text(MENU_X, by + 10 + LINE_H * 3, COLOR_GREEN, "[X] OK");

        display_rect(0, RI_FOOTER_Y, SCREEN_W, SCREEN_H - RI_FOOTER_Y, COLOR_TITLE_BG);
        display_hline(0, RI_FOOTER_Y, SCREEN_W, COLOR_GREEN);
        display_end();

        sceCtrlPeekBufferPositive(0, &ctrl, 1);
        if (ri_btn(&ctrl, &old, SCE_CTRL_CROSS)) break;
        old = ctrl;
    }
    ri_drain(&old);
}

/* ── Confirm dialog — returns 1 if X pressed, 0 if O pressed ────────────── */
static int ri_confirm(const char *prompt, const char *detail) {
    SceCtrlData old, ctrl;
    ri_drain(&old);
    while (1) {
        display_start();
        display_clear(COLOR_BG);
        display_rect(0, 0, SCREEN_W, RI_TITLE_LINE, COLOR_TITLE_BG);
        display_text(MENU_X, 6,  COLOR_TEXT,   "PS Vita Recovery Menu v1.0");
        display_text(MENU_X, 26, COLOR_YELLOW, "Boot Recovery Installer");
        display_hline(0, RI_TITLE_LINE, SCREEN_W, COLOR_GREEN);

        int bx = MENU_X - 8, by = TITLE_H + 30;
        int bw = SCREEN_W - bx * 2, bh = LINE_H * 7;
        display_rect(bx, by, bw, bh, RGBA8(20, 20, 20, 255));
        display_rect(bx, by, bw, 2,  COLOR_YELLOW);
        display_text(MENU_X, by + 10,               COLOR_YELLOW, prompt);
        if (detail && detail[0])
            display_text(MENU_X, by + 10 + LINE_H,  COLOR_DIM,    detail);
        display_text(MENU_X, by + 10 + LINE_H * 3,  COLOR_GREEN,  "[X] Confirm");
        display_text(MENU_X, by + 10 + LINE_H * 4,  COLOR_DIM,    "[O] Cancel");

        display_rect(0, RI_FOOTER_Y, SCREEN_W, SCREEN_H - RI_FOOTER_Y, COLOR_TITLE_BG);
        display_hline(0, RI_FOOTER_Y, SCREEN_W, COLOR_GREEN);
        display_end();

        sceCtrlPeekBufferPositive(0, &ctrl, 1);
        if (ri_btn(&ctrl, &old, SCE_CTRL_CROSS))  { ri_drain(&old); return 1; }
        if (ri_btn(&ctrl, &old, SCE_CTRL_CIRCLE)) { ri_drain(&old); return 0; }
        old = ctrl;
    }
}

/* ── File copy (chunked, no malloc > 32KB) ───────────────────────────────── */
static int ri_copy_file(const char *src, const char *dst) {
    SceUID fdin = sceIoOpen(src, SCE_O_RDONLY, 0);
    if (fdin < 0) return -1;

    /* Write to .tmp first, rename over dst atomically */
    char tmp[128];
    snprintf(tmp, sizeof(tmp), "%s.tmp", dst);

    SceUID fdout = sceIoOpen(tmp, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
    if (fdout < 0) { sceIoClose(fdin); return -2; }

    static char chunk[RI_COPY_BUF];
    int result = 0, n;
    while ((n = sceIoRead(fdin, chunk, sizeof(chunk))) > 0) {
        if (sceIoWrite(fdout, chunk, n) != n) { result = -3; break; }
    }
    sceIoClose(fdin);
    sceIoClose(fdout);
    if (result < 0 || n < 0) { sceIoRemove(tmp); return -4; }

    sceIoRemove(dst);
    return sceIoRename(tmp, dst);
}

/* ── Atomic config write ─────────────────────────────────────────────────── */
static int ri_write_config(const char *path, const char *data, int len) {
    char tmp[128];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    SceUID fd = sceIoOpen(tmp, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
    if (fd < 0) return -1;
    int wr = sceIoWrite(fd, data, len);
    sceIoClose(fd);
    if (wr != len) { sceIoRemove(tmp); return -2; }
    sceIoRemove(path);
    return sceIoRename(tmp, path);
}

/* ── Check if plugin is already in config (as an active, uncommented line) ── */
/*
 * strstr alone would match commented-out lines like:
 *   #ur0:recovery/boot_recovery.skprx
 * We walk every occurrence and check the line is not prefixed by '#'.
 */
static int ri_plugin_in_config(const char *config_path) {
    SceUID fd = sceIoOpen(config_path, SCE_O_RDONLY, 0);
    if (fd < 0) return 0;
    int sz = (int)sceIoLseek(fd, 0, SCE_SEEK_END);
    sceIoLseek(fd, 0, SCE_SEEK_SET);
    if (sz <= 0 || sz > 128*1024) { sceIoClose(fd); return 0; }
    char *buf = malloc(sz + 1);
    if (!buf) { sceIoClose(fd); return 0; }
    sceIoRead(fd, buf, sz);
    sceIoClose(fd);
    buf[sz] = '\0';

    int found = 0;
    const char *p = buf;
    while ((p = strstr(p, RI_PLUGIN_LINE)) != NULL) {
        /* Walk back to the start of this line */
        const char *sol = p;
        while (sol > buf && *(sol - 1) != '\n') sol--;
        /* Skip leading whitespace */
        while (*sol == ' ' || *sol == '\t') sol++;
        /* Only count as installed if the line is NOT commented out */
        if (*sol != '#') {
            found = 1;
            break;
        }
        p++;  /* advance past this occurrence and keep searching */
    }

    free(buf);
    return found;
}

/* ── Install ─────────────────────────────────────────────────────────────── */
static void ri_install(void) {
    const char *cfg = g_compat.active_tai_config;
    if (!cfg || cfg[0] == '(' || cfg[0] == '\0') {
        ri_message("No active tai config found.", "Cannot install.", COLOR_RED);
        return;
    }

    /* Check the plugin source exists in the VPK */
    if (!ri_path_exists(RI_PLUGIN_SRC)) {
        ri_message("boot_recovery.skprx not found in app0:",
                   "Rebuild VPK with plugin included.", COLOR_RED);
        return;
    }

    /* Already installed? */
    if (ri_plugin_in_config(cfg)) {
        ri_message("Boot Recovery already installed.",
                   "Uninstall first if you want to reinstall.", COLOR_YELLOW);
        return;
    }

    /* The app is already running, so app0: is mounted to this title's
     * directory. If app0: isn't accessible something is seriously wrong. */
    if (!ri_path_exists("app0:")) {
        ri_message("Cannot access app0: — reinstall the VPK.",
                   "", COLOR_RED);
        return;
    }

    if (!ri_confirm(
        "Install R-Trigger Boot Recovery?",
        "Hold R at power-on to enter Recovery Menu."))
        return;

    /* Step 1: Create destination directory */
    sceIoMkdir(RI_PLUGIN_DIR, 0777);

    /* Step 2: Copy plugin file */
    if (ri_copy_file(RI_PLUGIN_SRC, RI_PLUGIN_DST) < 0) {
        ri_message("Failed to copy boot_recovery.skprx",
                   "Check ur0: has enough free space.", COLOR_RED);
        return;
    }

    /* Step 3: Back up config */
    ri_copy_file(cfg, RI_BACKUP_PATH);

    /* Step 4: Read current config */
    SceUID fd = sceIoOpen(cfg, SCE_O_RDONLY, 0);
    if (fd < 0) {
        ri_message("Cannot read tai config.", "", COLOR_RED);
        return;
    }
    int sz = (int)sceIoLseek(fd, 0, SCE_SEEK_END);
    sceIoLseek(fd, 0, SCE_SEEK_SET);
    if (sz <= 0 || sz > 128*1024) {
        sceIoClose(fd);
        ri_message("Config file too large or empty.", "", COLOR_RED);
        return;
    }
    char *orig = malloc(sz + 256);
    if (!orig) { sceIoClose(fd); ri_message("Out of memory.", "", COLOR_RED); return; }
    sceIoRead(fd, orig, sz);
    sceIoClose(fd);
    orig[sz] = '\0';

    /* Step 5: Insert plugin line after *KERNEL header.
     * Strategy: find "*KERNEL" line, insert our line immediately after it.
     * If no *KERNEL section exists, prepend one at the top. */
    char *out = malloc(sz + 256);
    if (!out) { free(orig); ri_message("Out of memory.", "", COLOR_RED); return; }

    /* Find *KERNEL only at the start of a line — don't match commented
     * lines like "# *KERNEL" which would put the plugin in the wrong section */
    char *kernel_pos = NULL;
    {
        const char *p = orig;
        while ((p = strstr(p, "*KERNEL")) != NULL) {
            const char *sol = p;
            while (sol > orig && *(sol - 1) != '\n') sol--;
            while (*sol == ' ' || *sol == '\t') sol++;
            if (*sol == '*') {
                kernel_pos = (char *)p;
                break;
            }
            p++;
        }
    }
    int out_len = 0;

    if (kernel_pos) {
        /* Copy everything up to and including the *KERNEL line */
        char *eol = kernel_pos;
        while (*eol && *eol != '\n') eol++;
        if (*eol == '\n') eol++;
        int head = (int)(eol - orig);
        memcpy(out, orig, head);
        out_len = head;
        /* Insert our plugin line */
        int ll = snprintf(out + out_len, 256,
                          "%s\n", RI_PLUGIN_LINE);
        out_len += ll;
        /* Copy the rest */
        memcpy(out + out_len, orig + head, sz - head);
        out_len += sz - head;
    } else {
        /* No *KERNEL section — prepend one */
        int ll = snprintf(out, sz + 256,
                          "*KERNEL\n%s\n\n", RI_PLUGIN_LINE);
        out_len = ll;
        memcpy(out + out_len, orig, sz);
        out_len += sz;
    }

    free(orig);

    /* Step 6: Write new config atomically */
    int r = ri_write_config(cfg, out, out_len);
    free(out);

    if (r < 0) {
        ri_message("Failed to write tai config.", "Backup preserved.", COLOR_RED);
        return;
    }

    ri_message("Boot Recovery installed!",
               "Hold R / L at power-on. Reboot to activate.", COLOR_GREEN);
    if (ri_confirm("Reboot now to activate?",
                   "Boot recovery requires a reboot to take effect."))
        scePowerRequestColdReset();
}

/* ── Uninstall ───────────────────────────────────────────────────────────── */
static void ri_uninstall(void) {
    const char *cfg = g_compat.active_tai_config;
    if (!cfg || cfg[0] == '(' || cfg[0] == '\0') {
        ri_message("No active tai config found.", "", COLOR_RED);
        return;
    }

    if (!ri_plugin_in_config(cfg)) {
        ri_message("Boot Recovery is not installed.", "", COLOR_YELLOW);
        return;
    }

    if (!ri_confirm("Uninstall Boot Recovery?",
                    "Removes plugin line from tai config."))
        return;

    /* Read config */
    SceUID fd = sceIoOpen(cfg, SCE_O_RDONLY, 0);
    if (fd < 0) { ri_message("Cannot read tai config.", "", COLOR_RED); return; }
    int sz = (int)sceIoLseek(fd, 0, SCE_SEEK_END);
    sceIoLseek(fd, 0, SCE_SEEK_SET);
    if (sz <= 0 || sz > 128*1024) {
        sceIoClose(fd);
        ri_message("Config file too large or empty.", "", COLOR_RED);
        return;
    }
    char *buf = malloc(sz + 1);
    if (!buf) { sceIoClose(fd); ri_message("Out of memory.", "", COLOR_RED); return; }
    sceIoRead(fd, buf, sz);
    sceIoClose(fd);
    buf[sz] = '\0';

    /* Remove the plugin line (and its trailing newline) */
    char *pos = strstr(buf, RI_PLUGIN_LINE);
    if (pos) {
        /* Find start of this line */
        char *sol = pos;
        while (sol > buf && *(sol-1) != '\n') sol--;
        /* Find end of this line */
        char *eol = pos;
        while (*eol && *eol != '\n') eol++;
        if (*eol == '\n') eol++;
        /* Shift remainder over the line */
        int tail = (int)(sz - (eol - buf));
        memmove(sol, eol, tail + 1);
        sz = (int)(sz - (eol - sol));
    }

    /* Backup then write */
    ri_copy_file(cfg, RI_BACKUP_PATH);
    int r = ri_write_config(cfg, buf, sz);
    free(buf);

    if (r < 0) {
        ri_message("Failed to write tai config.", "", COLOR_RED);
        return;
    }

    /* Remove the plugin file */
    sceIoRemove(RI_PLUGIN_DST);

    ri_message("Boot Recovery uninstalled.", "Reboot to take effect.", COLOR_GREEN);
    if (ri_confirm("Reboot now?", ""))
        scePowerRequestColdReset();
}

/* ── Status query ─────────────────────────────────────────────────────────── */
static int ri_get_status(int *plugin_file_ok, int *config_ok) {
    *plugin_file_ok = ri_path_exists(RI_PLUGIN_DST);
    const char *cfg = g_compat.active_tai_config;
    *config_ok = (cfg && cfg[0] != '(' && cfg[0] != '\0')
                 ? ri_plugin_in_config(cfg) : 0;
    return (*plugin_file_ok && *config_ok);
}

/* ── Main screen ─────────────────────────────────────────────────────────── */

#define RI_OPT_COUNT 3
static const char *RI_LABELS[RI_OPT_COUNT] = {
    "Install Boot Recovery",
    "Uninstall Boot Recovery",
    "View Status",
};
static const char *RI_DESCS[RI_OPT_COUNT] = {
    "Install R-trigger kernel plugin + update tai config",
    "Remove plugin line from tai config + delete file",
    "Show current installation status",
};

void recovery_installer_run(void) {
    int sel = 0;
    SceCtrlData old, ctrl;
    memset(&old, 0, sizeof(old));
    ri_drain(&old);

    while (1) {
        /* Get current status */
        int pf_ok, cfg_ok;
        int installed = ri_get_status(&pf_ok, &cfg_ok);

        display_start();
        display_clear(COLOR_BG);

        /* Title */
        display_rect(0, 0, SCREEN_W, RI_TITLE_LINE, COLOR_TITLE_BG);
        display_text(MENU_X, 6,  COLOR_TEXT,   "PS Vita Recovery Menu v1.0");
        display_text(MENU_X, 26, COLOR_YELLOW, "Boot Recovery Installer");
        display_hline(0, RI_TITLE_LINE, SCREEN_W, COLOR_GREEN);

        /* Status banner */
        int bany = TITLE_H + 4;
        int banh = LINE_H * 3 + 6;
        display_rect(0, bany, SCREEN_W, banh,
                     installed ? RGBA8(0,40,0,255) : RGBA8(30,20,0,255));
        uint32_t st_col = installed ? COLOR_GREEN : COLOR_YELLOW;
        display_text(MENU_X, bany + 4,
                     st_col,
                     installed ? "Status: INSTALLED" : "Status: NOT INSTALLED");
        display_text(MENU_X, bany + 4 + LINE_H, COLOR_DIM,
                     pf_ok ? "Plugin file:   FOUND  (ur0:recovery/)"
                           : "Plugin file:   not found");
        display_text(MENU_X, bany + 4 + LINE_H * 2, COLOR_DIM,
                     cfg_ok ? "tai config:    entry present"
                             : "tai config:    no entry");
        display_hline(0, bany + banh, SCREEN_W, RGBA8(40, 40, 40, 255));

        /* Usage hint */
        int hint_y = bany + banh + 6;
        display_text(MENU_X, hint_y, COLOR_DIM,
                     "Hold R trigger at power-on to launch Recovery.");
        display_hline(0, hint_y + LINE_H + 4, SCREEN_W, RGBA8(40,40,40,255));

        /* Option rows */
        int opt_y = hint_y + LINE_H + 10;
        int row_h = LINE_H * 2 + 6;
        for (int i = 0; i < RI_OPT_COUNT; i++) {
            int ry = opt_y + i * row_h;
            if (ry + row_h > RI_FOOTER_Y - 4) break;
            if (i == sel)
                display_rect(0, ry - 1, SCREEN_W - 8, row_h, COLOR_SEL_BG);
            uint32_t tc = (i == sel) ? COLOR_SELECTED : COLOR_TEXT;
            uint32_t dc = (i == sel) ? COLOR_SELECTED : COLOR_DIM;
            display_text(MENU_X, ry,           tc, RI_LABELS[i]);
            display_text(MENU_X, ry + LINE_H,  dc, RI_DESCS[i]);
        }

        /* Footer */
        display_rect(0, RI_FOOTER_Y, SCREEN_W, SCREEN_H - RI_FOOTER_Y,
                     COLOR_TITLE_BG);
        display_hline(0, RI_FOOTER_Y, SCREEN_W, COLOR_GREEN);
        display_text(MENU_X, RI_FOOTER_Y + 7, COLOR_DIM,
                     "[X] Execute   [Up/Down] Select   [O] Back");
        display_end();

        sceCtrlPeekBufferPositive(0, &ctrl, 1);

        if (ri_btn(&ctrl, &old, SCE_CTRL_DOWN))
            sel = (sel + 1) % RI_OPT_COUNT;
        if (ri_btn(&ctrl, &old, SCE_CTRL_UP))
            sel = (sel - 1 + RI_OPT_COUNT) % RI_OPT_COUNT;

        if (ri_btn(&ctrl, &old, SCE_CTRL_CROSS)) {
            switch (sel) {
                case 0: ri_install();   break;
                case 1: ri_uninstall(); break;
                case 2: {
                    /* Show detailed status including where app is installed */
                    char l1[64], l2[64], l3[64];
                    snprintf(l1, sizeof(l1), "Plugin file: %s",
                             pf_ok ? "FOUND" : "NOT FOUND");
                    snprintf(l2, sizeof(l2), "tai config entry: %s",
                             cfg_ok ? "PRESENT" : "MISSING");
                    /* Show which partition the app is actually on */
                    const char *locs[] = {
                        "ux0:app/RECM00001",
                        "imc0:app/RECM00001",
                        "uma0:app/RECM00001",
                        "ur0:app/RECM00001"
                    };
                    const char *found_at = "App: NOT FOUND on any partition";
                    for (int i = 0; i < 4; i++) {
                        if (ri_path_exists(locs[i])) {
                            found_at = locs[i];
                            break;
                        }
                    }
                    snprintf(l3, sizeof(l3), "%.48s", found_at);
                    ri_message(l1, l3,
                               installed ? COLOR_GREEN : COLOR_YELLOW);
                    break;
                }
            }
            ri_drain(&old);
            continue;
        }

        if (ri_btn(&ctrl, &old, SCE_CTRL_CIRCLE)) break;

        old = ctrl;
    }

    ri_drain(&old);
}
