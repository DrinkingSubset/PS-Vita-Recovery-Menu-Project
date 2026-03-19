#include <stdio.h>
#include "display.h"
#include "draw.h"
#include "menu.h"

/* ── Layout ──────────────────────────────────────────────────────────────────
 * Font: 8x8px at 2x scale = 16px per char. Screen: 960x544.
 * Matches the original comfortable sizing.
 */
#define CONTENT_TOP   46    /* y where items start, below 2-line title bar */
#define CONTENT_BOT   (SCREEN_H - 30)
#define CONTENT_H     (CONTENT_BOT - CONTENT_TOP)

#define H_ITEM        LINE_H   /* 24px per normal item */
#define H_SEP          8       /* separator */

static int item_height(MenuItemType t) {
    return (t == ITEM_SEPARATOR) ? H_SEP : H_ITEM;
}

static int range_height(Menu *menu, int from, int to) {
    int h = 0;
    for (int i = from; i < to && i < menu->count; i++)
        h += item_height(menu->items[i].type);
    return h;
}

void draw_menu(void *unused, Menu *menu) {
    display_clear(COLOR_BG);

    /* ── Title bar ── */
    display_rect(0, 0, SCREEN_W, CONTENT_TOP - 2, COLOR_TITLE_BG);
    display_text(MENU_X, 4,  COLOR_TEXT, "PS Vita Recovery Menu v1.0");
    /* Show config path next to title when the menu provides one */
    if (menu->subtitle && menu->subtitle[0]) {
        char title_line[128];
        snprintf(title_line, sizeof(title_line), "%s  %s",
                 menu->title, menu->subtitle);
        display_text(MENU_X, 26, COLOR_DIM, title_line);
    } else {
        display_text(MENU_X, 26, COLOR_DIM, menu->title);
    }
    display_hline(0, CONTENT_TOP - 2, SCREEN_W, COLOR_GREEN);

    /* ── Scroll bar ── */
    int total_h = range_height(menu, 0, menu->count);
    if (total_h > CONTENT_H) {
        int bar_h  = CONTENT_H * CONTENT_H / total_h;
        if (bar_h < 6) bar_h = 6;
        int scrolled = range_height(menu, 0, menu->scroll);
        int bar_y = CONTENT_TOP + (scrolled * (CONTENT_H - bar_h)) / (total_h - CONTENT_H);
        display_rect(SCREEN_W - 4, CONTENT_TOP, 3, CONTENT_H, RGBA8(30,30,30,255));
        display_rect(SCREEN_W - 4, bar_y,        3, bar_h,     COLOR_GREEN);
    }

    /* ── Menu items ── */
    int y = CONTENT_TOP + 1;
    for (int i = menu->scroll; i < menu->count; i++) {
        MenuItem *item = &menu->items[i];
        if (y >= CONTENT_BOT) break;

        if (item->type == ITEM_SEPARATOR) {
            display_hline(MENU_X, y + H_SEP/2, SCREEN_W - MENU_X*2, RGBA8(50,50,50,255));
            y += H_SEP;
            continue;
        }

        if (item->type == ITEM_HEADER) {
            display_text(MENU_X, y, COLOR_YELLOW, item->label);
            /* Yellow separator line running to the right edge */
            display_hline(MENU_X, y + H_ITEM - 2, SCREEN_W - MENU_X, COLOR_YELLOW);
            y += H_ITEM;
            continue;
        }

        int is_essential = (item->type == ITEM_ACTION && item->enabled == 1
                            && item->action == NULL && item->submenu == NULL);

        if (i == menu->selected) {
            /* Draw highlight bar first, then text transparently on top */
            display_rect(0, y - 1, SCREEN_W - 4, H_ITEM + 1, COLOR_SEL_BG);
            display_text_transp(MENU_X, y, COLOR_SELECTED, item->label);
            if (is_essential) {
                display_text_transp(SCREEN_W - 88, y, COLOR_SELECTED, "[ON*]");
            } else if (item->type == ITEM_TOGGLE) {
                char buf[16];
                snprintf(buf, sizeof(buf), "[%s]", item->enabled ? "ON " : "OFF");
                display_text_transp(SCREEN_W - 88, y, COLOR_SELECTED, buf);
            } else if (item->type == ITEM_SUBMENU) {
                display_text_transp(SCREEN_W - 24, y, COLOR_SELECTED, ">");
            }
        } else {
            display_text(MENU_X, y, COLOR_TEXT, item->label);
            if (is_essential) {
                display_text(SCREEN_W - 88, y, RGBA8(0,220,220,255), "[ON*]");
            } else if (item->type == ITEM_TOGGLE) {
                char buf[16];
                snprintf(buf, sizeof(buf), "[%s]", item->enabled ? "ON " : "OFF");
                display_text(SCREEN_W - 88, y, item->enabled ? COLOR_GREEN : COLOR_RED, buf);
            } else if (item->type == ITEM_SUBMENU) {
                display_text(SCREEN_W - 24, y, COLOR_DIM, ">");
            }
        }
        y += H_ITEM;
    }

    /* ── Bottom bar ── */
    display_rect(0, CONTENT_BOT, SCREEN_W, SCREEN_H - CONTENT_BOT, COLOR_TITLE_BG);
    display_hline(0, CONTENT_BOT, SCREEN_W, COLOR_GREEN);
    display_text(MENU_X, CONTENT_BOT + 6, COLOR_DIM, "[X]Select [O]Back [Tri]Exit");
}
