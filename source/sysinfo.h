#pragma once

/* Max scroll offset in pixels — caller clamps to this */
#define SYSINFO_MAX_SCROLL  440   /* v1.1.9: + Drive access section */

void sysinfo_draw(void *font, int scroll_y);
