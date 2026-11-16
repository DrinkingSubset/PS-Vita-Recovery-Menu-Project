#pragma once
/*
 * theme.h - Classic / Retro themes (v1.2.2).
 */

/* Load the saved theme (ux0:data/vita_recovery/theme.cfg) and apply it.
 * Call once after display_init(). Missing file = Classic. */
void theme_load(void);

/* Full screen: pick Classic or a Retro colour, with live preview. */
void theme_screen_run(void);
