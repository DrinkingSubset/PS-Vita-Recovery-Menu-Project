#include <psp2/kernel/processmgr.h>
#include <psp2/ctrl.h>
#include <psp2/power.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/registrymgr.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "display.h"
#include "menu.h"
#include "compat.h"
#include "plugins.h"
#include "cpu.h"

#define FLAG_RECOVERY  "ur0:tai/recovery_boot_trigger"
#define FLAG_SAFEMODE  "ur0:tai/safemode_boot_trigger"

static int flag_exists(const char *path) {
    SceIoStat st;
    return sceIoGetstat(path, &st) >= 0;
}

int main(int argc, char *argv[]) {
    display_init();
    sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG_WIDE);
    compat_init();
    cpu_load_and_apply();   /* restore saved clock settings */

    if (g_compat.fw_tier == FW_367_HENCORE ||
        g_compat.fw_tier == FW_368_HENCORE) {
        sceIoMkdir("ur0:tai", 0777);
    }

    /* ── Check for boot trigger flags ──────────────────────────────────────
     *
     * recovery_boot_trigger: set by boot plugin when R or L was held.
     *   Just a marker — the menu opens normally.
     *
     * safemode_boot_trigger: set by boot plugin when L was held.
     *   Auto-apply Safe Mode (disable non-essential plugins) before
     *   opening the menu, then show a banner confirming it.
     */
    int booted_via_recovery = flag_exists(FLAG_RECOVERY);
    int booted_via_safemode = flag_exists(FLAG_SAFEMODE);

    /* Consume both flags immediately so they don't persist */
    if (booted_via_recovery) sceIoRemove(FLAG_RECOVERY);
    if (booted_via_safemode) sceIoRemove(FLAG_SAFEMODE);

    if (booted_via_safemode) {
        /* Auto-apply Safe Mode: load config and disable non-essential plugins */
        plugins_load();
        int disabled = plugins_apply_safe_mode();
        if (disabled >= 0) g_safemode_applied = 1;
    }

    menu_run();

    sceKernelExitProcess(0);
    return 0;
}
