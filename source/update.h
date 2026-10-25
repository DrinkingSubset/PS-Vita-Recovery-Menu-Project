#pragma once

/*
 * update.h — "Check for updates" screen
 *
 * Asks the GitHub Releases API for the newest release of the project,
 * compares it with this build's version, and can download the release's
 * .vpk file into ux0:downloads/. Nothing is installed automatically: the
 * user installs the downloaded VPK themselves (e.g. with VitaShell).
 *
 * update_screen_run() handles its own display and input loop.
 */

void update_screen_run(void);
