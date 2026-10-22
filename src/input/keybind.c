#include "input/keybind.h"

#include <string.h>
#include <strings.h>

#include "util/str.h"

static const char *const action_names[TE_ACT_COUNT] = {
    [TE_ACT_NONE] = "",
    [TE_ACT_COPY] = "copy",
    [TE_ACT_PASTE] = "paste",
    [TE_ACT_PASTE_PRIMARY] = "paste_primary",
    [TE_ACT_ZOOM_IN] = "zoom_in",
    [TE_ACT_ZOOM_OUT] = "zoom_out",
    [TE_ACT_ZOOM_RESET] = "zoom_reset",
    [TE_ACT_SCROLL_PAGE_UP] = "scroll_page_up",
    [TE_ACT_SCROLL_PAGE_DOWN] = "scroll_page_down",
    [TE_ACT_SCROLL_LINE_UP] = "scroll_line_up",
    [TE_ACT_SCROLL_LINE_DOWN] = "scroll_line_down",
    [TE_ACT_SCROLL_TOP] = "scroll_top",
    [TE_ACT_SCROLL_BOTTOM] = "scroll_bottom",
    [TE_ACT_SEARCH] = "search",
    [TE_ACT_HIGHLIGHT_CYCLE] = "highlight_cycle",
    [TE_ACT_RELOAD_CONFIG] = "reload_config",
    [TE_ACT_PREV_PROMPT] = "prev_prompt",
    [TE_ACT_NEXT_PROMPT] = "next_prompt",
    [TE_ACT_NEW_TAB] = "new_tab",
    [TE_ACT_CLOSE_PANE] = "close_pane",
    [TE_ACT_NEXT_TAB] = "next_tab",
    [TE_ACT_PREV_TAB] = "prev_tab",
    [TE_ACT_GOTO_TAB_1] = "goto_tab_1", [TE_ACT_GOTO_TAB_2] = "goto_tab_2", [TE_ACT_GOTO_TAB_3] = "goto_tab_3",
    [TE_ACT_GOTO_TAB_4] = "goto_tab_4", [TE_ACT_GOTO_TAB_5] = "goto_tab_5", [TE_ACT_GOTO_TAB_6] = "goto_tab_6",
    [TE_ACT_GOTO_TAB_7] = "goto_tab_7", [TE_ACT_GOTO_TAB_8] = "goto_tab_8", [TE_ACT_GOTO_TAB_9] = "goto_tab_9",
    [TE_ACT_SPLIT_RIGHT] = "split_right",
    [TE_ACT_SPLIT_DOWN] = "split_down",
    [TE_ACT_FOCUS_NEXT_PANE] = "focus_next_pane",
    [TE_ACT_FOCUS_PREV_PANE] = "focus_prev_pane",
    [TE_ACT_RESIZE_LEFT] = "resize_left",
    [TE_ACT_RESIZE_RIGHT] = "resize_right",
    [TE_ACT_RESIZE_UP] = "resize_up",
    [TE_ACT_RESIZE_DOWN] = "resize_down",
    [TE_ACT_TOGGLE_ZOOM] = "toggle_zoom",
    [TE_ACT_THEME_NEXT] = "theme_next",
};

typedef struct key_name {
    const char *name;
    te_key      key;
    uint32_t    cp;
} key_name;

static const key_name key_names[] = {
    { "enter", TE_KEY_ENTER, 0 }, { "return", TE_KEY_ENTER, 0 }, { "escape", TE_KEY_ESCAPE, 0 },
    { "esc", TE_KEY_ESCAPE, 0 }, { "backspace", TE_KEY_BACKSPACE, 0 }, { "tab", TE_KEY_TAB, 0 },
    { "insert", TE_KEY_INSERT, 0 }, { "delete", TE_KEY_DELETE, 0 }, { "home", TE_KEY_HOME, 0 },
    { "end", TE_KEY_END, 0 }, { "page_up", TE_KEY_PAGE_UP, 0 }, { "pageup", TE_KEY_PAGE_UP, 0 },
    { "page_down", TE_KEY_PAGE_DOWN, 0 }, { "pagedown", TE_KEY_PAGE_DOWN, 0 }, { "up", TE_KEY_UP, 0 },
    { "down", TE_KEY_DOWN, 0 }, { "left", TE_KEY_LEFT, 0 }, { "right", TE_KEY_RIGHT, 0 },
    { "f1", TE_KEY_F1, 0 }, { "f2", TE_KEY_F2, 0 }, { "f3", TE_KEY_F3, 0 }, { "f4", TE_KEY_F4, 0 },
    { "f5", TE_KEY_F5, 0 }, { "f6", TE_KEY_F6, 0 }, { "f7", TE_KEY_F7, 0 }, { "f8", TE_KEY_F8, 0 },
    { "f9", TE_KEY_F9, 0 }, { "f10", TE_KEY_F10, 0 }, { "f11", TE_KEY_F11, 0 }, { "f12", TE_KEY_F12, 0 },
    { "space", TE_KEY_CHAR, ' ' }, { "equal", TE_KEY_CHAR, '=' }, { "plus", TE_KEY_CHAR, '+' },
    { "minus", TE_KEY_CHAR, '-' }, { "underscore", TE_KEY_CHAR, '_' }, { "comma", TE_KEY_CHAR, ',' },
    { "period", TE_KEY_CHAR, '.' }, { "slash", TE_KEY_CHAR, '/' }, { "backslash", TE_KEY_CHAR, '\\' },
    { "semicolon", TE_KEY_CHAR, ';' }, { "apostrophe", TE_KEY_CHAR, '\'' }, { "grave", TE_KEY_CHAR, '`' },
    { "bracketleft", TE_KEY_CHAR, '[' }, { "bracketright", TE_KEY_CHAR, ']' },
};

static void bind(te_keymap *m, te_action a, size_t slot, te_key key, uint32_t cp, uint32_t mods)
{
    m->b[a][slot] = (te_keybind){ .key = key, .cp = cp, .mods = mods };
}

void keymap_defaults(te_keymap *m)
{
    const uint32_t cs = TE_MOD_CTRL | TE_MOD_SHIFT;
    memset(m, 0, sizeof *m);
    bind(m, TE_ACT_COPY, 0, TE_KEY_CHAR, 'c', cs);
    bind(m, TE_ACT_PASTE, 0, TE_KEY_CHAR, 'v', cs);
    bind(m, TE_ACT_PASTE_PRIMARY, 0, TE_KEY_INSERT, 0, TE_MOD_SHIFT);
    bind(m, TE_ACT_ZOOM_IN, 0, TE_KEY_CHAR, '=', cs);
    bind(m, TE_ACT_ZOOM_IN, 1, TE_KEY_CHAR, '+', cs);
    bind(m, TE_ACT_ZOOM_OUT, 0, TE_KEY_CHAR, '-', cs);
    bind(m, TE_ACT_ZOOM_RESET, 0, TE_KEY_CHAR, '0', cs);
    bind(m, TE_ACT_SCROLL_PAGE_UP, 0, TE_KEY_PAGE_UP, 0, TE_MOD_SHIFT);
    bind(m, TE_ACT_SCROLL_PAGE_DOWN, 0, TE_KEY_PAGE_DOWN, 0, TE_MOD_SHIFT);
    bind(m, TE_ACT_SCROLL_LINE_UP, 0, TE_KEY_UP, 0, cs);
    bind(m, TE_ACT_SCROLL_LINE_DOWN, 0, TE_KEY_DOWN, 0, cs);
    bind(m, TE_ACT_SCROLL_TOP, 0, TE_KEY_HOME, 0, TE_MOD_SHIFT);
    bind(m, TE_ACT_SCROLL_BOTTOM, 0, TE_KEY_END, 0, TE_MOD_SHIFT);
    bind(m, TE_ACT_SEARCH, 0, TE_KEY_CHAR, 'f', cs);
    bind(m, TE_ACT_HIGHLIGHT_CYCLE, 0, TE_KEY_CHAR, 'h', cs);
    bind(m, TE_ACT_RELOAD_CONFIG, 0, TE_KEY_CHAR, 'r', cs);
    bind(m, TE_ACT_PREV_PROMPT, 0, TE_KEY_CHAR, 'z', cs);
    bind(m, TE_ACT_NEXT_PROMPT, 0, TE_KEY_CHAR, 'x', cs);
    bind(m, TE_ACT_NEW_TAB, 0, TE_KEY_CHAR, 't', cs);
    bind(m, TE_ACT_CLOSE_PANE, 0, TE_KEY_CHAR, 'w', cs);
    bind(m, TE_ACT_NEXT_TAB, 0, TE_KEY_PAGE_DOWN, 0, TE_MOD_CTRL);
    bind(m, TE_ACT_NEXT_TAB, 1, TE_KEY_TAB, 0, TE_MOD_CTRL);
    bind(m, TE_ACT_PREV_TAB, 0, TE_KEY_PAGE_UP, 0, TE_MOD_CTRL);
    bind(m, TE_ACT_PREV_TAB, 1, TE_KEY_TAB, 0, cs);
    for (uint32_t i = 0; i < 9; i++) {
        bind(m, (te_action)(TE_ACT_GOTO_TAB_1 + i), 0, TE_KEY_CHAR, '1' + i, TE_MOD_ALT);
    }
    bind(m, TE_ACT_SPLIT_RIGHT, 0, TE_KEY_CHAR, 'e', cs);
    bind(m, TE_ACT_SPLIT_DOWN, 0, TE_KEY_CHAR, 'o', cs);
    bind(m, TE_ACT_FOCUS_NEXT_PANE, 0, TE_KEY_CHAR, ']', cs);
    bind(m, TE_ACT_FOCUS_PREV_PANE, 0, TE_KEY_CHAR, '[', cs);
    bind(m, TE_ACT_RESIZE_LEFT, 0, TE_KEY_LEFT, 0, cs | TE_MOD_ALT);
    bind(m, TE_ACT_RESIZE_RIGHT, 0, TE_KEY_RIGHT, 0, cs | TE_MOD_ALT);
    bind(m, TE_ACT_RESIZE_UP, 0, TE_KEY_UP, 0, cs | TE_MOD_ALT);
    bind(m, TE_ACT_RESIZE_DOWN, 0, TE_KEY_DOWN, 0, cs | TE_MOD_ALT);
    bind(m, TE_ACT_TOGGLE_ZOOM, 0, TE_KEY_ENTER, 0, cs);
    bind(m, TE_ACT_THEME_NEXT, 0, TE_KEY_CHAR, '.', cs);
}

/* Parse one spec like "ctrl+shift+page_up". */
static bool parse_one(const char *spec, te_keybind *out)
{
    char buf[64] = { 0 };
    if (str_copy(buf, sizeof buf, spec) != TE_OK) {
        return false;
    }
    char *s = str_trim(buf);
    if (*s == '\0') {
        return false;
    }
    uint32_t mods = 0;
    char *save = nullptr;
    char *last = nullptr;
    /* "+" alone (or "ctrl++") means the plus key: handle a trailing "++". */
    size_t sl = strlen(s);
    bool plus_key = sl >= 1 && s[sl - 1] == '+' && (sl == 1 || s[sl - 2] == '+');
    if (plus_key) {
        s[sl - 1] = '\0';
    }
    for (char *tok = strtok_r(s, "+", &save); tok != nullptr; tok = strtok_r(nullptr, "+", &save)) {
        tok = str_trim(tok);
        if (last != nullptr) {
            /* the previous token was a modifier */
            if (strcasecmp(last, "ctrl") == 0 || strcasecmp(last, "control") == 0) mods |= TE_MOD_CTRL;
            else if (strcasecmp(last, "shift") == 0) mods |= TE_MOD_SHIFT;
            else if (strcasecmp(last, "alt") == 0 || strcasecmp(last, "meta") == 0) mods |= TE_MOD_ALT;
            else if (strcasecmp(last, "super") == 0 || strcasecmp(last, "logo") == 0) mods |= TE_MOD_SUPER;
            else return false;
        }
        last = tok;
    }
    te_keybind kb = { .key = TE_KEY_NONE, .cp = 0, .mods = 0 };
    if (plus_key) {
        if (last != nullptr) {
            /* every token was a modifier */
            if (strcasecmp(last, "ctrl") == 0 || strcasecmp(last, "control") == 0) mods |= TE_MOD_CTRL;
            else if (strcasecmp(last, "shift") == 0) mods |= TE_MOD_SHIFT;
            else if (strcasecmp(last, "alt") == 0 || strcasecmp(last, "meta") == 0) mods |= TE_MOD_ALT;
            else if (strcasecmp(last, "super") == 0 || strcasecmp(last, "logo") == 0) mods |= TE_MOD_SUPER;
            else return false;
        }
        kb.key = TE_KEY_CHAR;
        kb.cp = '+';
    } else {
        if (last == nullptr) {
            return false;
        }
        for (size_t i = 0; i < TE_ARRAY_LEN(key_names); i++) {
            if (strcasecmp(last, key_names[i].name) == 0) {
                kb.key = key_names[i].key;
                kb.cp = key_names[i].cp;
                break;
            }
        }
        if (kb.key == TE_KEY_NONE) {
            size_t n = strlen(last);
            unsigned char c = (unsigned char)last[0];
            if (n != 1 || c < 0x21 || c > 0x7E) {
                return false;
            }
            kb.key = TE_KEY_CHAR;
            kb.cp = (c >= 'A' && c <= 'Z') ? (uint32_t)(c - 'A' + 'a') : c;
        }
    }
    kb.mods = mods;
    *out = kb;
    return true;
}

te_err keymap_set(te_keymap *m, const char *action, const char *spec)
{
    te_action a = TE_ACT_NONE;
    for (size_t i = 1; i < TE_ACT_COUNT; i++) {
        if (strcmp(action, action_names[i]) == 0) {
            a = (te_action)i;
            break;
        }
    }
    if (a == TE_ACT_NONE) {
        return TE_ERR_NOTFOUND;
    }
    te_keybind parsed[TE_KEYBIND_SLOTS] = { 0 };
    char buf[160] = { 0 };
    if (str_copy(buf, sizeof buf, spec) != TE_OK) {
        return TE_ERR_TRUNC;
    }
    char *s = str_trim(buf);
    if (strcasecmp(s, "none") != 0) {
        size_t n = 0;
        char *save = nullptr;
        for (char *tok = strtok_r(s, ",", &save); tok != nullptr; tok = strtok_r(nullptr, ",", &save)) {
            if (n >= TE_KEYBIND_SLOTS || !parse_one(tok, &parsed[n])) {
                return TE_ERR_PARSE;
            }
            n++;
        }
        if (n == 0) {
            return TE_ERR_PARSE;
        }
    }
    memcpy(m->b[a], parsed, sizeof parsed);
    return TE_OK;
}

te_action keymap_lookup(const te_keymap *m, te_key key, uint32_t cp, uint32_t mods)
{
    if (key == TE_KEY_NONE) {
        return TE_ACT_NONE;
    }
    if (key == TE_KEY_CHAR && cp >= 'A' && cp <= 'Z') {
        cp = cp - 'A' + 'a';
    }
    for (size_t a = 1; a < TE_ACT_COUNT; a++) {
        for (size_t i = 0; i < TE_KEYBIND_SLOTS; i++) {
            const te_keybind *kb = &m->b[a][i];
            if (kb->key == key && kb->mods == mods && (key != TE_KEY_CHAR || kb->cp == cp)) {
                return (te_action)a;
            }
        }
    }
    return TE_ACT_NONE;
}
