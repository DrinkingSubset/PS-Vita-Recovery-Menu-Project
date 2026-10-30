/*
 * layout.c - Home Screen Layout backup / restore (v1.2.0).
 *
 * Backup:  ur0:shell/db/app.db is copied as a plain file to
 *          ux0:data/vita_recovery/layout/app.db (the one before is kept as
 *          app.db.prev). The copy is checked with SQLite before it replaces
 *          the old backup. The live database is never opened with SQLite.
 *
 * Restore: the live app.db is copied to a work file, the backup's pages,
 *          folders and bubble order are applied to that copy
 *          (layout_core.c), and the result is staged as
 *          ur0:shell/db/app.db.new. The Vita restarts right away; the boot
 *          plugin (boot_recovery.skprx) swaps the files early in the next
 *          boot, before the home screen opens the database, and keeps the
 *          replaced one as app.db.prelayout.
 *
 * Max text width on screen: 55 characters.
 */

#include <psp2/ctrl.h>
#include <psp2/power.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/threadmgr.h>

#include <stdio.h>
#include <string.h>

#include "layout.h"
#include "layout_core.h"
#include "vita_sqlite.h"
#include "official_recovery.h"
#include "compat.h"
#include "draw.h"

#define LIVE_DB     "ur0:shell/db/app.db"
#define STAGE_DB    "ur0:shell/db/app.db.new"
#define DATA_DIR    "ux0:data/vita_recovery"
#define LAYOUT_DIR  DATA_DIR "/layout"
#define LATEST_DB   LAYOUT_DIR "/app.db"
#define PREV_DB     LAYOUT_DIR "/app.db.prev"
#define TMP_DB      LAYOUT_DIR "/app.db.tmp"
#define WORK_DB     LAYOUT_DIR "/work.db"
#define FLAG_LAYOUT "ur0:tai/layout_restore_trigger"
#define FLAG_REBUILD "ur0:tai/rebuild_db_trigger"

#define COPY_TRIES  3

static char g_l1[64], g_l2[64], g_l3[64], g_l4[64];

/* ── file helpers ──────────────────────────────────────────────────────── */

static int exists(const char *p) {
    SceIoStat st;
    return sceIoGetstat(p, &st) >= 0;
}

/* Plain byte copy. Returns 0 or a negative sceIo error. */
static int copy_file(const char *src, const char *dst) {
    static char buf[64 * 1024];
    SceUID in = sceIoOpen(src, SCE_O_RDONLY, 0);
    if (in < 0) return in;
    SceUID out = sceIoOpen(dst, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
    if (out < 0) { sceIoClose(in); return out; }
    int ret = 0;
    while (1) {
        int r = sceIoRead(in, buf, sizeof(buf));
        if (r < 0) { ret = r; break; }
        if (r == 0) break;
        int w = sceIoWrite(out, buf, r);
        if (w != r) { ret = (w < 0) ? w : -1; break; }
    }
    sceIoClose(in);
    if (ret == 0) sceIoSyncByFd(out, 0);
    sceIoClose(out);
    if (ret < 0) sceIoRemove(dst);
    return ret;
}

/* 1 if both files exist and have identical contents */
static int same_file(const char *a, const char *b) {
    static char ba[16 * 1024], bb[16 * 1024];
    SceUID fa = sceIoOpen(a, SCE_O_RDONLY, 0);
    if (fa < 0) return 0;
    SceUID fb = sceIoOpen(b, SCE_O_RDONLY, 0);
    if (fb < 0) { sceIoClose(fa); return 0; }
    int same = 1;
    while (1) {
        int ra = sceIoRead(fa, ba, sizeof(ba));
        int rb = sceIoRead(fb, bb, sizeof(bb));
        if (ra != rb || ra < 0) { same = 0; break; }
        if (ra == 0) break;
        if (memcmp(ba, bb, ra) != 0) { same = 0; break; }
    }
    sceIoClose(fa);
    sceIoClose(fb);
    return same;
}

/* Copy the live app.db to dst and check the copy. The home screen may be
 * writing to app.db at that moment, so a bad copy is retried. */
static int copy_live_checked(const char *dst, LayoutSummary *sum,
                             char *err, int errlen) {
    int r = 0;
    for (int i = 0; i < COPY_TRIES; i++) {
        r = copy_file(LIVE_DB, dst);
        if (r < 0) {
            snprintf(err, errlen, "Cannot read app.db (0x%08X)", (unsigned)r);
            continue;
        }
        r = layout_summary(dst, sum, err, errlen);
        if (r == 0) return 0;
    }
    sceIoRemove(dst);
    return r < 0 ? r : -1;
}

static void make_dirs(void) {
    sceIoMkdir("ux0:data", 0777);
    sceIoMkdir(DATA_DIR, 0777);
    sceIoMkdir(LAYOUT_DIR, 0777);
}

/* "2026-10-29 11:57" from a file's time, or "" */
static void file_date(const char *path, char *out, int outlen) {
    SceIoStat st;
    out[0] = '\0';
    if (sceIoGetstat(path, &st) < 0) return;
    snprintf(out, outlen, "%04u-%02u-%02u %02u:%02u",
             st.st_mtime.year, st.st_mtime.month, st.st_mtime.day,
             st.st_mtime.hour, st.st_mtime.minute);
}

void layout_cancel_pending(void) {
    sceIoRemove(FLAG_LAYOUT);
    sceIoRemove(STAGE_DB);
}

/* ── backup ────────────────────────────────────────────────────────────── */

static int do_backup(LayoutSummary *sum, char *err, int errlen) {
    if (vita_sqlite_init() != 0) {
        snprintf(err, errlen, "System SQLite module did not load");
        return -1;
    }
    make_dirs();
    sceIoRemove(TMP_DB);
    int r = copy_live_checked(TMP_DB, sum, err, errlen);
    if (r < 0) return r;

    if (same_file(TMP_DB, LATEST_DB)) {   /* nothing new: keep .prev as is */
        sceIoRemove(TMP_DB);
        return 2;
    }
    if (exists(LATEST_DB)) {
        sceIoRemove(PREV_DB);
        r = sceIoRename(LATEST_DB, PREV_DB);
        if (r < 0) {
            snprintf(err, errlen, "Cannot keep old backup (0x%08X)", (unsigned)r);
            sceIoRemove(TMP_DB);
            return r;
        }
    }
    r = sceIoRename(TMP_DB, LATEST_DB);
    if (r < 0) {
        snprintf(err, errlen, "Cannot save backup (0x%08X)", (unsigned)r);
        sceIoRename(PREV_DB, LATEST_DB);   /* put the old one back */
        sceIoRemove(TMP_DB);
        return r;
    }
    return 1;
}

int layout_backup_quiet(void) {
    LayoutSummary s;
    char err[96];
    return do_backup(&s, err, sizeof(err));
}

/* ── screen helpers ────────────────────────────────────────────────────── */

static int pressed(SceCtrlData *now, SceCtrlData *old, unsigned int b) {
    return (now->buttons & b) && !(old->buttons & b);
}

static void wait_release(void) {
    SceCtrlData d;
    do { sceCtrlPeekBufferPositive(0, &d, 1); } while (d.buttons);
}

static void titlebar(const char *sub) {
    display_rect(0, 0, SCREEN_W, TITLE_LINE, COLOR_TITLE_BG);
    display_app_title(6);
    draw_text(MENU_X, 26, COLOR_DIM, sub);
    display_hline(0, TITLE_LINE, SCREEN_W, COLOR_GREEN);
}

static void footer(const char *hint) {
    display_rect(0, FOOTER_Y, SCREEN_W, SCREEN_H - FOOTER_Y, COLOR_TITLE_BG);
    display_hline(0, FOOTER_LINE, SCREEN_W, COLOR_GREEN);
    draw_text(MENU_X, FOOTER_Y + 7, COLOR_DIM, hint);
}

/* Restart and never come back to the caller */
static void restart_now(void) {
    scePowerRequestColdReset();
    while (1) sceKernelDelayThread(100 * 1000);
}

static void busy(const char *what) {
    display_start();
    display_clear(COLOR_BG);
    titlebar("Home Screen Layout");
    draw_text(MENU_X, TITLE_H + 8, COLOR_YELLOW, what);
    draw_text(MENU_X, TITLE_H + 8 + LINE_H, COLOR_DIM, "Please wait. Do not power off.");
    footer("");
    display_end();
}

/* Lines on a box; returns 1 for X, 0 for O */
static int ask(const char *title, const char *head, uint32_t box,
               const char *const *lines, int n, const char *yes, const char *no) {
    SceCtrlData old, now;
    sceCtrlPeekBufferPositive(0, &old, 1);   /* held buttons are not presses */
    while (1) {
        display_start();
        display_clear(COLOR_BG);
        titlebar(title);
        int y = TITLE_H + 8;
        display_rect(MENU_X - 8, y, SCREEN_W - (MENU_X - 8) * 2,
                     LINE_H * (n + 1) + 8, box);
        draw_text(MENU_X, y + 4, COLOR_YELLOW, head);
        for (int i = 0; i < n; i++)
            draw_text(MENU_X, y + 4 + LINE_H * (i + 1), COLOR_TEXT, lines[i]);
        int by = y + LINE_H * (n + 1) + 20;
        draw_text(MENU_X, by, COLOR_GREEN, yes);
        if (no) draw_text(MENU_X, by + LINE_H, COLOR_RED, no);
        footer(no ? "[X] Yes    [O] No" : "[O] Back");
        display_end();

        sceCtrlPeekBufferPositive(0, &now, 1);
        if (no && pressed(&now, &old, SCE_CTRL_CROSS)) { wait_release(); return 1; }
        if (pressed(&now, &old, SCE_CTRL_CIRCLE))      { wait_release(); return 0; }
        old = now;
    }
}

/* Result screen with up to four lines */
static void result(int ok) {
    const char *L[4] = { g_l1, g_l2, g_l3, g_l4 };
    int n = 0;
    while (n < 4 && L[n][0]) n++;
    ask("Home Screen Layout", ok ? "DONE" : "ERROR",
        ok ? RGBA8(0, 50, 0, 255) : RGBA8(60, 0, 0, 255),
        L, n, "[O] Back", NULL);
    g_l1[0] = g_l2[0] = g_l3[0] = g_l4[0] = '\0';
}

/* ── actions ───────────────────────────────────────────────────────────── */

static void action_backup(void) {
    busy("Backing up home screen layout...");
    LayoutSummary s;
    char err[96] = "";
    int r = do_backup(&s, err, sizeof(err));
    if (r == 1 || r == 2) {
        snprintf(g_l1, sizeof(g_l1), r == 1 ? "Layout backed up."
                                            : "Layout unchanged since last backup.");
        snprintf(g_l2, sizeof(g_l2), "%d page(s), %d folder(s), %d app(s).",
                 s.home_pages, s.folders, s.apps);
        snprintf(g_l3, sizeof(g_l3), "ux0:data/vita_recovery/layout/");
        result(1);
    } else {
        snprintf(g_l1, sizeof(g_l1), "Backup failed. Old backups unchanged.");
        snprintf(g_l2, sizeof(g_l2), "%.54s", err);
        result(0);
    }
}

static void action_restore(int previous) {
    const char *src = previous ? PREV_DB : LATEST_DB;
    char err[96] = "", date[24];
    LayoutSummary bs;

    if (!exists(src)) {
        snprintf(g_l1, sizeof(g_l1), previous ? "No previous backup yet."
                                              : "No layout backup yet.");
        snprintf(g_l2, sizeof(g_l2), "Use 'Back Up Layout' first.");
        result(0);
        return;
    }
    if (exists(FLAG_REBUILD)) {
        snprintf(g_l1, sizeof(g_l1), "A Rebuild Database is scheduled.");
        snprintf(g_l2, sizeof(g_l2), "Restart the Vita first so it runs, then");
        snprintf(g_l3, sizeof(g_l3), "come back here to restore the layout.");
        result(0);
        return;
    }
    int bp = official_recovery_boot_plugin_state();
    if (bp != 2) {
        snprintf(g_l1, sizeof(g_l1), bp == 0
                 ? "Boot plugin (boot_recovery.skprx) not installed."
                 : "Boot plugin is an older version.");
        snprintf(g_l2, sizeof(g_l2), "Advanced > Boot Recovery > Install / Update,");
        snprintf(g_l3, sizeof(g_l3), "restart, then come back. Nothing changed.");
        result(0);
        return;
    }
    if (vita_sqlite_init() != 0) {
        snprintf(g_l1, sizeof(g_l1), "System SQLite module did not load.");
        result(0);
        return;
    }
    if (layout_summary(src, &bs, err, sizeof(err)) != 0) {
        snprintf(g_l1, sizeof(g_l1), "The backup file is not usable:");
        snprintf(g_l2, sizeof(g_l2), "%.54s", err);
        result(0);
        return;
    }

    file_date(src, date, sizeof(date));
    char b1[64], b2[64];
    snprintf(b1, sizeof(b1), "Backup: %s", date);
    snprintf(b2, sizeof(b2), "  %d page(s), %d folder(s), %d app(s)",
             bs.home_pages, bs.folders, bs.apps);
    int late = (g_compat.fw_tier >= FW_367_HENCORE && g_compat.fw_tier != FW_UNKNOWN);
    const char *const W[] = {
        b1, b2, "",
        "Pages, folders and bubble order are put back.",
        "Apps no longer installed are skipped.",
        "Apps installed later go after the last bubble.",
        "The Vita restarts to apply it (needs Enso).",
        late ? "NOTE: this firmware may not have Enso." : "",
    };
    if (!ask("Home Screen Layout - Restore", "RESTORE LAYOUT?",
             RGBA8(40, 40, 0, 255), W, late ? 8 : 7,
             "[X] Continue", "[O] Cancel"))
        return;

    busy("Preparing the restored layout...");
    make_dirs();
    sceIoRemove(WORK_DB);
    LayoutSummary cur;
    if (copy_live_checked(WORK_DB, &cur, err, sizeof(err)) < 0) {
        snprintf(g_l1, sizeof(g_l1), "Could not copy the current app.db.");
        snprintf(g_l2, sizeof(g_l2), "%.54s", err);
        snprintf(g_l3, sizeof(g_l3), "Nothing changed.");
        result(0);
        return;
    }
    LayoutReport rep;
    int r = layout_merge(src, WORK_DB, &rep, err, sizeof(err));
    if (r != 0) {
        sceIoRemove(WORK_DB);
        snprintf(g_l1, sizeof(g_l1), "Could not rebuild the layout (%d).", r);
        snprintf(g_l2, sizeof(g_l2), "%.54s", err);
        snprintf(g_l3, sizeof(g_l3), "Nothing changed.");
        result(0);
        return;
    }

    char r1[64], r2[64], r3[64];
    snprintf(r1, sizeof(r1), "%d page(s), %d folder(s), %d app(s) in place.",
             rep.pages, rep.folders, rep.placed);
    snprintf(r2, sizeof(r2), "Not installed any more (skipped): %d", rep.missing);
    snprintf(r3, sizeof(r3), "New apps added at the end: %d", rep.added);
    const char *const R[] = {
        r1, r2, r3, "",
        "Restart now to apply it.",
        "Old database is kept as app.db.prelayout.",
    };
    if (!ask("Home Screen Layout - Restore", "READY", RGBA8(0, 50, 0, 255),
             R, 6, "[X] Restart now and apply", "[O] Cancel (nothing changes)")) {
        sceIoRemove(WORK_DB);
        return;
    }

    /* Stage next to app.db, check it, then ask the boot plugin to swap */
    busy("Saving...");
    sceIoRemove(STAGE_DB);
    r = copy_file(WORK_DB, STAGE_DB);
    if (r < 0 || !same_file(WORK_DB, STAGE_DB)) {
        sceIoRemove(STAGE_DB);
        sceIoRemove(WORK_DB);
        snprintf(g_l1, sizeof(g_l1), "Could not write app.db.new (0x%08X).",
                 (unsigned)r);
        snprintf(g_l2, sizeof(g_l2), "Nothing changed.");
        result(0);
        return;
    }
    sceIoRemove(WORK_DB);
    sceIoMkdir("ur0:tai", 0777);
    SceUID fd = sceIoOpen(FLAG_LAYOUT, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
    if (fd < 0) {
        sceIoRemove(STAGE_DB);
        snprintf(g_l1, sizeof(g_l1), "Could not write the request (0x%08X).",
                 (unsigned)fd);
        snprintf(g_l2, sizeof(g_l2), "Nothing changed.");
        result(0);
        return;
    }
    sceIoWrite(fd, "1", 1);
    sceIoClose(fd);

    busy("Restarting to apply the layout...");
    restart_now();
}

/* A staged restore from before that never got its restart */
static void check_pending(void) {
    if (!exists(FLAG_LAYOUT)) {
        sceIoRemove(STAGE_DB);   /* leftover without a request */
        return;
    }
    if (!exists(STAGE_DB)) { sceIoRemove(FLAG_LAYOUT); return; }
    static const char *const L[] = {
        "A layout restore is waiting for a restart.",
        "Apps installed since then would be missing",
        "from it, so restart now or cancel it.",
    };
    if (ask("Home Screen Layout", "RESTORE PENDING", RGBA8(40, 40, 0, 255),
            L, 3, "[X] Restart now and apply", "[O] Cancel the restore")) {
        busy("Restarting to apply the layout...");
        restart_now();
    }
    layout_cancel_pending();
}

/* ── screen ────────────────────────────────────────────────────────────── */

typedef struct { const char *label, *desc; } Opt;
static const Opt OPTS[] = {
    { "1. Back Up Layout",        "Save pages, folders and bubble order." },
    { "2. Restore Latest Backup", "Put pages and folders back. Restarts." },
    { "3. Restore Older Backup",  "Same, from the backup before the latest." },
};
#define N_OPTS 3

static void backup_line(const char *path, const char *name, char *out, int len) {
    char date[24];
    LayoutSummary s;
    char err[96];
    if (!exists(path)) { snprintf(out, len, "%s: none", name); return; }
    file_date(path, date, sizeof(date));
    if (vita_sqlite_init() == 0 && layout_summary(path, &s, err, sizeof(err)) == 0)
        snprintf(out, len, "%s: %s  %dp %df %da", name, date,
                 s.home_pages, s.folders, s.apps);
    else
        snprintf(out, len, "%s: %s  (unreadable)", name, date);
}

void layout_screen_run(void *font) {
    (void)font;
    int sel = 0;
    char latest[64], older[64];
    SceCtrlData old, now;

    wait_release();
    check_pending();
    backup_line(LATEST_DB, "Latest", latest, sizeof(latest));
    backup_line(PREV_DB,   "Older ", older,  sizeof(older));
    sceCtrlPeekBufferPositive(0, &old, 1);

    while (1) {
        display_start();
        display_clear(COLOR_BG);
        titlebar("Home Screen Layout");
        int y = TITLE_H + 8;
        draw_text(MENU_X, y,          COLOR_TEXT, latest);
        draw_text(MENU_X, y + LINE_H, COLOR_DIM,  older);
        draw_text(MENU_X, y + LINE_H * 2, COLOR_DIM,
                  "(p = pages, f = folders, a = apps)");
        display_hline(0, y + LINE_H * 3 + 4, SCREEN_W, RGBA8(60, 60, 60, 255));

        int oy = y + LINE_H * 3 + 12;
        for (int i = 0; i < N_OPTS; i++) {
            int ry = oy + i * (LINE_H * 2 + 4);
            if (i == sel) {
                display_rect(0, ry - 1, SCREEN_W - 8, LINE_H * 2 + 5, COLOR_SEL_BG);
                display_text_transp(MENU_X, ry, COLOR_SELECTED, OPTS[i].label);
                display_text_transp(MENU_X, ry + LINE_H, COLOR_SELECTED, OPTS[i].desc);
            } else {
                draw_text(MENU_X, ry, COLOR_TEXT, OPTS[i].label);
                draw_text(MENU_X, ry + LINE_H, COLOR_DIM, OPTS[i].desc);
            }
        }
        footer("[X] Select    [O] Back");
        display_end();

        sceCtrlPeekBufferPositive(0, &now, 1);
        if (pressed(&now, &old, SCE_CTRL_DOWN)) sel = (sel + 1) % N_OPTS;
        if (pressed(&now, &old, SCE_CTRL_UP))   sel = (sel + N_OPTS - 1) % N_OPTS;
        if (pressed(&now, &old, SCE_CTRL_CIRCLE)) { wait_release(); return; }
        if (pressed(&now, &old, SCE_CTRL_CROSS)) {
            wait_release();
            if (sel == 0) action_backup();
            else          action_restore(sel == 2);
            backup_line(LATEST_DB, "Latest", latest, sizeof(latest));
            backup_line(PREV_DB,   "Older ", older,  sizeof(older));
            sceCtrlPeekBufferPositive(0, &now, 1);
        }
        old = now;
    }
}
