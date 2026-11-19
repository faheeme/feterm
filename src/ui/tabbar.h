/*
 * Tab bar interaction state: the right-click menu, renaming a tab in place,
 * dragging a tab to a new position, and the "close several tabs" question.
 * Pure UI state and decisions; the application performs the actions.
 *
 * Right-click only ever opens the menu: nothing happens until an item is chosen.
 */
#ifndef TE_TABBAR_H
#define TE_TABBAR_H

#include <stdbool.h>
#include "common.h"
#include "app/layout.h"
#include "input/keybind.h"
#include "ui/form.h"
#include "ui/menu.h"

/* Menu item ids (> 0, as menu_click/menu_key report them). */
enum {
    TE_TB_RENAME = 1,
    TE_TB_RESET_NAME,
    TE_TB_SPLIT_RIGHT,
    TE_TB_SPLIT_DOWN,
    TE_TB_DUPLICATE,
    TE_TB_RECONNECT,
    TE_TB_MOVE_LEFT,
    TE_TB_MOVE_RIGHT,
    TE_TB_CLOSE,
    TE_TB_CLOSE_OTHERS,
    TE_TB_CLOSE_RIGHT,
};

/* Confirmation dialog purposes. */
enum { TE_TB_CONFIRM_OTHERS = 1, TE_TB_CONFIRM_RIGHT };

/* What the menu needs to know about the tab it opens on. */
typedef struct te_tabbar_menu_ctx {
    size_t           tab;            /* index of the tab under the pointer */
    size_t           ntabs;
    bool             named;          /* the tab has a user-given name */
    bool             can_reconnect;  /* a session in the tab is down */
    const te_keymap *keys;           /* for shortcut hints; may be nullptr */
} te_tabbar_menu_ctx;

typedef enum { TE_TB_EDIT_NONE, TE_TB_EDIT_CHANGED, TE_TB_EDIT_SUBMIT, TE_TB_EDIT_CANCEL } te_tb_edit;

typedef struct te_tabbar_ui {
    te_menu       menu;
    size_t        menu_tab;          /* tab the open menu acts on */

    bool          editing;
    size_t        edit_tab;
    te_form_field edit;              /* value + cursor; maxlen TE_TAB_NAME_MAX */
    bool          edit_all;          /* the whole name is selected: typing replaces it */

    bool          pressed;           /* left button went down on a tab */
    size_t        press_tab;         /* follows the tab while it is dragged */
    float         press_x;
    bool          dragging;

    te_form       confirm;           /* "Close N tabs?" */
    size_t        confirm_tab;
} te_tabbar_ui;

void tabbar_init(te_tabbar_ui *ui);

/* Fill out (max >= TE_MENU_MAX_ITEMS) with the items for ctx. Returns the count. */
size_t tabbar_menu_items(const te_tabbar_menu_ctx *ctx, te_menu_item *out, size_t max);

/* Start renaming tab idx, with current (may be nullptr) as the starting text. */
void tabbar_edit_begin(te_tabbar_ui *ui, size_t idx, const char *current);
te_tb_edit tabbar_edit_key(te_tabbar_ui *ui, te_key key, uint32_t mods);
te_tb_edit tabbar_edit_text(te_tabbar_ui *ui, const char *utf8);
/* The edited name with surrounding blanks removed ("" = back to the automatic title). */
void tabbar_edit_value(const te_tabbar_ui *ui, char *out, size_t outsz);
void tabbar_edit_end(te_tabbar_ui *ui);

/* True while any part of the tab bar wants the keyboard (menu, rename or dialog). */
bool tabbar_captures(const te_tabbar_ui *ui);

/* Forget anything that refers to tab positions (tabs were closed or reordered). */
void tabbar_reset(te_tabbar_ui *ui);

/*
 * Drag: the slot a tab pressed at press_x should be in now that the pointer is
 * at x, given tab width tw and ntabs tabs starting at area_x. Returns from when
 * the pointer has not left the tab's own slot.
 */
size_t tabbar_drag_slot(size_t from, float x, float area_x, float tw, size_t ntabs);

#endif
