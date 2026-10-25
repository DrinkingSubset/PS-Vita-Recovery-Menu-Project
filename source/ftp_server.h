#pragma once

/*
 * ftp_server.h — Embedded FTP server
 *
 * Serves all Vita mount points (ux0, ur0, imc0, uma0, grw0, host0, psp0)
 * over WiFi so the user can browse and transfer files from a PC using any
 * standard FTP client (FileZilla, Windows Explorer, WinSCP, etc.).
 *
 * Usage: call ftp_screen_run() — it handles its own display and input loop.
 * The server runs until the user presses O (Circle) to stop it.
 */

void ftp_screen_run(void);

/* Brings up the Vita network stack (shared with the update checker).
 * Returns 0 on success or an SCE error. Safe to call repeatedly. */
int net_ensure_init(void);
