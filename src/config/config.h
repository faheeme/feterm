#ifndef TE_CONFIG_H
#define TE_CONFIG_H

#include <stdbool.h>
#include "common.h"
#include "input/keybind.h"

typedef enum { TE_CURSOR_BLOCK, TE_CURSOR_BEAM, TE_CURSOR_UNDERLINE } te_cursor_shape;
typedef enum { TE_BELL_NONE, TE_BELL_VISUAL, TE_BELL_URGENT } te_bell_mode;
typedef enum { TE_TABBAR_AUTO, TE_TABBAR_ALWAYS, TE_TABBAR_NEVER } te_tabbar_mode;
typedef enum { TE_NOTIFY_UNFOCUSED, TE_NOTIFY_ALWAYS, TE_NOTIFY_NEVER } te_notify_mode;
typedef enum { TE_RECONNECT_ASK, TE_RECONNECT_AUTO, TE_RECONNECT_NEVER } te_reconnect_mode;

/* [keyboard] macos_option_as_alt: which Option keys are Alt (Meta) instead of typing characters. */
enum { TE_OPTION_LEFT = 1u << 0, TE_OPTION_RIGHT = 1u << 1 };

typedef struct te_config {
    /* [font] */
    char            font_family[256];
    float           font_size;        /* points */
    float           line_height;      /* multiplier, 0.8 .. 2.0 */
    /* [window] */
    uint16_t        cols;
    uint16_t        rows;
    uint16_t        padding;          /* logical pixels */
    float           opacity;          /* 0.1 .. 1.0: background of the terminal area */
    bool            transparent_cells;/* coloured cell backgrounds follow opacity too */
    char            theme[128];
    te_tabbar_mode  tab_bar;          /* auto: shown when there is more than one tab */
    float           inactive_dim;     /* 0..0.8: darken panes without focus */
    /* [terminal] */
    char            shell[1024];      /* empty: $SHELL, then /etc/passwd, then /bin/sh */
    char            term[64];
    uint32_t        scrollback;       /* lines */
    bool            bold_is_bright;
    bool            banner;           /* logo, version and author at the top of new local shells */
    te_bell_mode    bell;
    te_notify_mode  notifications;    /* OSC 9 / 777 / 99 desktop notifications */
    /* [cursor] */
    te_cursor_shape cursor_shape;
    bool            cursor_blink;
    bool            cursor_animate;
    /* [highlight] */
    char            hl_mode[64];      /* "auto", "off", or a profile name to pin */
    char            hl_fallback[64];  /* profile used in auto mode before detection; "none" */
    bool            hl_alt_screen;    /* also highlight full-screen apps (vim, less) */
    /* [mouse] */
    bool            click_to_position;
    bool            click_emacs_keys; /* send Ctrl-B/Ctrl-F instead of arrow keys */
    bool            copy_on_select;   /* selecting text copies it to the PRIMARY selection */
    char            word_chars[64];   /* besides letters/digits, characters that are part of a word */
    /* [sidebar] */
    bool            sidebar_right;
    float           sidebar_width;    /* logical pixels */
    bool            sidebar_visible;
    /* [sessions] */
    int32_t         keepalive;        /* seconds: SSH ServerAliveInterval, TCP keepalive */
    int32_t         anti_idle;        /* seconds without typing before a keep-awake byte; 0 off */
    uint8_t         anti_idle_bytes[16];
    size_t          anti_idle_len;
    te_reconnect_mode reconnect;
    /* [files] */
    char            download_dir[1024]; /* may start with ~/ */
    bool            follow_terminal;  /* the Files tab follows the remote shell's folder (OSC 7) */
    char            editor[1024];     /* program for "Edit locally"; empty: xdg-open */
    /* [keys] */
    te_keymap       keys;
    /* [keyboard] */
    uint8_t         option_as_alt;    /* macOS: TE_OPTION_LEFT | TE_OPTION_RIGHT act as Alt; 0: both type characters */
    /* [security] */
    bool            osc52_write;      /* allow programs to SET the clipboard */
    bool            paste_sanitize;   /* strip control characters from pastes */
    bool            confirm_paste;    /* ask before pasting several lines into a shell without bracketed paste */
} te_config;

void config_defaults(te_config *c);

/*
 * Load path (or <config dir>/config.ini if path is nullptr) on top of the
 * current values. A missing default file is not an error.
 */
[[nodiscard]] te_err config_load(te_config *c, const char *path);

#endif
