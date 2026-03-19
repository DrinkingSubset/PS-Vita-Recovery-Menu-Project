/*
 * restore.c — Vita soft-brick recovery engine
 *
 * Covers 4 recovery scenarios:
 *   1. Safe Mode Boot      — Comment out ALL plugins so the system can boot clean
 *   2. Reset taiHEN config — Overwrite config.txt with a known-good minimal config
 *   3. Backup / Restore    — Copy ux0:tai/ to/from ux0:data/vita_recovery/tai_backup/
 *   4. Rebuild LiveArea DB — Delete the corrupt app.db so firmware regenerates it
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
#define APP_DB_PATH      "ur0:shell/db/app.db"
#define APP_DB_BACKUP    "ux0:data/vita_recovery/app.db.bak"
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
    if (size <= 0 || size > 512 * 1024) { sceIoClose(fd); return -2; }
    *out = malloc(size + 1);
    if (!*out) { sceIoClose(fd); return -2; }
    sceIoRead(fd, *out, size);
    sceIoClose(fd);
    (*out)[size] = '\0';
    if (out_len) *out_len = size;
    return 0;
}

static int write_file(const char *path, const char *data, int len) {
    SceUID fd = sceIoOpen(path,
                          SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
    if (fd < 0) return -1;
    sceIoWrite(fd, data, len);
    sceIoClose(fd);
    return 0;
}

static int copy_file(const char *src, const char *dst) {
    char *buf = NULL; int len = 0;
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
    char *line = strtok(buf, "\n");
    while (line) {
        int ll = strlen(line);
        if (ll > 0 && line[ll-1] == '\r') line[--ll] = '\0';
        /* A plugin line: not a comment, not a section header, has partition
         * prefix, is a .skprx or .suprx. Never disable henkaku.skprx note. */
        int is_plugin = (line[0] != '*' && line[0] != '#' && ll > 0
                         && strstr(line, "0:") != NULL
                         && (strstr(line, ".skprx") || strstr(line, ".suprx"))
                         && !strstr(line, "henkaku.skprx"));
        if (is_plugin) { out[out_pos++] = '#'; disabled++; }
        memcpy(out + out_pos, line, ll); out_pos += ll;
        out[out_pos++] = '\n';
        line = strtok(NULL, "\n");
    }
    out[out_pos] = '\0';
    free(buf);
    int r = write_file(config_path, out, out_pos);
    free(out);
    if (r < 0) return RESTORE_ERR_IO;
    char msg[192];
    snprintf(msg, sizeof(msg),
             "Safe Mode: %d plugin(s) disabled in %s", disabled, config_path);
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
             "Safe Mode OK — %d config(s) patched. Reboot to apply.", patched);
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
    copy_file(active, TAI_BACKUP_DIR "/config.txt.bak");

    set_status(RESTORE_RUNNING, "Writing safe default config to active path...");
    /* Always ensure ur0:tai/ directory exists (needed on 3.67/3.68) */
    sceIoMkdir("ur0:tai", 0777);
    /* Write safe config to the active location */
    if (write_file(active, SAFE_TAI_CONFIG, sizeof(SAFE_TAI_CONFIG) - 1) < 0) {
        set_status(RESTORE_ERR_IO, "ERROR: Cannot write config.txt");
        log_close(); return g_last_status;
    }
    /* Also ensure ur0:tai/config.txt exists as a fallback (important on
     * h-encore systems where ur0: may be the only persistent location) */
    if (active != TAI_CONFIG_UR0) {
        write_file(TAI_CONFIG_UR0, SAFE_TAI_CONFIG, sizeof(SAFE_TAI_CONFIG) - 1);
    }
    char msg[192];
    snprintf(msg, sizeof(msg),
             "Config reset OK. Wrote to %s. Backup -> tai_backup/config.txt.bak",
             active);
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
             "Backup OK — %d file(s) from %d location(s) -> tai_backup/",
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
        set_status(RESTORE_ERR_IO, "ERROR: Restore failed — no backup subdirs found.");
        log_close(); return RESTORE_ERR_IO;
    }
    char msg[192];
    snprintf(msg, sizeof(msg),
             "Restore OK — %d file(s) to %d location(s). Reboot to apply.",
             total, restored);
    set_status(RESTORE_OK, msg);
    log_close(); return RESTORE_OK;
}

/* ── FUNCTION 4: Rebuild LiveArea database ───────────────────────────────── */
RestoreStatus restore_rebuild_livearea(void) {
    log_open();
    restore_ensure_dirs();
    set_status(RESTORE_RUNNING, "Checking for app.db...");

    SceIoStat st;
    if (sceIoGetstat(APP_DB_PATH, &st) >= 0) {
        set_status(RESTORE_RUNNING, "Backing up app.db...");
        copy_file(APP_DB_PATH, APP_DB_BACKUP);

        if (sceIoRemove(APP_DB_PATH) < 0) {
            set_status(RESTORE_ERR_IO,
                       "ERROR: Cannot remove app.db (needs kernel plugin)");
            log_close(); return g_last_status;
        }
        set_status(RESTORE_OK,
                   "app.db removed. Reboot - firmware will rebuild it.");
    } else {
        set_status(RESTORE_OK,
                   "app.db not found (already gone). Reboot to regenerate.");
    }
    log_close(); return RESTORE_OK;
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
