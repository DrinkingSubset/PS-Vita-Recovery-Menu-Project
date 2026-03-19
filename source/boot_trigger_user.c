/*
 * boot_trigger_user.c — User-space plugin (suprx)
 * Loaded by taiHEN for the SceShell process.
 * Checks for the flag file written by boot_recovery.skprx and
 * launches the recovery app from user space where AppMgr is ready.
 */

#include <psp2/kernel/modulemgr.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/appmgr.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/threadmgr.h>

#define FLAG_RECOVERY  "ur0:tai/recovery_boot_trigger"
#define FLAG_SAFEMODE  "ur0:tai/safemode_boot_trigger"
#define RECOVERY_URI   "psgm:play?titleid=RECM00001"

static int flag_exists(const char *path) {
    SceIoStat st;
    return sceIoGetstat(path, &st) >= 0;
}

static int launch_thread(SceSize args, void *argp) {
    /* Small delay to ensure SceShell and AppMgr are fully ready */
    sceKernelDelayThread(2000000); /* 2 seconds */

    if (flag_exists(FLAG_RECOVERY)) {
        sceIoRemove(FLAG_RECOVERY);
        sceIoRemove(FLAG_SAFEMODE); /* ok if missing */
        sceAppMgrLaunchAppByUri(0x20000, RECOVERY_URI);
    }

    return sceKernelExitDeleteThread(0);
}

void _start() __attribute__((weak, alias("module_start")));

int module_start(SceSize argc, const void *args) {
    (void)argc; (void)args;

    /* Only act if flag exists — otherwise zero overhead */
    if (!flag_exists(FLAG_RECOVERY))
        return SCE_KERNEL_START_SUCCESS;

    SceUID thid = sceKernelCreateThread(
        "boot_trigger",
        (SceKernelThreadEntry)launch_thread,
        0x40, 0x1000, 0, 0x00070000, NULL
    );
    if (thid >= 0)
        sceKernelStartThread(thid, 0, NULL);

    return SCE_KERNEL_START_SUCCESS;
}

int module_stop(SceSize argc, const void *args) {
    (void)argc; (void)args;
    return SCE_KERNEL_STOP_SUCCESS;
}
