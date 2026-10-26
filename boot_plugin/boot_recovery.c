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
        int r = ksceIoRemove(APP_DB);
        pos = put_hex(out, pos, "appdb=", (unsigned int)r);
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

static int recovery_thread(SceSize args, void *argp) {
    unsigned int buttons = 0;
    int i;

    /* Do the database job first, before the home screen opens app.db */
    handle_rebuild_db();

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
