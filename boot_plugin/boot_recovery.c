/*
 * boot_recovery.c — Kernel plugin
 * Checks buttons at boot and writes a flag file only.
 * Never attempts to launch anything from kernel space.
 */

#include <psp2kern/kernel/modulemgr.h>
#include <psp2kern/kernel/threadmgr.h>
#include <psp2kern/ctrl.h>
#include <psp2kern/io/fcntl.h>

#define FLAG_RECOVERY  "ur0:tai/recovery_boot_trigger"
#define FLAG_SAFEMODE  "ur0:tai/safemode_boot_trigger"

static void write_flag(const char *path) {
    SceUID fd = ksceIoOpen(path, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
    if (fd >= 0) ksceIoClose(fd);
}

static int recovery_thread(SceSize args, void *argp) {
    unsigned int buttons = 0;
    int i;

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
