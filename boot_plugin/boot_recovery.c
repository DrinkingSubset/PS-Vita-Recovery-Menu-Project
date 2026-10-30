/*
 * boot_recovery.c - Kernel plugin
 * Checks buttons at boot and writes a flag file only.
 * Never attempts to launch anything from kernel space.
 *
 * v1.1.6: also carries out "Rebuild Database" requested from the app.
 * The app is a safe homebrew and cannot see ur0:shell, so it only writes
 * ur0:tai/rebuild_db_trigger. At the next boot, before the home screen
 * starts, this plugin deletes ur0:shell/db/app.db so the system rebuilds
 * it, and writes the result to ur0:tai/rebuild_db_result.txt.
 * v1.1.7: app.db is renamed to app.db.bak (kept) instead of deleted.
 * v1.2.0: also swaps in a restored home screen layout (app.db.new) that
 *         the app staged, keeping the old database as app.db.prelayout.
 */

#include <psp2kern/kernel/modulemgr.h>
#include <psp2kern/kernel/threadmgr.h>
#include <psp2kern/ctrl.h>
#include <psp2kern/io/fcntl.h>
#include <psp2kern/io/stat.h>
#include <psp2kern/kernel/threadmgr/misc.h>

#define FLAG_RECOVERY  "ur0:tai/recovery_boot_trigger"
#define FLAG_SAFEMODE  "ur0:tai/safemode_boot_trigger"
#define FLAG_REBUILD   "ur0:tai/rebuild_db_trigger"
#define REBUILD_RESULT "ur0:tai/rebuild_db_result.txt"
#define APP_DB         "ur0:shell/db/app.db"
#define APP_DB_BAK     "ur0:shell/db/app.db.bak"   /* v1.1.7: kept, not deleted */
#define FLAG_LAYOUT    "ur0:tai/layout_restore_trigger"     /* v1.2.0 */
#define LAYOUT_RESULT  "ur0:tai/layout_restore_result.txt"
#define APP_DB_NEW     "ur0:shell/db/app.db.new"
#define APP_DB_PRE     "ur0:shell/db/app.db.prelayout"

/* Only touch app.db this early after power-on. With Enso, kernel plugins
 * load within a few seconds of boot, before the home screen opens the
 * database. With h-encore they load much later, while the home screen is
 * already using app.db, so the delete is skipped there. */
#define REBUILD_MAX_BOOT_US  (45LL * 1000 * 1000)

static void write_flag(const char *path) {
    SceUID fd = ksceIoOpen(path, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
    if (fd >= 0) ksceIoClose(fd);
}

/* Append "<label><8 hex digits>\n" to buf (no libc in this module) */
static int put_hex(char *buf, int pos, const char *label, unsigned int v) {
    static const char hx[] = "0123456789ABCDEF";
    while (*label) buf[pos++] = *label++;
    for (int i = 7; i >= 0; i--) buf[pos++] = hx[(v >> (i * 4)) & 0xF];
    buf[pos++] = '\n';
    return pos;
}

static void handle_rebuild_db(void) {
    SceIoStat st;
    if (ksceIoGetstat(FLAG_REBUILD, &st) < 0) return;   /* not requested */

    /* Consume the request FIRST so a problem can never repeat every boot */
    ksceIoRemove(FLAG_REBUILD);

    char out[128];
    int pos = 0;
    SceInt64 t = ksceKernelGetSystemTimeWide();
    if (t > REBUILD_MAX_BOOT_US) {
        /* Not early boot (h-encore / plugins reloaded): do nothing */
        /* t >> 20 is roughly seconds; avoids 64-bit division (no libgcc) */
        pos = put_hex(out, pos, "skipped=", (unsigned int)(t >> 20));
    } else {
        /* v1.1.7: RENAME instead of delete, so the old database is kept
         * as app.db.bak (same partition, no extra space, instant). The
         * system still sees no app.db and rebuilds it. One previous
         * backup is kept; an older one is replaced. */
        ksceIoRemove(APP_DB_BAK);
        int r = ksceIoRename(APP_DB, APP_DB_BAK);
        int bak = (r >= 0);
        if (r < 0)
            r = ksceIoRemove(APP_DB);   /* rename refused: plain delete */
        pos = put_hex(out, pos, "appdb=", (unsigned int)(r < 0 ? r : 0));
        pos = put_hex(out, pos, "bak=", (unsigned int)bak);
        /* Leftover SQLite side files; missing files are fine */
        ksceIoRemove(APP_DB "-journal");
        ksceIoRemove(APP_DB "-wal");
        ksceIoRemove(APP_DB "-shm");
    }

    SceUID fd = ksceIoOpen(REBUILD_RESULT, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
    if (fd >= 0) {
        ksceIoWrite(fd, out, pos);
        ksceIoClose(fd);
    }
}

/* v1.2.0: Home Screen Layout restore. The app stages the finished database
 * as app.db.new and writes FLAG_LAYOUT, then restarts right away. Here,
 * before the home screen opens app.db, the files are swapped:
 *     app.db      -> app.db.prelayout   (kept)
 *     app.db.new  -> app.db
 * If anything fails, the original app.db is put back. */
static void write_result(const char *path, const char *buf, int len) {
    SceUID fd = ksceIoOpen(path, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
    if (fd >= 0) {
        ksceIoWrite(fd, buf, len);
        ksceIoClose(fd);
    }
}

static void handle_layout_restore(void) {
    SceIoStat st;
    if (ksceIoGetstat(FLAG_LAYOUT, &st) < 0) return;   /* not requested */

    /* Consume the request FIRST so a problem can never repeat every boot */
    ksceIoRemove(FLAG_LAYOUT);

    char out[128];
    int pos = 0;
    SceInt64 t = ksceKernelGetSystemTimeWide();

    if (ksceIoGetstat(FLAG_REBUILD, &st) >= 0) {
        /* A rebuild is also requested: the staged layout was made from the
         * database that is about to be thrown away. The rebuild wins. */
        ksceIoRemove(APP_DB_NEW);
        pos = put_hex(out, pos, "conflict=", 1);
    } else if (t > REBUILD_MAX_BOOT_US) {
        /* Not early boot: the home screen may already use app.db */
        ksceIoRemove(APP_DB_NEW);
        pos = put_hex(out, pos, "skipped=", (unsigned int)(t >> 20));
    } else {
        int r = ksceIoGetstat(APP_DB_NEW, &st);
        if (r >= 0) {
            ksceIoRemove(APP_DB_PRE);
            ksceIoRemove(APP_DB_PRE "-journal");
            /* A leftover journal belongs to the OLD database: keep it with
             * it, or SQLite would apply it to the new file. */
            if (ksceIoGetstat(APP_DB "-journal", &st) >= 0)
                ksceIoRename(APP_DB "-journal", APP_DB_PRE "-journal");
            r = ksceIoRename(APP_DB, APP_DB_PRE);
            if (r >= 0) {
                r = ksceIoRename(APP_DB_NEW, APP_DB);
                if (r < 0) {
                    /* put everything back as it was */
                    ksceIoRename(APP_DB_PRE, APP_DB);
                    if (ksceIoGetstat(APP_DB_PRE "-journal", &st) >= 0)
                        ksceIoRename(APP_DB_PRE "-journal", APP_DB "-journal");
                }
            } else if (ksceIoGetstat(APP_DB_PRE "-journal", &st) >= 0) {
                ksceIoRename(APP_DB_PRE "-journal", APP_DB "-journal");
            }
        }
        ksceIoRemove(APP_DB_NEW);   /* never left behind */
        pos = put_hex(out, pos, "layout=", (unsigned int)(r < 0 ? r : 0));
    }
    write_result(LAYOUT_RESULT, out, pos);
}

static int recovery_thread(SceSize args, void *argp) {
    unsigned int buttons = 0;
    int i;

    /* Do the database jobs first, before the home screen opens app.db.
     * Layout first: it checks for a pending rebuild itself. */
    handle_layout_restore();
    handle_rebuild_db();

    /* v1.1.7: clear flags left over from an earlier boot (for example the
     * app never started), so only a button held THIS boot triggers recovery.
     * The user plugin in SceShell reads these flags later in boot. */
    ksceIoRemove(FLAG_RECOVERY);
    ksceIoRemove(FLAG_SAFEMODE);

    /* Poll up to 5 seconds for ctrl to be ready */
    for (i = 0; i < 500; i++) {
        SceCtrlData pad;
        pad.buttons = 0;
        if (ksceCtrlPeekBufferPositive(0, &pad, 1) >= 0) {
            buttons = pad.buttons;
            if (buttons & (SCE_CTRL_RTRIGGER | SCE_CTRL_LTRIGGER))
                break;
        }
        ksceKernelDelayThread(10000); /* 10ms */
    }

    /* Write flag files only — never launch from kernel */
    if (buttons & (SCE_CTRL_RTRIGGER | SCE_CTRL_LTRIGGER)) {
        write_flag(FLAG_RECOVERY);
        if (buttons & SCE_CTRL_LTRIGGER)
            write_flag(FLAG_SAFEMODE);
    }

    return ksceKernelExitDeleteThread(0);
}

void _start() __attribute__((weak, alias("module_start")));

int module_start(SceSize argc, const void *args) {
    (void)argc; (void)args;

    SceUID thid = ksceKernelCreateThread(
        "recovery_check",
        (SceKernelThreadEntry)recovery_thread,
        0x3C, 0x1000, 0, 0, NULL
    );
    if (thid >= 0)
        ksceKernelStartThread(thid, 0, NULL);

    return SCE_KERNEL_START_SUCCESS;
}

int module_stop(SceSize argc, const void *args) {
    (void)argc; (void)args;
    return SCE_KERNEL_STOP_SUCCESS;
}
