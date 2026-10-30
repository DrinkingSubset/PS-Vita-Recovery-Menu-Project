#pragma once
/*
 * layout_core.h - home screen layout logic on app.db copies (v1.2.0).
 * Pure SQLite, no screen or sceIo code, so it can be tested on a PC.
 */

typedef struct {
    int home_pages;   /* pages on the home screen                 */
    int folders;      /* folder bubbles                           */
    int apps;         /* app bubbles (on pages and inside folders) */
} LayoutSummary;

typedef struct {
    int pages;            /* home pages in the result                   */
    int folders;          /* folders restored                           */
    int folders_skipped;  /* folders left out: none of their apps found */
    int placed;           /* apps put back where the backup had them    */
    int missing;          /* apps in the backup that are not installed  */
    int added;            /* apps not in the backup, added at the end   */
} LayoutReport;

/* Error codes (negative) */
#define LAYOUT_ERR_OPEN     -1   /* could not open a database          */
#define LAYOUT_ERR_SCHEMA   -2   /* not an app.db / different version  */
#define LAYOUT_ERR_SQL      -3   /* an SQL step failed                 */
#define LAYOUT_ERR_FULL     -4   /* more home pages needed than allowed */
#define LAYOUT_ERR_CHECK    -5   /* result failed a safety check       */
#define LAYOUT_ERR_EMPTY    -6   /* backup has no home screen pages    */

/* The home screen holds at most this many pages */
#define LAYOUT_MAX_HOME_PAGES  10
#define LAYOUT_SLOTS_PER_PAGE  10

/* Count pages/folders/apps in an app.db file and run SQLite's
 * integrity_check on it. Returns 0 when the file is a healthy app.db. */
int layout_summary(const char *db_path, LayoutSummary *out,
                   char *err, int errlen);

/* Rearrange the icons in work_path (a COPY of the current app.db) to match
 * the layout saved in saved_path (an older app.db copy):
 *  - apps go back to their saved page and slot, folders are recreated with
 *    their names and icons;
 *  - apps in the backup that are no longer installed are skipped;
 *  - apps installed after the backup are added after the last bubble.
 * Only the two home screen tables are changed. Everything happens in one
 * transaction and is checked before it is committed; on any error
 * work_path is left unchanged (and the caller throws it away anyway).
 * Returns 0 or a LAYOUT_ERR_* code. */
int layout_merge(const char *saved_path, const char *work_path,
                 LayoutReport *rep, char *err, int errlen);
