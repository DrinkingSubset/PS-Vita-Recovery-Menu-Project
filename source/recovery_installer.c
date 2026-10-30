/*
 * recovery_installer.c — R-Trigger Boot Recovery Installer
 *
 * Installs or removes the two-part boot recovery system:
 *   - boot_recovery.skprx  (kernel plugin: R-button detection at boot)
 *   - boot_trigger.suprx   (user plugin:   launches Recovery Menu from user space)
 *
 * Install steps:
 *   1. Copy boot_recovery.skprx from app0: to tai_dir/
 *   2. Copy boot_trigger.suprx  from app0: to tai_dir/
 *   3. Back up active tai config
 *   4. Add boot_recovery.skprx under *KERNEL in tai config
 *   5. Add boot_trigger.suprx  under *main   in tai config
 *
 * Uninstall steps:
 *   1. Remove both plugin lines from tai config
 *   2. Delete both plugin files
 *
 * Safety rules:
 *   - Config is always backed up before modification
 *   - Atomic write (tmp -> verify -> rename, old kept as .old) for config
 *   - v1.1.7: Install becomes Install / Update when already installed
 *   - Plugin lines are verified to exist before uninstalling
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
#define RI_SKPRX_SRC   "app0:boot_recovery.skprx"   /* kernel plugin source */
#define RI_SUPRX_SRC   "app0:boot_trigger.suprx"    /* user plugin source   */
#define RI_COPY_BUF    (32 * 1024)

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
        display_app_title(6);
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
        display_app_title(6);
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

/* ── Replace dst with tmp without a window where dst is missing ──────────
 * v1.1.7 (review finding 13): the old code did remove(dst) then
 * rename(tmp, dst); a power cut between the two left no config at all.
 * Now dst is first renamed to dst.old, so at every moment either dst or
 * dst.old exists, and a failed step is rolled back. */
static int ri_replace(const char *tmp, const char *dst) {
    if (sceIoRename(tmp, dst) >= 0) return 0;          /* dst did not exist */
    char old[140];
    snprintf(old, sizeof(old), "%s.old", dst);
    sceIoRemove(old);
    int r = sceIoRename(dst, old);
    if (r < 0) { sceIoRemove(tmp); return r; }          /* dst untouched */
    r = sceIoRename(tmp, dst);
    if (r < 0) { sceIoRename(old, dst); sceIoRemove(tmp); return r; }  /* roll back */
    sceIoRemove(old);
    return 0;
}

/* 1 if both files exist and have identical contents */
static int ri_files_equal(const char *a, const char *b) {
    SceUID fa = sceIoOpen(a, SCE_O_RDONLY, 0);
    if (fa < 0) return 0;
    SceUID fb = sceIoOpen(b, SCE_O_RDONLY, 0);
    if (fb < 0) { sceIoClose(fa); return 0; }
    static char ba[4096], bb[4096];
    int same = 1;
    while (1) {
        int ra = sceIoRead(fa, ba, sizeof(ba));
        int rb = sceIoRead(fb, bb, sizeof(bb));
        if (ra != rb || ra < 0) { same = 0; break; }
        if (ra == 0) break;
        if (memcmp(ba, bb, ra) != 0) { same = 0; break; }
    }
    sceIoClose(fa); sceIoClose(fb);
    return same;
}

/* ── File copy (chunked, no malloc > 32KB) ───────────────────────────────── */
static int ri_copy_file(const char *src, const char *dst) {
    SceUID fdin = sceIoOpen(src, SCE_O_RDONLY, 0);
    if (fdin < 0) return -1;

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
    if (!ri_files_equal(src, tmp)) { sceIoRemove(tmp); return -5; }  /* verify */

    return ri_replace(tmp, dst);
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

    /* Read back and compare before touching the real config */
    fd = sceIoOpen(tmp, SCE_O_RDONLY, 0);
    if (fd < 0) { sceIoRemove(tmp); return -3; }
    char *chk = malloc(len + 1);
    int ok = 0;
    if (chk) {
        int rd = sceIoRead(fd, chk, len + 1);
        ok = (rd == len) && memcmp(chk, data, len) == 0;
        free(chk);
    }
    sceIoClose(fd);
    if (!ok) { sceIoRemove(tmp); return -4; }

    return ri_replace(tmp, path);
}

/* ── Check if a plugin line is in config (uncommented) ───────────────────── */
static int ri_plugin_in_config(const char *config_path, const char *plugin_line) {
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
    while ((p = strstr(p, plugin_line)) != NULL) {
        const char *sol = p;
        while (sol > buf && *(sol - 1) != '\n') sol--;
        while (*sol == ' ' || *sol == '\t') sol++;
        if (*sol != '#') { found = 1; break; }
        p++;
    }

    free(buf);
    return found;
}

/*
 * ri_insert_after_section — insert new_line immediately after a *SECTION header.
 *
 * Writes result into out (caller must supply out with orig_sz + extra slack).
 * Returns the length of the new buffer.
 * If the section is not found, appends "*section\nnew_line\n" at the end.
 */
static int ri_insert_after_section(const char *orig, int orig_sz,
                                   const char *section, const char *new_line,
                                   char *out) {
    /* Find section header at the real start of a line (not commented) */
    const char *section_pos = NULL;
    const char *p = orig;
    while ((p = strstr(p, section)) != NULL) {
        const char *sol = p;
        while (sol > orig && *(sol - 1) != '\n') sol--;
        while (*sol == ' ' || *sol == '\t') sol++;
        if (*sol == '*') { section_pos = p; break; }
        p++;
    }

    int out_len = 0;
    int line_len = strlen(new_line);

    if (section_pos) {
        /* Copy up to and including the section header line */
        const char *eol = section_pos;
        while (*eol && *eol != '\n') eol++;
        if (*eol == '\n') eol++;
        int head = (int)(eol - orig);
        memcpy(out, orig, head);
        out_len = head;
        /* Insert our plugin line */
        memcpy(out + out_len, new_line, line_len);
        out_len += line_len;
        out[out_len++] = '\n';
        /* Copy the rest of the original */
        memcpy(out + out_len, orig + head, orig_sz - head);
        out_len += orig_sz - head;
    } else {
        /* Section not found — append it at the end */
        memcpy(out, orig, orig_sz);
        out_len = orig_sz;
        /* Ensure file ends with a newline before our new block */
        if (out_len > 0 && out[out_len - 1] != '\n')
            out[out_len++] = '\n';
        int ll = snprintf(out + out_len, 256, "%s\n%s\n", section, new_line);
        out_len += ll;
    }

    return out_len;
}

/*
 * ri_remove_line — remove the first uncommented occurrence of line_to_remove
 * from buf in-place.  Returns the new buffer length.
 */
static int ri_remove_line(char *buf, int sz, const char *line_to_remove) {
    char *pos = strstr(buf, line_to_remove);
    if (!pos) return sz;
    /* Walk back to start-of-line */
    char *sol = pos;
    while (sol > buf && *(sol - 1) != '\n') sol--;
    /* Skip leading whitespace to reach the first real char */
    const char *chk = sol;
    while (*chk == ' ' || *chk == '\t') chk++;
    if (*chk == '#') return sz;   /* commented-out — leave it alone */
    /* Find end of line */
    char *eol = pos;
    while (*eol && *eol != '\n') eol++;
    if (*eol == '\n') eol++;
    /* Shift the rest of the buffer over the deleted line */
    int tail = (int)(sz - (eol - buf));
    memmove(sol, eol, tail + 1);
    return sz - (int)(eol - sol);
}

/* ── Install ─────────────────────────────────────────────────────────────── */
static void ri_install(void) {
    const char *cfg = g_compat.active_tai_config;
    if (!cfg || cfg[0] == '(' || cfg[0] == '\0') {
        ri_message("No active tai config found.", "Cannot install.", COLOR_RED);
        return;
    }

    /* Derive tai dir and plugin destinations from active config path */
    char tai_dir[48];
    char skprx_dst[64], skprx_line[64];
    char suprx_dst[64], suprx_line[64];
    char backup[64];

    snprintf(tai_dir, sizeof(tai_dir), "%s", cfg);
    char *slash = strrchr(tai_dir, '/');
    if (slash) *slash = '\0'; else snprintf(tai_dir, sizeof(tai_dir), "ux0:tai");

    snprintf(skprx_dst,  sizeof(skprx_dst),  "%s/boot_recovery.skprx", tai_dir);
    snprintf(skprx_line, sizeof(skprx_line), "%s/boot_recovery.skprx", tai_dir);
    snprintf(suprx_dst,  sizeof(suprx_dst),  "%s/boot_trigger.suprx",  tai_dir);
    snprintf(suprx_line, sizeof(suprx_line), "%s/boot_trigger.suprx",  tai_dir);
    snprintf(backup,     sizeof(backup),     "%s/config_backup_bootrecov.txt", tai_dir);

    /* Check both source files exist in the VPK */
    if (!ri_path_exists(RI_SKPRX_SRC)) {
        ri_message("boot_recovery.skprx not found in app0:",
                   "Rebuild VPK with both plugins included.", COLOR_RED);
        return;
    }
    if (!ri_path_exists(RI_SUPRX_SRC)) {
        ri_message("boot_trigger.suprx not found in app0:",
                   "Rebuild VPK with both plugins included.", COLOR_RED);
        return;
    }

    /* Already installed? v1.1.7 (review finding 3): offer an in-place
     * update of the plugin files instead of "uninstall first". The config
     * lines are already there, so the config is not touched. */
    int skprx_cfg = ri_plugin_in_config(cfg, skprx_line);
    int suprx_cfg = ri_plugin_in_config(cfg, suprx_line);
    if (skprx_cfg && suprx_cfg &&
        ri_path_exists(skprx_dst) && ri_path_exists(suprx_dst)) {
        int current = ri_files_equal(RI_SKPRX_SRC, skprx_dst) &&
                      ri_files_equal(RI_SUPRX_SRC, suprx_dst);
        if (!ri_confirm(current ? "Already installed and up to date."
                                : "Update Boot Recovery plugins?",
                        current ? "Press X to reinstall the files anyway."
                                : "Copies this version's plugins. Config unchanged."))
            return;
        if (ri_copy_file(RI_SKPRX_SRC, skprx_dst) < 0 ||
            ri_copy_file(RI_SUPRX_SRC, suprx_dst) < 0) {
            ri_message("Update failed: could not copy plugin files.",
                       "Old plugins are still in place.", COLOR_RED);
            return;
        }
        ri_message("Boot Recovery plugins updated.",
                   "Reboot to load the new plugins.", COLOR_GREEN);
        if (ri_confirm("Reboot now?", "The new plugins load at the next boot."))
            scePowerRequestColdReset();
        return;
    }

    if (!ri_confirm(
        "Install R-Trigger Boot Recovery?",
        "Hold R at power-on to enter Recovery Menu."))
        return;

    /* Step 1: Copy kernel plugin */
    if (ri_copy_file(RI_SKPRX_SRC, skprx_dst) < 0) {
        char emsg[72];
        snprintf(emsg, sizeof(emsg), "Failed to copy boot_recovery.skprx to %s", tai_dir);
        ri_message(emsg, "Check available space.", COLOR_RED);
        return;
    }

    /* Step 2: Copy user plugin */
    if (ri_copy_file(RI_SUPRX_SRC, suprx_dst) < 0) {
        char emsg[72];
        snprintf(emsg, sizeof(emsg), "Failed to copy boot_trigger.suprx to %s", tai_dir);
        ri_message(emsg, "Check available space.", COLOR_RED);
        sceIoRemove(skprx_dst);   /* roll back the kernel plugin we just copied */
        return;
    }

    /* Step 3: Back up config */
    ri_copy_file(cfg, backup);

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
    /* Extra slack: 512 bytes covers two inserted lines + two new section headers */
    char *orig = malloc(sz + 512);
    if (!orig) { sceIoClose(fd); ri_message("Out of memory.", "", COLOR_RED); return; }
    sceIoRead(fd, orig, sz);
    sceIoClose(fd);
    orig[sz] = '\0';

    /*
     * Step 5a: Insert boot_recovery.skprx after *KERNEL.
     *   Pass 1: orig → pass1_buf
     */
    char *pass1 = malloc(sz + 512);
    if (!pass1) { free(orig); ri_message("Out of memory.", "", COLOR_RED); return; }

    int pass1_sz = sz;
    if (!skprx_cfg) {
        pass1_sz = ri_insert_after_section(orig, sz, "*KERNEL", skprx_line, pass1);
    } else {
        /* Already present — just copy through */
        memcpy(pass1, orig, sz + 1);
        pass1_sz = sz;
    }
    free(orig);

    /*
     * Step 5b: Insert boot_trigger.suprx after *main.
     *   Pass 2: pass1_buf → pass2_buf
     */
    char *pass2 = malloc(pass1_sz + 512);
    if (!pass2) { free(pass1); ri_message("Out of memory.", "", COLOR_RED); return; }

    int pass2_sz = pass1_sz;
    if (!suprx_cfg) {
        pass2_sz = ri_insert_after_section(pass1, pass1_sz, "*main", suprx_line, pass2);
    } else {
        memcpy(pass2, pass1, pass1_sz + 1);
        pass2_sz = pass1_sz;
    }
    free(pass1);

    /* Step 6: Write new config atomically */
    int r = ri_write_config(cfg, pass2, pass2_sz);
    free(pass2);

    if (r < 0) {
        ri_message("Failed to write tai config.", "Backup preserved.", COLOR_RED);
        return;
    }

    ri_message("Boot Recovery installed!",
               "Hold R at power-on to enter Recovery.", COLOR_GREEN);
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

    /* Derive paths from active config */
    char tai_dir[48];
    char skprx_dst[64], skprx_line[64];
    char suprx_dst[64], suprx_line[64];
    char backup[64];

    snprintf(tai_dir, sizeof(tai_dir), "%s", cfg);
    char *slash = strrchr(tai_dir, '/');
    if (slash) *slash = '\0'; else snprintf(tai_dir, sizeof(tai_dir), "ux0:tai");

    snprintf(skprx_dst,  sizeof(skprx_dst),  "%s/boot_recovery.skprx", tai_dir);
    snprintf(skprx_line, sizeof(skprx_line), "%s/boot_recovery.skprx", tai_dir);
    snprintf(suprx_dst,  sizeof(suprx_dst),  "%s/boot_trigger.suprx",  tai_dir);
    snprintf(suprx_line, sizeof(suprx_line), "%s/boot_trigger.suprx",  tai_dir);
    snprintf(backup,     sizeof(backup),     "%s/config_backup_bootrecov.txt", tai_dir);

    int skprx_cfg = ri_plugin_in_config(cfg, skprx_line);
    int suprx_cfg = ri_plugin_in_config(cfg, suprx_line);

    if (!skprx_cfg && !suprx_cfg &&
        !ri_path_exists(skprx_dst) && !ri_path_exists(suprx_dst)) {
        ri_message("Boot Recovery is not installed.", "", COLOR_YELLOW);
        return;
    }

    if (!ri_confirm("Uninstall Boot Recovery?",
                    "Removes both plugin lines from tai config."))
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

    /* Remove both plugin lines (each call is in-place, updates sz) */
    sz = ri_remove_line(buf, sz, skprx_line);
    sz = ri_remove_line(buf, sz, suprx_line);

    /* Backup then write */
    ri_copy_file(cfg, backup);
    int r = ri_write_config(cfg, buf, sz);
    free(buf);

    if (r < 0) {
        ri_message("Failed to write tai config.", "", COLOR_RED);
        return;
    }

    /* Delete both plugin files (ignore errors if already gone) */
    sceIoRemove(skprx_dst);
    sceIoRemove(suprx_dst);

    ri_message("Boot Recovery uninstalled.", "Reboot to take effect.", COLOR_GREEN);
    if (ri_confirm("Reboot now?", ""))
        scePowerRequestColdReset();
}

/* ── Status query ─────────────────────────────────────────────────────────── */
/*
 * Fills four independent status flags:
 *   skprx_ok     — kernel plugin file exists on storage
 *   skprx_cfg_ok — *KERNEL entry present in tai config
 *   suprx_ok     — user plugin file exists on storage
 *   suprx_cfg_ok — *main entry present in tai config
 *
 * Returns 1 only if all four are true (fully installed).
 */
static int ri_get_status(int *skprx_ok,     int *skprx_cfg_ok,
                         int *suprx_ok,     int *suprx_cfg_ok) {
    const char *cfg = g_compat.active_tai_config;
    char tai_dir[48], skprx_dst[64], skprx_line[64], suprx_dst[64], suprx_line[64];

    snprintf(tai_dir, sizeof(tai_dir), "%s",
             (cfg && cfg[0] != '(') ? cfg : "ux0:tai/config.txt");
    char *sl = strrchr(tai_dir, '/');
    if (sl) *sl = '\0'; else snprintf(tai_dir, sizeof(tai_dir), "ux0:tai");

    snprintf(skprx_dst,  sizeof(skprx_dst),  "%s/boot_recovery.skprx", tai_dir);
    snprintf(skprx_line, sizeof(skprx_line), "%s/boot_recovery.skprx", tai_dir);
    snprintf(suprx_dst,  sizeof(suprx_dst),  "%s/boot_trigger.suprx",  tai_dir);
    snprintf(suprx_line, sizeof(suprx_line), "%s/boot_trigger.suprx",  tai_dir);

    *skprx_ok     = ri_path_exists(skprx_dst);
    *suprx_ok     = ri_path_exists(suprx_dst);
    *skprx_cfg_ok = (cfg && cfg[0] != '(' && cfg[0] != '\0')
                    ? ri_plugin_in_config(cfg, skprx_line) : 0;
    *suprx_cfg_ok = (cfg && cfg[0] != '(' && cfg[0] != '\0')
                    ? ri_plugin_in_config(cfg, suprx_line) : 0;

    return (*skprx_ok && *skprx_cfg_ok && *suprx_ok && *suprx_cfg_ok);
}

/* 1 if the installed plugin files differ from this app's copies */
static int ri_status_dirty = 1;
static int ri_outdated(void) {
    const char *cfg = g_compat.active_tai_config;
    char tai_dir[48], k[64], u[64];
    snprintf(tai_dir, sizeof(tai_dir), "%s",
             (cfg && cfg[0] != '(') ? cfg : "ux0:tai/config.txt");
    char *sl = strrchr(tai_dir, '/');
    if (sl) *sl = '\0'; else snprintf(tai_dir, sizeof(tai_dir), "ux0:tai");
    snprintf(k, sizeof(k), "%s/boot_recovery.skprx", tai_dir);
    snprintf(u, sizeof(u), "%s/boot_trigger.suprx",  tai_dir);
    return !(ri_files_equal(RI_SKPRX_SRC, k) && ri_files_equal(RI_SUPRX_SRC, u));
}

int recovery_installer_needs_update(void) {
    int a, b, c, d;
    int installed = ri_get_status(&a, &b, &c, &d);
    if (installed) return ri_outdated() ? 1 : 0;
    return (a || b || c || d) ? 2 : 0;
}

/* ── Main screen ─────────────────────────────────────────────────────────── */

#define RI_OPT_COUNT 3
static const char *RI_LABELS[RI_OPT_COUNT] = {
    "Install / Update Boot Recovery",
    "Uninstall Boot Recovery",
    "View Status",
};
static const char *RI_DESCS[RI_OPT_COUNT] = {
    "Install plugins, or update them to this version",
    "Remove both plugin lines from tai config + delete files",
    "Show current installation status",
};

void recovery_installer_run(void) {
    int sel = 0;
    ri_status_dirty = 1;   /* re-check plugin versions on every visit */
    SceCtrlData old, ctrl;
    memset(&old, 0, sizeof(old));
    ri_drain(&old);

    while (1) {
        /* Get current status */
        int skprx_ok, skprx_cfg_ok, suprx_ok, suprx_cfg_ok;
        int installed = ri_get_status(&skprx_ok, &skprx_cfg_ok,
                                      &suprx_ok, &suprx_cfg_ok);
        /* Partial: some but not all components present */
        int partial = !installed &&
                      (skprx_ok || skprx_cfg_ok || suprx_ok || suprx_cfg_ok);
        /* Installed files differ from the ones in this app version? */
        static int outdated = -1;
        if (outdated < 0 || ri_status_dirty) {
            outdated = installed ? ri_outdated() : 0;
            ri_status_dirty = 0;
        }

        display_start();
        display_clear(COLOR_BG);

        /* Title */
        display_rect(0, 0, SCREEN_W, RI_TITLE_LINE, COLOR_TITLE_BG);
        display_app_title(6);
        display_text(MENU_X, 26, COLOR_YELLOW, "Boot Recovery Installer");
        display_hline(0, RI_TITLE_LINE, SCREEN_W, COLOR_GREEN);

        /* Status banner — 5 rows: overall + 4 component lines */
        int bany = TITLE_H + 4;
        int banh = LINE_H * 5 + 8;
        uint32_t ban_bg = installed ? RGBA8(0, 40, 0, 255)
                        : partial  ? RGBA8(40, 20, 0, 255)
                                   : RGBA8(30, 10, 10, 255);
        display_rect(0, bany, SCREEN_W, banh, ban_bg);

        uint32_t st_col = installed ? (outdated ? COLOR_YELLOW : COLOR_GREEN)
                        : partial   ? COLOR_YELLOW
                                    : COLOR_RED;
        const char *st_str = installed
                           ? (outdated ? "Status: INSTALLED - UPDATE AVAILABLE"
                                       : "Status: FULLY INSTALLED (up to date)")
                           : partial   ? "Status: PARTIAL (reinstall recommended)"
                                       : "Status: NOT INSTALLED";
        display_text(MENU_X, bany + 4,                st_col,    st_str);
        display_text(MENU_X, bany + 4 + LINE_H,       COLOR_DIM,
                     skprx_ok     ? "  .skprx kernel file : FOUND"
                                  : "  .skprx kernel file : NOT FOUND");
        display_text(MENU_X, bany + 4 + LINE_H * 2,   COLOR_DIM,
                     skprx_cfg_ok ? "  *KERNEL cfg entry  : present"
                                  : "  *KERNEL cfg entry  : MISSING");
        display_text(MENU_X, bany + 4 + LINE_H * 3,   COLOR_DIM,
                     suprx_ok     ? "  .suprx user file   : FOUND"
                                  : "  .suprx user file   : NOT FOUND");
        display_text(MENU_X, bany + 4 + LINE_H * 4,   COLOR_DIM,
                     suprx_cfg_ok ? "  *main cfg entry    : present"
                                  : "  *main cfg entry    : MISSING");

        display_hline(0, bany + banh, SCREEN_W, RGBA8(40, 40, 40, 255));

        /* Usage hint */
        int hint_y = bany + banh + 6;
        display_text(MENU_X, hint_y, COLOR_DIM,
                     "Hold R trigger at power-on to launch Recovery.");
        display_hline(0, hint_y + LINE_H + 4, SCREEN_W, RGBA8(40, 40, 40, 255));

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
            display_text(MENU_X, ry,          tc, RI_LABELS[i]);
            display_text(MENU_X, ry + LINE_H, dc, RI_DESCS[i]);
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
                case 0: ri_install();   ri_status_dirty = 1; break;
                case 1: ri_uninstall(); ri_status_dirty = 1; break;
                case 2: {
                    /* Detailed status popup */
                    char l1[80], l2[80];
                    snprintf(l1, sizeof(l1), "%s  |  cfg: %s",
                             installed ? "FULLY INSTALLED" : partial ? "PARTIAL" : "NOT INSTALLED",
                             g_compat.active_tai_config ? g_compat.active_tai_config : "unknown");
                    snprintf(l2, sizeof(l2),
                             ".skprx:%s cfg:%s  .suprx:%s cfg:%s",
                             skprx_ok     ? "OK" : "NO",
                             skprx_cfg_ok ? "OK" : "NO",
                             suprx_ok     ? "OK" : "NO",
                             suprx_cfg_ok ? "OK" : "NO");
                    ri_message(l1, l2,
                               installed ? COLOR_GREEN : partial ? COLOR_YELLOW : COLOR_RED);
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
