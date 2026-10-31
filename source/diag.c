/*
 * diag.c - Boot Diagnostics for PS Vita Recovery Menu
 *
 * v1.2.1 rewrite. Goal: help someone whose Vita misbehaves find the real
 * cause, without false alarms.
 *
 * Severity:
 *   FAIL  can stop the Vita booting CFW or cut it off from storage
 *   WARN  a likely cause of trouble
 *   INFO  a plain fact
 *   OK    checked and fine
 *
 * Screens:
 *   Summary  overall status, one row per section with its result, and the
 *            suggested fixes. Up/Down + X opens a section.
 *   Section  that section's results (long lines wrap) and its fixes.
 *            L/R = previous/next section, O = back to the summary.
 *   Square re-scans from either screen.
 *
 * Sections:
 *   System        model, firmware / exploit, Enso
 *   CFW           taiHEN / HENkaku running, HENkaku files (any tai folder)
 *   tai Config    which config.txt files exist, which one taiHEN reads,
 *                 plugin counts, config backups
 *   Plugins       missing plugin files, wrong section, listed twice,
 *                 HENkaku listed by hand, newest plugin files
 *   Storage       drives, storage plugins (more than one = conflict),
 *                 storage_config.txt
 *   Boot Recovery this app's boot plugins and the hold-R loop breaker
 *
 * Removed in v1.2.1 (false alarms):
 *   - "High plugin count (boot loop risk)" for more than 10 plugins
 *   - HENkaku "MISSING / reinstall CFW" when HENkaku is running
 *   - storage plugin detected on switched-off ('#') lines
 *   - "Config backup: none" when only the automatic backups existed
 *   - "imc0 loads before ux0/ur0" (not how taiHEN works)
 */

#include <psp2/ctrl.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/io/dirent.h>
#include <psp2/vshbridge.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <strings.h>
#include <stdlib.h>

#include "diag.h"
#include "compat.h"
#include "plugins.h"
#include "recovery_installer.h"
#include "draw.h"
#include "display.h"
#include "menu.h"

/* ── Severity ────────────────────────────────────────────────────────────── */
typedef enum { SEV_OK = 0, SEV_INFO = 1, SEV_WARN = 2, SEV_FAIL = 3 } DiagSev;

static const char *SEV_TAG[] = { "[ OK ]", "[INFO]", "[WARN]", "[FAIL]" };
static const uint32_t SEV_COL[] = {
    RGBA8( 80,255, 80,255),   /* OK   green  */
    RGBA8(160,160,160,255),   /* INFO dim    */
    RGBA8(255,220,  0,255),   /* WARN yellow */
    RGBA8(255, 80, 80,255),   /* FAIL red    */
};

/* ── Results ─────────────────────────────────────────────────────────────── */
enum { S_SYSTEM, S_CFW, S_CONFIG, S_PLUGINS, S_STORAGE, S_BOOTREC, S_COUNT };
static const char *const SEC_NAME[S_COUNT] = {
    "System", "CFW (taiHEN / HENkaku)", "tai Config",
    "Plugins", "Storage", "Boot Recovery",
};

#define DIAG_MAX      128
#define DIAG_MSG_LEN  100
#define DIAG_FIX_MAX  24
#define DIAG_FIX_LEN  64

typedef struct { DiagSev sev; int sec; char msg[DIAG_MSG_LEN]; } DiagResult;
typedef struct { int sec; char msg[DIAG_FIX_LEN]; } DiagFix;

static DiagResult g_res[DIAG_MAX];
static int        g_res_n = 0;
static DiagFix    g_fix[DIAG_FIX_MAX];
static int        g_fix_n = 0;
static int        g_sec_warn[S_COUNT], g_sec_fail[S_COUNT];
static int        g_cur = 0;          /* section being filled */

static void begin(int sec) { g_cur = sec; }

static void emit(DiagSev sev, const char *msg) {
    if (g_res_n >= DIAG_MAX) return;
    g_res[g_res_n].sev = sev;
    g_res[g_res_n].sec = g_cur;
    snprintf(g_res[g_res_n].msg, DIAG_MSG_LEN, "%s", msg);
    g_res_n++;
    if (sev == SEV_WARN) g_sec_warn[g_cur]++;
    if (sev == SEV_FAIL) g_sec_fail[g_cur]++;
}

static void emitf(DiagSev sev, const char *fmt, ...) {
    char buf[DIAG_MSG_LEN];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    emit(sev, buf);
}

static void fix(const char *msg) {
    if (g_fix_n >= DIAG_FIX_MAX) return;
    for (int i = 0; i < g_fix_n; i++)                 /* same advice once */
        if (strcmp(g_fix[i].msg, msg) == 0) return;
    g_fix[g_fix_n].sec = g_cur;
    snprintf(g_fix[g_fix_n].msg, DIAG_FIX_LEN, "%s", msg);
    g_fix_n++;
}

/* ── Probe helpers ───────────────────────────────────────────────────────── */
static int path_exists(const char *p) {
    SceIoStat st;
    return sceIoGetstat(p, &st) >= 0;
}

static int mount_ok(const char *p) {
    SceUID d = sceIoDopen(p);
    if (d >= 0) { sceIoDclose(d); return 1; }
    return 0;
}

static const char *base_name(const char *p) {
    const char *b = strrchr(p, '/');
    if (!b) b = strrchr(p, ':');
    return b ? b + 1 : p;
}

static int ends_with_ci(const char *s, const char *suf) {
    int a = (int)strlen(s), b = (int)strlen(suf);
    return a >= b && strcasecmp(s + a - b, suf) == 0;
}

/* Compare two file times: <0, 0, >0 */
static int time_cmp(const SceDateTime *a, const SceDateTime *b) {
    const unsigned short *x = &a->year, *y = &b->year;
    for (int i = 0; i < 6; i++) if (x[i] != y[i]) return (int)x[i] - (int)y[i];
    return 0;
}

/* Is a kernel module loaded right now? The old check only looked for
 * files in ur0:tai, so a running HENkaku was reported as missing. */
static int module_running(const char *name) {
    int buf[2] = { 0, 0 };
    return _vshKernelSearchModuleByName(name, buf) >= 0;
}

/* ── Raw config scan ─────────────────────────────────────────────────────
 * Every plugin line as written in the active config: switched-off ('#')
 * lines are counted but skipped, duplicates are kept (plugins_load()
 * merges them, which would hide "listed twice"). */
#define RAW_MAX 160
typedef struct { char section[32]; char path[160]; } RawPlug;
static RawPlug g_raw[RAW_MAX];
static int     g_raw_n = 0, g_raw_off = 0, g_raw_ok = 0;

static void raw_load(void) {
    g_raw_n = g_raw_off = g_raw_ok = 0;
    const char *cp = plugins_get_config_path();
    if (!cp || cp[0] == '(' || cp[0] == '\0') return;
    SceUID fd = sceIoOpen(cp, SCE_O_RDONLY, 0);
    if (fd < 0) return;
    int sz = (int)sceIoLseek(fd, 0, SCE_SEEK_END);
    sceIoLseek(fd, 0, SCE_SEEK_SET);
    if (sz < 0 || sz > 128 * 1024) { sceIoClose(fd); return; }
    char *buf = malloc(sz + 1);
    if (!buf) { sceIoClose(fd); return; }
    int rd = sceIoRead(fd, buf, sz);
    sceIoClose(fd);
    if (rd != sz) { free(buf); return; }
    buf[sz] = '\0';
    g_raw_ok = 1;

    char sec[32] = "";
    char *line = buf;
    while (line && *line) {
        char *nl = strchr(line, '\n');
        if (nl) *nl = '\0';
        while (*line == ' ' || *line == '\t') line++;
        int n = (int)strlen(line);
        while (n > 0 && (line[n-1] == '\r' || line[n-1] == ' ' || line[n-1] == '\t'))
            line[--n] = '\0';
        if (n > 0) {
            if (line[0] == '*') {
                snprintf(sec, sizeof(sec), "%s", line + 1);
            } else if (line[0] == '#') {
                if (strchr(line, ':') && (strstr(line, ".skprx") || strstr(line, ".suprx")))
                    g_raw_off++;
            } else if (g_raw_n < RAW_MAX) {
                snprintf(g_raw[g_raw_n].section, sizeof(g_raw[0].section), "%s", sec);
                snprintf(g_raw[g_raw_n].path,    sizeof(g_raw[0].path),    "%s", line);
                g_raw_n++;
            }
        }
        line = nl ? nl + 1 : NULL;
    }
    free(buf);
}

/* Enabled *KERNEL line with this file name? */
static int kernel_has(const char *file) {
    for (int i = 0; i < g_raw_n; i++)
        if (strcasecmp(g_raw[i].section, "KERNEL") == 0 &&
            strcasecmp(base_name(g_raw[i].path), file) == 0)
            return 1;
    return 0;
}

/* ══ Checks ═════════════════════════════════════════════════════════════ */

static int g_taihen_up = 0, g_henkaku_up = 0;

static void check_system(void) {
    begin(S_SYSTEM);
    emitf(SEV_INFO, "Model: %s", compat_model_name());
    if (g_compat.fw_reported[0] &&
        strcmp(g_compat.fw_reported, g_compat.fw_display) != 0)
        emitf(SEV_INFO, "Firmware: %s (system reports %s)",
              g_compat.fw_display, g_compat.fw_reported);
    else
        emitf(SEV_INFO, "Firmware: %s", g_compat.fw_display);
    emitf(SEV_INFO, "Exploit: %s", compat_exploit_name());

    if (g_compat.has_enso)
        emit(SEV_OK, "Enso: installed (CFW loads at every boot)");
    else if (g_compat.enso_capable)
        emit(SEV_INFO, "Enso: not installed (run the exploit after each reboot)");
    else {
        emit(SEV_INFO, "Enso: not available on this firmware");
        if (g_compat.fw_tier != FW_UNKNOWN) {
            emit(SEV_INFO, "Run the exploit again after every reboot");
            fix("Want Enso? Downgrade to 3.65 with modoru first");
        }
    }
}

static void check_cfw(void) {
    begin(S_CFW);
    g_taihen_up  = module_running("taihen");
    g_henkaku_up = module_running("HENkaku");

    emit(g_taihen_up  ? SEV_OK : SEV_INFO,
         g_taihen_up  ? "taiHEN: running" : "taiHEN: not detected by name");
    emit(g_henkaku_up ? SEV_OK : SEV_INFO,
         g_henkaku_up ? "HENkaku: running" : "HENkaku: not detected by name");

    /* which: 0 = taiHEN, 1 = HENkaku kernel, 2 = HENkaku Settings menu */
    static const struct { const char *file; int which; } FILES[] = {
        { "taihen.skprx", 0 }, { "henkaku.skprx", 1 }, { "henkaku.suprx", 2 },
    };
    static const char *const DIRS[] = { "ur0:tai/", "ux0:tai/", "imc0:tai/" };
    char path[48];

    for (int i = 0; i < 3; i++) {
        int found = -1;
        for (int d = 0; d < 3 && found < 0; d++) {
            snprintf(path, sizeof(path), "%s%s", DIRS[d], FILES[i].file);
            if (path_exists(path)) found = d;
        }
        if (found >= 0) { emitf(SEV_OK, "%s found in %s", FILES[i].file, DIRS[found]); continue; }

        int running = FILES[i].which == 0 ? g_taihen_up : g_henkaku_up;
        if (FILES[i].which == 2)
            emitf(SEV_INFO, "%s not found (only adds HENkaku Settings)",
                  FILES[i].file);
        else if (running)
            emitf(SEV_INFO, "%s not on disk (loaded by the exploit)", FILES[i].file);
        else if (g_compat.has_enso) {
            emitf(SEV_WARN, "%s missing, and Enso loads it at boot",
                  FILES[i].file);
            fix("Reinstall HENkaku (VitaDeploy) to restore ur0:tai");
        } else
            emitf(SEV_INFO, "%s not on disk (your exploit brings it)", FILES[i].file);
    }
}

static void check_config(void) {
    begin(S_CONFIG);
    int f_ux0  = path_exists("ux0:tai/config.txt");
    int f_ur0  = path_exists("ur0:tai/config.txt");
    int f_imc0 = path_exists("imc0:tai/config.txt");
    const char *active = plugins_get_config_path();

    if (!f_ux0 && !f_ur0 && !f_imc0) {
        emit(SEV_FAIL, "No tai config.txt found anywhere");
        fix("Plugin Fix Mode > Reset to Minimal makes a new config");
        return;
    }
    emitf(SEV_OK, "This app edits: %s", active ? active : "?");

    if (f_ux0 && f_ur0) {
        emit(SEV_WARN, "Two configs: ux0:tai and ur0:tai config.txt");
        emit(SEV_INFO, "taiHEN reads ux0:tai/config.txt first if it exists");
        fix("Keep one config.txt: move the unused one out of tai/");
    } else {
        if (f_ux0) emit(SEV_INFO, "ux0:tai/config.txt present");
        if (f_ur0) emit(SEV_INFO, "ur0:tai/config.txt present");
    }
    if (f_imc0) emit(SEV_INFO, "imc0:tai/config.txt present (internal storage)");

    if (!g_raw_ok) {
        emit(SEV_WARN, "Could not read the active config");
        return;
    }

    /* Counts by section (a count alone is not a problem) */
    char line[DIAG_MSG_LEN];
    emitf(SEV_INFO, "Plugins: %d on, %d off", g_raw_n, g_raw_off);
    char seen[12][32]; int cnt[12]; int ns = 0;
    for (int i = 0; i < g_raw_n; i++) {
        int k;
        for (k = 0; k < ns; k++) if (strcasecmp(seen[k], g_raw[i].section) == 0) break;
        if (k == ns && ns < 12) { snprintf(seen[ns], 32, "%s", g_raw[i].section); cnt[ns++] = 0; }
        if (k < 12) cnt[k]++;
    }
    int pos = snprintf(line, sizeof(line), "By section:");
    for (int k = 0; k < ns && pos < (int)sizeof(line) - 1; k++)
        pos += snprintf(line + pos, sizeof(line) - pos, " %s %d%s",
                        seen[k][0] ? seen[k] : "?", cnt[k], k + 1 < ns ? "," : "");
    if (ns) emit(SEV_INFO, line);

    /* Backups made by this app */
    int backups = 0;
    char p[96];
    snprintf(p, sizeof(p), "%s.bak", active ? active : "");
    if (active && path_exists(p)) backups++;
    if (path_exists("ur0:tai/config_backup.txt")) backups++;   /* Plugin Fix */
    SceUID d = sceIoDopen("ux0:data/vita_recovery/tai_backup");
    if (d >= 0) {
        SceIoDirent e;
        while (sceIoDread(d, &e) > 0) if (e.d_name[0] != '.') backups++;
        sceIoDclose(d);
    }
    if (backups) emitf(SEV_OK, "Config backups: %d saved", backups);
    else {
        emit(SEV_INFO, "Config backups: none yet");
        fix("Plugin Fix Mode > Backup Config before big changes");
    }
}

static void check_plugins(void) {
    begin(S_PLUGINS);
    if (!g_raw_ok) { emit(SEV_INFO, "Config not readable, skipped"); return; }
    int problems = 0, shown = 0;

    /* 1. Missing plugin files */
    for (int i = 0; i < g_raw_n; i++) {
        if (path_exists(g_raw[i].path)) continue;
        problems++;
        if (shown++ < 10)
            emitf(SEV_WARN, "File missing (*%s): %s", g_raw[i].section, g_raw[i].path);
        fix("Reinstall missing plugins or turn them off");
    }
    if (shown > 10) emitf(SEV_WARN, "...and %d more missing files", shown - 10);

    /* 2. Wrong section: kernel plugin outside *KERNEL, user plugin in it */
    for (int i = 0; i < g_raw_n; i++) {
        int is_k = strcasecmp(g_raw[i].section, "KERNEL") == 0;
        if (!is_k && ends_with_ci(g_raw[i].path, ".skprx")) {
            problems++;
            emitf(SEV_WARN, "Kernel plugin under *%s won't load: %s",
                  g_raw[i].section, base_name(g_raw[i].path));
            fix("Move .skprx lines under *KERNEL in config.txt");
        } else if (is_k && ends_with_ci(g_raw[i].path, ".suprx")) {
            problems++;
            emitf(SEV_WARN, "User plugin under *KERNEL won't load: %s",
                  base_name(g_raw[i].path));
            fix("Move .suprx lines under *main or a game section");
        }
    }

    /* 3. Listed twice in the same section */
    for (int i = 0; i < g_raw_n; i++)
        for (int j = 0; j < i; j++)
            if (strcasecmp(g_raw[i].section, g_raw[j].section) == 0 &&
                strcasecmp(g_raw[i].path, g_raw[j].path) == 0) {
                problems++;
                emitf(SEV_WARN, "Listed twice in *%s: %s",
                      g_raw[i].section, base_name(g_raw[i].path));
                fix("Plugins > Remove Duplicates");
                break;
            }

    /* 4. HENkaku/taiHEN listed by hand (they are loaded by the exploit) */
    if (kernel_has("henkaku.skprx") || kernel_has("taihen.skprx")) {
        problems++;
        emit(SEV_WARN, "taihen/henkaku.skprx listed in *KERNEL");
        emit(SEV_INFO, "They load automatically; listing them loads them twice");
        fix("Turn off the henkaku.skprx/taihen.skprx lines");
    }

    if (!problems)
        emitf(SEV_OK, "No problems in %d enabled plugin lines", g_raw_n);

    /* 5. Newest plugin files: after "it broke after I installed X",
     *    the newest one is the first suspect. */
    int idx[3] = { -1, -1, -1 };
    SceIoStat st[3], s;
    for (int i = 0; i < g_raw_n; i++) {
        int dup = 0;                        /* same file listed twice */
        for (int k = 0; k < 3; k++)
            if (idx[k] >= 0 && strcasecmp(g_raw[idx[k]].path, g_raw[i].path) == 0) dup = 1;
        if (dup || sceIoGetstat(g_raw[i].path, &s) < 0) continue;
        for (int k = 0; k < 3; k++) {
            if (idx[k] < 0 || time_cmp(&s.st_mtime, &st[k].st_mtime) > 0) {
                for (int m = 2; m > k; m--) { idx[m] = idx[m-1]; st[m] = st[m-1]; }
                idx[k] = i; st[k] = s;
                break;
            }
        }
    }
    for (int k = 0; k < 3 && idx[k] >= 0; k++)
        emitf(SEV_INFO, "%s %04u-%02u-%02u  %s (*%s)", k == 0 ? "Newest:" : "       ",
              st[k].st_mtime.year, st[k].st_mtime.month, st[k].st_mtime.day,
              base_name(g_raw[idx[k]].path), g_raw[idx[k]].section);
    {
        SceIoStat cs;
        const char *cp = plugins_get_config_path();
        if (cp && sceIoGetstat(cp, &cs) >= 0)
            emitf(SEV_INFO, "Config last changed: %04u-%02u-%02u %02u:%02u",
                  cs.st_mtime.year, cs.st_mtime.month, cs.st_mtime.day,
                  cs.st_mtime.hour, cs.st_mtime.minute);
    }
    if (idx[0] >= 0)
        fix("New plugin, new trouble? Turn the newest one off");
}

static void check_storage(void) {
    begin(S_STORAGE);

    if (mount_ok("ux0:")) emit(SEV_OK, "ux0: mounted (memory card / SD2Vita)");
    else {
        emit(SEV_FAIL, "ux0: not mounted");
        fix("ux0: missing: check the SD card and storage plugin");
    }
    if (mount_ok("ur0:")) emit(SEV_OK, "ur0: mounted (system data)");
    else {
        emit(SEV_FAIL, "ur0: not mounted");
        fix("ur0: missing: restart; if it stays, Sony Safe Mode");
    }
    int slim = (g_compat.model == MODEL_VITA_2000 || g_compat.model == MODEL_PSTV);
    if (mount_ok("imc0:")) emit(SEV_INFO, "imc0: mounted (internal storage)");
    else if (slim) emit(SEV_INFO, "imc0: not mounted (may be remapped)");
    if (mount_ok("uma0:")) emit(SEV_INFO, "uma0: mounted");
    if (mount_ok("grw0:")) emit(SEV_INFO, "grw0: mounted");
    if (mount_ok("xmc0:")) emit(SEV_INFO, "xmc0: mounted");

    /* Storage plugins, enabled lines only */
    static const struct { const char *file; const char *name; } SP[] = {
        { "storagemgr.skprx", "StorageMgr" }, { "yamt.skprx",     "YAMT"     },
        { "gamesd.skprx",     "gamesd"     }, { "sdgamesd.skprx", "sdgamesd" },
        { "usbmc.skprx",      "usbmc"      },
    };
    char on[80] = "";
    int n_on = 0, has_smgr = 0;
    for (int i = 0; i < (int)(sizeof(SP) / sizeof(SP[0])); i++)
        if (kernel_has(SP[i].file)) {
            n_on++;
            if (i == 0) has_smgr = 1;
            snprintf(on + strlen(on), sizeof(on) - strlen(on), "%s%s",
                     n_on > 1 ? " + " : "", SP[i].name);
        }
    if (n_on == 0)
        emit(SEV_INFO, "No storage plugin on (Sony memory card mode)");
    else if (n_on == 1)
        emitf(SEV_OK, "Storage plugin: %s", on);
    else {
        emitf(SEV_FAIL, "Storage plugins fight each other: %s", on);
        fix("Keep only ONE storage plugin on (Plugins menu)");
    }

    if (has_smgr) {
        if (path_exists("ur0:tai/storage_config.txt") ||
            path_exists("ux0:tai/storage_config.txt"))
            emit(SEV_OK, "storage_config.txt found");
        else {
            emit(SEV_WARN, "StorageMgr on, but storage_config.txt missing");
            fix("Storage Manager > Install StorageMgr Plugin");
        }
    }
}

static void check_bootrec(void) {
    begin(S_BOOTREC);
    int st = recovery_installer_needs_update();
    int listed = kernel_has("boot_recovery.skprx");
    if (st == 1) {
        emit(SEV_WARN, "Boot plugins are older than this app");
        fix("Advanced > Boot Recovery > Install / Update");
    } else if (st == 2) {
        emit(SEV_WARN, "Boot Recovery is only partly installed");
        fix("Advanced > Boot Recovery > Install / Update");
    } else if (listed) {
        emit(SEV_OK, "Boot plugins installed and up to date");
    } else {
        emit(SEV_INFO, "Boot Recovery not installed (hold R off)");
    }
    if (path_exists("ur0:tai/recovery_launch_pending"))
        emit(SEV_INFO, "A hold-R launch did not reach the app last boot");
}

/* ══ Scan ═══════════════════════════════════════════════════════════════ */

static void diag_scan(void) {
    g_res_n = g_fix_n = 0;
    memset(g_sec_warn, 0, sizeof(g_sec_warn));
    memset(g_sec_fail, 0, sizeof(g_sec_fail));
    plugins_load();
    raw_load();
    check_system();
    check_cfw();
    check_config();
    check_plugins();
    check_storage();
    check_bootrec();
}

static int total_warn(void) { int n = 0; for (int i = 0; i < S_COUNT; i++) n += g_sec_warn[i]; return n; }
static int total_fail(void) { int n = 0; for (int i = 0; i < S_COUNT; i++) n += g_sec_fail[i]; return n; }

/* ══ Drawing ════════════════════════════════════════════════════════════ */

#define MSG_X      (MENU_X + 104)                  /* after the "[WARN]" tag  */
#define MSG_CHARS  ((SCREEN_W - MSG_X - 24) / FONT_W)   /* 49 per row       */
#define FULL_CHARS ((SCREEN_W - MENU_X * 2) / FONT_W)   /* 55 per row       */
#define BODY_T     (TITLE_H + 6)
#define BODY_B     (FOOTER_Y - 6)

static void chrome(const char *sub, const char *hint) {
    display_start();
    display_clear(COLOR_BG);
    display_rect(0, 0, SCREEN_W, TITLE_LINE, COLOR_TITLE_BG);
    display_app_title(6);
    display_text(MENU_X, 26, COLOR_YELLOW, sub);
    display_hline(0, TITLE_LINE, SCREEN_W, COLOR_GREEN);
    display_rect(0, FOOTER_Y, SCREEN_W, SCREEN_H - FOOTER_Y, COLOR_TITLE_BG);
    display_hline(0, FOOTER_LINE, SCREEN_W, COLOR_GREEN);
    display_text(MENU_X, FOOTER_Y + 7, COLOR_DIM, hint);
}

/* Split text into rows of at most w chars, breaking at spaces.
 * Calls back once per row; returns the number of rows. */
typedef void (*RowFn)(int row, const char *txt, void *ctx);
static int wrap(const char *s, int w, RowFn fn, void *ctx) {
    char row[DIAG_MSG_LEN + 1];
    int n = 0;
    while (*s) {
        int len = (int)strlen(s);
        int take = len <= w ? len : w;
        if (len > w) {
            int b = take;
            while (b > 0 && s[b] != ' ') b--;
            if (b > w / 3) take = b;           /* break at a space if sensible */
        }
        memcpy(row, s, take);
        row[take] = '\0';
        if (fn) fn(n, row, ctx);
        n++;
        s += take;
        while (*s == ' ') s++;
    }
    return n ? n : 1;
}

/* Status text and colour for a section row */
static void sec_status(int s, char *out, int len, uint32_t *col) {
    if (g_sec_fail[s]) {
        snprintf(out, len, "%d PROBLEM%s", g_sec_fail[s], g_sec_fail[s] > 1 ? "S" : "");
        *col = SEV_COL[SEV_FAIL];
    } else if (g_sec_warn[s]) {
        snprintf(out, len, "%d WARNING%s", g_sec_warn[s], g_sec_warn[s] > 1 ? "S" : "");
        *col = SEV_COL[SEV_WARN];
    } else {
        snprintf(out, len, "OK");
        *col = SEV_COL[SEV_OK];
    }
}

static void draw_summary(int sel) {
    chrome("Boot Diagnostics",
           "[X] Open  [Up/Down] Select  [Square] Re-scan  [O] Back");

    int fails = total_fail(), warns = total_warn();
    int y = BODY_T + 2;

    /* Overall status band */
    uint32_t band = fails ? RGBA8(70, 10, 10, 255)
                  : warns ? RGBA8(60, 45, 0, 255) : RGBA8(0, 50, 0, 255);
    display_rect(0, y - 2, SCREEN_W, LINE_H * 2 + 6, band);
    char head[64];
    if (!fails && !warns)
        snprintf(head, sizeof(head), "NO PROBLEMS FOUND");
    else if (!warns)
        snprintf(head, sizeof(head), "%d PROBLEM%s", fails, fails == 1 ? "" : "S");
    else if (!fails)
        snprintf(head, sizeof(head), "%d WARNING%s", warns, warns == 1 ? "" : "S");
    else
        snprintf(head, sizeof(head), "%d PROBLEM%s, %d WARNING%s",
                 fails, fails == 1 ? "" : "S", warns, warns == 1 ? "" : "S");
    display_text(MENU_X, y + 2, fails ? SEV_COL[SEV_FAIL]
                               : warns ? SEV_COL[SEV_WARN] : SEV_COL[SEV_OK], head);
    char facts[96];
    snprintf(facts, sizeof(facts), "%s | FW %s | %s", compat_model_name(),
             g_compat.fw_display, g_compat.has_enso ? "Enso" : "no Enso");
    facts[FULL_CHARS] = '\0';
    display_text(MENU_X, y + 2 + LINE_H, COLOR_DIM, facts);
    y += LINE_H * 2 + 12;

    /* One row per section */
    for (int s = 0; s < S_COUNT; s++) {
        char st[24]; uint32_t col;
        sec_status(s, st, sizeof(st), &col);
        int sx = SCREEN_W - MENU_X - display_text_width(st);
        if (s == sel) {
            display_rect(0, y - 2, SCREEN_W, LINE_H + 2, COLOR_SEL_BG);
            display_text_transp(MENU_X, y + 2, COLOR_SELECTED, SEC_NAME[s]);
            display_text_transp(sx, y + 2, COLOR_SELECTED, st);
        } else {
            display_text(MENU_X, y + 2, COLOR_TEXT, SEC_NAME[s]);
            display_text(sx, y + 2, col, st);
        }
        y += LINE_H + 2;
    }

    /* Suggested fixes, most important sections first */
    y += 8;
    display_hline(MENU_X, y, SCREEN_W - MENU_X * 2, RGBA8(60, 60, 60, 255));
    y += 8;
    if (g_fix_n == 0) {
        display_text(MENU_X, y, COLOR_DIM, "Nothing to fix.");
        return;
    }
    display_text(MENU_X, y, COLOR_DIM, (fails || warns) ? "What to do:" : "Tips:");
    y += LINE_H;
    int shown = 0, full = 0;
    for (int pass = 0; pass < 3 && !full; pass++)   /* FAIL, WARN, the rest */
        for (int i = 0; i < g_fix_n && !full; i++) {
            int s = g_fix[i].sec;
            int lvl = g_sec_fail[s] ? 0 : g_sec_warn[s] ? 1 : 2;
            if (lvl != pass) continue;
            if (y + LINE_H * 2 > BODY_B && shown + 1 < g_fix_n) { full = 1; break; }
            char t[DIAG_FIX_LEN + 4];
            snprintf(t, sizeof(t), "- %s", g_fix[i].msg);
            t[FULL_CHARS] = '\0';
            display_text(MENU_X, y, lvl == 2 ? COLOR_DIM : COLOR_YELLOW, t);
            y += LINE_H;
            shown++;
        }
    if (full) {
        char t[48];
        snprintf(t, sizeof(t), "(+%d more inside the sections)", g_fix_n - shown);
        display_text(MENU_X, y, COLOR_DIM, t);
    }
}

/* Detail view: rows built once per open, drawn with scrolling */
typedef struct { int y; int scroll; int top; int bottom; uint32_t col; } RowCtx;
static void put_row(int row, const char *txt, void *vctx) {
    RowCtx *c = (RowCtx *)vctx;
    int ry = c->y - c->scroll;
    if (ry >= c->top && ry + LINE_H <= c->bottom)
        display_text(MSG_X, ry, c->col, txt);
    (void)row;
    c->y += LINE_H;
}

/* Height in pixels of the section's content (for scroll limits) */
static int detail_height(int sec) {
    int rows = 0;
    for (int i = 0; i < g_res_n; i++)
        if (g_res[i].sec == sec) rows += wrap(g_res[i].msg, MSG_CHARS, NULL, NULL);
    int nf = 0;
    for (int i = 0; i < g_fix_n; i++)
        if (g_fix[i].sec == sec) { if (!nf++) rows += 2; rows++; }
    return rows * LINE_H;
}

static void draw_detail(int sec, int scroll) {
    char sub[64];
    snprintf(sub, sizeof(sub), "Boot Diagnostics > %s  (%d/%d)",
             SEC_NAME[sec], sec + 1, S_COUNT);
    chrome(sub, "[L/R] Section [Up/Dn] Scroll [Square] Rescan [O] Back");

    RowCtx c = { BODY_T + 4, scroll, BODY_T, BODY_B, 0 };
    for (int i = 0; i < g_res_n; i++) {
        if (g_res[i].sec != sec) continue;
        DiagResult *r = &g_res[i];
        int ry = c.y - scroll;
        if (ry >= BODY_T && ry + LINE_H <= BODY_B)
            display_text(MENU_X, ry, SEV_COL[r->sev], SEV_TAG[r->sev]);
        c.col = r->sev == SEV_OK   ? COLOR_TEXT
              : r->sev == SEV_INFO ? COLOR_DIM
              : r->sev == SEV_WARN ? COLOR_YELLOW : COLOR_RED;
        wrap(r->msg, MSG_CHARS, put_row, &c);
    }

    int first = 1;
    for (int i = 0; i < g_fix_n; i++) {
        if (g_fix[i].sec != sec) continue;
        if (first) {
            c.y += LINE_H;
            int ry = c.y - scroll;
            if (ry >= BODY_T && ry + LINE_H <= BODY_B)
                display_text(MENU_X, ry, COLOR_DIM, "What to do:");
            c.y += LINE_H;
            first = 0;
        }
        int ry = c.y - scroll;
        if (ry >= BODY_T && ry + LINE_H <= BODY_B) {
            char t[DIAG_FIX_LEN + 4];
            snprintf(t, sizeof(t), "- %s", g_fix[i].msg);
            t[FULL_CHARS] = '\0';
            display_text(MENU_X, ry, COLOR_YELLOW, t);
        }
        c.y += LINE_H;
    }

    /* Scroll bar when the section is taller than the screen */
    int h = detail_height(sec), view = BODY_B - BODY_T - 4;
    if (h > view) {
        int bar = view * view / h; if (bar < 16) bar = 16;
        int by = BODY_T + (scroll * (view - bar)) / (h - view);
        display_rect(SCREEN_W - 6, BODY_T, 3, view, RGBA8(30, 30, 30, 255));
        display_rect(SCREEN_W - 6, by, 3, bar, COLOR_GREEN);
    }
}

/* ══ Input / main loop ═════════════════════════════════════════════════ */

static int pressed(SceCtrlData *now, SceCtrlData *old, unsigned int b) {
    return (now->buttons & b) && !(old->buttons & b);
}

static void drain(SceCtrlData *old) {
    do { sceCtrlPeekBufferPositive(0, old, 1); } while (old->buttons);
}

static void busy(void) {
    chrome("Boot Diagnostics", "");
    display_text(MENU_X, BODY_T + 8, COLOR_YELLOW, "Checking your Vita...");
    display_end();
}

void diag_run(void) {
    busy();
    diag_scan();

    /* Open on the first section with a problem, else on the first one */
    int sel = 0, open = -1, scroll = 0;
    for (int s = S_COUNT - 1; s >= 0; s--)
        if (g_sec_fail[s] || g_sec_warn[s]) sel = s;

    SceCtrlData old, now;
    drain(&old);

    while (1) {
        if (open < 0) draw_summary(sel);
        else          draw_detail(open, scroll);
        display_end();

        sceCtrlPeekBufferPositive(0, &now, 1);

        if (pressed(&now, &old, SCE_CTRL_SQUARE)) {
            busy();
            diag_scan();
            scroll = 0;
            drain(&old);
            continue;
        }

        if (open < 0) {
            if (pressed(&now, &old, SCE_CTRL_DOWN)) sel = (sel + 1) % S_COUNT;
            if (pressed(&now, &old, SCE_CTRL_UP))   sel = (sel + S_COUNT - 1) % S_COUNT;
            if (pressed(&now, &old, SCE_CTRL_CROSS)) { open = sel; scroll = 0; }
            if (pressed(&now, &old, SCE_CTRL_CIRCLE)) { drain(&old); break; }
        } else {
            int view = BODY_B - BODY_T - 4;
            int max = detail_height(open) - view;
            if (max < 0) max = 0;
            if (pressed(&now, &old, SCE_CTRL_DOWN)) { scroll += LINE_H * 3; if (scroll > max) scroll = max; }
            if (pressed(&now, &old, SCE_CTRL_UP))   { scroll -= LINE_H * 3; if (scroll < 0) scroll = 0; }
            if (pressed(&now, &old, SCE_CTRL_RTRIGGER) || pressed(&now, &old, SCE_CTRL_RIGHT)) {
                open = (open + 1) % S_COUNT; sel = open; scroll = 0;
            }
            if (pressed(&now, &old, SCE_CTRL_LTRIGGER) || pressed(&now, &old, SCE_CTRL_LEFT)) {
                open = (open + S_COUNT - 1) % S_COUNT; sel = open; scroll = 0;
            }
            if (pressed(&now, &old, SCE_CTRL_CIRCLE)) { open = -1; drain(&old); continue; }
        }
        old = now;
    }
}
