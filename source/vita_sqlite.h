#pragma once
/*
 * vita_sqlite.h - the small part of the SQLite API this app uses (v1.2.0).
 *
 * On the Vita the functions come from the system's own SQLite module
 * (SceSqlite, linked with SceSqlite_stub, loaded with SCE_SYSMODULE_SQLITE).
 * vitasdk ships no sqlite3.h, so the declarations are here. They follow the
 * public SQLite C API, which is public domain.
 *
 * On a PC (host tests) the real <sqlite3.h> is used instead.
 */

#ifndef __vita__
#include <sqlite3.h>
#else

#ifdef __cplusplus
extern "C" {
#endif

#define SQLITE_OK           0
#define SQLITE_ERROR        1
#define SQLITE_PERM         3
#define SQLITE_BUSY         5
#define SQLITE_NOMEM        7
#define SQLITE_READONLY     8
#define SQLITE_IOERR       10
#define SQLITE_CORRUPT     11
#define SQLITE_NOTFOUND    12
#define SQLITE_FULL        13
#define SQLITE_CANTOPEN    14
#define SQLITE_CONSTRAINT  19
#define SQLITE_ROW        100
#define SQLITE_DONE       101

#define SQLITE_IOERR_READ        (SQLITE_IOERR | (1 << 8))
#define SQLITE_IOERR_SHORT_READ  (SQLITE_IOERR | (2 << 8))
#define SQLITE_IOERR_WRITE       (SQLITE_IOERR | (3 << 8))
#define SQLITE_IOERR_FSYNC       (SQLITE_IOERR | (4 << 8))
#define SQLITE_IOERR_TRUNCATE    (SQLITE_IOERR | (6 << 8))
#define SQLITE_IOERR_FSTAT       (SQLITE_IOERR | (7 << 8))
#define SQLITE_IOERR_DELETE      (SQLITE_IOERR | (10 << 8))

#define SQLITE_OPEN_READONLY   0x00000001
#define SQLITE_OPEN_READWRITE  0x00000002
#define SQLITE_OPEN_CREATE     0x00000004
#define SQLITE_OPEN_MAIN_DB    0x00000100

#define SQLITE_INTEGER  1
#define SQLITE_FLOAT    2
#define SQLITE_TEXT     3
#define SQLITE_BLOB     4
#define SQLITE_NULL     5

typedef long long int sqlite3_int64;
typedef unsigned long long int sqlite3_uint64;
typedef sqlite3_int64 sqlite_int64;

typedef void (*sqlite3_destructor_type)(void *);
#define SQLITE_STATIC     ((sqlite3_destructor_type)0)
#define SQLITE_TRANSIENT  ((sqlite3_destructor_type)-1)

typedef struct sqlite3 sqlite3;
typedef struct sqlite3_stmt sqlite3_stmt;
typedef struct sqlite3_value sqlite3_value;

typedef struct sqlite3_file sqlite3_file;
struct sqlite3_file {
    const struct sqlite3_io_methods *pMethods;
};

typedef struct sqlite3_io_methods sqlite3_io_methods;
struct sqlite3_io_methods {
    int iVersion;
    int (*xClose)(sqlite3_file *);
    int (*xRead)(sqlite3_file *, void *, int, sqlite3_int64);
    int (*xWrite)(sqlite3_file *, const void *, int, sqlite3_int64);
    int (*xTruncate)(sqlite3_file *, sqlite3_int64);
    int (*xSync)(sqlite3_file *, int);
    int (*xFileSize)(sqlite3_file *, sqlite3_int64 *);
    int (*xLock)(sqlite3_file *, int);
    int (*xUnlock)(sqlite3_file *, int);
    int (*xCheckReservedLock)(sqlite3_file *, int *);
    int (*xFileControl)(sqlite3_file *, int, void *);
    int (*xSectorSize)(sqlite3_file *);
    int (*xDeviceCharacteristics)(sqlite3_file *);
    /* version 2+ members are not used (iVersion = 1) */
};

typedef struct sqlite3_vfs sqlite3_vfs;
struct sqlite3_vfs {
    int iVersion;
    int szOsFile;
    int mxPathname;
    sqlite3_vfs *pNext;
    const char *zName;
    void *pAppData;
    int (*xOpen)(sqlite3_vfs *, const char *, sqlite3_file *, int, int *);
    int (*xDelete)(sqlite3_vfs *, const char *, int);
    int (*xAccess)(sqlite3_vfs *, const char *, int, int *);
    int (*xFullPathname)(sqlite3_vfs *, const char *, int, char *);
    void *(*xDlOpen)(sqlite3_vfs *, const char *);
    void (*xDlError)(sqlite3_vfs *, int, char *);
    void (*(*xDlSym)(sqlite3_vfs *, void *, const char *))(void);
    void (*xDlClose)(sqlite3_vfs *, void *);
    int (*xRandomness)(sqlite3_vfs *, int, char *);
    int (*xSleep)(sqlite3_vfs *, int);
    int (*xCurrentTime)(sqlite3_vfs *, double *);
    int (*xGetLastError)(sqlite3_vfs *, int, char *);
    /* version 2+ members are not used (iVersion = 1) */
};

const char *sqlite3_libversion(void);
int  sqlite3_initialize(void);
int  sqlite3_open_v2(const char *, sqlite3 **, int, const char *);
int  sqlite3_close(sqlite3 *);
int  sqlite3_exec(sqlite3 *, const char *, int (*)(void *, int, char **, char **),
                  void *, char **);
const char *sqlite3_errmsg(sqlite3 *);
void sqlite3_free(void *);
int  sqlite3_prepare_v2(sqlite3 *, const char *, int, sqlite3_stmt **, const char **);
int  sqlite3_step(sqlite3_stmt *);
int  sqlite3_reset(sqlite3_stmt *);
int  sqlite3_finalize(sqlite3_stmt *);
int  sqlite3_clear_bindings(sqlite3_stmt *);
int  sqlite3_bind_int(sqlite3_stmt *, int, int);
int  sqlite3_bind_int64(sqlite3_stmt *, int, sqlite3_int64);
int  sqlite3_bind_text(sqlite3_stmt *, int, const char *, int, void (*)(void *));
int  sqlite3_bind_value(sqlite3_stmt *, int, const sqlite3_value *);
int  sqlite3_column_int(sqlite3_stmt *, int);
sqlite3_int64 sqlite3_column_int64(sqlite3_stmt *, int);
const unsigned char *sqlite3_column_text(sqlite3_stmt *, int);
int  sqlite3_column_type(sqlite3_stmt *, int);
sqlite3_value *sqlite3_column_value(sqlite3_stmt *, int);
sqlite3_int64 sqlite3_last_insert_rowid(sqlite3 *);
int  sqlite3_changes(sqlite3 *);
sqlite3_vfs *sqlite3_vfs_find(const char *);
int  sqlite3_vfs_register(sqlite3_vfs *, int);
int  sqlite3_vfs_unregister(sqlite3_vfs *);

#ifdef __cplusplus
}
#endif

#endif /* __vita__ */

/* Name of the read/write file layer registered by vita_sqlite_init().
 * NULL on a PC (the normal SQLite file layer is used there). */
#ifdef __vita__
#define VR_SQLITE_VFS "vr_rw"
#else
#define VR_SQLITE_VFS NULL
#endif

/* Load the system SQLite module and register the read/write file layer.
 * Safe to call more than once. Returns 0 or a negative/SQLite error. */
int vita_sqlite_init(void);
