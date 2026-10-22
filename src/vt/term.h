/* Terminal state: screen grids, scrollback, cursor, modes, and escape-sequence semantics. */
#ifndef TE_VT_TERM_H
#define TE_VT_TERM_H

#include <stdbool.h>
#include "common.h"
#include "config/theme.h"
#include "vt/images.h"
#include "vt/parser.h"

/* Cell colour encoding: 0 = default, 0x01 << 24 | index = palette, 0x02 << 24 | rgb = truecolour. */
#define TE_COLOR_DEFAULT     0u
#define TE_COLOR_PAL(i)      (0x01000000u | ((uint32_t)(i) & 0xFFu))
#define TE_COLOR_RGB(rgb)    (0x02000000u | ((uint32_t)(rgb) & 0xFFFFFFu))
#define TE_COLOR_KIND(c)     ((c) >> 24)

enum {
    TE_ATTR_BOLD      = 1u << 0,
    TE_ATTR_DIM       = 1u << 1,
    TE_ATTR_ITALIC    = 1u << 2,
    TE_ATTR_BLINK     = 1u << 3,
    TE_ATTR_INVERSE   = 1u << 4,
    TE_ATTR_HIDDEN    = 1u << 5,
    TE_ATTR_STRIKE    = 1u << 6,
    TE_ATTR_OVERLINE  = 1u << 7,
    TE_ATTR_WIDE      = 1u << 8,   /* first half of a double-width character */
    TE_ATTR_SPACER    = 1u << 9,   /* second half of a double-width character */
    TE_ATTR_UL_SHIFT  = 12,        /* 3 bits: underline style */
    TE_ATTR_UL_MASK   = 7u << 12,
};

typedef enum { TE_UL_NONE, TE_UL_SINGLE, TE_UL_DOUBLE, TE_UL_CURLY, TE_UL_DOTTED, TE_UL_DASHED } te_ul_style;

#define TE_CELL_MAX_COMBINING 2

typedef struct te_cell {
    uint32_t cp;                           /* 0 = empty */
    uint32_t comb[TE_CELL_MAX_COMBINING];  /* combining marks drawn on top */
    uint32_t fg, bg, ul;
    uint16_t attrs;
    uint16_t link;                         /* OSC 8 hyperlink id, 0 = none */
} te_cell;

#define TE_MAX_LINKS     4096u

/* Kitty keyboard flags we implement: disambiguate (1), event types (2), all keys as escapes (8). */
#define TE_KITTY_SUPPORTED 0x0Bu
#define TE_KITTY_STACK     8u
#define TE_MAX_URI       2048u

enum {
    TE_LINE_WRAPPED = 1u << 0,   /* continues on the next line (soft wrap) */
    TE_LINE_PROMPT  = 1u << 1,   /* a shell prompt starts here (OSC 133;A) */
};

typedef struct te_line {
    te_cell *cells;
    uint16_t cols;
    uint8_t  flags;
    uint16_t input_col;          /* OSC 133;B: column where typed input starts, plus 1; 0 = unknown */
} te_line;

typedef struct te_grid {
    te_line *lines;
    uint16_t rows;
} te_grid;

typedef enum {
    TE_MOUSE_NONE = 0, TE_MOUSE_X10 = 9, TE_MOUSE_NORMAL = 1000, TE_MOUSE_BUTTON = 1002, TE_MOUSE_ANY = 1003,
} te_mouse_mode;

typedef enum { TE_MOUSE_ENC_DEFAULT, TE_MOUSE_ENC_UTF8, TE_MOUSE_ENC_SGR, TE_MOUSE_ENC_URXVT, TE_MOUSE_ENC_SGR_PIXELS } te_mouse_enc;

typedef struct te_modes {
    bool app_cursor;       /* DECCKM */
    bool app_keypad;       /* DECKPAM */
    bool autowrap;         /* DECAWM */
    bool origin;           /* DECOM */
    bool insert;           /* IRM */
    bool newline;          /* LNM */
    bool cursor_visible;   /* DECTCEM */
    bool cursor_blink;     /* ATT610 */
    bool reverse_video;    /* DECSCNM */
    bool bracketed_paste;  /* 2004 */
    bool focus_events;     /* 1004 */
    bool alt_scroll;       /* 1007 */
    bool sync_output;      /* 2026 */
    bool alt_screen;
    uint8_t decscusr;      /* 0 = user default, 1..6 as set by DECSCUSR */
    uint8_t kitty_flags;   /* kitty keyboard protocol flags for the active screen */
    te_mouse_mode mouse;
    te_mouse_enc  mouse_enc;
} te_modes;

typedef struct te_cursor_state {
    uint16_t x, y;
    bool     pending_wrap;
    te_cell  pen;
    bool     origin;
    uint8_t  charset[4];
    uint8_t  gl;
} te_cursor_state;

/* Callbacks into the application (window title, bell, clipboard, size queries). */
typedef struct te_term_host {
    void *user;
    void (*set_title)(void *user, const char *utf8);
    void (*bell)(void *user);
    void (*clipboard_set)(void *user, const char *utf8);   /* OSC 52 */
    void (*cell_size)(void *user, uint16_t *w, uint16_t *h);
    void (*notify)(void *user, const char *title, const char *body);  /* OSC 9 / 777 / 99 */
} te_term_host;

typedef struct te_term te_term;

[[nodiscard]] te_err term_create(te_term **out, uint16_t cols, uint16_t rows, uint32_t scrollback,
                                 const te_theme *theme, const te_term_host *host);
void term_destroy(te_term **t);

void term_feed(te_term *t, const uint8_t *data, size_t len);
[[nodiscard]] te_err term_resize(te_term *t, uint16_t cols, uint16_t rows);
void term_set_theme(te_term *t, const te_theme *theme);

/* Replies the terminal wants to send to the application (DA, DSR, ...). */
const uint8_t *term_output(const te_term *t, size_t *len);
void term_output_consume(te_term *t, size_t n);

/* Viewing. Row 0 is the top of the visible window, taking scrollback offset into account. */
uint16_t term_cols(const te_term *t);
uint16_t term_rows(const te_term *t);
const te_line *term_view_line(const te_term *t, uint16_t row);
void term_scroll_view(te_term *t, int delta_lines);
void term_view_reset(te_term *t);
uint32_t term_view_offset(const te_term *t);
uint32_t term_scrollback_count(const te_term *t);

void term_cursor(const te_term *t, uint16_t *x, uint16_t *y);
const te_modes *term_modes(const te_term *t);
const char *term_title(const te_term *t);

/* Resolve a cell colour to 0xRRGGBB using the live palette. is_fg selects the default. */
uint32_t term_resolve_color(const te_term *t, uint32_t c, bool is_fg);
uint32_t term_default_fg(const te_term *t);
uint32_t term_default_bg(const te_term *t);
uint32_t term_cursor_color(const te_term *t);

/*
 * Shell integration (OSC 133). The shell marks prompt start (A), input start (B),
 * command start (C) and command end (D).
 */
bool term_shell_integration(const te_term *t);   /* any OSC 133 seen */
bool term_shell_input_active(const te_term *t);  /* between B and C: the shell is reading input */
bool term_shell_click_events(const te_term *t);  /* shell asked for clicks as mouse events */
uint32_t term_prompt_count(const te_term *t);    /* increments on every OSC 133;A */
/* Grid line (not view line) for row y, or nullptr. */
const te_line *term_grid_line(const te_term *t, uint16_t y);

/* Current working directory reported by the shell (OSC 7), or "" if unknown. */
const char *term_cwd(const te_term *t);

/* Inline images (sixel / kitty graphics) currently stored. */
size_t term_image_count(const te_term *t);
const te_image *term_image_at(const te_term *t, size_t i);

/* OSC 8 hyperlinks: URI for a cell's link id, or nullptr. */
const char *term_link_uri(const te_term *t, uint16_t id);

/*
 * Absolute line numbers stay attached to the same text while it scrolls
 * (they only become invalid when the line drops out of the scrollback).
 * Used by selection and search.
 */
uint64_t term_view_abs(const te_term *t, uint16_t row);   /* abs number of a visible row */
uint64_t term_abs_first(const te_term *t);                /* oldest line still stored */
uint64_t term_abs_end(const te_term *t);                  /* one past the last grid line */
const te_line *term_abs_line(const te_term *t, uint64_t abs);
/* Scroll the view so line abs is visible (at the top when at_top, else centred). */
void term_scroll_to_abs(te_term *t, uint64_t abs, bool at_top);
/* Nearest shell prompt (OSC 133;A) before (dir < 0) or after (dir > 0) line from. */
[[nodiscard]] bool term_find_prompt(const te_term *t, uint64_t from, int dir, uint64_t *out);
/* Increments whenever output arrives; lets search know its results may be stale. */
uint64_t term_content_gen(const te_term *t);

/* Dirty tracking for the renderer. */
bool term_is_dirty(const te_term *t);
void term_clear_dirty(te_term *t);
void term_mark_dirty(te_term *t);

#endif
