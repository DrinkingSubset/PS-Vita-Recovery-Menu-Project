#pragma once
#include <stdint.h>

/* Pass as bg color to skip drawing background pixels (text over colored bars) */
#define PSV_TRANSPARENT 0x00000000u

void psvDebugScreenInit(void);
void psvDebugScreenClear(uint32_t color);
void psvDebugScreenSetFgColor(uint32_t color);
void psvDebugScreenSetBgColor(uint32_t color);
void psvDebugScreenSetCoordsXY(int x, int y);
void psvDebugScreenFlip(void);
void psvDebugScreenFillRect(int x, int y, int w, int h, uint32_t color);
int  psvDebugScreenPrintf(const char *fmt, ...);
