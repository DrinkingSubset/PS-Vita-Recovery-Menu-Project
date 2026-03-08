#pragma once


/*
 * sd2vita.h — SD2Vita microSD adapter management
 *
 * StorageMgr config at ur0:tai/storage_config.txt (or ux0:tai/ fallback):
 *   GCD=<mp>   Game Card Device  = SD2Vita  (usually ux0 or uma0)
 *   MCD=<mp>   Memory Card       = Sony card
 *   INT=<mp>   Internal flash    = imc0 (Vita 2000 / PSTV only)
 *   UMA=<mp>   Extra / USB       = grw0
 */

void sd2vita_screen_run(void *font);
