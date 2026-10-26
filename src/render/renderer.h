/* GPU renderer: OpenGL ES 3.0, glyph atlas, instanced quads. */
#ifndef TE_RENDERER_H
#define TE_RENDERER_H

#include <stdbool.h>
#include "common.h"
#include "config/config.h"
#include "highlight/highlight.h"
#include "render/font.h"
#include "ui/paint.h"
#include "ui/search.h"
#include "ui/selection.h"
#include "ui/url.h"
#include "vt/term.h"

typedef struct te_render_opts {
    int             padding;         /* physical pixels */
    float           opacity;         /* default background; 1.0 when the desktop cannot composite */
    bool            transparent_cells; /* coloured cell backgrounds follow opacity too */
    te_cursor_shape cursor_shape;    /* used when the application has not set DECSCUSR */
    bool            bold_is_bright;
    uint32_t        cursor_text;     /* text colour under a block cursor */
    te_highlighter *hl;              /* nullptr: no syntax highlighting */
    bool            hl_alt_screen;   /* highlight the alternate screen too */
    uint32_t        toast_fg, toast_bg;
    const te_selection *sel;         /* may be nullptr */
    uint32_t        sel_fg, sel_bg;
    const te_search *search;         /* may be nullptr; matches shown while open */
    uint32_t        search_fg, search_bg, search_cur_bg;
    const te_url_hit *hover;         /* link under the pointer (Ctrl held), underlined */
} te_render_opts;

typedef struct te_frame {
    int   width, height;             /* drawable size in pixels */
    bool  focused;
    bool  cursor_on;                 /* blink phase */
    float bell;                      /* 0..1 visual bell intensity */
    float scale;                     /* content scale, for decoration sizes */
    const char *toast;               /* short status message drawn top-right, or nullptr */
    const char *bar;                 /* search bar text along the bottom, or nullptr */
    const char *modal;               /* centred question (paste confirmation), or nullptr */
    int   x, y;                      /* pane origin in the window (pixels) */
    float dim;                       /* 0..1: darken an inactive pane */
    bool  animate_cursor;            /* smooth cursor movement (only for the focused pane) */
    uint32_t anim_key;               /* identifies the pane whose cursor is animated */
    uint64_t now_ms;
} te_frame;

/* Window decorations drawn after all panes. */
typedef struct te_chrome {
    int                 win_w, win_h;
    const char *const  *tab_titles;  /* ntabs entries */
    size_t              ntabs;
    size_t              active_tab;
    int                 tab_bar_h;   /* 0: no tab bar */
    int                 area_x, area_w; /* horizontal extent of the tab bar (the terminal area) */
    const uint32_t     *tab_colors;  /* ntabs entries, 0 = no colour tag; may be nullptr */
    const int         (*dividers)[4];/* x, y, w, h */
    size_t              ndividers;
    uint32_t            bar_bg, bar_fg, active_bg, active_fg, accent, divider;
    const char         *toast;
    const char         *modal;
    uint32_t            toast_fg, toast_bg, dim_bg;
    float               scale;
} te_chrome;

typedef struct te_renderer te_renderer;

[[nodiscard]] te_err renderer_create(te_renderer **out, te_font *font);
void renderer_destroy(te_renderer **r);
/* Switch to a new font (zoom, DPI change). The renderer does not own the font. */
void renderer_set_font(te_renderer *r, te_font *font);
/* Font for the sidebar, menus and dialogs, which do not zoom with the terminal.
 * nullptr: use the terminal font. The renderer does not own it. */
void renderer_set_ui_font(te_renderer *r, te_font *font);
/* Start a frame: clear the whole window. */
void renderer_begin(te_renderer *r, int win_w, int win_h, uint32_t bg, float opacity);
/* Draw one pane at f->x, f->y (size f->width x f->height), clipped to its rectangle. */
[[nodiscard]] te_err renderer_draw(te_renderer *r, const te_term *t, const te_render_opts *o, const te_frame *f);
/* Tab bar, dividers, toast and modal. */
void renderer_chrome(te_renderer *r, const te_chrome *c);
/* Finish the frame; reports GL errors. */
[[nodiscard]] te_err renderer_end(te_renderer *r);
/* True while the cursor animation needs more frames. */
bool renderer_animating(const te_renderer *r);
/* Tab index under a point in the tab bar, or -1. */
int renderer_tab_at(const te_renderer *r, const te_chrome *c, int x, int y);

/*
 * Free-form UI (sidebar, menus, dialogs) drawn on top of everything else, in the UI font:
 * renderer_ui_begin, then draw through the painter, then renderer_ui_end.
 */
void renderer_painter(te_renderer *r, float scale, te_painter *out);
void renderer_ui_begin(te_renderer *r);
void renderer_ui_end(te_renderer *r);

#endif
