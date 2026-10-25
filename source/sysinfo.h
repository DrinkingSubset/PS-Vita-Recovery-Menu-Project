#pragma once

/* Max scroll offset in pixels — caller clamps to this */
#define SYSINFO_MAX_SCROLL  288

void sysinfo_draw(void *font, int scroll_y);
