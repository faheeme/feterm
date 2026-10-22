#include "config/config.h"

#include <string.h>

#include "config/ini.h"
#include "config/paths.h"
#include "util/log.h"
#include "util/str.h"

void config_defaults(te_config *c)
{
    memset(c, 0, sizeof *c);
    (void)str_copy(c->font_family, sizeof c->font_family, "monospace");
    c->font_size = 11.0f;
    c->line_height = 1.0f;
    c->cols = 100;
    c->rows = 30;
    c->padding = 8;
    c->opacity = 0.9f;
    c->transparent_cells = false;
    (void)str_copy(c->theme, sizeof c->theme, "catppuccin-mocha");
    (void)str_copy(c->term, sizeof c->term, "xterm-256color");
    c->scrollback = 10000;
    c->bold_is_bright = false;
    c->bell = TE_BELL_VISUAL;
    c->cursor_shape = TE_CURSOR_BLOCK;
    c->cursor_blink = true;
    c->cursor_animate = true;
    c->tab_bar = TE_TABBAR_AUTO;
    c->inactive_dim = 0.3f;
    c->notifications = TE_NOTIFY_UNFOCUSED;
    c->sidebar_right = false;
    c->sidebar_width = 280.0f;
    c->sidebar_visible = true;
    c->keepalive = 30;
    c->anti_idle = 240;
    c->anti_idle_bytes[0] = 0;   /* a NUL byte: counts as activity, prints nothing */
    c->anti_idle_len = 1;
    c->reconnect = TE_RECONNECT_ASK;
    (void)str_copy(c->hl_mode, sizeof c->hl_mode, "auto");
    (void)str_copy(c->hl_fallback, sizeof c->hl_fallback, "network-common");
    c->hl_alt_screen = false;
    c->click_to_position = true;
    c->click_emacs_keys = false;
    c->copy_on_select = true;
    (void)str_copy(c->word_chars, sizeof c->word_chars, "-_./~:@%+=?&#*");
    keymap_defaults(&c->keys);
    c->confirm_paste = true;
    c->osc52_write = true;
    c->paste_sanitize = true;
}

#define BAD(what) LOG_W("%s:%u: invalid %s '%s'", path, line, what, value)

static void set_str(char *dst, size_t sz, const char *value, const char *path, unsigned line)
{
    if (str_copy(dst, sz, value) != TE_OK) {
        LOG_W("%s:%u: value too long, truncated", path, line);
    }
}

static te_err config_kv(void *user, const char *sec, const char *key, const char *raw,
                        const char *path, unsigned line)
{
    te_config *c = user;
    /* Config values may be quoted (family = "JetBrains Mono"); strip one matching pair. */
    char value[INI_MAX_LINE] = { 0 };
    if (str_copy(value, sizeof value, raw) != TE_OK) {
        LOG_W("%s:%u: value too long", path, line);
        return TE_OK;
    }
    size_t vlen = strlen(value);
    if (vlen >= 2 && ((value[0] == '"' && value[vlen - 1] == '"') || (value[0] == '\'' && value[vlen - 1] == '\''))) {
        memmove(value, value + 1, vlen - 2);
        value[vlen - 2] = '\0';
    }
    long l = 0;
    float f = 0.0f;
    bool b = false;

    if (strcmp(sec, "font") == 0) {
        if (strcmp(key, "family") == 0) {
            set_str(c->font_family, sizeof c->font_family, value, path, line);
        } else if (strcmp(key, "size") == 0) {
            if (str_to_float(value, 4.0f, 96.0f, &f)) c->font_size = f; else BAD("font size");
        } else if (strcmp(key, "line_height") == 0) {
            if (str_to_float(value, 0.8f, 2.0f, &f)) c->line_height = f; else BAD("line height");
        } else {
            LOG_W("%s:%u: unknown key [font] %s", path, line, key);
        }
    } else if (strcmp(sec, "window") == 0) {
        if (strcmp(key, "columns") == 0) {
            if (str_to_long(value, 10, 1000, &l)) c->cols = (uint16_t)l; else BAD("column count");
        } else if (strcmp(key, "rows") == 0) {
            if (str_to_long(value, 3, 500, &l)) c->rows = (uint16_t)l; else BAD("row count");
        } else if (strcmp(key, "padding") == 0) {
            if (str_to_long(value, 0, 200, &l)) c->padding = (uint16_t)l; else BAD("padding");
        } else if (strcmp(key, "opacity") == 0) {
            if (str_to_float(value, 0.1f, 1.0f, &f)) c->opacity = f; else BAD("opacity");
        } else if (strcmp(key, "transparent_cell_backgrounds") == 0) {
            if (str_to_bool(value, &b)) c->transparent_cells = b; else BAD("boolean");
        } else if (strcmp(key, "tab_bar") == 0) {
            if (strcmp(value, "auto") == 0) c->tab_bar = TE_TABBAR_AUTO;
            else if (strcmp(value, "always") == 0) c->tab_bar = TE_TABBAR_ALWAYS;
            else if (strcmp(value, "never") == 0) c->tab_bar = TE_TABBAR_NEVER;
            else BAD("tab_bar (auto|always|never)");
        } else if (strcmp(key, "inactive_dim") == 0) {
            if (str_to_float(value, 0.0f, 0.8f, &f)) c->inactive_dim = f; else BAD("inactive_dim (0..0.8)");
        } else if (strcmp(key, "theme") == 0) {
            if (paths_is_safe_name(value)) {
                set_str(c->theme, sizeof c->theme, value, path, line);
            } else {
                BAD("theme name");
            }
        } else {
            LOG_W("%s:%u: unknown key [window] %s", path, line, key);
        }
    } else if (strcmp(sec, "terminal") == 0) {
        if (strcmp(key, "shell") == 0) {
            set_str(c->shell, sizeof c->shell, value, path, line);
        } else if (strcmp(key, "term") == 0) {
            if (paths_is_safe_name(value)) {
                set_str(c->term, sizeof c->term, value, path, line);
            } else {
                BAD("TERM value");
            }
        } else if (strcmp(key, "scrollback") == 0) {
            if (str_to_long(value, 0, 1000000, &l)) c->scrollback = (uint32_t)l; else BAD("scrollback");
        } else if (strcmp(key, "bold_is_bright") == 0) {
            if (str_to_bool(value, &b)) c->bold_is_bright = b; else BAD("boolean");
        } else if (strcmp(key, "bell") == 0) {
            if (strcmp(value, "none") == 0) c->bell = TE_BELL_NONE;
            else if (strcmp(value, "visual") == 0) c->bell = TE_BELL_VISUAL;
            else if (strcmp(value, "urgent") == 0) c->bell = TE_BELL_URGENT;
            else BAD("bell mode (none|visual|urgent)");
        } else if (strcmp(key, "notifications") == 0) {
            if (strcmp(value, "unfocused") == 0) c->notifications = TE_NOTIFY_UNFOCUSED;
            else if (strcmp(value, "always") == 0) c->notifications = TE_NOTIFY_ALWAYS;
            else if (strcmp(value, "never") == 0) c->notifications = TE_NOTIFY_NEVER;
            else BAD("notifications (unfocused|always|never)");
        } else {
            LOG_W("%s:%u: unknown key [terminal] %s", path, line, key);
        }
    } else if (strcmp(sec, "cursor") == 0) {
        if (strcmp(key, "shape") == 0) {
            if (strcmp(value, "block") == 0) c->cursor_shape = TE_CURSOR_BLOCK;
            else if (strcmp(value, "beam") == 0) c->cursor_shape = TE_CURSOR_BEAM;
            else if (strcmp(value, "underline") == 0) c->cursor_shape = TE_CURSOR_UNDERLINE;
            else BAD("cursor shape (block|beam|underline)");
        } else if (strcmp(key, "blink") == 0) {
            if (str_to_bool(value, &b)) c->cursor_blink = b; else BAD("boolean");
        } else if (strcmp(key, "animate") == 0) {
            if (str_to_bool(value, &b)) c->cursor_animate = b; else BAD("boolean");
        } else {
            LOG_W("%s:%u: unknown key [cursor] %s", path, line, key);
        }
    } else if (strcmp(sec, "highlight") == 0) {
        if (strcmp(key, "mode") == 0) {
            if (paths_is_safe_name(value)) {
                set_str(c->hl_mode, sizeof c->hl_mode, value, path, line);
            } else {
                BAD("highlight mode (auto|off|<profile>)");
            }
        } else if (strcmp(key, "fallback") == 0) {
            if (paths_is_safe_name(value)) {
                set_str(c->hl_fallback, sizeof c->hl_fallback, value, path, line);
            } else {
                BAD("profile name");
            }
        } else if (strcmp(key, "alt_screen") == 0) {
            if (str_to_bool(value, &b)) c->hl_alt_screen = b; else BAD("boolean");
        } else {
            LOG_W("%s:%u: unknown key [highlight] %s", path, line, key);
        }
    } else if (strcmp(sec, "mouse") == 0) {
        if (strcmp(key, "click_to_position") == 0) {
            if (str_to_bool(value, &b)) c->click_to_position = b; else BAD("boolean");
        } else if (strcmp(key, "click_keys") == 0) {
            if (strcmp(value, "arrows") == 0) c->click_emacs_keys = false;
            else if (strcmp(value, "emacs") == 0) c->click_emacs_keys = true;
            else BAD("click_keys (arrows|emacs)");
        } else if (strcmp(key, "copy_on_select") == 0) {
            if (str_to_bool(value, &b)) c->copy_on_select = b; else BAD("boolean");
        } else if (strcmp(key, "word_chars") == 0) {
            set_str(c->word_chars, sizeof c->word_chars, value, path, line);
        } else {
            LOG_W("%s:%u: unknown key [mouse] %s", path, line, key);
        }
    } else if (strcmp(sec, "sidebar") == 0) {
        if (strcmp(key, "position") == 0) {
            if (strcmp(value, "left") == 0) c->sidebar_right = false;
            else if (strcmp(value, "right") == 0) c->sidebar_right = true;
            else BAD("position (left|right)");
        } else if (strcmp(key, "width") == 0) {
            if (str_to_float(value, 180.0f, 640.0f, &f)) c->sidebar_width = f; else BAD("width (180..640)");
        } else if (strcmp(key, "visible") == 0) {
            if (str_to_bool(value, &b)) c->sidebar_visible = b; else BAD("boolean");
        } else {
            LOG_W("%s:%u: unknown key [sidebar] %s", path, line, key);
        }
    } else if (strcmp(sec, "sessions") == 0) {
        if (strcmp(key, "keepalive") == 0) {
            if (str_to_long(value, 5, 3600, &l)) c->keepalive = (int32_t)l; else BAD("keepalive (5..3600 seconds)");
        } else if (strcmp(key, "anti_idle") == 0) {
            if (str_to_long(value, 0, 86400, &l)) c->anti_idle = (int32_t)l; else BAD("anti_idle (0..86400 seconds)");
        } else if (strcmp(key, "anti_idle_string") == 0) {
            uint8_t tmp[sizeof c->anti_idle_bytes] = { 0 };
            size_t n = 0;
            if (str_unescape(value, tmp, sizeof tmp, &n) && n > 0) {
                memcpy(c->anti_idle_bytes, tmp, n);
                c->anti_idle_len = n;
            } else {
                BAD("anti_idle_string (1-16 bytes; escapes \\0 \\r \\n \\t \\e \\xHH)");
            }
        } else if (strcmp(key, "reconnect") == 0) {
            if (strcmp(value, "ask") == 0) c->reconnect = TE_RECONNECT_ASK;
            else if (strcmp(value, "auto") == 0) c->reconnect = TE_RECONNECT_AUTO;
            else if (strcmp(value, "never") == 0) c->reconnect = TE_RECONNECT_NEVER;
            else BAD("reconnect (ask|auto|never)");
        } else {
            LOG_W("%s:%u: unknown key [sessions] %s", path, line, key);
        }
    } else if (strcmp(sec, "keys") == 0) {
        te_err krc = keymap_set(&c->keys, key, value);
        if (krc == TE_ERR_NOTFOUND) {
            LOG_W("%s:%u: unknown action '%s'", path, line, key);
        } else if (krc != TE_OK) {
            LOG_W("%s:%u: cannot parse key binding '%s' (e.g. ctrl+shift+c, f5, none)", path, line, value);
        }
    } else if (strcmp(sec, "security") == 0) {
        if (strcmp(key, "osc52_write") == 0) {
            if (str_to_bool(value, &b)) c->osc52_write = b; else BAD("boolean");
        } else if (strcmp(key, "paste_sanitize") == 0) {
            if (str_to_bool(value, &b)) c->paste_sanitize = b; else BAD("boolean");
        } else if (strcmp(key, "confirm_multiline_paste") == 0) {
            if (str_to_bool(value, &b)) c->confirm_paste = b; else BAD("boolean");
        } else {
            LOG_W("%s:%u: unknown key [security] %s", path, line, key);
        }
    } else {
        LOG_W("%s:%u: unknown section [%s]", path, line, sec);
    }
    return TE_OK;
}

te_err config_load(te_config *c, const char *path)
{
    if (c == nullptr) {
        return TE_ERR_INVAL;
    }
    char buf[TE_PATH_MAX] = { 0 };
    bool explicit_path = path != nullptr;
    if (!explicit_path) {
        te_err rc = paths_config_dir(buf, sizeof buf);
        if (rc != TE_OK || str_append(buf, sizeof buf, "/config.ini") != TE_OK) {
            return TE_OK; /* no usable home: run on defaults */
        }
        path = buf;
    }
    te_err rc = ini_parse_file(path, config_kv, c);
    if (rc == TE_ERR_NOTFOUND && !explicit_path) {
        return TE_OK;
    }
    if (rc != TE_OK) {
        LOG_E("cannot read config %s: %s", path, te_strerror(rc));
    }
    return rc;
}
