#pragma once
#include <psp2/ctrl.h>
#include "display.h"

// Menu item types
typedef enum {
    ITEM_ACTION,
    ITEM_TOGGLE,
    ITEM_SUBMENU,
    ITEM_BACK,
    ITEM_SEPARATOR,
    ITEM_HEADER
} MenuItemType;

typedef struct MenuItem {
    const char *label;
    MenuItemType type;
    int enabled;
    void (*action)(void);
    struct Menu *submenu;
} MenuItem;

typedef struct Menu {
    const char *title;
    const char *subtitle;
    MenuItem *items;
    int count;
    int selected;
    int scroll;       // index of first visible item
    struct Menu *parent;
} Menu;

void menu_run(void);
int  menu_dev_unlock_enabled(void);   /* returns 1 when System Write Mode is ON */
extern int g_safemode_applied;        /* set by main.c on L-trigger boot */
void menu_draw(Menu *menu);
void menu_handle_input(Menu **current, SceCtrlData *old_ctrl);
