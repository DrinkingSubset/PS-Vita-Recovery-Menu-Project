/*
 * fs_policy.c - write allowlist for File Manager and FTP (v1.1.9).
 * Allowlist, not blocklist: a drive we do not know about stays read-only.
 */
#include <string.h>
#include <strings.h>
#include "fs_policy.h"

static const char *const WRITABLE[] = {
    "ux0", "ur0", "uma0", "imc0", "xmc0", "grw0",
};

/* Copy the drive name ("os0" from "os0:/x") into out; 0 if no ':' found */
static int drive_of(const char *path, char *out, int outlen) {
    if (!path) return 0;
    while (*path == '/') path++;           /* FTP virtual paths start with '/' */
    const char *c = strchr(path, ':');
    if (!c) return 0;
    int n = (int)(c - path);
    if (n <= 0 || n >= outlen) return 0;
    memcpy(out, path, n);
    out[n] = '\0';
    return 1;
}

int fs_path_writable(const char *path) {
    char d[16];
    if (!drive_of(path, d, sizeof(d))) return 0;
    for (unsigned i = 0; i < sizeof(WRITABLE) / sizeof(WRITABLE[0]); i++)
        if (strcasecmp(d, WRITABLE[i]) == 0) return 1;
    return 0;
}

int fs_drive_protected(const char *path) {
    return !fs_path_writable(path);
}
