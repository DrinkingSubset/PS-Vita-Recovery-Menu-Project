#pragma once


/*
 * cheat_manager.h — Unified cheat & trainer manager
 *
 * Two systems in one screen:
 *
 * A) VITA native cheats  (.psv format, used by VitaCheat / FinalCheat)
 *    Files: ux0:data/vitacheat/cheats/TITLEID.psv
 *    Format:
 *      [PCSE00123]
 *      $Cheat Name
 *      V0 20AABBCC 000F423F   <- V0=disabled, V1=enabled
 *
 * B) PSP CWCheat codes  (.db format, used by CWCheat inside Adrenaline)
 *    File:  ux0:pspemu/seplugins/cwcheat/CHEAT.db
 *    Format:
 *      _S GAMEID-00000
 *      _G Game Title
 *      _C0 Cheat Name         <- _C0=disabled, _C1=enabled
 *      _L 0xADDRESS 0xVALUE
 *
 * All changes are written to disk — no live memory patching.
 * Plugins (VitaCheat / CWCheat) apply them on the next game launch.
 */

void cheat_manager_screen_run(void *font);
