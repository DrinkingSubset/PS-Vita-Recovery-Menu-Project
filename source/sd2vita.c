/*
 * sd2vita.c — SD2Vita management screen
 *
 * Bug fixes vs. initial design:
 *   - sceIoGetstatByFd() doesn't exist in vitasdk -> use sceIoLseek to get size
 *   - copy_file loaded whole file into RAM -> 64KB chunked I/O loop
 *   - STORAGEMGR_PLUGIN+4 produced wrong path -> use literal string directly
 *   - sd2vita_detect() called parse_config() resetting g_cfg every frame -> separated
 *   - Unused MOUNT_POINTS[], MOUNT_COUNT, kernel_entry -> removed
 *   - Path buffers increased from 256 to 512 for deep Vita paths
 */

#include <psp2/ctrl.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/io/dirent.h>
#include <psp2/power.h>

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "sd2vita.h"
#include "draw.h"
#include "menu.h"

/* ── Paths ──────────────────────────────────────────────────────────────── */
#define STORAGE_CFG_UR0   "ur0:tai/storage_config.txt"
#define STORAGE_CFG_UX0   "ux0:tai/storage_config.txt"
#define STORAGEMGR_PLUGIN "ur0:tai/storagemgr.skprx"
#define TAI_CONFIG_IMC0   "imc0:tai/config.txt"   /* PCH-2000/PSTV — highest priority */
#define TAI_CONFIG_UR0    "ur0:tai/config.txt"
#define TAI_CONFIG_UX0    "ux0:tai/config.txt"
#define BACKUP_ROOT       "ux0:data/vita_recovery"
#define LOG_PATH          "ux0:data/vita_recovery/sd2vita.log"
#define CHUNK_SIZE        (64 * 1024)

/* ── State ──────────────────────────────────────────────────────────────── */
static char  g_status[256] = "Ready.";
static SceUID g_log = -1;

static void log_open(void) {
    sceIoMkdir(BACKUP_ROOT, 0777);
    g_log = sceIoOpen(LOG_PATH,
                      SCE_O_WRONLY | SCE_O_CREAT | SCE_O_APPEND, 0777);
}
static void log_line(const char *s) {
    if (g_log >= 0) { sceIoWrite(g_log, s, strlen(s)); sceIoWrite(g_log,"\n",1); }
}
static void log_close(void) {
    if (g_log >= 0) { sceIoClose(g_log); g_log = -1; }
}
static void set_status(const char *s) {
    snprintf(g_status, sizeof(g_status), "%s", s);
    log_line(s);
}

/* ── File I/O helpers ───────────────────────────────────────────────────── */

/* Read small text config files (< 4 KB) into a fixed buffer.
 * FIXED: use sceIoLseek to get size; sceIoGetstatByFd does not exist. */
static int read_text_file(const char *path, char *out, int out_size) {
    SceUID fd = sceIoOpen(path, SCE_O_RDONLY, 0);
    if (fd < 0) return -1;
    int sz = (int)sceIoLseek(fd, 0, SCE_SEEK_END);
    sceIoLseek(fd, 0, SCE_SEEK_SET);
    if (sz < 0 || sz >= out_size - 1) { sceIoClose(fd); return -2; }
    sceIoRead(fd, out, sz);
    sceIoClose(fd);
    out[sz] = '\0';
    return sz;
}

static int write_text_file(const char *path, const char *data) {
    SceUID fd = sceIoOpen(path,
                          SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
    if (fd < 0) return -1;
    sceIoWrite(fd, data, strlen(data));
    sceIoClose(fd);
    return 0;
}

/* FIXED: Chunked copy — previous version malloc'd whole file causing crash
 * on game files (1-4 GB). Now uses a 64 KB heap chunk and loops. */
static int copy_file_chunked(const char *src, const char *dst) {
    SceUID fdin = sceIoOpen(src, SCE_O_RDONLY, 0);
    if (fdin < 0) return -1;
    SceUID fdout = sceIoOpen(dst,
                             SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
    if (fdout < 0) { sceIoClose(fdin); return -2; }

    char *chunk = malloc(CHUNK_SIZE);
    if (!chunk) { sceIoClose(fdin); sceIoClose(fdout); return -3; }

    int result = 0, n;
    while ((n = sceIoRead(fdin, chunk, CHUNK_SIZE)) > 0) {
        if (sceIoWrite(fdout, chunk, n) != n) { result = -4; break; }
    }
    free(chunk);
    sceIoClose(fdin);
    sceIoClose(fdout);
    return result;
}

/* FIXED: path buffers 512 bytes (was 256 — too small for nested Vita paths) */
static int copy_dir_recursive(const char *src, const char *dst) {
    sceIoMkdir(dst, 0777);
    SceUID dfd = sceIoDopen(src);
    if (dfd < 0) return 0;
    SceIoDirent e; int n = 0;
    while (sceIoDread(dfd, &e) > 0) {
        if (e.d_name[0] == '.') continue;
        char sp[512], dp[512];
        snprintf(sp, sizeof(sp), "%s/%s", src, e.d_name);
        snprintf(dp, sizeof(dp), "%s/%s", dst, e.d_name);
        if (SCE_S_ISDIR(e.d_stat.st_mode))
            n += copy_dir_recursive(sp, dp);
        else if (copy_file_chunked(sp, dp) == 0)
            n++;
    }
    sceIoDclose(dfd);
    return n;
}

static void delete_dir_recursive(const char *path) {
    SceUID dfd = sceIoDopen(path);
    if (dfd < 0) { sceIoRemove(path); return; }
    SceIoDirent e;
    while (sceIoDread(dfd, &e) > 0) {
        if (e.d_name[0] == '.') continue;
        char child[512];
        snprintf(child, sizeof(child), "%s/%s", path, e.d_name);
        delete_dir_recursive(child);
    }
    sceIoDclose(dfd);
    sceIoRmdir(path);
}

/* ── storage_config.txt parser ──────────────────────────────────────────── */
typedef struct {
    char gcd[16];   /* Game Card Device  = SD2Vita */
    char mcd[16];   /* Memory Card       = Sony card */
    char intv[16];  /* Internal flash    = imc0 */
    char uma[16];   /* Extra / USB       = grw0 */
    int  loaded;
    char path[128]; /* which file was loaded */
} StorageConfig;

static StorageConfig g_cfg;

static void parse_config(void) {
    memset(&g_cfg, 0, sizeof(g_cfg));
    SceIoStat st;
    if      (sceIoGetstat(STORAGE_CFG_UR0, &st) >= 0)
        snprintf(g_cfg.path, sizeof(g_cfg.path), "%s", STORAGE_CFG_UR0);
    else if (sceIoGetstat(STORAGE_CFG_UX0, &st) >= 0)
        snprintf(g_cfg.path, sizeof(g_cfg.path), "%s", STORAGE_CFG_UX0);
    else
        return;

    char buf[2048], tmp[2048];
    if (read_text_file(g_cfg.path, buf, sizeof(buf)) < 0) return;
    snprintf(tmp, sizeof(tmp), "%s", buf);

    char *line = strtok(tmp, "\n");
    while (line) {
        int ll = strlen(line);
        if (ll > 0 && line[ll-1] == '\r') line[--ll] = '\0';
        if      (strncmp(line, "GCD=", 4) == 0)
            snprintf(g_cfg.gcd,  sizeof(g_cfg.gcd),  "%s", line+4);
        else if (strncmp(line, "MCD=", 4) == 0)
            snprintf(g_cfg.mcd,  sizeof(g_cfg.mcd),  "%s", line+4);
        else if (strncmp(line, "INT=", 4) == 0)
            snprintf(g_cfg.intv, sizeof(g_cfg.intv),  "%s", line+4);
        else if (strncmp(line, "UMA=", 4) == 0)
            snprintf(g_cfg.uma,  sizeof(g_cfg.uma),  "%s", line+4);
        line = strtok(NULL, "\n");
    }
    g_cfg.loaded = 1;
}

static int save_config(void) {
    char out[512]; int pos = 0;
    if (g_cfg.gcd[0])  pos += snprintf(out+pos,sizeof(out)-pos,"GCD=%s\n",g_cfg.gcd);
    if (g_cfg.mcd[0])  pos += snprintf(out+pos,sizeof(out)-pos,"MCD=%s\n",g_cfg.mcd);
    if (g_cfg.intv[0]) pos += snprintf(out+pos,sizeof(out)-pos,"INT=%s\n",g_cfg.intv);
    if (g_cfg.uma[0])  pos += snprintf(out+pos,sizeof(out)-pos,"UMA=%s\n",g_cfg.uma);
    const char *dst = g_cfg.path[0] ? g_cfg.path : STORAGE_CFG_UR0;
    return write_text_file(dst, out);
}

/* ── Detection ──────────────────────────────────────────────────────────── */
/* FIXED: separated from parse_config() — calling detect in the draw loop
 * no longer resets g_cfg every frame. Uses already-loaded config. */
static int sd2vita_detect(char *mp_out, int mp_out_size) {
    if (g_cfg.loaded && g_cfg.gcd[0]) {
        char test[32];
        snprintf(test, sizeof(test), "%s:", g_cfg.gcd);
        SceIoStat st;
        if (sceIoGetstat(test, &st) >= 0) {
            if (mp_out) snprintf(mp_out, mp_out_size, "%s", test);
            return 1;
        }
    }
    const char *cands[] = { "uma0:", "grw0:" };
    for (int i = 0; i < 2; i++) {
        SceIoStat st;
        if (sceIoGetstat(cands[i], &st) >= 0) {
            if (mp_out) snprintf(mp_out, mp_out_size, "%s", cands[i]);
            return 1;
        }
    }
    return 0;
}

/* ── StorageMgr tai config ──────────────────────────────────────────────── */
/*
 * Check all three tai config locations for a storagemgr entry.
 * On PCH-2000/PSTV, imc0:tai/config.txt is the active one — we must
 * check it too, otherwise SD2Vita appears "not configured" even when it is.
 */
static int storagemgr_in_tai_config(void) {
    SceIoStat st;
    if (sceIoGetstat(STORAGEMGR_PLUGIN, &st) < 0) return -1;   /* plugin not installed */
    char buf[4096];
    const char *cfgs[] = { TAI_CONFIG_IMC0, TAI_CONFIG_UR0, TAI_CONFIG_UX0 };
    for (int i = 0; i < 3; i++) {
        if (read_text_file(cfgs[i], buf, sizeof(buf)) >= 0)
            if (strstr(buf, "storagemgr.skprx")) return 1;
    }
    return 0;
}

/*
 * Add storagemgr.skprx to *KERNEL in the ACTIVE tai config.
 * Priority: imc0: (PCH-2000/PSTV) > ur0: > ux0:
 * We also ensure ur0:tai/ exists (needed on fresh 3.67/3.68 installs).
 */
static int storagemgr_add_to_tai_config(void) {
    SceIoStat st;
    const char *cfg_path;
    if (sceIoGetstat(TAI_CONFIG_IMC0, &st) >= 0)
        cfg_path = TAI_CONFIG_IMC0;
    else if (sceIoGetstat(TAI_CONFIG_UR0, &st) >= 0)
        cfg_path = TAI_CONFIG_UR0;
    else
        cfg_path = TAI_CONFIG_UX0;

    /* Ensure ur0:tai/ directory exists even on 3.67/3.68 without Enso */
    sceIoMkdir("ur0:tai", 0777);

    char buf[4096];
    if (read_text_file(cfg_path, buf, sizeof(buf)) < 0) return -1;

    const char *entry = "ur0:tai/storagemgr.skprx\n";
    char *kp = strstr(buf, "*KERNEL");
    char out[5120];
    if (!kp) {
        snprintf(out, sizeof(out), "*KERNEL\n%s\n%s", entry, buf);
    } else {
        char *nl = strchr(kp, '\n');
        if (!nl) return -1;
        int off = (int)(nl - buf) + 1;
        memcpy(out, buf, off);
        int ins = snprintf(out + off, sizeof(out) - off, "%s", entry);
        snprintf(out + off + ins, sizeof(out) - off - ins, "%s", buf + off);
    }
    return write_text_file(cfg_path, out);
}

/* ── UI helpers ─────────────────────────────────────────────────────────── */

static int btn_pressed(SceCtrlData *n, SceCtrlData *o, unsigned int b) {
    return (n->buttons & b) && !(o->buttons & b);
}

static void s2v_titlebar(const char *subtitle) {
    display_rect(0, 0, SCREEN_W, TITLE_LINE, COLOR_TITLE_BG);
    draw_text(MENU_X, 6,  COLOR_TEXT, "PS Vita Recovery Menu v1.0");
    draw_text(MENU_X, 26, COLOR_DIM,  subtitle);
    display_hline(0, TITLE_LINE, SCREEN_W, COLOR_GREEN);
}

static void s2v_footer(const char *hint) {
    display_rect(0, FOOTER_Y, SCREEN_W, SCREEN_H - FOOTER_Y, COLOR_TITLE_BG);
    display_hline(0, FOOTER_LINE, SCREEN_W, COLOR_GREEN);
    draw_text(MENU_X, FOOTER_Y + 7, COLOR_DIM, hint);
}

static int confirm_dialog(void *font, const char *subtitle, const char *warn) {
    SceCtrlData old, ctrl; memset(&old, 0, sizeof(old));
    do { sceCtrlPeekBufferPositive(0, &old, 1); } while (old.buttons);
    while (1) {
        display_start(); display_clear(COLOR_BG);
        s2v_titlebar(subtitle);
        int y = TITLE_H + 8;
        display_rect(MENU_X - 8, y, SCREEN_W - (MENU_X-8)*2, LINE_H*5,
                     RGBA8(60, 0, 0, 255));
        draw_text(MENU_X, y + 4,            COLOR_YELLOW, "!! CONFIRM !!");
        draw_text(MENU_X, y + 4 + LINE_H,   COLOR_TEXT,   warn);
        draw_text(MENU_X, y + 4 + LINE_H*3, COLOR_GREEN,  "[X] Confirm");
        draw_text(MENU_X, y + 4 + LINE_H*4, COLOR_DIM,    "[O] Cancel");
        s2v_footer("[X] Confirm    [O] Cancel");
        display_end();
        sceCtrlPeekBufferPositive(0, &ctrl, 1);
        if (btn_pressed(&ctrl, &old, SCE_CTRL_CROSS)) {
            do { sceCtrlPeekBufferPositive(0, &old, 1); } while (old.buttons);
            return 1;
        }
        if (btn_pressed(&ctrl, &old, SCE_CTRL_CIRCLE)) {
            do { sceCtrlPeekBufferPositive(0, &old, 1); } while (old.buttons);
            return 0;
        }
        old = ctrl;
    }
}

static void show_progress(void *font, const char *subtitle, const char *msg) {
    display_start(); display_clear(COLOR_BG);
    s2v_titlebar(subtitle);
    draw_text(MENU_X, TITLE_H + 8,          COLOR_YELLOW, msg);
    draw_text(MENU_X, TITLE_H + 8 + LINE_H, COLOR_DIM,
              "Please wait — do NOT remove the card.");
    s2v_footer("");
    display_end();
}

static void show_result(void *font, const char *subtitle,
                         int ok, int offer_reboot) {
    SceCtrlData old, ctrl; memset(&old, 0, sizeof(old));
    do { sceCtrlPeekBufferPositive(0, &old, 1); } while (old.buttons);
    while (1) {
        display_start(); display_clear(COLOR_BG);
        s2v_titlebar(subtitle);
        int y = TITLE_H + 8;
        draw_text(MENU_X, y,           ok ? COLOR_GREEN : COLOR_RED,
                                        ok ? "SUCCESS" : "ERROR");
        draw_text(MENU_X, y + LINE_H,  COLOR_TEXT, g_status);
        if (offer_reboot && ok)
            draw_text(MENU_X, y + LINE_H*3, COLOR_DIM, "[Start] Reboot   [O] Back");
        else
            draw_text(MENU_X, y + LINE_H*3, COLOR_DIM, "[O] Back");
        s2v_footer(offer_reboot && ok ? "[O] Back    [Start] Reboot" : "[O] Back");
        display_end();
        sceCtrlPeekBufferPositive(0, &ctrl, 1);
        if (btn_pressed(&ctrl, &old, SCE_CTRL_CIRCLE)) {
            do { sceCtrlPeekBufferPositive(0, &old, 1); } while (old.buttons);
            return;
        }
        if (offer_reboot && ok && btn_pressed(&ctrl, &old, SCE_CTRL_START))
            scePowerRequestColdReset();
        old = ctrl;
    }
}

/* ══ Operations ══════════════════════════════════════════════════════════════ */

/* 1 — Card & Config Info */
static void op_show_config(void *font) {
    char mp[32] = "(not detected)";
    int present = sd2vita_detect(mp, sizeof(mp));
    SceCtrlData old, ctrl; memset(&old, 0, sizeof(old));
    do { sceCtrlPeekBufferPositive(0, &old, 1); } while (old.buttons);
    while (1) {
        display_start(); display_clear(COLOR_BG);
        s2v_titlebar("SD2Vita — Card & Config Info");

        int y = TITLE_H + 8;
        draw_text(MENU_X, y, COLOR_DIM, "SD2Vita detected:");
        draw_text(VAL_X,  y, present ? COLOR_GREEN : COLOR_RED,
                  present ? mp : "NO (StorageMgr not loaded)");
        y += LINE_H + 4;

        draw_text(MENU_X, y, COLOR_DIM, "Config file:");
        draw_text(VAL_X,  y, g_cfg.loaded ? COLOR_TEXT : COLOR_YELLOW,
                  g_cfg.loaded ? g_cfg.path : "(not found)");
        y += LINE_H;

        if (g_cfg.loaded) {
            y += 4;
            draw_text(MENU_X, y, COLOR_DIM, "GCD (SD2Vita):");
            draw_text(VAL_X,  y, COLOR_TEXT, g_cfg.gcd[0]  ? g_cfg.gcd  : "-"); y += LINE_H;
            draw_text(MENU_X, y, COLOR_DIM, "MCD (Sony card):");
            draw_text(VAL_X,  y, COLOR_TEXT, g_cfg.mcd[0]  ? g_cfg.mcd  : "-"); y += LINE_H;
            draw_text(MENU_X, y, COLOR_DIM, "INT (internal):");
            draw_text(VAL_X,  y, COLOR_TEXT, g_cfg.intv[0] ? g_cfg.intv : "-"); y += LINE_H;
            draw_text(MENU_X, y, COLOR_DIM, "UMA (extra):");
            draw_text(VAL_X,  y, COLOR_TEXT, g_cfg.uma[0]  ? g_cfg.uma  : "-"); y += LINE_H;
        } else {
            draw_text(MENU_X, y, COLOR_RED,
                      "storage_config.txt not found."); y += LINE_H;
            draw_text(MENU_X, y, COLOR_DIM,
                      "Use 'Install StorageMgr' to create it."); y += LINE_H;
        }
        y += 4;

        /* StorageMgr label is 25 chars wide — give it its own line */
        draw_text(MENU_X, y, COLOR_DIM, "StorageMgr in tai config:"); y += LINE_H;
        int sm = storagemgr_in_tai_config();
        draw_text(MENU_X + 16, y,
                  sm ==  1 ? COLOR_GREEN :
                  sm == -1 ? COLOR_RED   : COLOR_YELLOW,
                  sm ==  1 ? "YES — active" :
                  sm == -1 ? "Plugin file missing" : "NO — not installed");

        s2v_footer("[O] Back");
        display_end();
        sceCtrlPeekBufferPositive(0, &ctrl, 1);
        if (btn_pressed(&ctrl, &old, SCE_CTRL_CIRCLE)) {
            do { sceCtrlPeekBufferPositive(0, &old, 1); } while (old.buttons);
            return;
        }
        old = ctrl;
    }
}

/* 2 — Switch mount point */
static void op_switch_mount(void *font) {
    typedef struct { const char *gcd; const char *mcd; const char *label; } Preset;
    static const Preset presets[] = {
        { "ux0",  "uma0", "SD2Vita=ux0  / Sony Card=uma0  (SD as main)" },
        { "uma0", "ux0",  "SD2Vita=uma0 / Sony Card=ux0   (Sony as main)" },
        { "grw0", "ux0",  "SD2Vita=grw0 / Sony Card=ux0   (SD as extra)" },
    };
    static int sel = 0;
    SceCtrlData old, ctrl; memset(&old, 0, sizeof(old));
    do { sceCtrlPeekBufferPositive(0, &old, 1); } while (old.buttons);

    while (1) {
        display_start(); display_clear(COLOR_BG);
        s2v_titlebar("SD2Vita — Switch Mount Point");

        int y = TITLE_H + 8;
        draw_text(MENU_X, y, COLOR_DIM, "Current config:"); y += LINE_H;
        if (g_cfg.loaded) {
            char line[64];
            snprintf(line, sizeof(line), "GCD=%-4s  MCD=%-4s  INT=%-4s  UMA=%-4s",
                     g_cfg.gcd, g_cfg.mcd, g_cfg.intv, g_cfg.uma);
            draw_text(MENU_X, y, COLOR_TEXT, line);
        } else {
            draw_text(MENU_X, y, COLOR_YELLOW, "No config — will be created.");
        }
        y += LINE_H + 8;
        draw_text(MENU_X, y, COLOR_DIM, "Select preset:"); y += LINE_H + 4;

        for (int i = 0; i < 3; i++) {
            if (i == sel) {
                display_rect(0, y - 1, SCREEN_W - 8, LINE_H + 2, COLOR_SEL_BG);
                display_text_transp(MENU_X, y, COLOR_SELECTED, presets[i].label);
            } else {
                draw_text(MENU_X, y, COLOR_TEXT, presets[i].label);
            }
            y += LINE_H + 4;
        }

        s2v_footer("[Up/Dn] Select   [X] Save   [O] Cancel");
        display_end();
        sceCtrlPeekBufferPositive(0, &ctrl, 1);

        if (btn_pressed(&ctrl, &old, SCE_CTRL_DOWN)) sel = (sel+1)%3;
        if (btn_pressed(&ctrl, &old, SCE_CTRL_UP))   sel = (sel+2)%3;
        if (btn_pressed(&ctrl, &old, SCE_CTRL_CROSS)) {
            snprintf(g_cfg.gcd,  sizeof(g_cfg.gcd),  "%s", presets[sel].gcd);
            snprintf(g_cfg.mcd,  sizeof(g_cfg.mcd),  "%s", presets[sel].mcd);
            if (!g_cfg.path[0])
                snprintf(g_cfg.path, sizeof(g_cfg.path), "%s", STORAGE_CFG_UR0);
            log_open();
            int r = save_config();
            set_status(r == 0 ? "Config saved. Reboot to apply."
                               : "ERROR: Could not write storage_config.txt");
            log_close();
            show_result(font, "SD2Vita — Switch Mount Point", r == 0, 1);
            do { sceCtrlPeekBufferPositive(0, &old, 1); } while (old.buttons);
            return;
        }
        if (btn_pressed(&ctrl, &old, SCE_CTRL_CIRCLE)) {
            do { sceCtrlPeekBufferPositive(0, &old, 1); } while (old.buttons);
            return;
        }
        old = ctrl;
    }
}
static void op_storagemgr(void *font) {
    log_open();
    int r = storagemgr_in_tai_config();
    if (r == -1) {
        set_status("storagemgr.skprx not found at ur0:tai/. "
                   "Copy the plugin there first, then run this again.");
    } else if (r == 1) {
        set_status("StorageMgr already in tai config. Nothing to do.");
    } else {
        if (confirm_dialog(font,
                "SD2Vita — Install StorageMgr",
                "Add storagemgr.skprx to *KERNEL in tai config?")) {
            r = storagemgr_add_to_tai_config();
            set_status(r == 0
                ? "StorageMgr added to tai config. Reboot to activate."
                : "ERROR: Could not write tai config.");
        } else {
            set_status("Cancelled.");
            r = 0;
        }
    }
    log_close();
    show_result(font, "SD2Vita — StorageMgr", r >= 0, r == 0);
}

/* 4 — Copy ux0 → SD2Vita */
static void op_copy_ux0_to_sd(void *font) {
    char sd_mp[32];
    snprintf(sd_mp, sizeof(sd_mp), "%s:", g_cfg.gcd[0] ? g_cfg.gcd : "uma0");

    char warn[256];
    snprintf(warn, sizeof(warn),
             "Copy all of ux0: to %s? Can take many minutes.", sd_mp);
    if (!confirm_dialog(font, "SD2Vita — Copy ux0 to SD", warn)) {
        set_status("Cancelled."); return;
    }

    log_open();
    SceIoStat st;
    if (sceIoGetstat(sd_mp, &st) < 0) {
        set_status("ERROR: SD2Vita not mounted. Load StorageMgr and reboot first.");
        log_close();
        show_result(font, "SD2Vita — Copy ux0 to SD", 0, 0);
        return;
    }
    show_progress(font, "SD2Vita — Copy ux0 to SD",
                  "Copying ux0: to SD2Vita — please wait...");

    SceUID dfd = sceIoDopen("ux0:");
    if (dfd < 0) {
        set_status("ERROR: Cannot open ux0:");
        log_close(); show_result(font, "SD2Vita — Copy ux0 to SD", 0, 0);
        return;
    }
    SceIoDirent e; int total = 0;
    while (sceIoDread(dfd, &e) > 0) {
        if (e.d_name[0] == '.') continue;
        char src[512], dst[512];
        snprintf(src, sizeof(src), "ux0:%s", e.d_name);
        snprintf(dst, sizeof(dst), "%s/%s", sd_mp, e.d_name);
        if (SCE_S_ISDIR(e.d_stat.st_mode))
            total += copy_dir_recursive(src, dst);
        else if (copy_file_chunked(src, dst) == 0)
            total++;
    }
    sceIoDclose(dfd);
    char msg[256];
    snprintf(msg, sizeof(msg), "Done. %d items copied to %s.", total, sd_mp);
    set_status(msg);
    log_close();
    show_result(font, "SD2Vita — Copy ux0 to SD", 1, 0);
}

/* 5 — Copy SD2Vita → ux0 */
static void op_copy_sd_to_ux0(void *font) {
    char sd_mp[32];
    snprintf(sd_mp, sizeof(sd_mp), "%s:", g_cfg.gcd[0] ? g_cfg.gcd : "uma0");

    char warn[256];
    snprintf(warn, sizeof(warn),
             "Copy all of %s to ux0:? May overwrite existing files.", sd_mp);
    if (!confirm_dialog(font, "SD2Vita — Copy SD to ux0", warn)) {
        set_status("Cancelled."); return;
    }

    log_open();
    SceIoStat st;
    if (sceIoGetstat(sd_mp, &st) < 0) {
        set_status("ERROR: SD2Vita not mounted.");
        log_close(); show_result(font, "SD2Vita — Copy SD to ux0", 0, 0);
        return;
    }
    show_progress(font, "SD2Vita — Copy SD to ux0",
                  "Copying SD2Vita to ux0: — please wait...");

    SceUID dfd = sceIoDopen(sd_mp);
    if (dfd < 0) {
        set_status("ERROR: Cannot open SD2Vita mount point.");
        log_close(); show_result(font, "SD2Vita — Copy SD to ux0", 0, 0);
        return;
    }
    SceIoDirent e; int total = 0;
    while (sceIoDread(dfd, &e) > 0) {
        if (e.d_name[0] == '.') continue;
        char src[512], dst[512];
        snprintf(src, sizeof(src), "%s/%s", sd_mp, e.d_name);
        snprintf(dst, sizeof(dst), "ux0:%s", e.d_name);
        if (SCE_S_ISDIR(e.d_stat.st_mode))
            total += copy_dir_recursive(src, dst);
        else if (copy_file_chunked(src, dst) == 0)
            total++;
    }
    sceIoDclose(dfd);
    char msg[256];
    snprintf(msg, sizeof(msg), "Done. %d items copied to ux0:.", total);
    set_status(msg);
    log_close();
    show_result(font, "SD2Vita — Copy SD to ux0", 1, 0);
}

/* 6 — Format SD2Vita card */
static void op_format_sd(void *font) {
    char mp[32];
    snprintf(mp, sizeof(mp), "%s:", g_cfg.gcd[0] ? g_cfg.gcd : "uma0");

    char warn[256];
    snprintf(warn, sizeof(warn),
             "ALL data on %s (SD2Vita) will be permanently erased.", mp);
    if (!confirm_dialog(font, "SD2Vita — Format Card", warn)) {
        set_status("Cancelled."); return;
    }
    if (!confirm_dialog(font, "SD2Vita — Format Card [2/2]",
                        "Final confirmation — CANNOT be undone.")) {
        set_status("Cancelled."); return;
    }

    log_open();
    SceIoStat st;
    if (sceIoGetstat(mp, &st) < 0) {
        set_status("ERROR: SD2Vita not mounted. Is StorageMgr loaded?");
        log_close(); show_result(font, "SD2Vita — Format Card", 0, 0);
        return;
    }
    show_progress(font, "SD2Vita — Format Card", "Formatting...");

    SceUID dfd = sceIoDopen(mp);
    if (dfd < 0) {
        set_status("ERROR: Cannot open SD2Vita mount point.");
        log_close(); show_result(font, "SD2Vita — Format Card", 0, 0);
        return;
    }
    SceIoDirent e; int n = 0;
    while (sceIoDread(dfd, &e) > 0) {
        if (e.d_name[0] == '.') continue;
        char child[512];
        snprintf(child, sizeof(child), "%s/%s", mp, e.d_name);
        delete_dir_recursive(child);
        n++;
    }
    sceIoDclose(dfd);
    char msg[256];
    snprintf(msg, sizeof(msg),
             "Format complete — %d items removed from %s.", n, mp);
    set_status(msg);
    log_close();
    show_result(font, "SD2Vita — Format Card", 1, 0);
}

/* ══════════════════════════════════════════════════════════════
 * Main screen
 * ══════════════════════════════════════════════════════════════ */
typedef struct {
    const char *label;
    const char *description;
    void (*fn)(void *);
    int dangerous;
} Sd2VitaOpt;

static const Sd2VitaOpt OPTS[] = {
    { "Card & Config Info",
      "Mount points, storage_config.txt, StorageMgr status.",
      op_show_config, 0 },
    { "Switch Mount Point",
      "Set SD2Vita as ux0 / uma0 / grw0 in storage_config.txt.",
      op_switch_mount, 0 },
    { "Install StorageMgr Plugin",
      "Add storagemgr.skprx to *KERNEL in tai config.",
      op_storagemgr, 0 },
    { "Copy ux0 to SD2Vita",
      "Migrate all ux0: content to the SD card.",
      op_copy_ux0_to_sd, 0 },
    { "Copy SD2Vita to ux0",
      "Copy SD card contents back to ux0: (reverse migration).",
      op_copy_sd_to_ux0, 0 },
    { "Format SD2Vita Card",
      "ERASE all data on SD2Vita mount point. IRREVERSIBLE.",
      op_format_sd, 1 },
};
static const int OPT_COUNT = 6;


void sd2vita_screen_run(void *font) {
    draw_init(font);
    int sel = 0;
    SceCtrlData old, ctrl; memset(&old, 0, sizeof(old));
    do { sceCtrlPeekBufferPositive(0, &old, 1); } while (old.buttons);

    parse_config();
    char sd_mp[32] = "";
    int  present   = sd2vita_detect(sd_mp, sizeof(sd_mp));

    while (1) {
        display_start(); display_clear(COLOR_BG);

        s2v_titlebar("Storage Manager");

        /* Status banner — single dedicated row, nothing else on this line */
        display_rect(0, TITLE_H, SCREEN_W, LINE_H + 4,
                     present ? RGBA8(0, 50, 0, 255) : RGBA8(60, 30, 0, 255));
        if (present) {
            char det[48];
            snprintf(det, sizeof(det), "SD2Vita active at %s", sd_mp);
            draw_text(MENU_X, TITLE_H + 4, COLOR_GREEN, det);
        } else {
            draw_text(MENU_X, TITLE_H + 4, COLOR_YELLOW,
                      "Storage device NOT detected — StorageMgr not loaded");
        }
        display_hline(0, TITLE_H + LINE_H + 4, SCREEN_W, RGBA8(60, 60, 60, 255));

        for (int i = 0; i < OPT_COUNT; i++) {
            int y = TITLE_H + LINE_H + 10 + i * (LINE_H * 2 + 4);
            if (y + LINE_H * 2 > FOOTER_Y) break;
            if (i == sel) {
                display_rect(0, y - 1, SCREEN_W - 8,
                             LINE_H * 2 + 2, COLOR_SEL_BG);
                display_text_transp(MENU_X, y,          COLOR_SELECTED, OPTS[i].label);
                display_text_transp(MENU_X, y + LINE_H, COLOR_SELECTED, OPTS[i].description);
                if (OPTS[i].dangerous)
                    display_text_transp(SCREEN_W - 128 - MENU_X, y,
                                        COLOR_SELECTED, "[DANGER]");
            } else {
                draw_text(MENU_X, y,
                          OPTS[i].dangerous ? COLOR_YELLOW : COLOR_TEXT,
                          OPTS[i].label);
                draw_text(MENU_X, y + LINE_H, COLOR_DIM, OPTS[i].description);
                if (OPTS[i].dangerous)
                    draw_text(SCREEN_W - 128 - MENU_X, y, COLOR_RED, "[DANGER]");
            }
        }

        s2v_footer("[X] Execute   [O] Back");
        display_end();

        sceCtrlPeekBufferPositive(0, &ctrl, 1);
        if (btn_pressed(&ctrl, &old, SCE_CTRL_DOWN))
            sel = (sel + 1) % OPT_COUNT;
        if (btn_pressed(&ctrl, &old, SCE_CTRL_UP))
            sel = (sel - 1 + OPT_COUNT) % OPT_COUNT;
        if (btn_pressed(&ctrl, &old, SCE_CTRL_CROSS)) {
            OPTS[sel].fn(font);
            do { sceCtrlPeekBufferPositive(0, &old, 1); } while (old.buttons);
            parse_config();
            present = sd2vita_detect(sd_mp, sizeof(sd_mp));
        }
        if (btn_pressed(&ctrl, &old, SCE_CTRL_CIRCLE)) {
            do { sceCtrlPeekBufferPositive(0, &old, 1); } while (old.buttons);
            return;
        }
        old = ctrl;
    }
}
