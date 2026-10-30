/*
 * layout_core.c - home screen layout logic on app.db copies (v1.2.0).
 *
 * How the Vita stores the home screen (ur0:shell/db/app.db):
 *   tbl_appinfo_page  one row per page. pageNo 0,1,2... are home screen
 *                     pages; a negative pageNo is the inside of a folder.
 *                     Triggers renumber pageNo >= 0 when pages are added
 *                     or removed.
 *   tbl_appinfo_icon  one row per bubble: (pageId, pos 0-9) is the slot.
 *                     type 0 = app (titleId), type 5 = folder (title is the
 *                     folder name, reserved01 = pageNo of its inside page,
 *                     reserved05 = its icon picture).
 *
 * The merge never deletes or recreates an app's row: the current row (with
 * its current icon path, title, lock level...) is only moved to a new
 * (pageId, pos). Folder rows and folder pages come from the backup.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "vita_sqlite.h"
#include "layout_core.h"

#define HOLD_POS_BASE  100000   /* parking slots on the holding page */

/* ── small helpers ─────────────────────────────────────────────────────── */

static void set_err(char *err, int errlen, const char *what, sqlite3 *db) {
    if (!err || errlen <= 0) return;
    if (db) snprintf(err, errlen, "%s: %s", what, sqlite3_errmsg(db));
    else    snprintf(err, errlen, "%s", what);
}

static int open_db(const char *path, int writable, sqlite3 **out) {
    *out = NULL;
    int flags = writable ? SQLITE_OPEN_READWRITE : SQLITE_OPEN_READONLY;
    int rc = sqlite3_open_v2(path, out, flags, VR_SQLITE_VFS);
    if (rc != SQLITE_OK) {
        if (*out) sqlite3_close(*out);
        *out = NULL;
        return rc;
    }
    /* No journal/temp files: work copies only, see vita_sqlite.c */
    sqlite3_exec(*out, "PRAGMA journal_mode=MEMORY", NULL, NULL, NULL);
    sqlite3_exec(*out, "PRAGMA temp_store=MEMORY", NULL, NULL, NULL);
    return SQLITE_OK;
}

/* Run a query that returns one integer. Returns def on any problem. */
static sqlite3_int64 q_int(sqlite3 *db, const char *sql, sqlite3_int64 def) {
    sqlite3_stmt *st = NULL;
    sqlite3_int64 v = def;
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK) return def;
    if (sqlite3_step(st) == SQLITE_ROW && sqlite3_column_type(st, 0) != SQLITE_NULL)
        v = sqlite3_column_int64(st, 0);
    sqlite3_finalize(st);
    return v;
}

/* Same, with one integer parameter */
static sqlite3_int64 q_int1(sqlite3 *db, const char *sql, sqlite3_int64 a,
                            sqlite3_int64 def) {
    sqlite3_stmt *st = NULL;
    sqlite3_int64 v = def;
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK) return def;
    sqlite3_bind_int64(st, 1, a);
    if (sqlite3_step(st) == SQLITE_ROW && sqlite3_column_type(st, 0) != SQLITE_NULL)
        v = sqlite3_column_int64(st, 0);
    sqlite3_finalize(st);
    return v;
}

/* Run one statement with up to three integer parameters (NULL-free) */
static int exec3(sqlite3 *db, const char *sql, int n,
                 sqlite3_int64 a, sqlite3_int64 b, sqlite3_int64 c) {
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK) return -1;
    if (n > 0) sqlite3_bind_int64(st, 1, a);
    if (n > 1) sqlite3_bind_int64(st, 2, b);
    if (n > 2) sqlite3_bind_int64(st, 3, c);
    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    return (rc == SQLITE_DONE || rc == SQLITE_ROW) ? 0 : -1;
}

/* 1 if "PRAGMA integrity_check" says ok */
static int integrity_ok(sqlite3 *db) {
    sqlite3_stmt *st = NULL;
    int ok = 0;
    if (sqlite3_prepare_v2(db, "PRAGMA integrity_check", -1, &st, NULL) != SQLITE_OK)
        return 0;
    if (sqlite3_step(st) == SQLITE_ROW) {
        const unsigned char *t = sqlite3_column_text(st, 0);
        ok = (t && strcmp((const char *)t, "ok") == 0);
    }
    sqlite3_finalize(st);
    return ok;
}

/* 1 if the db has the two home screen tables */
static int has_layout_tables(sqlite3 *db) {
    return q_int(db, "SELECT COUNT(*) FROM sqlite_master WHERE type='table' AND "
                     "name IN ('tbl_appinfo_page','tbl_appinfo_icon')", 0) == 2;
}

/* Copy the CREATE statement of a table into out ("" if missing) */
static void table_sql(sqlite3 *db, const char *name, char *out, int outlen) {
    sqlite3_stmt *st = NULL;
    out[0] = '\0';
    if (sqlite3_prepare_v2(db, "SELECT sql FROM sqlite_master WHERE type='table' "
                               "AND name=?", -1, &st, NULL) != SQLITE_OK)
        return;
    sqlite3_bind_text(st, 1, name, -1, SQLITE_STATIC);
    if (sqlite3_step(st) == SQLITE_ROW) {
        const unsigned char *t = sqlite3_column_text(st, 0);
        if (t) snprintf(out, outlen, "%s", (const char *)t);
    }
    sqlite3_finalize(st);
}

/* ── summary ───────────────────────────────────────────────────────────── */

int layout_summary(const char *db_path, LayoutSummary *out,
                   char *err, int errlen) {
    sqlite3 *db = NULL;
    memset(out, 0, sizeof(*out));
    if (open_db(db_path, 0, &db) != SQLITE_OK) {
        set_err(err, errlen, "Cannot open database", NULL);
        return LAYOUT_ERR_OPEN;
    }
    int ret = 0;
    if (!has_layout_tables(db)) {
        set_err(err, errlen, "Not a home screen database", NULL);
        ret = LAYOUT_ERR_SCHEMA;
    } else if (!integrity_ok(db)) {
        set_err(err, errlen, "Database failed integrity check", NULL);
        ret = LAYOUT_ERR_CHECK;
    } else {
        out->home_pages = (int)q_int(db,
            "SELECT COUNT(*) FROM tbl_appinfo_page WHERE pageNo>=0", 0);
        out->folders = (int)q_int(db,
            "SELECT COUNT(*) FROM tbl_appinfo_icon i JOIN tbl_appinfo_page p "
            "ON p.pageId=i.pageId WHERE p.pageNo>=0 AND i.type=5", 0);
        out->apps = (int)q_int(db,
            "SELECT COUNT(*) FROM tbl_appinfo_icon WHERE type<>5", 0);
        if (out->home_pages <= 0) {
            set_err(err, errlen, "Database has no home screen pages", NULL);
            ret = LAYOUT_ERR_EMPTY;
        }
    }
    sqlite3_close(db);
    return ret;
}

/* ── merge ─────────────────────────────────────────────────────────────── */

typedef struct {
    sqlite3 *w;                 /* work copy (written)   */
    sqlite3 *s;                 /* saved backup (read)   */
    sqlite3_int64 hold;         /* holding page id       */
    sqlite3_int64 home[LAYOUT_MAX_HOME_PAGES];  /* home page ids by index */
    int n_home;                 /* home pages that exist in w right now   */
    sqlite3_stmt *find;         /* find a parked row by key   */
    sqlite3_stmt *move;         /* move a row to (page, pos)  */
    LayoutReport *rep;
    char *err; int errlen;
} Merge;

/* Page id for home page index idx, creating the page if needed.
 * New pages copy the look (theme fields) of the last existing page. */
static sqlite3_int64 home_page(Merge *m, int idx) {
    if (idx < 0 || idx >= LAYOUT_MAX_HOME_PAGES) return -1;
    if (idx < m->n_home) return m->home[idx];
    if (idx != m->n_home || m->n_home == 0) return -1;   /* always sequential */
    /* pageNo = idx = number of home pages, so the insert trigger (which
     * shifts pages with pageNo >= new pageNo) has nothing to shift. */
    if (exec3(m->w,
        "INSERT INTO tbl_appinfo_page(pageNo,themeFile,bgColor,texWidth,"
        "texHeight,imageWidth,imageHeight,reserved01,reserved02,reserved03,"
        "reserved04,reserved05) SELECT ?,themeFile,bgColor,texWidth,texHeight,"
        "imageWidth,imageHeight,reserved01,reserved02,reserved03,reserved04,"
        "reserved05 FROM tbl_appinfo_page WHERE pageId=?",
        2, idx, m->home[m->n_home - 1], 0) < 0)
        return -1;
    m->home[m->n_home] = sqlite3_last_insert_rowid(m->w);
    return m->home[m->n_home++];
}

/* Find a parked row matching the saved bubble at saved rowid srow.
 * Match: same type, titleId and command. Returns rowid or 0. */
static sqlite3_int64 find_parked(Merge *m, sqlite3_stmt *saved_row) {
    /* saved_row columns: 0 rowid, 1 type, 2 titleId, 3 command */
    sqlite3_reset(m->find);
    sqlite3_clear_bindings(m->find);
    sqlite3_bind_int64(m->find, 1, m->hold);
    sqlite3_bind_value(m->find, 2, sqlite3_column_value(saved_row, 1));
    const unsigned char *tid = sqlite3_column_text(saved_row, 2);
    const unsigned char *cmd = sqlite3_column_text(saved_row, 3);
    sqlite3_bind_text(m->find, 3, tid ? (const char *)tid : "", -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(m->find, 4, cmd ? (const char *)cmd : "", -1, SQLITE_TRANSIENT);
    sqlite3_int64 r = 0;
    if (sqlite3_step(m->find) == SQLITE_ROW)
        r = sqlite3_column_int64(m->find, 0);
    sqlite3_reset(m->find);
    return r;
}

static int move_row(Merge *m, sqlite3_int64 rowid, sqlite3_int64 page, int pos) {
    sqlite3_reset(m->move);
    sqlite3_bind_int64(m->move, 1, page);
    sqlite3_bind_int(m->move, 2, pos);
    sqlite3_bind_int64(m->move, 3, rowid);
    int rc = sqlite3_step(m->move);
    sqlite3_reset(m->move);
    return rc == SQLITE_DONE ? 0 : -1;
}

/* Park every bubble of one page on the holding page, in order.
 * Folder bubbles: their contents are parked right after them. */
static int park_page(Merge *m, sqlite3_int64 page, int *seq,
                     sqlite3_int64 *folder_pages, int *n_folder_pages,
                     int max_folder_pages, int depth) {
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(m->w, "SELECT rowid,type,reserved01 FROM "
            "tbl_appinfo_icon WHERE pageId=? ORDER BY pos", -1, &st, NULL) != SQLITE_OK)
        return -1;
    sqlite3_bind_int64(st, 1, page);
    /* Collect first: moving rows while stepping the same table is unsafe */
    int cap = (int)q_int1(m->w, "SELECT COUNT(*) FROM tbl_appinfo_icon "
                                "WHERE pageId=?", page, -1);
    if (cap < 0) { sqlite3_finalize(st); return -1; }
    if (cap == 0) { sqlite3_finalize(st); return 0; }
    sqlite3_int64 *rows = (sqlite3_int64 *)malloc(sizeof(sqlite3_int64) * cap * 2);
    int *types = (int *)malloc(sizeof(int) * cap);
    if (!rows || !types) { free(rows); free(types); sqlite3_finalize(st); return -1; }
    sqlite3_int64 *fpn = rows + cap;
    int n = 0;
    while (n < cap && sqlite3_step(st) == SQLITE_ROW) {
        rows[n]  = sqlite3_column_int64(st, 0);
        types[n] = sqlite3_column_int(st, 1);
        fpn[n]   = sqlite3_column_type(st, 2) == SQLITE_INTEGER
                   ? sqlite3_column_int64(st, 2) : 0;
        n++;
    }
    sqlite3_finalize(st);

    int ret = 0;
    for (int i = 0; i < n && ret == 0; i++) {
        if (move_row(m, rows[i], m->hold, HOLD_POS_BASE + (*seq)++) < 0) { ret = -1; break; }
        if (types[i] == 5 && depth == 0 && fpn[i] < 0) {
            sqlite3_int64 fp = q_int1(m->w, "SELECT pageId FROM tbl_appinfo_page "
                                      "WHERE pageNo=? LIMIT 1", fpn[i], 0);
            if (fp > 0 && fp != m->hold) {
                int seen = 0;
                for (int k = 0; k < *n_folder_pages; k++)
                    if (folder_pages[k] == fp) seen = 1;
                if (!seen && *n_folder_pages < max_folder_pages) {
                    folder_pages[(*n_folder_pages)++] = fp;
                    if (park_page(m, fp, seq, folder_pages, n_folder_pages,
                                  max_folder_pages, 1) < 0)
                        ret = -1;
                }
            }
        }
    }
    free(rows);
    free(types);
    return ret;
}

/* A negative pageNo not used by any page in w, close to want */
static sqlite3_int64 free_folder_pageno(Merge *m, sqlite3_int64 want) {
    if (want >= 0 || want <= -99000000) want = -1000000;
    for (int i = 0; i < 1000; i++, want--) {
        if (q_int1(m->w, "SELECT COUNT(*) FROM tbl_appinfo_page WHERE pageNo=?",
                   want, 1) == 0)
            return want;
    }
    return 0;
}

/* Copy one saved row's columns into a prepared INSERT */
static void bind_cols(sqlite3_stmt *ins, int first_param,
                      sqlite3_stmt *src, int first_col, int count) {
    for (int i = 0; i < count; i++)
        sqlite3_bind_value(ins, first_param + i, sqlite3_column_value(src, first_col + i));
}

/* Recreate one saved folder at (home index hidx, pos) with the apps that
 * are still installed. Returns 1 placed, 0 skipped (no apps), -1 error. */
static int place_folder(Merge *m, sqlite3_int64 saved_icon_rowid,
                        sqlite3_int64 saved_fpn, int hidx, int pos) {
    sqlite3_int64 saved_fp = q_int1(m->s, "SELECT pageId FROM tbl_appinfo_page "
                                    "WHERE pageNo=? LIMIT 1", saved_fpn, 0);
    if (saved_fp <= 0) return 0;

    sqlite3_stmt *mem = NULL;
    if (sqlite3_prepare_v2(m->s, "SELECT rowid,type,titleId,command,pos FROM "
            "tbl_appinfo_icon WHERE pageId=? AND type<>5 ORDER BY pos",
            -1, &mem, NULL) != SQLITE_OK)
        return -1;
    sqlite3_bind_int64(mem, 1, saved_fp);

    sqlite3_int64 new_fp = 0;   /* folder page in w, made on first app found */
    int ret = 0;
    while (sqlite3_step(mem) == SQLITE_ROW) {
        int mpos = sqlite3_column_int(mem, 4);
        sqlite3_int64 r = find_parked(m, mem);
        if (r == 0) { m->rep->missing++; continue; }

        if (new_fp == 0) {
            sqlite3_int64 hp = home_page(m, hidx);
            sqlite3_int64 pn = free_folder_pageno(m, saved_fpn);
            if (hp <= 0 || pn == 0) { ret = -1; break; }

            /* folder's inside page: copy the saved page's fields */
            sqlite3_stmt *sp = NULL, *ins = NULL;
            int ok = 0;
            if (sqlite3_prepare_v2(m->s, "SELECT themeFile,bgColor,texWidth,"
                    "texHeight,imageWidth,imageHeight,reserved01,reserved02,"
                    "reserved03,reserved04,reserved05 FROM tbl_appinfo_page "
                    "WHERE pageId=?", -1, &sp, NULL) == SQLITE_OK &&
                sqlite3_prepare_v2(m->w, "INSERT INTO tbl_appinfo_page(pageNo,"
                    "themeFile,bgColor,texWidth,texHeight,imageWidth,imageHeight,"
                    "reserved01,reserved02,reserved03,reserved04,reserved05) "
                    "VALUES(?,?,?,?,?,?,?,?,?,?,?,?)", -1, &ins, NULL) == SQLITE_OK) {
                sqlite3_bind_int64(sp, 1, saved_fp);
                if (sqlite3_step(sp) == SQLITE_ROW) {
                    sqlite3_bind_int64(ins, 1, pn);
                    bind_cols(ins, 2, sp, 0, 11);
                    ok = (sqlite3_step(ins) == SQLITE_DONE);
                }
            }
            if (ok) new_fp = sqlite3_last_insert_rowid(m->w);
            sqlite3_finalize(sp);
            sqlite3_finalize(ins);
            if (!ok) { ret = -1; break; }

            /* folder bubble: copy the saved row, new page/pos/link */
            sp = ins = NULL; ok = 0;
            if (sqlite3_prepare_v2(m->s, "SELECT iconPath,title,type,command,"
                    "titleId,icon0Type,parentalLockLv,status,reserved02,"
                    "reserved03,reserved04,reserved05 FROM tbl_appinfo_icon "
                    "WHERE rowid=?", -1, &sp, NULL) == SQLITE_OK &&
                sqlite3_prepare_v2(m->w, "INSERT INTO tbl_appinfo_icon(pageId,pos,"
                    "reserved01,iconPath,title,type,command,titleId,icon0Type,"
                    "parentalLockLv,status,reserved02,reserved03,reserved04,"
                    "reserved05) VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)",
                    -1, &ins, NULL) == SQLITE_OK) {
                sqlite3_bind_int64(sp, 1, saved_icon_rowid);
                if (sqlite3_step(sp) == SQLITE_ROW) {
                    sqlite3_bind_int64(ins, 1, hp);
                    sqlite3_bind_int(ins, 2, pos);
                    sqlite3_bind_int64(ins, 3, pn);
                    bind_cols(ins, 4, sp, 0, 12);
                    ok = (sqlite3_step(ins) == SQLITE_DONE);
                }
            }
            sqlite3_finalize(sp);
            sqlite3_finalize(ins);
            if (!ok) { ret = -1; break; }
            ret = 1;
        }
        if (move_row(m, r, new_fp, mpos) < 0) { ret = -1; break; }
        m->rep->placed++;
    }
    sqlite3_finalize(mem);
    return ret;
}

/* v1.2.0 build 2: give home page `page` (in w) the look of saved page
 * `saved_page` (wave colour = bgColor, background/theme fields). Before,
 * pages kept the look of the current (e.g. freshly rebuilt) database, so
 * the wave colour changed after a restore. */
static int apply_saved_look(Merge *m, sqlite3_int64 page, sqlite3_int64 saved_page) {
    sqlite3_stmt *sp = NULL, *up = NULL;
    int ok = 0;
    if (sqlite3_prepare_v2(m->s, "SELECT themeFile,bgColor,texWidth,texHeight,"
            "imageWidth,imageHeight,reserved01,reserved02,reserved03,reserved04,"
            "reserved05 FROM tbl_appinfo_page WHERE pageId=?", -1, &sp, NULL) == SQLITE_OK &&
        sqlite3_prepare_v2(m->w, "UPDATE tbl_appinfo_page SET themeFile=?,bgColor=?,"
            "texWidth=?,texHeight=?,imageWidth=?,imageHeight=?,reserved01=?,"
            "reserved02=?,reserved03=?,reserved04=?,reserved05=? WHERE pageId=?",
            -1, &up, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(sp, 1, saved_page);
        if (sqlite3_step(sp) == SQLITE_ROW) {
            bind_cols(up, 1, sp, 0, 11);
            sqlite3_bind_int64(up, 12, page);
            ok = (sqlite3_step(up) == SQLITE_DONE);
        }
    }
    sqlite3_finalize(sp);
    sqlite3_finalize(up);
    return ok ? 0 : -1;
}

/* Saved home page ids, in page order */
static int saved_home_pages(sqlite3 *s, sqlite3_int64 *out, int max) {
    sqlite3_stmt *st = NULL;
    int n = 0;
    if (sqlite3_prepare_v2(s, "SELECT pageId FROM tbl_appinfo_page WHERE "
            "pageNo>=0 ORDER BY pageNo", -1, &st, NULL) != SQLITE_OK)
        return -1;
    while (sqlite3_step(st) == SQLITE_ROW && n < max)
        out[n++] = sqlite3_column_int64(st, 0);
    sqlite3_finalize(st);
    return n;
}

/* Checks on the finished result (inside the transaction) */
static int verify(Merge *m, sqlite3_int64 apps_before) {
    sqlite3 *w = m->w;
    const char *why = NULL;
    sqlite3_int64 pages = q_int(w, "SELECT COUNT(*) FROM tbl_appinfo_page "
                                   "WHERE pageNo>=0", -1);
    if (q_int(w, "SELECT COUNT(*) FROM tbl_appinfo_icon WHERE type<>5", -1) != apps_before)
        why = "app count changed";
    else if (pages < 1 || pages > LAYOUT_MAX_HOME_PAGES)
        why = "bad number of home pages";
    else if (q_int(w, "SELECT MIN(pageNo) FROM tbl_appinfo_page WHERE pageNo>=0", -1) != 0 ||
             q_int(w, "SELECT MAX(pageNo) FROM tbl_appinfo_page WHERE pageNo>=0", -1) != pages - 1)
        why = "home pages not numbered 0..n-1";
    else if (q_int(w, "SELECT COUNT(*) FROM tbl_appinfo_icon i JOIN tbl_appinfo_page p "
                      "ON p.pageId=i.pageId WHERE p.pageNo>=0 AND (i.pos<0 OR i.pos>9)", -1) != 0)
        why = "bubble outside slots 0-9";
    else if (q_int(w, "SELECT COUNT(*) FROM tbl_appinfo_icon WHERE pageId NOT IN "
                      "(SELECT pageId FROM tbl_appinfo_page)", -1) != 0)
        why = "bubble on a missing page";
    else if (q_int(w, "SELECT COUNT(*) FROM tbl_appinfo_icon WHERE type=5 AND "
                      "reserved01 NOT IN (SELECT pageNo FROM tbl_appinfo_page "
                      "WHERE pageNo<0)", -1) != 0)
        why = "folder without its page";
    else if (q_int(w, "SELECT COUNT(*) FROM (SELECT reserved01 FROM tbl_appinfo_icon "
                      "WHERE type=5 GROUP BY reserved01 HAVING COUNT(*)>1)", -1) != 0)
        why = "two folders share a page";
    else if (q_int(w, "SELECT COUNT(*) FROM tbl_appinfo_icon WHERE pos>=100000", -1) != 0)
        why = "parked bubble left over";
    if (why) {
        set_err(m->err, m->errlen, why, NULL);
        return -1;
    }
    return 0;
}

int layout_merge(const char *saved_path, const char *work_path,
                 LayoutReport *rep, char *err, int errlen) {
    Merge m;
    memset(&m, 0, sizeof(m));
    memset(rep, 0, sizeof(*rep));
    m.rep = rep; m.err = err; m.errlen = errlen;
    if (err && errlen > 0) err[0] = '\0';
    int ret = 0, in_tx = 0;
    sqlite3_int64 folder_pages[LAYOUT_MAX_HOME_PAGES * LAYOUT_SLOTS_PER_PAGE];
    int n_folder_pages = 0;
    sqlite3_int64 *left = NULL;

    if (open_db(saved_path, 0, &m.s) != SQLITE_OK) {
        set_err(err, errlen, "Cannot open the layout backup", NULL);
        ret = LAYOUT_ERR_OPEN; goto out;
    }
    if (open_db(work_path, 1, &m.w) != SQLITE_OK) {
        set_err(err, errlen, "Cannot open the work copy", NULL);
        ret = LAYOUT_ERR_OPEN; goto out;
    }

    /* Same database layout (same system software generation)? */
    {
        char a[1024], b[1024];
        int same = has_layout_tables(m.s) && has_layout_tables(m.w);
        if (same) {
            table_sql(m.s, "tbl_appinfo_icon", a, sizeof(a));
            table_sql(m.w, "tbl_appinfo_icon", b, sizeof(b));
            same = a[0] && strcmp(a, b) == 0;
        }
        if (same) {
            table_sql(m.s, "tbl_appinfo_page", a, sizeof(a));
            table_sql(m.w, "tbl_appinfo_page", b, sizeof(b));
            same = a[0] && strcmp(a, b) == 0;
        }
        if (same)
            same = q_int(m.s, "PRAGMA user_version", -1) ==
                   q_int(m.w, "PRAGMA user_version", -2);
        if (!same) {
            set_err(err, errlen, "Backup is from a different database format", NULL);
            ret = LAYOUT_ERR_SCHEMA; goto out;
        }
    }

    sqlite3_int64 saved_pages[LAYOUT_MAX_HOME_PAGES * 2];
    int n_saved = saved_home_pages(m.s, saved_pages, LAYOUT_MAX_HOME_PAGES * 2);
    if (n_saved <= 0) {
        set_err(err, errlen, "Backup has no home screen pages", NULL);
        ret = LAYOUT_ERR_EMPTY; goto out;
    }

    if (sqlite3_exec(m.w, "BEGIN", NULL, NULL, NULL) != SQLITE_OK) {
        set_err(err, errlen, "BEGIN", m.w); ret = LAYOUT_ERR_SQL; goto out;
    }
    in_tx = 1;

    sqlite3_int64 apps_before = q_int(m.w,
        "SELECT COUNT(*) FROM tbl_appinfo_icon WHERE type<>5", -1);

    /* Current home pages, in order */
    m.n_home = saved_home_pages(m.w, m.home, LAYOUT_MAX_HOME_PAGES);
    if (m.n_home <= 0 ||
        q_int(m.w, "SELECT COUNT(*) FROM tbl_appinfo_page WHERE pageNo>=0", 0) != m.n_home) {
        set_err(err, errlen, "Current home screen has too many pages", NULL);
        ret = LAYOUT_ERR_FULL; goto out;
    }

    /* Holding page: a negative pageNo, so no page renumbering trigger */
    {
        sqlite3_int64 hold_no = free_folder_pageno(&m, -98000000);
        if (hold_no == 0 ||
            exec3(m.w, "INSERT INTO tbl_appinfo_page(pageNo) VALUES(?)", 1,
                  hold_no, 0, 0) < 0) {
            set_err(err, errlen, "Create holding page", m.w);
            ret = LAYOUT_ERR_SQL; goto out;
        }
        m.hold = sqlite3_last_insert_rowid(m.w);
    }

    if (sqlite3_prepare_v2(m.w, "SELECT rowid FROM tbl_appinfo_icon WHERE pageId=? "
            "AND type=? AND IFNULL(titleId,'')=? AND IFNULL(command,'')=? "
            "ORDER BY pos LIMIT 1", -1, &m.find, NULL) != SQLITE_OK ||
        sqlite3_prepare_v2(m.w, "UPDATE tbl_appinfo_icon SET pageId=?, pos=? "
            "WHERE rowid=?", -1, &m.move, NULL) != SQLITE_OK) {
        set_err(err, errlen, "Prepare", m.w); ret = LAYOUT_ERR_SQL; goto out;
    }

    /* 1. Park every bubble of the home screen and its folders */
    {
        int seq = 0;
        for (int i = 0; i < m.n_home; i++)
            if (park_page(&m, m.home[i], &seq, folder_pages, &n_folder_pages,
                          (int)(sizeof(folder_pages) / sizeof(folder_pages[0])), 0) < 0) {
                set_err(err, errlen, "Park bubbles", m.w);
                ret = LAYOUT_ERR_SQL; goto out;
            }
        /* Old folder bubbles and folder pages go; the backup's come back */
        if (exec3(m.w, "DELETE FROM tbl_appinfo_icon WHERE pageId=? AND type=5",
                  1, m.hold, 0, 0) < 0) {
            set_err(err, errlen, "Remove old folders", m.w);
            ret = LAYOUT_ERR_SQL; goto out;
        }
        for (int i = 0; i < n_folder_pages; i++)
            if (q_int1(m.w, "SELECT COUNT(*) FROM tbl_appinfo_icon WHERE pageId=?",
                       folder_pages[i], 1) == 0 &&
                exec3(m.w, "DELETE FROM tbl_appinfo_page WHERE pageId=?", 1,
                      folder_pages[i], 0, 0) < 0) {
                set_err(err, errlen, "Remove old folder page", m.w);
                ret = LAYOUT_ERR_SQL; goto out;
            }
    }

    /* 2. Put bubbles back where the backup had them */
    int used = 0;   /* home pages used so far */
    {
        sqlite3_stmt *si = NULL;
        if (sqlite3_prepare_v2(m.s, "SELECT rowid,type,titleId,command,pos,"
                "reserved01 FROM tbl_appinfo_icon WHERE pageId=? ORDER BY pos",
                -1, &si, NULL) != SQLITE_OK) {
            set_err(err, errlen, "Read backup", m.s); ret = LAYOUT_ERR_SQL; goto out;
        }
        for (int p = 0; p < n_saved && ret == 0; p++) {
            int placed_here = 0;
            sqlite3_reset(si);
            sqlite3_bind_int64(si, 1, saved_pages[p]);
            while (sqlite3_step(si) == SQLITE_ROW) {
                int pos  = sqlite3_column_int(si, 4);
                int type = sqlite3_column_int(si, 1);
                if (pos < 0 || pos >= LAYOUT_SLOTS_PER_PAGE) continue;
                if (used >= LAYOUT_MAX_HOME_PAGES) {
                    /* backup itself has more pages than allowed */
                    if (type == 5 || find_parked(&m, si)) {
                        set_err(err, errlen, "Too many home pages", NULL);
                        ret = LAYOUT_ERR_FULL;
                    }
                    break;
                }
                if (type == 5) {
                    if (sqlite3_column_type(si, 5) != SQLITE_INTEGER) continue;
                    int r = place_folder(&m, sqlite3_column_int64(si, 0),
                                         sqlite3_column_int64(si, 5), used, pos);
                    if (r < 0) {
                        set_err(err, errlen, "Restore folder", m.w);
                        ret = LAYOUT_ERR_SQL; break;
                    }
                    if (r == 1) { rep->folders++; placed_here = 1; }
                    else rep->folders_skipped++;
                } else {
                    sqlite3_int64 r = find_parked(&m, si);
                    if (r == 0) { rep->missing++; continue; }
                    sqlite3_int64 hp = home_page(&m, used);
                    if (hp <= 0 || move_row(&m, r, hp, pos) < 0) {
                        set_err(err, errlen, "Place app", m.w);
                        ret = LAYOUT_ERR_SQL; break;
                    }
                    rep->placed++;
                    placed_here = 1;
                }
            }
            if (placed_here) {
                if (apply_saved_look(&m, m.home[used], saved_pages[p]) < 0) {
                    set_err(err, errlen, "Copy page colour", m.w);
                    ret = LAYOUT_ERR_SQL;
                }
                used++;
            }
        }
        sqlite3_finalize(si);
        if (ret) goto out;
    }

    /* 3. Apps not in the backup: after the last bubble, then new pages */
    {
        int n_left = (int)q_int1(m.w, "SELECT COUNT(*) FROM tbl_appinfo_icon "
                                      "WHERE pageId=?", m.hold, 0);
        if (n_left > 0) {
            left = (sqlite3_int64 *)malloc(sizeof(sqlite3_int64) * n_left);
            if (!left) { set_err(err, errlen, "Out of memory", NULL);
                         ret = LAYOUT_ERR_SQL; goto out; }
            sqlite3_stmt *st = NULL;
            int k = 0;
            if (sqlite3_prepare_v2(m.w, "SELECT rowid FROM tbl_appinfo_icon WHERE "
                    "pageId=? ORDER BY pos", -1, &st, NULL) == SQLITE_OK) {
                sqlite3_bind_int64(st, 1, m.hold);
                while (sqlite3_step(st) == SQLITE_ROW && k < n_left)
                    left[k++] = sqlite3_column_int64(st, 0);
            }
            sqlite3_finalize(st);
            if (k != n_left) { set_err(err, errlen, "List new apps", m.w);
                               ret = LAYOUT_ERR_SQL; goto out; }

            int page = used > 0 ? used - 1 : 0;
            int slot = 0;
            if (used > 0) {
                sqlite3_int64 hp = home_page(&m, page);
                slot = (int)q_int1(m.w, "SELECT IFNULL(MAX(pos),-1) FROM "
                                        "tbl_appinfo_icon WHERE pageId=?", hp, -1) + 1;
            }
            for (int i = 0; i < n_left; i++) {
                if (slot >= LAYOUT_SLOTS_PER_PAGE) { page++; slot = 0; }
                sqlite3_int64 hp = home_page(&m, page);
                if (hp <= 0) {
                    set_err(err, errlen, "Not enough room on the home screen", NULL);
                    ret = LAYOUT_ERR_FULL; goto out;
                }
                if (move_row(&m, left[i], hp, slot++) < 0) {
                    set_err(err, errlen, "Add new app", m.w);
                    ret = LAYOUT_ERR_SQL; goto out;
                }
                rep->added++;
            }
            if (page + 1 > used) used = page + 1;
        }
        if (used == 0) used = 1;   /* keep one (empty) page at least */
    }

    /* 4. Remove home pages that are no longer used (from the top, so the
     *    delete trigger has nothing to renumber), then the holding page. */
    for (int i = m.n_home - 1; i >= used; i--) {
        if (q_int1(m.w, "SELECT COUNT(*) FROM tbl_appinfo_icon WHERE pageId=?",
                   m.home[i], 1) != 0 ||
            exec3(m.w, "DELETE FROM tbl_appinfo_page WHERE pageId=?", 1,
                  m.home[i], 0, 0) < 0) {
            set_err(err, errlen, "Remove empty page", m.w);
            ret = LAYOUT_ERR_SQL; goto out;
        }
    }
    if (q_int1(m.w, "SELECT COUNT(*) FROM tbl_appinfo_icon WHERE pageId=?",
               m.hold, 1) != 0 ||
        exec3(m.w, "DELETE FROM tbl_appinfo_page WHERE pageId=?", 1, m.hold, 0, 0) < 0) {
        set_err(err, errlen, "Remove holding page", m.w);
        ret = LAYOUT_ERR_SQL; goto out;
    }
    rep->pages = used;

    /* 5. Check, then commit */
    if (verify(&m, apps_before) < 0) { ret = LAYOUT_ERR_CHECK; goto out; }
    sqlite3_finalize(m.find); m.find = NULL;
    sqlite3_finalize(m.move); m.move = NULL;
    if (sqlite3_exec(m.w, "COMMIT", NULL, NULL, NULL) != SQLITE_OK) {
        set_err(err, errlen, "COMMIT", m.w); ret = LAYOUT_ERR_SQL; goto out;
    }
    in_tx = 0;
    if (!integrity_ok(m.w)) {
        set_err(err, errlen, "Result failed integrity check", NULL);
        ret = LAYOUT_ERR_CHECK;
    }

out:
    free(left);
    if (m.find) sqlite3_finalize(m.find);
    if (m.move) sqlite3_finalize(m.move);
    if (in_tx && m.w) sqlite3_exec(m.w, "ROLLBACK", NULL, NULL, NULL);
    if (m.w) sqlite3_close(m.w);
    if (m.s) sqlite3_close(m.s);
    return ret;
}
