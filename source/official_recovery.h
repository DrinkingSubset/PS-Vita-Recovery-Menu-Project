#pragma once


/*
 * official_recovery.h
 * Reimplements all 5 functions from Sony's built-in PS Vita safe-mode menu.
 *
 *   1. Restart System           -- clean cold reboot
 *   2. Rebuild Database         -- boot plugin renames app.db at next boot
 *   3. Format Memory Card       -- wipe ux0: (preserves ux0:tai/ for HENkaku)
 *   4. Restore PS Vita System   -- explains Sony Safe Mode option 5 (v1.1.7)
 *   5. Update System Software   -- install PSP2UPDAT.PUP from ux0:update/
 */

void official_recovery_screen_run(void *font);

/* Rebuild Database flow (warnings + boot plugin request); also used by
 * Restore / Unbrick option 5. */
void official_recovery_rebuild_database(void *font);
