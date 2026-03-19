#pragma once


/*
 * official_recovery.h
 * Reimplements all 5 functions from Sony's built-in PS Vita safe-mode menu.
 *
 *   1. Restart System           -- clean cold reboot
 *   2. Rebuild Database         -- delete app.db, firmware regenerates on boot
 *   3. Format Memory Card       -- wipe ux0: (preserves ux0:tai/ for HENkaku)
 *   4. Restore PS Vita System   -- factory reset via ScePromoterUtil
 *   5. Update System Software   -- install PSP2UPDAT.PUP from ux0:update/
 */

void official_recovery_screen_run(void *font);
