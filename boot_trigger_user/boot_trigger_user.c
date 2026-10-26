/*
 * boot_trigger_user.c - Boot recovery user plugin (loaded in *main / SceShell)
 *
 * Based on Rinnegatamante's AutoBoot technique.
 * Waits for NPXS10079 to confirm LiveArea is ready, then launches
 * the recovery app via sceAppMgrLaunchAppByUri.
 *
 * Install: ur0:tai/boot_trigger.suprx (or the active tai folder)
 * Config:  *main section
 *
 * v1.1.7 changes (code review findings 1 and 2):
 *  - Launch when the kernel plugin's flag ur0:tai/recovery_boot_trigger
 *    exists (R or L held during early boot), OR when R/L is held right now.
 *    Before, only a ~100 ms button check at *main load was used, so holding
 *    R only at power-on could miss the window.
 *  - Every wait has a time limit. Before, the NPXS10079 wait and the launch
 *    retry looped forever, which could leave a thread spinning in SceShell
 *    (a likely cause of the custom boot splash + R boot loop).
 *  - Loop breaker: ur0:tai/recovery_launch_pending counts launch attempts
 *    that did not reach the app (main.c deletes it on start). After 2 such
 *    boots the plugin stops auto-launching and clears the flags.
 */

#include <psp2/kernel/modulemgr.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/ctrl.h>
#include <psp2/appmgr.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <string.h>

#define RECOVERY_URI    "psgm:play?titleid=RECM00001"
#define FLAG_RECOVERY   "ur0:tai/recovery_boot_trigger"
#define FLAG_SAFEMODE   "ur0:tai/safemode_boot_trigger"
#define FLAG_PENDING    "ur0:tai/recovery_launch_pending"

#define SHELL_WAIT_STEPS   600   /* x 100 ms = 60 s for LiveArea to appear */
#define LAUNCH_TRY_STEPS   150   /* x 100 ms = 15 s of launch retries      */
#define MAX_FAILED_LAUNCH  2     /* boots that never reached the app       */

static int g_l_held = 0;

static int file_exists(const char *path) {
    SceIoStat st;
    return sceIoGetstat(path, &st) >= 0;
}

static void write_flag(const char *path) {
    SceUID fd = sceIoOpen(path, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
    if (fd >= 0) sceIoClose(fd);
}

/* Read the pending-launch counter (0 if missing) */
static int read_pending(void) {
    SceUID fd = sceIoOpen(FLAG_PENDING, SCE_O_RDONLY, 0);
    if (fd < 0) return 0;
    char c = '0';
    sceIoRead(fd, &c, 1);
    sceIoClose(fd);
    return (c >= '0' && c <= '9') ? c - '0' : MAX_FAILED_LAUNCH;
}

static void write_pending(int n) {
    SceUID fd = sceIoOpen(FLAG_PENDING, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
    if (fd < 0) return;
    char c = (char)('0' + (n > 9 ? 9 : n));
    sceIoWrite(fd, &c, 1);
    sceIoClose(fd);
}

static int boot_trigger_thread(SceSize args, void *argp) {
    (void)args; (void)argp;
    SceInt32 appId = 0;
    char appName[12];
    SceUID pid = 0;
    int ready = 0;

    /* Wait (with a limit) for NPXS10079 - launched alongside LiveArea */
    for (int i = 0; i < SHELL_WAIT_STEPS; i++) {
        appId = 0;
        sceAppMgrGetRunningAppIdListForShell(&appId, 1);
        if (appId != 0) {
            pid = sceAppMgrGetProcessIdByAppIdForShell(appId);
            if (pid > 0) {
                memset(appName, 0, sizeof(appName));
                sceAppMgrGetNameById(pid, appName);
                if (strcmp(appName, "NPXS10079") == 0) { ready = 1; break; }
            }
        }
        sceKernelDelayThread(100000); /* 100 ms */
    }
    /* LiveArea never showed up: still try a few launches below; if they
     * fail too, the thread simply ends. Nothing spins forever. */
    (void)ready;

    if (g_l_held)
        write_flag(FLAG_SAFEMODE);

    for (int i = 0; i < LAUNCH_TRY_STEPS; i++) {
        if (sceAppMgrLaunchAppByUri(0xFFFFF, RECOVERY_URI) == 0)
            break;
        sceKernelDelayThread(100000);
    }

    return sceKernelExitDeleteThread(0);
}

void _start() __attribute__((weak, alias("module_start")));

int module_start(SceSize argc, const void *args) {
    (void)argc; (void)args;

    /* 1. Kernel flag from early boot (the reliable path) */
    int want = file_exists(FLAG_RECOVERY);
    g_l_held = file_exists(FLAG_SAFEMODE);

    /* 2. Buttons held right now (fallback, e.g. kernel plugin missing) */
    SceCtrlData pad;
    unsigned int buttons = 0;
    for (int i = 0; i < 20; i++) {
        pad.buttons = 0;
        sceCtrlPeekBufferPositive(0, &pad, 1);
        buttons |= pad.buttons;
        sceKernelDelayThread(5000);
    }
    if (buttons & (SCE_CTRL_RTRIGGER | SCE_CTRL_LTRIGGER)) {
        want = 1;
        if (buttons & SCE_CTRL_LTRIGGER) g_l_held = 1;
    }

    if (!want)
        return SCE_KERNEL_START_SUCCESS;

    /* 3. Loop breaker: if the last boots asked for recovery but the app
     *    never started, stop auto-launching and clear everything so the
     *    console boots normally. */
    int pending = read_pending();
    if (pending >= MAX_FAILED_LAUNCH) {
        sceIoRemove(FLAG_PENDING);
        sceIoRemove(FLAG_RECOVERY);
        sceIoRemove(FLAG_SAFEMODE);
        return SCE_KERNEL_START_SUCCESS;
    }
    write_pending(pending + 1);

    SceUID thid = sceKernelCreateThread(
        "boot_trigger",
        (SceKernelThreadEntry)boot_trigger_thread,
        0x40, 0x100000,
        0, 0x00070000, NULL
    );
    if (thid >= 0)
        sceKernelStartThread(thid, 0, NULL);

    return SCE_KERNEL_START_SUCCESS;
}

int module_stop(SceSize argc, const void *args) {
    (void)argc; (void)args;
    return SCE_KERNEL_STOP_SUCCESS;
}
