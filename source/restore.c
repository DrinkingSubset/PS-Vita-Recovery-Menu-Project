/*
 * restore.c — Vita soft-brick recovery engine
 *
 * Covers 4 recovery scenarios:
 *   1. Safe Mode Boot      — Comment out ALL plugins so the system can boot clean
 *   2. Reset taiHEN config — Overwrite config.txt with a known-good minimal config
 *   3. Backup / Restore    — Copy ux0:tai/ to/from ux0:data/vita_recovery/tai_backup/
 *   4. Rebuild LiveArea DB — handled by the Sony Recovery flow (boot plugin)
 *
 * v1.1.7 (code review finding 4): every config write goes through
 * write_file(): temp file -> read-back check -> swap with the old file kept
 * as .old until the swap succeeds. Safe Mode and Reset back up each config
 * first, keep the plugins needed to reach ux0: and this menu, and keep
 * blank lines / comments.
 *
 * Storage root: ux0:data/vita_recovery/
 */

#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/io/dirent.h>
#include <psp2/kernel/processmgr.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <strings.h>
#include "restore.h"
#include "compat.h"

/* ── Paths ────────────────────────────────────────────────────────────────── */
#define RECOVERY_ROOT    "ux0:data/vita_recovery"
#define TAI_BACKUP_DIR   "ux0:data/vita_recovery/tai_backup"
/*
 * taiHEN config load order (ALL models):
 *   1. imc0:tai/config.txt  — PCH-2000 & PSTV only, HIGHEST priority
 *   2. ux0:tai/config.txt   — overrides ur0: when present
 *   3. ur0:tai/config.txt   — canonical HENkaku location
 * We must check and patch ALL locations that exist on this hardware.
 */
#define TAI_CONFIG_IMC0  "imc0:tai/config.txt"
#define TAI_CONFIG_UX0   "ux0:tai/config.txt"
#define TAI_CONFIG_UR0   "ur0:tai/config.txt"
/* Legacy single-path macro kept for internal helpers */
#define TAI_SOURCE_DIR   "ux0:tai"
#define TAI_CONFIG       TAI_CONFIG_UR0
#define LOG_PATH         "ux0:data/vita_recovery/restore.log"

/* ── Small file-existence helper ──────────────────────────────────────────── */
static int file_exists(const char *p) {
    SceIoStat s; return (sceIoGetstat(p, &s) >= 0);
}

/* ── State ────────────────────────────────────────────────────────────────── */
static char          g_status_msg[256] = "Ready.";
static RestoreStatus g_last_status     = RESTORE_OK;
static SceUID        g_log_fd          = -1;

/* ── Logging ──────────────────────────────────────────────────────────────── */
static void log_open(void) {
    sceIoMkdir(RECOVERY_ROOT, 0777);
    /* v1.1.8: keep the log small. Over 32 KB it is moved to restore.log.old
     * (one previous log kept) and a new one is started. */
    SceIoStat ls;
    if (sceIoGetstat(LOG_PATH, &ls) >= 0 && ls.st_size > 32 * 1024) {
        sceIoRemove(LOG_PATH ".old");
        sceIoRename(LOG_PATH, LOG_PATH ".old");
    }
    g_log_fd = sceIoOpen(LOG_PATH,
                         SCE_O_WRONLY | SCE_O_CREAT | SCE_O_APPEND, 0777);
}
static void log_write(const char *msg) {
    if (g_log_fd >= 0) {
        sceIoWrite(g_log_fd, msg, strlen(msg));
        sceIoWrite(g_log_fd, "\n", 1);
    }
}
static void log_close(void) {
    if (g_log_fd >= 0) { sceIoClose(g_log_fd); g_log_fd = -1; }
}
static void set_status(RestoreStatus st, const char *msg) {
    g_last_status = st;
    snprintf(g_status_msg, sizeof(g_status_msg), "%s", msg);
    log_write(msg);
}

/* ── File helpers ─────────────────────────────────────────────────────────── */
static int read_file(const char *path, char **out, int *out_len) {
    SceUID fd = sceIoOpen(path, SCE_O_RDONLY, 0);
    if (fd < 0) return -1;
    /* Use sceIoLseek for size — sceIoGetstatByFd is broken on some FW */
    int size = (int)sceIoLseek(fd, 0, SCE_SEEK_END);
    sceIoLseek(fd, 0, SCE_SEEK_SET);
    if (size < 0 || size > 512 * 1024) { sceIoClose(fd); return -2; }
    *out = malloc(size + 1);
    if (!*out) { sceIoClose(fd); return -2; }
    int rd = 0;
    while (rd < size) {
        int r = sceIoRead(fd, *out + rd, size - rd);
        if (r <= 0) break;
        rd += r;
    }
    sceIoClose(fd);
    if (rd != size) { free(*out); *out = NULL; return -3; }
    (*out)[size] = '\0';
    if (out_len) *out_len = size;
    return 0;
}

static int write_raw(const char *path, const char *data, int len) {
    SceUID fd = sceIoOpen(path,
                          SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
    if (fd < 0) return -1;
    int done = 0;
    while (done < len) {
        int w = sceIoWrite(fd, data + done, len - done);
        if (w <= 0) { sceIoClose(fd); return -1; }
        done += w;
    }
    sceIoClose(fd);
    return 0;
}

/* 1 if the file at path holds exactly data */
static int file_matches(const char *path, const char *data, int len) {
    char *back = NULL; int bl = 0;
    if (read_file(path, &back, &bl) < 0) return 0;
    int same = (bl == len) && memcmp(back, data, len) == 0;
    free(back);
    return same;
}

/* Safe write: tmp -> verify -> swap (old kept as .old until the swap is done).
 * At every moment either the old or the new file exists. */
static int write_file(const char *path, const char *data, int len) {
    char tmp[300], old[300];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    snprintf(old, sizeof(old), "%s.old", path);
    if (write_raw(tmp, data, len) < 0 || !file_matches(tmp, data, len)) {
        sceIoRemove(tmp);
        return -1;                       /* original untouched */
    }
    if (!file_exists(path))
        return sceIoRename(tmp, path) < 0 ? -2 : 0;
    sceIoRemove(old);
    if (sceIoRename(path, old) < 0) { sceIoRemove(tmp); return -3; }
    if (sceIoRename(tmp, path) < 0) {
        sceIoRename(old, path);          /* roll back */
        sceIoRemove(tmp);
        return -4;
    }
    sceIoRemove(old);
    return file_matches(path, data, len) ? 0 : -5;
}

/* Plugins that must stay enabled in Safe Mode / Reset, or the console loses
 * HENkaku features, ux0: (SD2Vita) or the way back into this menu. */
static int keep_plugin(const char *line) {
    static const char *KEEP[] = {
        "henkaku.skprx", "henkaku.suprx", "storagemgr.skprx", "gamesd.skprx",
        "boot_recovery.skprx", "boot_trigger.suprx",
    };
    const char *name = strrchr(line, '/');
    name = name ? name + 1 : line;
    for (unsigned i = 0; i < sizeof(KEEP) / sizeof(KEEP[0]); i++)
        if (strncasecmp(name, KEEP[i], strlen(KEEP[i])) == 0) return 1;
    return 0;
}

/* Backup name for a config path, e.g. ur0:tai/config.txt ->
 * tai_backup/ur0_config_<tag>.txt */
static void backup_name(const char *config_path, const char *tag,
                        char *out, int outlen) {
    char dev[8] = "x";
    int i = 0;
    while (config_path[i] && config_path[i] != ':' && i < 7) { dev[i] = config_path[i]; i++; }
    dev[i] = '\0';
    snprintf(out, outlen, TAI_BACKUP_DIR "/%s_config_%s.txt", dev, tag);
}

static int copy_file(const char *src, const char *dst) {
    char *buf = NULL; int len = 0;   /* uses the safe write_file() */
    if (read_file(src, &buf, &len) < 0) return -1;
    int ret = write_file(dst, buf, len);
    free(buf);
    return ret;
}

/* Copies every file in src_dir into dst_dir (one level, no deep recursion) */
static int copy_dir(const char *src, const char *dst) {
    sceIoMkdir(dst, 0777);
    SceUID dfd = sceIoDopen(src);
    if (dfd < 0) return -1;
    SceIoDirent entry;
    int copied = 0;
    while (sceIoDread(dfd, &entry) > 0) {
        if (entry.d_name[0] == '.') continue;
        char sp[256], dp[256];
        snprintf(sp, sizeof(sp), "%s/%s", src, entry.d_name);
        snprintf(dp, sizeof(dp), "%s/%s", dst, entry.d_name);
        if (SCE_S_ISDIR(entry.d_stat.st_mode)) {
            copy_dir(sp, dp);
        } else {
            if (copy_file(sp, dp) == 0) copied++;
        }
    }
    sceIoDclose(dfd);
    return copied;
}

/* ── Safe minimal taiHEN config ───────────────────────────────────────────── */
/*
 * Minimal safe tai config — works on ALL supported hardware/firmware:
 *   PCH-1000, PCH-2000, PSTV  /  FW 3.60, 3.65, 3.67, 3.68
 *
 * Includes the mandatory henkaku.suprx entries for *NPXS10015 / *NPXS10016
 * (firmware version string patches) and *main (SceShell patches).
 * These are required for HENkaku to function correctly on all four firmware
 * versions. Without them on 3.65-3.68 the HENkaku Settings menu disappears.
 *
 * henkaku.skprx is hard-coded to load at kernel level and MUST NOT be listed
 * under *KERNEL — taiHEN handles it automatically.
 */
static const char SAFE_TAI_CONFIG[] =
    "# taiHEN config.txt - SAFE DEFAULTS\n"
    "# Generated by Vita Recovery Menu.\n"
    "# Compatible: PCH-1000 / PCH-2000 / PSTV | FW 3.60 / 3.65 / 3.67 / 3.68\n"
    "#\n"
    "# Re-add your own plugins ONE AT A TIME to find which one caused the brick.\n"
    "# Kernel plugins need a full REBOOT to take effect.\n"
    "# User plugins need a taiHEN refresh (HENkaku Settings) to take effect.\n"
    "#\n"
    "*KERNEL\n"
    "# henkaku.skprx is hard-coded; do NOT list it here.\n"
    "# Add kernel plugins (.skprx) here:\n"
    "# ur0:tai/storagemgr.skprx\n"
    "\n"
    "*NPXS10015\n"
    "# Required: patches the firmware version string display\n"
    "ur0:tai/henkaku.suprx\n"
    "\n"
    "*NPXS10016\n"
    "# Required: patches firmware version in Settings widget\n"
    "ur0:tai/henkaku.suprx\n"
    "\n"
    "*main\n"
    "# Required: SceShell (LiveArea) patches for HENkaku Settings\n"
    "ur0:tai/henkaku.suprx\n"
    "# Add LiveArea user plugins (.suprx) here:\n"
    "\n"
    "*ALL\n"
    "# Add plugins that should run in ALL apps here:\n"
    "\n";

/* ════════════════════════════════════════════════════════════════════════════
 * PUBLIC API
 * ════════════════════════════════════════════════════════════════════════════ */

const char   *restore_get_status_msg(void)    { return g_status_msg; }
RestoreStatus restore_get_last_status(void)   { return g_last_status; }

void restore_ensure_dirs(void) {
    sceIoMkdir(RECOVERY_ROOT,  0777);
    sceIoMkdir(TAI_BACKUP_DIR, 0777);
}

/* ── FUNCTION 1: Safe Mode Boot ───────────────────────────────────────────── */
/*
 * Patch EVERY tai config that exists on this device.
 * On PCH-2000/PSTV: patches imc0:tai/, ux0:tai/, and ur0:tai/ — all three.
 * On PCH-1000/1100: patches ux0:tai/ and ur0:tai/ (no imc0:).
 * On 3.67/3.68 without Enso: ur0:tai/ may not exist; handled gracefully.
 * This guarantees plugins are disabled regardless of which config taiHEN picks.
 */
static RestoreStatus safe_mode_one_config(const char *config_path) {
    char *buf = NULL; int len = 0;
    if (read_file(config_path, &buf, &len) < 0) return RESTORE_ERR_IO;

    char *out = malloc(len * 2 + 256);
    if (!out) { free(buf); return RESTORE_ERR_IO; }
    int out_pos = 0, disabled = 0;

    /* Walk lines by hand so blank lines and \r\n endings are kept */
    char *p = buf, *end = buf + len;
    while (p < end) {
        char *nl = memchr(p, '\n', end - p);
        int ll = nl ? (int)(nl - p) : (int)(end - p);
        int cr = (ll > 0 && p[ll - 1] == '\r');
        int tl = cr ? ll - 1 : ll;

        char *t = p; int tlen = tl;
        while (tlen > 0 && (*t == ' ' || *t == '\t')) { t++; tlen--; }
        char tmp[300];
        int cl = tlen < (int)sizeof(tmp) - 1 ? tlen : (int)sizeof(tmp) - 1;
        memcpy(tmp, t, cl); tmp[cl] = '\0';

        int is_plugin = (cl > 0 && tmp[0] != '*' && tmp[0] != '#'
                         && strstr(tmp, "0:") != NULL
                         && (strstr(tmp, ".skprx") || strstr(tmp, ".suprx"))
                         && !keep_plugin(tmp));
        if (is_plugin) { out[out_pos++] = '#'; disabled++; }
        memcpy(out + out_pos, p, ll); out_pos += ll;
        if (nl) out[out_pos++] = '\n';
        p = nl ? nl + 1 : end;
    }
    /* Back up first; no backup = no change. v1.1.8: only when something
     * will change, so running Safe Mode twice never replaces the real
     * pre-Safe-Mode config with the already-disabled one. */
    char bak[160] = "(none)";
    if (disabled > 0) {
        backup_name(config_path, "before_safemode", bak, sizeof(bak));
        restore_ensure_dirs();
        if (write_file(bak, buf, len) < 0) {
            free(buf); free(out);
            log_write("Safe Mode: backup failed, config left unchanged");
            return RESTORE_ERR_IO;
        }
    }
    free(buf);

    int r = (disabled > 0) ? write_file(config_path, out, out_pos) : 0;
    free(out);
    if (r < 0) return RESTORE_ERR_IO;
    char msg[192];
    snprintf(msg, sizeof(msg),
             "Safe Mode: %d plugin(s) disabled in %s (backup: %s)",
             disabled, config_path, bak);
    log_write(msg);
    return RESTORE_OK;
}

RestoreStatus restore_safe_mode(void) {
    log_open();
    set_status(RESTORE_RUNNING, "Safe Mode: patching all tai configs...");

    static const char *CONFIGS[] = {
        "imc0:tai/config.txt",   /* PCH-2000 / PSTV only — highest priority */
        "ux0:tai/config.txt",    /* all models — overrides ur0: when present */
        "ur0:tai/config.txt",    /* canonical HENkaku fallback               */
    };
    int patched = 0; RestoreStatus final_r = RESTORE_OK;
    for (int i = 0; i < 3; i++) {
        SceIoStat st;
        if (sceIoGetstat(CONFIGS[i], &st) < 0) continue;
        char msg[128];
        snprintf(msg, sizeof(msg), "Patching %s ...", CONFIGS[i]);
        set_status(RESTORE_RUNNING, msg);
        RestoreStatus r = safe_mode_one_config(CONFIGS[i]);
        if (r == RESTORE_OK) patched++;
        else final_r = r;
    }
    if (patched == 0) {
        set_status(RESTORE_ERR_IO,
                   "ERROR: No tai/config.txt found (imc0:, ux0:, or ur0:).");
        log_close(); return RESTORE_ERR_IO;
    }
    char msg[192];
    snprintf(msg, sizeof(msg),
             "Safe Mode OK - %d config(s) patched. Reboot to apply.", patched);
    set_status(final_r, msg);
    log_close(); return final_r;
}

/* ── FUNCTION 2: Reset taiHEN config to safe defaults ────────────────────── */
/*
 * Write the safe minimal config to the ACTIVE tai config path.
 * If imc0:tai/config.txt is active (PCH-2000/PSTV), we write there.
 * We also ensure ur0:tai/config.txt always exists as a fallback.
 *
 * On 3.67/3.68 (h-encore, no Enso): ur0:tai/ may not exist yet.
 * We create it, because once the user re-runs h-encore and taiHEN
 * loads, ur0:tai/config.txt must already be present.
 */
/* Build the safe config, carrying over the essential plugin lines that were
 * enabled in the old config (storagemgr/gamesd for SD2Vita, the boot
 * recovery plugins). Returns malloc'd text; caller frees. */
static char *build_safe_config(const char *old_cfg, int old_len, int *out_len) {
    char kern[1024] = "", mainl[512] = "";
    if (old_cfg) {
        char section[64] = "";
        const char *p = old_cfg, *end = old_cfg + old_len;
        while (p < end) {
            const char *nl = memchr(p, '\n', end - p);
            int ll = nl ? (int)(nl - p) : (int)(end - p);
            char line[300];
            int cl = ll < (int)sizeof(line) - 1 ? ll : (int)sizeof(line) - 1;
            memcpy(line, p, cl); line[cl] = '\0';
            while (cl > 0 && (line[cl-1] == '\r' || line[cl-1] == ' ')) line[--cl] = '\0';
            char *t = line; while (*t == ' ' || *t == '\t') t++;
            if (*t == '*') snprintf(section, sizeof(section), "%s", t + 1);
            else if (*t && *t != '#' && keep_plugin(t) &&
                     strcasecmp(strrchr(t, '/') ? strrchr(t, '/') + 1 : t, "henkaku.suprx") != 0 &&
                     strcasecmp(strrchr(t, '/') ? strrchr(t, '/') + 1 : t, "henkaku.skprx") != 0) {
                char *dst = (strcmp(section, "KERNEL") == 0) ? kern
                          : (strcmp(section, "main") == 0)   ? mainl : NULL;
                int cap = (dst == kern) ? (int)sizeof(kern) : (int)sizeof(mainl);
                if (dst && (int)(strlen(dst) + strlen(t) + 2) < cap) {
                    strcat(dst, t); strcat(dst, "\n");
                }
            }
            p = nl ? nl + 1 : end;
        }
    }
    /* Split SAFE_TAI_CONFIG at the two insertion points */
    const char *k_at = strstr(SAFE_TAI_CONFIG, "# ur0:tai/storagemgr.skprx\n");
    const char *m_at = strstr(SAFE_TAI_CONFIG, "# Add LiveArea user plugins (.suprx) here:\n");
    int total = (int)sizeof(SAFE_TAI_CONFIG) + (int)strlen(kern) + (int)strlen(mainl) + 8;
    char *out = malloc(total);
    if (!out) return NULL;
    int a_len = (int)(k_at - SAFE_TAI_CONFIG) + (int)strlen("# ur0:tai/storagemgr.skprx\n");
    int b_len = (int)(m_at - (SAFE_TAI_CONFIG + a_len));
    int pos = 0;
    memcpy(out + pos, SAFE_TAI_CONFIG, a_len); pos += a_len;
    memcpy(out + pos, kern, strlen(kern)); pos += strlen(kern);
    memcpy(out + pos, SAFE_TAI_CONFIG + a_len, b_len); pos += b_len;
    memcpy(out + pos, mainl, strlen(mainl)); pos += strlen(mainl);
    strcpy(out + pos, m_at); pos += strlen(m_at);
    *out_len = pos;
    return out;
}

RestoreStatus restore_reset_tai_config(void) {
    log_open();
    restore_ensure_dirs();

    /* Determine active config (where taiHEN actually loads from) */
    const char *active = TAI_CONFIG_UR0; /* safe default */
    SceIoStat st;
    if (sceIoGetstat("imc0:tai/config.txt", &st) >= 0)
        active = TAI_CONFIG_IMC0;
    else if (sceIoGetstat(TAI_CONFIG_UX0, &st) >= 0)
        active = TAI_CONFIG_UX0;

    set_status(RESTORE_RUNNING, "Backing up current config...");
    char *old = NULL; int old_len = 0;
    int have_old = (read_file(active, &old, &old_len) == 0);
    if (have_old) {
        char bak[160];
        backup_name(active, "before_reset", bak, sizeof(bak));
        if (write_file(bak, old, old_len) < 0 ||
            write_file(TAI_BACKUP_DIR "/config.txt.bak", old, old_len) < 0) {
            free(old);
            set_status(RESTORE_ERR_IO, "ERROR: Backup failed. Config NOT changed.");
            log_close(); return g_last_status;
        }
    }

    int new_len = 0;
    char *safe = build_safe_config(have_old ? old : NULL, old_len, &new_len);
    free(old);
    if (!safe) {
        set_status(RESTORE_ERR_IO, "ERROR: Out of memory. Config NOT changed.");
        log_close(); return g_last_status;
    }

    set_status(RESTORE_RUNNING, "Writing safe default config to active path...");
    /* Always ensure ur0:tai/ directory exists (needed on 3.67/3.68) */
    sceIoMkdir("ur0:tai", 0777);
    if (write_file(active, safe, new_len) < 0) {
        free(safe);
        set_status(RESTORE_ERR_IO, "ERROR: Cannot write config.txt (unchanged)");
        log_close(); return g_last_status;
    }
    /* Also ensure ur0:tai/config.txt exists as a fallback (important on
     * h-encore systems). v1.1.7: back it up first and only create it if
     * missing - an existing ur0 config is not overwritten silently. */
    if (strcmp(active, TAI_CONFIG_UR0) != 0 && !file_exists(TAI_CONFIG_UR0))
        write_file(TAI_CONFIG_UR0, safe, new_len);
    free(safe);

    char msg[192];
    snprintf(msg, sizeof(msg),
             "Config reset OK (%s). Backup in tai_backup/.", active);
    set_status(RESTORE_OK, msg);
    log_close(); return RESTORE_OK;
}

/* ── FUNCTION 3a: Backup all tai/ locations ──────────────────────────────── */
/*
 * Backs up ALL three tai config locations into separate subdirectories.
 * This preserves data from whichever location is active on any model:
 *   tai_backup/imc0_tai/  — PCH-2000/PSTV
 *   tai_backup/ux0_tai/   — all models
 *   tai_backup/ur0_tai/   — all models (canonical)
 */
RestoreStatus restore_backup_tai(void) {
    log_open();
    restore_ensure_dirs();

    typedef struct { const char *src; const char *dst_sub; } TaiLoc;
    static const TaiLoc LOCS[] = {
        { "imc0:tai", TAI_BACKUP_DIR "/imc0_tai" },
        { "ux0:tai",  TAI_BACKUP_DIR "/ux0_tai"  },
        { "ur0:tai",  TAI_BACKUP_DIR "/ur0_tai"  },
    };

    int total = 0, sources = 0;
    for (int i = 0; i < 3; i++) {
        SceIoStat st;
        if (sceIoGetstat(LOCS[i].src, &st) < 0) continue;
        char msg[128];
        snprintf(msg, sizeof(msg), "Backing up %s ...", LOCS[i].src);
        set_status(RESTORE_RUNNING, msg);
        int n = copy_dir(LOCS[i].src, LOCS[i].dst_sub);
        if (n >= 0) { total += n; sources++; }
    }

    if (sources == 0) {
        set_status(RESTORE_ERR_IO,
                   "ERROR: No tai/ directory found to back up.");
        log_close(); return RESTORE_ERR_IO;
    }
    char msg[192];
    snprintf(msg, sizeof(msg),
             "Backup OK - %d file(s) from %d location(s) -> tai_backup/",
             total, sources);
    set_status(RESTORE_OK, msg);
    log_close(); return RESTORE_OK;
}

/* ── FUNCTION 3b: Restore tai/ from backup ──────────────────────────────── */
/*
 * Restores each backed-up location to its original path.
 * Backup subdirs: imc0_tai/ → imc0:tai/
 *                 ux0_tai/  → ux0:tai/
 *                 ur0_tai/  → ur0:tai/
 * Only restores locations that were backed up (subdir exists).
 */
RestoreStatus restore_restore_tai(void) {
    log_open();
    SceIoStat st;
    if (sceIoGetstat(TAI_BACKUP_DIR, &st) < 0) {
        set_status(RESTORE_ERR_NO_BACKUP,
                   "ERROR: No backup found. Run 'Backup tai/' first.");
        log_close(); return g_last_status;
    }

    typedef struct { const char *src_sub; const char *dst; } RestorePair;
    static const RestorePair PAIRS[] = {
        { TAI_BACKUP_DIR "/imc0_tai", "imc0:tai" },
        { TAI_BACKUP_DIR "/ux0_tai",  "ux0:tai"  },
        { TAI_BACKUP_DIR "/ur0_tai",  "ur0:tai"  },
    };

    int total = 0, restored = 0;
    for (int i = 0; i < 3; i++) {
        SceIoStat ss;
        if (sceIoGetstat(PAIRS[i].src_sub, &ss) < 0) continue;
        char msg[192];
        snprintf(msg, sizeof(msg), "Restoring %s ...", PAIRS[i].dst);
        set_status(RESTORE_RUNNING, msg);
        /* Ensure destination directory exists (important for ur0:tai on 3.67/3.68) */
        sceIoMkdir(PAIRS[i].dst, 0777);
        int n = copy_dir(PAIRS[i].src_sub, PAIRS[i].dst);
        if (n >= 0) { total += n; restored++; }
    }

    if (restored == 0) {
        set_status(RESTORE_ERR_IO, "ERROR: Restore failed - no backup subdirs found.");
        log_close(); return RESTORE_ERR_IO;
    }
    char msg[192];
    snprintf(msg, sizeof(msg),
             "Restore OK - %d file(s) to %d location(s). Reboot to apply.",
             total, restored);
    set_status(RESTORE_OK, msg);
    log_close(); return RESTORE_OK;
}

/* ── FUNCTION 4: Rebuild LiveArea database ───────────────────────────────── */
/* v1.1.7 (review finding 6): this app cannot see ur0:shell, so the old
 * direct delete never worked (and "not found" was reported as success).
 * The Restore screen now runs the same flow as Sony Recovery > Rebuild
 * Database (boot plugin). This stub only remains for API compatibility. */
RestoreStatus restore_rebuild_livearea(void) {
    set_status(RESTORE_ERR_KERNEL,
               "Use Sony Recovery > Rebuild Database (boot plugin).");
    return g_last_status;
}

/* ── Utility ──────────────────────────────────────────────────────────────── */
int restore_backup_exists(void) {
    /* Check for any of the three backup subdirectories */
    SceIoStat st;
    return (sceIoGetstat(TAI_BACKUP_DIR "/ur0_tai",  &st) >= 0 ||
            sceIoGetstat(TAI_BACKUP_DIR "/ux0_tai",  &st) >= 0 ||
            sceIoGetstat(TAI_BACKUP_DIR "/imc0_tai", &st) >= 0 ||
            /* legacy single-dir backup from older versions */
            sceIoGetstat(TAI_BACKUP_DIR "/config.txt", &st) >= 0);
}
