/*
 * Pop-up context menu. Opening one never performs an action: something only
 * happens when an item is clicked, or chosen with Up/Down and Enter. Esc or a
 * click outside closes it with no effect. The menu always fits in the window.
 */
#ifndef TE_MENU_H
#define TE_MENU_H

#include <stdbool.h>
#include "common.h"
#include "platform/platform.h"
#include "ui/paint.h"

#define TE_MENU_MAX_ITEMS 24

typedef struct te_menu_item {
    int  id;
    char label[48];
    char hint[24];           /* shortcut shown on the right, may be empty */
    bool enabled;
    bool danger;             /* drawn in the danger colour (Delete...) */
    bool separator_before;
} te_menu_item;

typedef struct te_menu {
    bool          open;
    te_menu_item  items[TE_MENU_MAX_ITEMS];
    size_t        n;
    int           hover;     /* item index, -1 none */
    float         x, y, w, h;
} te_menu;

/* Result of a key or click: an item id, or one of these. */
enum { TE_MENU_NOTHING = -1, TE_MENU_CLOSED = -2 };

/* Item helper for building menus. */
te_menu_item menu_item(int id, const char *label, const char *hint, bool enabled);

/* Open at (x, y), moved as needed to stay inside win_w x win_h. */
void menu_open(te_menu *m, const te_menu_item *items, size_t n, float x, float y, float win_w, float win_h,
               const te_painter *p);
void menu_close(te_menu *m);
bool menu_contains(const te_menu *m, float x, float y);
/* Item index under the point (enabled or not), or -1. */
int menu_index_at(const te_menu *m, float x, float y, const te_painter *p);
void menu_motion(te_menu *m, float x, float y, const te_painter *p);
/* A press: inside on an enabled item returns its id (and closes); outside closes. */
int menu_click(te_menu *m, float x, float y, const te_painter *p);
int menu_key(te_menu *m, te_key key);
void menu_draw(const te_menu *m, const te_painter *p, const te_ui_colors *c);

#endif
