#pragma once
/*
 * layout.h - Home Screen Layout backup / restore screen (v1.2.0).
 */

/* Full screen: Back Up Layout / Restore Latest / Restore Previous */
void layout_screen_run(void *font);

/* Back up the current layout without any screen (used before Rebuild
 * Database). Returns 1 = saved, 2 = unchanged since the last backup,
 * negative = failed (the existing backups are left alone). */
int layout_backup_quiet(void);

/* Remove a staged restore that has not been applied yet (used when a
 * Rebuild Database is scheduled, which would make it out of date). */
void layout_cancel_pending(void);
