#pragma once
/*
 * recovery_installer.h — R-Trigger Boot Recovery Installer
 *
 * Installs / uninstalls the boot_recovery.skprx kernel plugin.
 * Accessed from Advanced -> Boot Recovery Installer.
 */

void recovery_installer_run(void);

/* v1.2.0: 0 = nothing to do (up to date, or not installed at all),
 * 1 = installed but the plugin files are older than this app's,
 * 2 = partly installed (a file or config line is missing). */
int recovery_installer_needs_update(void);
