#pragma once
/*
 * rain.h - Matrix-style rain behind the retro theme (v1.2.2 themes test).
 */

void rain_set(int on);    /* the user's choice (saved with the theme)   */
int  rain_is_on(void);    /* 1 = on AND a retro theme is active          */
void rain_draw(void);     /* called by display_clear() every frame      */
