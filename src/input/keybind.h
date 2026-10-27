/*
 * Configurable shortcuts. [keys] in config.ini maps an action to up to two
 * key specs, e.g.  copy = ctrl+shift+c, ctrl+insert   or   search = none.
 * The macOS spellings work too: cmd/command for super, option/opt for alt.
 */
#ifndef TE_KEYBIND_H
#define TE_KEYBIND_H

#include <stdbool.h>
#include "common.h"
#include "platform/platform.h"

typedef enum te_action {
    TE_ACT_NONE = 0,
    TE_ACT_COPY,
    TE_ACT_PASTE,
    TE_ACT_PASTE_PRIMARY,
    TE_ACT_ZOOM_IN,
    TE_ACT_ZOOM_OUT,
    TE_ACT_ZOOM_RESET,
    TE_ACT_SCROLL_PAGE_UP,
    TE_ACT_SCROLL_PAGE_DOWN,
    TE_ACT_SCROLL_LINE_UP,
    TE_ACT_SCROLL_LINE_DOWN,
    TE_ACT_SCROLL_TOP,
    TE_ACT_SCROLL_BOTTOM,
    TE_ACT_SEARCH,
    TE_ACT_HIGHLIGHT_CYCLE,
    TE_ACT_RELOAD_CONFIG,
    TE_ACT_PREV_PROMPT,
    TE_ACT_NEXT_PROMPT,
    TE_ACT_NEW_TAB,
    TE_ACT_CLOSE_PANE,
    TE_ACT_NEXT_TAB,
    TE_ACT_PREV_TAB,
    TE_ACT_GOTO_TAB_1, TE_ACT_GOTO_TAB_2, TE_ACT_GOTO_TAB_3, TE_ACT_GOTO_TAB_4, TE_ACT_GOTO_TAB_5,
    TE_ACT_GOTO_TAB_6, TE_ACT_GOTO_TAB_7, TE_ACT_GOTO_TAB_8, TE_ACT_GOTO_TAB_9,
    TE_ACT_SPLIT_RIGHT,
    TE_ACT_SPLIT_DOWN,
    TE_ACT_FOCUS_NEXT_PANE,
    TE_ACT_FOCUS_PREV_PANE,
    TE_ACT_RESIZE_LEFT,
    TE_ACT_RESIZE_RIGHT,
    TE_ACT_RESIZE_UP,
    TE_ACT_RESIZE_DOWN,
    TE_ACT_TOGGLE_ZOOM,
    TE_ACT_THEME_NEXT,
    TE_ACT_TOGGLE_SIDEBAR,
    TE_ACT_SIDEBAR_SESSIONS,
    TE_ACT_SIDEBAR_FILES,
    TE_ACT_QUICK_CONNECT,
    TE_ACT_SEND_BREAK,
    TE_ACT_COUNT,
} te_action;

#define TE_KEYBIND_SLOTS 2

typedef struct te_keybind {
    te_key   key;     /* TE_KEY_NONE: unbound */
    uint32_t cp;      /* for TE_KEY_CHAR: lower-case code point */
    uint32_t mods;
} te_keybind;

typedef struct te_keymap {
    te_keybind b[TE_ACT_COUNT][TE_KEYBIND_SLOTS];
} te_keymap;

void keymap_defaults(te_keymap *m);
/* Set an action's bindings from a spec ("ctrl+shift+c, ctrl+insert" or "none"). */
[[nodiscard]] te_err keymap_set(te_keymap *m, const char *action, const char *spec);
[[nodiscard]] te_action keymap_lookup(const te_keymap *m, te_key key, uint32_t cp, uint32_t mods);
/* The action's first binding as text for hints, e.g. "Ctrl+Shift+W" or, on macOS,
 * "Cmd+W". TE_ERR_NOTFOUND (and "") when the action is unbound. */
[[nodiscard]] te_err keymap_describe(const te_keymap *m, te_action a, char *out, size_t outsz);

#endif
