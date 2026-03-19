/*
 * boot_trigger_user.c — Boot recovery user plugin
 *
 * Based on Rinnegatamante's AutoBoot technique.
 * Waits for NPXS10079 to confirm LiveArea is ready, then launches
 * the recovery app via sceAppMgrLaunchAppByUri.
 *
 * Install: ur0:tai/boot_trigger.suprx
 * Config:  *main section
 */

#include <psp2/kernel/modulemgr.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/ctrl.h>
#include <psp2/appmgr.h>
#include <psp2/io/fcntl.h>
#include <string.h>

#define RECOVERY_URI   "psgm:play?titleid=RECM00001"
#define FLAG_SAFEMODE  "ur0:tai/safemode_boot_trigger"

static int g_l_held = 0;

static void write_flag(const char *path) {
    SceUID fd = sceIoOpen(path, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
    if (fd >= 0) sceIoClose(fd);
}

static int boot_trigger_thread(SceSize args, void *argp) {
    SceInt32 appId = 0;
    char appName[12];
    SceUID pid = 0;

    /* Wait for NPXS10079 — launched alongside LiveArea */
    do {
        appId = 0;
        sceAppMgrGetRunningAppIdListForShell(&appId, 1);
        if (appId != 0) {
            pid = sceAppMgrGetProcessIdByAppIdForShell(appId);
            if (pid > 0) {
                memset(appName, 0, sizeof(appName));
                sceAppMgrGetNameById(pid, appName);
                if (strcmp(appName, "NPXS10079") == 0)
                    break;
            }
        }
        sceKernelDelayThread(100000); /* 100ms */
    } while (1);

    if (g_l_held)
        write_flag(FLAG_SAFEMODE);

    /* Launch recovery — retry until accepted */
    while (sceAppMgrLaunchAppByUri(0xFFFFF, RECOVERY_URI) != 0)
        sceKernelDelayThread(100000);

    return sceKernelExitDeleteThread(0);
}

void _start() __attribute__((weak, alias("module_start")));

int module_start(SceSize argc, const void *args) {
    (void)argc; (void)args;

    SceCtrlData pad;
    unsigned int buttons = 0;
    int i;

    for (i = 0; i < 20; i++) {
        pad.buttons = 0;
        sceCtrlPeekBufferPositive(0, &pad, 1);
        buttons |= pad.buttons;
        sceKernelDelayThread(5000);
    }

    if (!(buttons & (SCE_CTRL_RTRIGGER | SCE_CTRL_LTRIGGER)))
        return SCE_KERNEL_START_SUCCESS;

    g_l_held = (buttons & SCE_CTRL_LTRIGGER) != 0;

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
