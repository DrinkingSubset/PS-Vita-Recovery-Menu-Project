#pragma once
/*
 * fs_policy.h - which drives the app may WRITE to (v1.1.9).
 *
 * From v1.1.9 the app is built as "unsafe" homebrew, so it can read every
 * mounted drive. Writing is only allowed on user storage. System drives are
 * read-only for File Manager and the FTP server, even though the OS might
 * allow some writes, because a bad write there can brick the console.
 *
 * Writable : ux0: ur0: uma0: imc0: xmc0: grw0:
 * Read-only: everything else, including os0: vs0: sa0: pd0: tm0: ud0: vd0:
 *            and raw devices such as sdstor0:
 */

/* 1 if a write (create, delete, rename, mkdir, rmdir, upload) to this path
 * is allowed. Unknown or malformed paths are NOT writable. */
int fs_path_writable(const char *path);

/* 1 if the drive (e.g. "os0:") is one of the protected read-only ones */
int fs_drive_protected(const char *path);
