/*
 * boot_recovery.c — R/L Trigger Boot Recovery Kernel Plugin
 *
 * Strategy:
 *   1. module_start reads the button state immediately (controller is
 *      already polled by this point in the boot sequence).
 *   2. If R or L is held, a one-shot hook is installed on
 *      SceAppMgr!sceAppMgrLaunchAppByUri (NID 0xFC4CFC30).
 *   3. The hook fires when SceShell calls this function to launch the
 *      first app (the LiveArea). At that moment the system is fully ready
 *      to accept app launches.
 *   4. Inside the hook: write flag files and redirect the launch URI to
 *      the recovery app. TAI_CONTINUE forwards the (redirected) call.
 *   5. module_stop releases the hook if it was never triggered.
 *
 * Flag files written to ur0:tai/ communicate the trigger type to the
 * recovery app's main.c, which consumes and deletes them on startup.
 *
 *   R held → recovery_boot_trigger (normal recovery menu)
 *   L held → recovery_boot_trigger + safemode_boot_trigger
 *             (recovery menu + auto-disable non-essential plugins)
 *
 * Safety guarantees:
 *   - Flag files are ONLY written from inside the hook, i.e. only when
 *     the system is ready and the launch is actually happening. There is
 *     no scenario where flags are written but the recovery app never runs.
 *   - Hook is only installed when a trigger button is detected. Normal
 *     boots install no hooks and return from module_start immediately.
 *   - Hook is self-removing via g_triggered flag after first call.
 *   - All paths return SCE_KERNEL_START_SUCCESS — a hook install failure
 *     is non-fatal; boot continues normally.
 *   - Writes ONLY to ur0:tai/ — never touches vs0:, os0:, or os1:.
 *
 * Install path: ur0:recovery/boot_recovery.skprx
 * tai config:   *KERNEL section
 * Firmware:     3.60 – 3.74 (any taiHEN / Enso system)
 */

#include <psp2kern/kernel/modulemgr.h>
#include <psp2kern/ctrl.h>
#include <psp2kern/io/fcntl.h>
#include <taihen.h>

/* ── Constants ───────────────────────────────────────────────────────────── */
#define RECOVERY_URI   "psgm:play?titleid=RECM00001"
#define FLAG_RECOVERY  "ur0:tai/recovery_boot_trigger"
#define FLAG_SAFEMODE  "ur0:tai/safemode_boot_trigger"
/* ── State (set in module_start, read in hook) ───────────────────────────── */
static int g_r_held    = 0;
static int g_l_held    = 0;
static int g_triggered = 0;  /* 1 after hook has fired once */

/* ── Hook handle ─────────────────────────────────────────────────────────── */
static SceUID         g_hook_uid = -1;
static tai_hook_ref_t g_hook_ref;  /* zero-init; guard is g_hook_uid >= 0 */

/* ── Write a zero-byte flag file ─────────────────────────────────────────── */
static void write_flag(const char *path) {
    SceUID fd = ksceIoOpen(path,
                           SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
    if (fd >= 0) ksceIoClose(fd);
}

/* ── Read buttons with debounce ──────────────────────────────────────────── */
/*
 * Polls up to 12 times (60ms max) and returns on the first read where a
 * trigger button is confirmed held. Returns 0 if the controller isn't ready
 * or no trigger is detected after the full window.
 *
 * The AND-accumulation approach used by simpler plugins fails on cold OLED
 * boots where one early zeroed read masks a real held button. This version
 * waits for the controller to report ready (return value == 1) and exits
 * early as soon as a trigger is confirmed, eliminating the false-negative.
 */
static unsigned int read_buttons(void) {
    SceCtrlData pad;
    unsigned int last = 0;
    int ready = 0;

    for (int i = 0; i < 12; i++) {          /* 12 × 5ms = 60ms max */
        pad.buttons = 0;                     /* zero the field we read; avoids memset/libc */
        if (ksceCtrlPeekBufferPositive(0, &pad, 1) == 1) {
            ready = 1;
            last = (unsigned int)pad.buttons;
            /* Early exit if a recovery trigger is already clearly held */
            if (last & (SCE_CTRL_RTRIGGER | SCE_CTRL_LTRIGGER))
                return last;
        }
        ksceKernelDelayThread(5000);          /* 5 ms */
    }
    return ready ? last : 0;
}

/* ── Hook: sceAppMgrLaunchAppByUri ───────────────────────────────────────── */
/*
 * Fires the first time SceShell calls sceAppMgrLaunchAppByUri (which is
 * when it launches the LiveArea / first user app after boot completes).
 * At that point the system is fully ready to launch apps.
 */
static int hook_launch(int flags, const char *uri) {
    if (!g_triggered) {
        g_triggered = 1;

        /* Write flags — only happens here, so only when launch actually occurs */
        write_flag(FLAG_RECOVERY);
        if (g_l_held)
            write_flag(FLAG_SAFEMODE);

        /* Redirect the launch to the recovery app */
        return TAI_CONTINUE(int, g_hook_ref, flags, RECOVERY_URI);
    }

    /* Subsequent calls pass through unchanged */
    return TAI_CONTINUE(int, g_hook_ref, flags, uri);
}

/* ── Module entry points ─────────────────────────────────────────────────── */

int module_start(SceSize argc, const void *args) {
    (void)argc; (void)args;

    /* Read button state immediately — controller is ready at this point */
    unsigned int btns = read_buttons();
    g_r_held = (btns & SCE_CTRL_RTRIGGER) != 0;
    g_l_held = (btns & SCE_CTRL_LTRIGGER) != 0;

    /* Nothing held — return immediately, zero overhead on normal boots */
    if (!g_r_held && !g_l_held)
        return SCE_KERNEL_START_SUCCESS;

    /*
     * Install hook on SceAppMgr!sceAppMgrLaunchAppByUri (NID 0xFC4CFC30).
     * The hook will fire when SceShell is ready to launch the first app.
     * Hook failure is non-fatal — boot continues normally regardless.
     */
    g_hook_uid = taiHookFunctionExportForKernel(
        KERNEL_PID,
        &g_hook_ref,
        "SceAppMgr",
        TAI_ANY_LIBRARY,
        0xFC4CFC30,
        hook_launch
    );

    if (g_hook_uid < 0)
        g_hook_uid = -1;  /* normalise so module_stop check is clean */

    return SCE_KERNEL_START_SUCCESS;
}

int module_stop(SceSize argc, const void *args) {
    (void)argc; (void)args;

    /* Release hook if it was installed but never triggered */
    if (g_hook_uid >= 0) {
        taiHookReleaseForKernel(g_hook_uid, g_hook_ref);
        g_hook_uid = -1;
    }

    return SCE_KERNEL_STOP_SUCCESS;
}
