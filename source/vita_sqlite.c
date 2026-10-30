/*
 * vita_sqlite.c - use the Vita's own SQLite module from this app (v1.2.0).
 *
 * The system SQLite file layer ("VFS") opens every database read-only, so
 * this file adds a small read/write file layer named "vr_rw" built directly
 * on sceIo. It is only used for the app's own work files in
 * ux0:data/vita_recovery/layout/ - the live ur0:shell/db/app.db is never
 * opened through SQLite (it is copied as a plain file, and replaced by the
 * boot plugin at the next boot).
 *
 * Kept simple on purpose:
 *  - No locking. Nothing else ever opens our work files.
 *  - No temp/journal files. The caller sets journal_mode=MEMORY and
 *    temp_store=MEMORY, and always works on a throw-away copy, so a crash
 *    in the middle can only damage the copy.
 */

#include "vita_sqlite.h"

#ifndef __vita__

int vita_sqlite_init(void) { return 0; }

#else

#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/sqlite.h>
#include <psp2/sysmodule.h>
#include <stdlib.h>
#include <string.h>

#define IS_ERR(x) ((int)(x) < 0)

typedef struct {
    sqlite3_file base;   /* must be first */
    SceUID fd;
} VrFile;

static sqlite3_vfs  g_vfs;
static sqlite3_vfs *g_sys_vfs = NULL;   /* system layer, for time/random */
static int          g_ready = 0;

/* ── file methods ──────────────────────────────────────────────────────── */

static int vr_close(sqlite3_file *f) {
    VrFile *p = (VrFile *)f;
    if (p->fd >= 0) sceIoClose(p->fd);
    p->fd = -1;
    return SQLITE_OK;
}

static int vr_read(sqlite3_file *f, void *buf, int amt, sqlite3_int64 off) {
    VrFile *p = (VrFile *)f;
    if (sceIoLseek(p->fd, off, SCE_SEEK_SET) != off) return SQLITE_IOERR_READ;
    int got = 0;
    while (got < amt) {
        int r = sceIoRead(p->fd, (char *)buf + got, amt - got);
        if (r < 0) return SQLITE_IOERR_READ;
        if (r == 0) break;
        got += r;
    }
    if (got < amt) {
        /* SQLite requires the missing part to be zero-filled */
        memset((char *)buf + got, 0, amt - got);
        return SQLITE_IOERR_SHORT_READ;
    }
    return SQLITE_OK;
}

static int vr_write(sqlite3_file *f, const void *buf, int amt, sqlite3_int64 off) {
    VrFile *p = (VrFile *)f;
    if (sceIoLseek(p->fd, off, SCE_SEEK_SET) != off) return SQLITE_IOERR_WRITE;
    int put = 0;
    while (put < amt) {
        int r = sceIoWrite(p->fd, (const char *)buf + put, amt - put);
        if (r <= 0) return SQLITE_IOERR_WRITE;
        put += r;
    }
    return SQLITE_OK;
}

static int vr_truncate(sqlite3_file *f, sqlite3_int64 size) {
    VrFile *p = (VrFile *)f;
    SceIoStat st;
    memset(&st, 0, sizeof(st));
    st.st_size = size;
    if (IS_ERR(sceIoChstatByFd(p->fd, &st, SCE_CST_SIZE)))
        return SQLITE_IOERR_TRUNCATE;
    return SQLITE_OK;
}

static int vr_sync(sqlite3_file *f, int flags) {
    (void)flags;
    VrFile *p = (VrFile *)f;
    if (IS_ERR(sceIoSyncByFd(p->fd, 0))) return SQLITE_IOERR_FSYNC;
    return SQLITE_OK;
}

static int vr_filesize(sqlite3_file *f, sqlite3_int64 *out) {
    VrFile *p = (VrFile *)f;
    SceOff cur = sceIoLseek(p->fd, 0, SCE_SEEK_CUR);
    SceOff end = sceIoLseek(p->fd, 0, SCE_SEEK_END);
    if (cur >= 0) sceIoLseek(p->fd, cur, SCE_SEEK_SET);
    if (end < 0) return SQLITE_IOERR_FSTAT;
    *out = end;
    return SQLITE_OK;
}

static int vr_lock(sqlite3_file *f, int lvl)   { (void)f; (void)lvl; return SQLITE_OK; }
static int vr_unlock(sqlite3_file *f, int lvl) { (void)f; (void)lvl; return SQLITE_OK; }
static int vr_reserved(sqlite3_file *f, int *out) { (void)f; *out = 0; return SQLITE_OK; }
static int vr_fcntl(sqlite3_file *f, int op, void *arg) {
    (void)f; (void)op; (void)arg;
    return SQLITE_NOTFOUND;
}
static int vr_sector(sqlite3_file *f) { (void)f; return 512; }
static int vr_devchar(sqlite3_file *f) { (void)f; return 0; }

static const sqlite3_io_methods g_methods = {
    1,
    vr_close, vr_read, vr_write, vr_truncate, vr_sync, vr_filesize,
    vr_lock, vr_unlock, vr_reserved, vr_fcntl, vr_sector, vr_devchar,
};

/* ── VFS methods ───────────────────────────────────────────────────────── */

static int vr_open(sqlite3_vfs *vfs, const char *name, sqlite3_file *f,
                   int flags, int *out_flags) {
    (void)vfs;
    VrFile *p = (VrFile *)f;
    p->base.pMethods = NULL;   /* NULL = nothing to close if we fail */
    p->fd = -1;
    /* Temp files and journals are disabled by the caller (MEMORY modes) */
    if (name == NULL) return SQLITE_CANTOPEN;

    int mode;
    if (flags & SQLITE_OPEN_READWRITE) {
        mode = SCE_O_RDWR;
        if (flags & SQLITE_OPEN_CREATE) mode |= SCE_O_CREAT;
    } else {
        mode = SCE_O_RDONLY;
    }
    SceUID fd = sceIoOpen(name, mode, 0666);
    if (IS_ERR(fd)) return SQLITE_CANTOPEN;

    p->fd = fd;
    p->base.pMethods = &g_methods;
    if (out_flags) *out_flags = flags;
    return SQLITE_OK;
}

static int vr_delete(sqlite3_vfs *vfs, const char *name, int sync_dir) {
    (void)vfs; (void)sync_dir;
    SceIoStat st;
    if (sceIoGetstat(name, &st) < 0) return SQLITE_OK;   /* already gone */
    return IS_ERR(sceIoRemove(name)) ? SQLITE_IOERR_DELETE : SQLITE_OK;
}

static int vr_access(sqlite3_vfs *vfs, const char *name, int flags, int *out) {
    (void)vfs; (void)flags;
    SceIoStat st;
    *out = (sceIoGetstat(name, &st) >= 0);
    return SQLITE_OK;
}

static int vr_fullpath(sqlite3_vfs *vfs, const char *name, int n, char *out) {
    (void)vfs;
    /* Vita paths ("ux0:...") are already absolute */
    if ((int)strlen(name) >= n) return SQLITE_CANTOPEN;
    strcpy(out, name);
    return SQLITE_OK;
}

static int vr_random(sqlite3_vfs *vfs, int n, char *out) {
    (void)vfs;
    if (g_sys_vfs && g_sys_vfs->xRandomness)
        return g_sys_vfs->xRandomness(g_sys_vfs, n, out);
    for (int i = 0; i < n; i++) out[i] = (char)rand();
    return n;
}

static int vr_sleep(sqlite3_vfs *vfs, int us) {
    (void)vfs;
    sceKernelDelayThread(us);
    return us;
}

static int vr_time(sqlite3_vfs *vfs, double *out) {
    (void)vfs;
    if (g_sys_vfs && g_sys_vfs->xCurrentTime)
        return g_sys_vfs->xCurrentTime(g_sys_vfs, out);
    *out = 2440587.5;   /* 1970-01-01 as a Julian day; time is not used */
    return SQLITE_OK;
}

static int vr_lasterr(sqlite3_vfs *vfs, int n, char *out) {
    (void)vfs;
    if (n > 0) out[0] = '\0';
    return 0;
}

static void *my_malloc(int n)            { return malloc(n); }
static void *my_realloc(void *p, int n)  { return realloc(p, n); }
static void  my_free(void *p)            { free(p); }

int vita_sqlite_init(void) {
    if (g_ready) return 0;

    if (sceSysmoduleIsLoaded(SCE_SYSMODULE_SQLITE) != 0) {
        int r = sceSysmoduleLoadModule(SCE_SYSMODULE_SQLITE);
        if (r < 0) return r;
    }

    /* Let SQLite use our heap (must happen before the first SQLite call) */
    static SceSqliteMallocMethods mm = { my_malloc, my_realloc, my_free };
    sceSqliteConfigMallocMethods(&mm);

    g_sys_vfs = sqlite3_vfs_find(NULL);

    memset(&g_vfs, 0, sizeof(g_vfs));
    g_vfs.iVersion      = 1;
    g_vfs.szOsFile      = sizeof(VrFile);
    g_vfs.mxPathname    = 256;
    g_vfs.zName         = VR_SQLITE_VFS;
    g_vfs.xOpen         = vr_open;
    g_vfs.xDelete       = vr_delete;
    g_vfs.xAccess       = vr_access;
    g_vfs.xFullPathname = vr_fullpath;
    g_vfs.xRandomness   = vr_random;
    g_vfs.xSleep        = vr_sleep;
    g_vfs.xCurrentTime  = vr_time;
    g_vfs.xGetLastError = vr_lasterr;

    int rc = sqlite3_vfs_register(&g_vfs, 0);   /* 0 = not the default */
    if (rc != SQLITE_OK) return rc;

    g_ready = 1;
    return 0;
}

#endif /* __vita__ */
