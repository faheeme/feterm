#include "vt/term.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "util/log.h"
#include "util/mem.h"
#include "util/str.h"
#include "util/utf8.h"

#define TE_OUT_CAP     65536u
#define TE_TITLE_MAX   256u
#define TE_TITLE_STACK 8u

enum { CS_ASCII = 0, CS_DEC_GRAPHICS = 1 };

struct te_term {
    uint16_t        cols, rows;
    te_grid         main, alt;
    te_grid        *grid;            /* active grid: &main or &alt */

    te_line        *sb;              /* scrollback ring buffer (main screen only) */
    uint32_t        sb_cap, sb_count, sb_head;
    uint64_t        sb_base;         /* absolute number of the oldest scrollback line */
    uint64_t        content_gen;

    struct { char *uri; char id[64]; } links[TE_MAX_LINKS + 1];   /* index 0 unused */
    uint16_t        nlinks;
    bool            links_full_logged;
    uint32_t        view_offset;     /* lines scrolled back by the user */

    te_cursor_state cur;
    te_cursor_state saved_main, saved_alt;
    uint16_t        top, bottom;     /* scroll region, inclusive */
    uint8_t        *tabs;            /* one flag per column */
    te_modes        modes;

    uint32_t        palette[256], default_palette[256];
    uint32_t        fg, bg, cursor_color;
    uint32_t        default_fg, default_bg, default_cursor;

    char            title[TE_TITLE_MAX];
    char            title_stack[TE_TITLE_STACK][TE_TITLE_MAX];
    uint8_t         title_depth;
    char            cwd[1024];

    uint32_t        last_cp;
    bool            dirty;
    bool            out_overflow_logged;
    bool            sb_alloc_logged;

    uint8_t         kitty_cur[2];                     /* [0] main screen, [1] alternate */
    uint8_t         kitty_stack[2][TE_KITTY_STACK];
    uint8_t         kitty_depth[2];

    te_images       img;

    bool            shell_integration;
    bool            shell_input;
    bool            shell_click_events;
    uint32_t        prompt_count;

    vt_parser       parser;
    te_term_host    host;

    size_t          out_len;
    uint8_t         out[TE_OUT_CAP];
};

/* ------------------------------------------------------------------------- */
/* Line and grid helpers. Every cell access is bounds-checked against l->cols. */

static te_cell blank_cell(const te_term *t)
{
    te_cell c = { 0 };
    c.bg = t->cur.pen.bg;   /* background colour erase (BCE) */
    return c;
}

static te_cell *cell_at(te_line *l, uint16_t x)
{
    if (l == nullptr || l->cells == nullptr || x >= l->cols) {
        return nullptr;
    }
    return &l->cells[x];
}

static void line_fill(te_line *l, uint16_t x0, uint16_t x1, te_cell blank)
{
    if (l == nullptr || l->cells == nullptr) {
        return;
    }
    if (x1 > l->cols) {
        x1 = l->cols;
    }
    for (uint16_t x = x0; x < x1; x++) {
        l->cells[x] = blank;
    }
}

[[nodiscard]] static te_err line_alloc(te_line *l, uint16_t cols, te_cell blank)
{
    l->cells = te_calloc(cols, sizeof *l->cells);
    if (l->cells == nullptr) {
        l->cols = 0;
        return TE_ERR_NOMEM;
    }
    l->cols = cols;
    l->flags = 0;
    l->input_col = 0;
    line_fill(l, 0, cols, blank);
    return TE_OK;
}

static void line_free(te_line *l)
{
    if (l == nullptr) {
        return;
    }
    TE_FREE(l->cells);
    l->cols = 0;
    l->flags = 0;
    l->input_col = 0;
}

[[nodiscard]] static te_err line_ensure(te_line *l, uint16_t cols, te_cell blank)
{
    if (l->cols == cols && l->cells != nullptr) {
        return TE_OK;
    }
    void *p = l->cells;
    te_err rc = te_realloc_array(&p, l->cells != nullptr ? l->cols : 0, cols, sizeof(te_cell));
    if (rc != TE_OK) {
        return rc;
    }
    uint16_t old = l->cells != nullptr ? l->cols : 0;
    l->cells = p;
    l->cols = cols;
    line_fill(l, old, cols, blank);
    /* Never leave half of a wide character at the new right edge. */
    if (cols > 0 && (l->cells[cols - 1].attrs & TE_ATTR_WIDE) != 0) {
        l->cells[cols - 1] = blank;
    }
    return TE_OK;
}

static void grid_free(te_grid *g)
{
    if (g->lines != nullptr) {
        for (uint16_t i = 0; i < g->rows; i++) {
            line_free(&g->lines[i]);
        }
    }
    TE_FREE(g->lines);
    g->rows = 0;
}

[[nodiscard]] static te_err grid_alloc(te_grid *g, uint16_t rows, uint16_t cols, te_cell blank)
{
    g->lines = te_calloc(rows, sizeof *g->lines);
    if (g->lines == nullptr) {
        return TE_ERR_NOMEM;
    }
    g->rows = rows;
    for (uint16_t i = 0; i < rows; i++) {
        if (line_alloc(&g->lines[i], cols, blank) != TE_OK) {
            grid_free(g);
            return TE_ERR_NOMEM;
        }
    }
    return TE_OK;
}

static te_line *line_at(te_term *t, uint16_t y)
{
    if (y >= t->grid->rows) {
        return nullptr;
    }
    return &t->grid->lines[y];
}

/* Clearing one half of a wide character must clear the other half too. */
static void fix_wide(te_line *l, uint16_t x)
{
    te_cell *c = cell_at(l, x);
    if (c == nullptr) {
        return;
    }
    te_cell blank = { 0 };
    blank.bg = c->bg;
    if ((c->attrs & TE_ATTR_SPACER) != 0 && x > 0) {
        te_cell *prev = cell_at(l, (uint16_t)(x - 1));
        if (prev != nullptr) {
            *prev = blank;
        }
        *c = blank;
    } else if ((c->attrs & TE_ATTR_WIDE) != 0) {
        te_cell *next = cell_at(l, (uint16_t)(x + 1));
        if (next != nullptr) {
            *next = blank;
        }
        *c = blank;
    }
}

static void clear_range(te_term *t, uint16_t y, uint16_t x0, uint16_t x1)
{
    te_line *l = line_at(t, y);
    if (l == nullptr || x0 >= x1) {
        return;
    }
    fix_wide(l, x0);
    if (x1 > 0) {
        fix_wide(l, (uint16_t)(x1 - 1));
    }
    line_fill(l, x0, x1, blank_cell(t));
    if (x0 == 0 && x1 >= l->cols) {
        l->flags = 0;
        l->input_col = 0;
    } else if (x1 >= l->cols) {
        l->flags &= (uint8_t)~TE_LINE_WRAPPED;
    }
    t->dirty = true;
}

static void clear_lines(te_term *t, uint16_t y0, uint16_t y1)
{
    for (uint16_t y = y0; y < y1 && y < t->rows; y++) {
        clear_range(t, y, 0, t->cols);
    }
}

/* ------------------------------------------------------------------------- */
/* Scrollback */

static te_line *sb_line(const te_term *t, uint32_t idx)
{
    if (idx >= t->sb_count || t->sb_cap == 0) {
        return nullptr;
    }
    return &t->sb[(t->sb_head + idx) % t->sb_cap];
}

static void sb_clear(te_term *t)
{
    if (t->sb != nullptr) {
        for (uint32_t i = 0; i < t->sb_cap; i++) {
            line_free(&t->sb[i]);
        }
    }
    t->sb_base += t->sb_count;
    t->sb_count = 0;
    t->sb_head = 0;
    t->view_offset = 0;
}

/*
 * Move *victim's buffer into the scrollback. On success *victim receives a
 * replacement buffer of exactly t->cols cells. On failure nothing changes.
 */
static bool sb_push(te_term *t, te_line *victim)
{
    if (t->sb_cap == 0 || t->sb == nullptr) {
        return false;
    }
    te_cell *replacement = nullptr;
    bool full = t->sb_count == t->sb_cap;
    te_line *slot = full ? &t->sb[t->sb_head] : &t->sb[(t->sb_head + t->sb_count) % t->sb_cap];

    if (full && slot->cells != nullptr && slot->cols == t->cols) {
        replacement = slot->cells;            /* recycle the oldest line's buffer */
    } else {
        replacement = te_calloc(t->cols, sizeof *replacement);
        if (replacement == nullptr) {
            if (!t->sb_alloc_logged) {
                LOG_W("out of memory: scrollback line dropped");
                t->sb_alloc_logged = true;
            }
            return false;
        }
        if (full) {
            line_free(slot);
        }
    }
    *slot = *victim;
    if (full) {
        t->sb_head = (t->sb_head + 1) % t->sb_cap;
        t->sb_base++;
    } else {
        t->sb_count++;
    }
    victim->cells = replacement;
    victim->cols = t->cols;
    victim->flags = 0;
    victim->input_col = 0;
    return true;
}

/* ------------------------------------------------------------------------- */
/* Scrolling */

static void scroll_up(te_term *t, uint16_t top, uint16_t bottom, uint32_t n, bool allow_sb)
{
    if (top > bottom || bottom >= t->grid->rows || n == 0) {
        return;
    }
    uint32_t height = (uint32_t)(bottom - top) + 1u;
    if (n > height) {
        n = height;
    }
    te_cell blank = blank_cell(t);
    bool to_sb = allow_sb && t->grid == &t->main && top == 0;
    te_line *lines = t->grid->lines;
    uint32_t pushed = 0;
    for (uint32_t i = 0; i < n; i++) {
        te_line victim = lines[top];
        memmove(&lines[top], &lines[top + 1], (size_t)(bottom - top) * sizeof(te_line));
        if (to_sb && sb_push(t, &victim)) {
            pushed++;
        }
        if (line_ensure(&victim, t->cols, blank) != TE_OK) {
            /* Keep the old (shorter) buffer; accessors bounds-check it. */
        }
        line_fill(&victim, 0, victim.cols, blank);
        victim.flags = 0;
        victim.input_col = 0;
        lines[bottom] = victim;
    }
    /* Keep the user's scrolled-back view anchored on the same text. */
    if (t->view_offset > 0 && pushed > 0) {
        uint32_t v = t->view_offset + pushed;
        t->view_offset = v > t->sb_count ? t->sb_count : v;
    }
    t->dirty = true;
}

static void scroll_down(te_term *t, uint16_t top, uint16_t bottom, uint32_t n)
{
    if (top > bottom || bottom >= t->grid->rows || n == 0) {
        return;
    }
    uint32_t height = (uint32_t)(bottom - top) + 1u;
    if (n > height) {
        n = height;
    }
    te_cell blank = blank_cell(t);
    te_line *lines = t->grid->lines;
    for (uint32_t i = 0; i < n; i++) {
        te_line victim = lines[bottom];
        memmove(&lines[top + 1], &lines[top], (size_t)(bottom - top) * sizeof(te_line));
        line_fill(&victim, 0, victim.cols, blank);
        victim.flags = 0;
        victim.input_col = 0;
        lines[top] = victim;
    }
    t->dirty = true;
}

/* ------------------------------------------------------------------------- */
/* Output (replies to the application) */

static void reply_raw(te_term *t, const char *s, size_t len)
{
    if (len > TE_OUT_CAP - t->out_len) {
        if (!t->out_overflow_logged) {
            LOG_W("reply buffer full; dropping terminal responses");
            t->out_overflow_logged = true;
        }
        return;
    }
    memcpy(t->out + t->out_len, s, len);
    t->out_len += len;
}

TE_PRINTF(2, 3)
static void reply(te_term *t, const char *fmt, ...)
{
    char buf[512] = { 0 };
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= sizeof buf) {
        return; /* never send a truncated response */
    }
    reply_raw(t, buf, (size_t)n);
}

/* ------------------------------------------------------------------------- */
/* Cursor */

static void cursor_set(te_term *t, int64_t x, int64_t y)
{
    int64_t miny = 0;
    int64_t maxy = (int64_t)t->rows - 1;
    if (t->modes.origin) {
        miny = t->top;
        maxy = t->bottom;
    }
    t->cur.x = (uint16_t)TE_CLAMP(x, 0, (int64_t)t->cols - 1);
    t->cur.y = (uint16_t)TE_CLAMP(y, miny, maxy);
    t->cur.pending_wrap = false;
    t->dirty = true;
}

static void cursor_save(te_term *t)
{
    te_cursor_state *s = t->grid == &t->alt ? &t->saved_alt : &t->saved_main;
    *s = t->cur;
    s->origin = t->modes.origin;
}

static void cursor_restore(te_term *t)
{
    const te_cursor_state *s = t->grid == &t->alt ? &t->saved_alt : &t->saved_main;
    t->cur = *s;
    t->modes.origin = s->origin;
    if (t->cur.x >= t->cols) t->cur.x = (uint16_t)(t->cols - 1);
    if (t->cur.y >= t->rows) t->cur.y = (uint16_t)(t->rows - 1);
    if (t->cur.gl > 3) t->cur.gl = 0;
    t->dirty = true;
}

static void index_down(te_term *t)
{
    if (t->cur.y == t->bottom) {
        scroll_up(t, t->top, t->bottom, 1, true);
    } else if (t->cur.y + 1 < t->rows) {
        t->cur.y++;
    }
    t->cur.pending_wrap = false;
}

static void reverse_index(te_term *t)
{
    if (t->cur.y == t->top) {
        scroll_down(t, t->top, t->bottom, 1);
    } else if (t->cur.y > 0) {
        t->cur.y--;
    }
    t->cur.pending_wrap = false;
}

static void tab_forward(te_term *t, uint32_t n)
{
    while (n-- > 0 && t->cur.x + 1 < t->cols) {
        uint16_t x = (uint16_t)(t->cur.x + 1);
        while (x + 1 < t->cols && t->tabs[x] == 0) {
            x++;
        }
        t->cur.x = x;
    }
    t->cur.pending_wrap = false;
}

static void tab_backward(te_term *t, uint32_t n)
{
    while (n-- > 0 && t->cur.x > 0) {
        uint16_t x = (uint16_t)(t->cur.x - 1);
        while (x > 0 && t->tabs[x] == 0) {
            x--;
        }
        t->cur.x = x;
    }
    t->cur.pending_wrap = false;
}

static void tabs_reset(te_term *t, uint16_t from)
{
    for (uint16_t x = from; x < t->cols; x++) {
        t->tabs[x] = (x % 8 == 0 && x != 0) ? 1 : 0;
    }
}

/* ------------------------------------------------------------------------- */
/* Printing */

static uint32_t map_charset(const te_term *t, uint32_t cp)
{
    static const uint16_t dec_graphics[31] = {
        0x25C6, 0x2592, 0x2409, 0x240C, 0x240D, 0x240A, 0x00B0, 0x00B1, 0x2424, 0x240B, 0x2518,
        0x2510, 0x250C, 0x2514, 0x253C, 0x23BA, 0x23BB, 0x2500, 0x23BC, 0x23BD, 0x251C, 0x2524,
        0x2534, 0x252C, 0x2502, 0x2264, 0x2265, 0x03C0, 0x2260, 0x00A3, 0x00B7,
    };
    uint8_t gl = t->cur.gl < 4 ? t->cur.gl : 0;
    if (t->cur.charset[gl] == CS_DEC_GRAPHICS && cp >= 0x60u && cp <= 0x7Eu) {
        uint8_t idx = (uint8_t)(cp - 0x60u);   /* 0..30 */
        static_assert(TE_ARRAY_LEN(dec_graphics) == 0x7Fu - 0x60u, "table covers 0x60..0x7E");
        return dec_graphics[idx % TE_ARRAY_LEN(dec_graphics)];
    }
    return cp;
}

static void add_combining(te_term *t, uint32_t cp)
{
    te_line *l = line_at(t, t->cur.y);
    uint16_t x = t->cur.x;
    if (!t->cur.pending_wrap) {
        if (x == 0) {
            return;
        }
        x--;
    }
    te_cell *c = cell_at(l, x);
    if (c != nullptr && (c->attrs & TE_ATTR_SPACER) != 0 && x > 0) {
        c = cell_at(l, (uint16_t)(x - 1));
    }
    if (c == nullptr || c->cp == 0) {
        return;
    }
    for (size_t i = 0; i < TE_CELL_MAX_COMBINING; i++) {
        if (c->comb[i] == 0) {
            c->comb[i] = cp;
            t->dirty = true;
            return;
        }
    }
}

static void insert_blanks(te_term *t, uint16_t n);

static void print_cp(te_term *t, uint32_t cp)
{
    cp = map_charset(t, cp);
    int w = te_wcwidth(cp);
    if (w <= 0) {
        add_combining(t, cp);
        return;
    }
    if (t->view_offset != 0) {
        t->view_offset = 0;   /* new output snaps the view back to the bottom */
    }
    if (t->cur.pending_wrap && t->modes.autowrap) {
        te_line *l = line_at(t, t->cur.y);
        if (l != nullptr) {
            l->flags |= TE_LINE_WRAPPED;
        }
        index_down(t);
        t->cur.x = 0;
    }
    t->cur.pending_wrap = false;

    if (w == 2 && t->cols < 2) {
        cp = '?';
        w = 1;
    }
    if (w == 2 && t->cur.x + 1 >= t->cols) {
        if (t->modes.autowrap) {
            clear_range(t, t->cur.y, t->cur.x, t->cols);
            te_line *l = line_at(t, t->cur.y);
            if (l != nullptr) {
                l->flags |= TE_LINE_WRAPPED;
            }
            index_down(t);
            t->cur.x = 0;
        } else {
            t->cur.x = (uint16_t)(t->cols - 2);
        }
    }
    if (t->modes.insert) {
        insert_blanks(t, (uint16_t)w);
    }

    te_line *l = line_at(t, t->cur.y);
    fix_wide(l, t->cur.x);
    if (w == 2) {
        fix_wide(l, (uint16_t)(t->cur.x + 1));
    }
    te_cell *c = cell_at(l, t->cur.x);
    if (c == nullptr) {
        return;
    }
    *c = t->cur.pen;
    c->cp = cp;
    c->comb[0] = 0;
    c->comb[1] = 0;
    c->attrs &= (uint16_t)~(TE_ATTR_WIDE | TE_ATTR_SPACER);
    if (w == 2) {
        c->attrs |= TE_ATTR_WIDE;
        te_cell *s = cell_at(l, (uint16_t)(t->cur.x + 1));
        if (s != nullptr) {
            *s = t->cur.pen;
            s->cp = 0;
            s->comb[0] = 0;
            s->comb[1] = 0;
            s->attrs = (uint16_t)((s->attrs & ~(TE_ATTR_WIDE)) | TE_ATTR_SPACER);
        }
    }
    t->last_cp = cp;
    uint32_t nx = (uint32_t)t->cur.x + (uint32_t)w;
    if (nx >= t->cols) {
        t->cur.x = (uint16_t)(t->cols - 1);
        t->cur.pending_wrap = t->modes.autowrap;
    } else {
        t->cur.x = (uint16_t)nx;
    }
    t->dirty = true;
}

/* ------------------------------------------------------------------------- */
/* Editing */

static void insert_blanks(te_term *t, uint16_t n)
{
    te_line *l = line_at(t, t->cur.y);
    if (l == nullptr || l->cells == nullptr || t->cur.x >= l->cols) {
        return;
    }
    uint16_t x = t->cur.x;
    uint16_t avail = (uint16_t)(l->cols - x);
    if (n > avail) {
        n = avail;
    }
    fix_wide(l, x);
    memmove(&l->cells[x + n], &l->cells[x], (size_t)(avail - n) * sizeof(te_cell));
    line_fill(l, x, (uint16_t)(x + n), blank_cell(t));
    /* A wide character pushed half off the right edge is removed. */
    te_cell *last = cell_at(l, (uint16_t)(l->cols - 1));
    if (last != nullptr && (last->attrs & TE_ATTR_WIDE) != 0) {
        *last = blank_cell(t);
    }
    t->cur.pending_wrap = false;
    t->dirty = true;
}

static void delete_chars(te_term *t, uint16_t n)
{
    te_line *l = line_at(t, t->cur.y);
    if (l == nullptr || l->cells == nullptr || t->cur.x >= l->cols) {
        return;
    }
    uint16_t x = t->cur.x;
    uint16_t avail = (uint16_t)(l->cols - x);
    if (n > avail) {
        n = avail;
    }
    fix_wide(l, x);
    if (n < avail) {
        fix_wide(l, (uint16_t)(x + n));
    }
    memmove(&l->cells[x], &l->cells[x + n], (size_t)(avail - n) * sizeof(te_cell));
    line_fill(l, (uint16_t)(l->cols - n), l->cols, blank_cell(t));
    t->cur.pending_wrap = false;
    t->dirty = true;
}

static void erase_display(te_term *t, uint16_t mode)
{
    switch (mode) {
    case 0:
        clear_range(t, t->cur.y, t->cur.x, t->cols);
        clear_lines(t, (uint16_t)(t->cur.y + 1), t->rows);
        break;
    case 1:
        clear_lines(t, 0, t->cur.y);
        clear_range(t, t->cur.y, 0, (uint16_t)(t->cur.x + 1));
        break;
    case 2:
        clear_lines(t, 0, t->rows);
        images_drop_screen(&t->img, t->modes.alt_screen);
        break;
    case 3:
        sb_clear(t);
        images_prune(&t->img, t->sb_base + t->sb_count);
        t->dirty = true;
        break;
    default:
        break;
    }
}

static void erase_line(te_term *t, uint16_t mode)
{
    switch (mode) {
    case 0: clear_range(t, t->cur.y, t->cur.x, t->cols); break;
    case 1: clear_range(t, t->cur.y, 0, (uint16_t)(t->cur.x + 1)); break;
    case 2: clear_range(t, t->cur.y, 0, t->cols); break;
    default: break;
    }
}

/* ------------------------------------------------------------------------- */
/* Modes */

/* SGR 0: reset colours and attributes. An open OSC 8 hyperlink stays open. */
static void reset_pen(te_term *t)
{
    uint16_t link = t->cur.pen.link;
    memset(&t->cur.pen, 0, sizeof t->cur.pen);
    t->cur.pen.link = link;
}

static void set_alt_screen(te_term *t, bool on, bool clear_alt)
{
    if (on == t->modes.alt_screen) {
        return;
    }
    if (!on) {
        images_drop_screen(&t->img, true);   /* the alternate screen's content is gone */
    }
    t->modes.alt_screen = on;
    t->modes.kitty_flags = t->kitty_cur[on ? 1 : 0];
    t->grid = on ? &t->alt : &t->main;
    t->view_offset = 0;
    if (on && clear_alt) {
        clear_lines(t, 0, t->rows);
    }
    t->dirty = true;
}

static void full_reset(te_term *t);

static void set_private_mode(te_term *t, uint16_t mode, bool on)
{
    switch (mode) {
    case 1:    t->modes.app_cursor = on; break;
    case 5:    t->modes.reverse_video = on; t->dirty = true; break;
    case 6:    t->modes.origin = on; cursor_set(t, 0, on ? t->top : 0); break;
    case 7:    t->modes.autowrap = on; if (!on) t->cur.pending_wrap = false; break;
    case 12:   t->modes.cursor_blink = on; break;
    case 25:   t->modes.cursor_visible = on; t->dirty = true; break;
    case 66:   t->modes.app_keypad = on; break;
    case 9:    t->modes.mouse = on ? TE_MOUSE_X10 : TE_MOUSE_NONE; break;
    case 1000: t->modes.mouse = on ? TE_MOUSE_NORMAL : TE_MOUSE_NONE; break;
    case 1002: t->modes.mouse = on ? TE_MOUSE_BUTTON : TE_MOUSE_NONE; break;
    case 1003: t->modes.mouse = on ? TE_MOUSE_ANY : TE_MOUSE_NONE; break;
    case 1004: t->modes.focus_events = on; break;
    case 1005: t->modes.mouse_enc = on ? TE_MOUSE_ENC_UTF8 : TE_MOUSE_ENC_DEFAULT; break;
    case 1006: t->modes.mouse_enc = on ? TE_MOUSE_ENC_SGR : TE_MOUSE_ENC_DEFAULT; break;
    case 1015: t->modes.mouse_enc = on ? TE_MOUSE_ENC_URXVT : TE_MOUSE_ENC_DEFAULT; break;
    case 1016: t->modes.mouse_enc = on ? TE_MOUSE_ENC_SGR_PIXELS : TE_MOUSE_ENC_DEFAULT; break;
    case 1007: t->modes.alt_scroll = on; break;
    case 47:
    case 1047: set_alt_screen(t, on, true); break;
    case 1048: if (on) cursor_save(t); else cursor_restore(t); break;
    case 1049:
        if (on) {
            cursor_save(t);
            set_alt_screen(t, true, true);
        } else {
            set_alt_screen(t, false, false);
            cursor_restore(t);
        }
        break;
    case 2004: t->modes.bracketed_paste = on; break;
    case 2026: t->modes.sync_output = on; if (!on) t->dirty = true; break;
    default:
        LOG_D("unhandled DEC private mode %u", mode);
        break;
    }
}

/* DECRQM state: 1 set, 2 reset, 0 unknown. */
static int private_mode_state(const te_term *t, uint16_t mode)
{
    const te_modes *m = &t->modes;
    switch (mode) {
    case 1:    return m->app_cursor ? 1 : 2;
    case 5:    return m->reverse_video ? 1 : 2;
    case 6:    return m->origin ? 1 : 2;
    case 7:    return m->autowrap ? 1 : 2;
    case 12:   return m->cursor_blink ? 1 : 2;
    case 25:   return m->cursor_visible ? 1 : 2;
    case 66:   return m->app_keypad ? 1 : 2;
    case 9:    return m->mouse == TE_MOUSE_X10 ? 1 : 2;
    case 1000: return m->mouse == TE_MOUSE_NORMAL ? 1 : 2;
    case 1002: return m->mouse == TE_MOUSE_BUTTON ? 1 : 2;
    case 1003: return m->mouse == TE_MOUSE_ANY ? 1 : 2;
    case 1004: return m->focus_events ? 1 : 2;
    case 1006: return m->mouse_enc == TE_MOUSE_ENC_SGR ? 1 : 2;
    case 1007: return m->alt_scroll ? 1 : 2;
    case 47:
    case 1047:
    case 1049: return m->alt_screen ? 1 : 2;
    case 2004: return m->bracketed_paste ? 1 : 2;
    case 2026: return m->sync_output ? 1 : 2;
    default:   return 0;
    }
}

static void set_ansi_mode(te_term *t, uint16_t mode, bool on)
{
    switch (mode) {
    case 4:  t->modes.insert = on; break;
    case 20: t->modes.newline = on; break;
    default: break;
    }
}

/* ------------------------------------------------------------------------- */
/* SGR */

static uint16_t param_or(const vt_params *p, size_t i, uint16_t def)
{
    if (i >= p->n || p->v[i] == 0) {
        return def;
    }
    return p->v[i];
}

/*
 * Parse an extended colour starting at index i (which holds 38/48/58).
 * Accepts 38;5;n  38;2;r;g;b  38:5:n  38:2:r:g:b  38:2:cs:r:g:b.
 * Returns the number of extra parameters consumed; *out = colour or unchanged.
 */
static size_t parse_ext_color(const vt_params *p, size_t i, uint32_t *out)
{
    if (i + 1 >= p->n) {
        return 0;
    }
    bool colon = p->sub[i + 1];
    uint16_t kind = p->v[i + 1];
    if (kind == 5) {
        if (i + 2 >= p->n) {
            return 1;
        }
        *out = TE_COLOR_PAL(p->v[i + 2] & 0xFFu);
        return 2;
    }
    if (kind == 2) {
        size_t start = i + 2;
        if (colon) {
            /* Count the sub-parameters that follow the "2". */
            size_t cnt = 0;
            while (start + cnt < p->n && p->sub[start + cnt]) {
                cnt++;
            }
            if (cnt >= 4) {
                start++;   /* skip the colour-space id */
            } else if (cnt < 3) {
                return 1 + cnt;
            }
            size_t consumed = 1 + cnt;
            uint32_t r = p->v[start] & 0xFFu, g = p->v[start + 1] & 0xFFu, b = p->v[start + 2] & 0xFFu;
            *out = TE_COLOR_RGB((r << 16) | (g << 8) | b);
            return consumed;
        }
        if (start + 2 >= p->n) {
            return p->n - i - 1;
        }
        uint32_t r = p->v[start] & 0xFFu, g = p->v[start + 1] & 0xFFu, b = p->v[start + 2] & 0xFFu;
        *out = TE_COLOR_RGB((r << 16) | (g << 8) | b);
        return 4;
    }
    return 1;
}

static void set_ul(te_cell *pen, unsigned style)
{
    pen->attrs = (uint16_t)(((unsigned)pen->attrs & ~(unsigned)TE_ATTR_UL_MASK) | ((style & 7u) << TE_ATTR_UL_SHIFT));
}

static void sgr(te_term *t, const vt_params *p)
{
    te_cell *pen = &t->cur.pen;
    if (p->n == 0) {
        reset_pen(t);
        return;
    }
    for (size_t i = 0; i < p->n; i++) {
        uint16_t v = p->v[i];
        /* Skip stray sub-parameters that do not belong to a handled code. */
        if (p->sub[i]) {
            continue;
        }
        switch (v) {
        case 0:  reset_pen(t); break;
        case 1:  pen->attrs |= TE_ATTR_BOLD; break;
        case 2:  pen->attrs |= TE_ATTR_DIM; break;
        case 3:  pen->attrs |= TE_ATTR_ITALIC; break;
        case 4:
            if (i + 1 < p->n && p->sub[i + 1]) {
                uint16_t style = p->v[i + 1];
                set_ul(pen, style <= TE_UL_DASHED ? style : TE_UL_SINGLE);
                i++;
            } else {
                set_ul(pen, TE_UL_SINGLE);
            }
            break;
        case 5:
        case 6:  pen->attrs |= TE_ATTR_BLINK; break;
        case 7:  pen->attrs |= TE_ATTR_INVERSE; break;
        case 8:  pen->attrs |= TE_ATTR_HIDDEN; break;
        case 9:  pen->attrs |= TE_ATTR_STRIKE; break;
        case 21: set_ul(pen, TE_UL_DOUBLE); break;
        case 22: pen->attrs &= (uint16_t)~(TE_ATTR_BOLD | TE_ATTR_DIM); break;
        case 23: pen->attrs &= (uint16_t)~TE_ATTR_ITALIC; break;
        case 24: set_ul(pen, TE_UL_NONE); break;
        case 25: pen->attrs &= (uint16_t)~TE_ATTR_BLINK; break;
        case 27: pen->attrs &= (uint16_t)~TE_ATTR_INVERSE; break;
        case 28: pen->attrs &= (uint16_t)~TE_ATTR_HIDDEN; break;
        case 29: pen->attrs &= (uint16_t)~TE_ATTR_STRIKE; break;
        case 38: i += parse_ext_color(p, i, &pen->fg); break;
        case 39: pen->fg = TE_COLOR_DEFAULT; break;
        case 48: i += parse_ext_color(p, i, &pen->bg); break;
        case 49: pen->bg = TE_COLOR_DEFAULT; break;
        case 53: pen->attrs |= TE_ATTR_OVERLINE; break;
        case 55: pen->attrs &= (uint16_t)~TE_ATTR_OVERLINE; break;
        case 58: i += parse_ext_color(p, i, &pen->ul); break;
        case 59: pen->ul = TE_COLOR_DEFAULT; break;
        default:
            if (v >= 30 && v <= 37) {
                pen->fg = TE_COLOR_PAL(v - 30);
            } else if (v >= 40 && v <= 47) {
                pen->bg = TE_COLOR_PAL(v - 40);
            } else if (v >= 90 && v <= 97) {
                pen->fg = TE_COLOR_PAL(v - 90 + 8);
            } else if (v >= 100 && v <= 107) {
                pen->bg = TE_COLOR_PAL(v - 100 + 8);
            }
            break;
        }
    }
}

/* ------------------------------------------------------------------------- */
/* CSI dispatch */

/* Kitty keyboard protocol: CSI > f u (push), CSI < n u (pop), CSI = f ; m u (set). */
static void kitty_keyboard(te_term *t, const vt_params *p, uint8_t priv)
{
    size_t s = t->modes.alt_screen ? 1u : 0u;
    uint8_t *cur = &t->kitty_cur[s];
    uint8_t *depth = &t->kitty_depth[s];
    uint8_t *stack = t->kitty_stack[s];
    if (priv == '>') {
        if (*depth == TE_KITTY_STACK) {
            memmove(stack, stack + 1, TE_KITTY_STACK - 1);   /* full: forget the oldest */
            (*depth)--;
        }
        stack[(*depth)++] = *cur;
        *cur = (uint8_t)(param_or(p, 0, 0) & TE_KITTY_SUPPORTED);
    } else if (priv == '<') {
        uint16_t n = param_or(p, 0, 1);
        for (uint16_t i = 0; i < n; i++) {
            if (*depth == 0) {
                *cur = 0;
                break;
            }
            *cur = stack[--(*depth)];
        }
    } else {
        uint8_t f = (uint8_t)(param_or(p, 0, 0) & TE_KITTY_SUPPORTED);
        uint16_t mode = param_or(p, 1, 1);
        if (mode == 1) *cur = f;
        else if (mode == 2) *cur |= f;
        else if (mode == 3) *cur &= (uint8_t)~f;
    }
    t->modes.kitty_flags = *cur;
}

static void report_decrqm(te_term *t, uint16_t mode, bool private_mode)
{
    if (private_mode) {
        reply(t, "\x1b[?%u;%d$y", mode, private_mode_state(t, mode));
    } else {
        int st = 0;
        if (mode == 4) st = t->modes.insert ? 1 : 2;
        else if (mode == 20) st = t->modes.newline ? 1 : 2;
        reply(t, "\x1b[%u;%d$y", mode, st);
    }
}

static void window_ops(te_term *t, const vt_params *p)
{
    uint16_t op = param_or(p, 0, 0);
    uint16_t cw = 0;
    uint16_t ch = 0;
    if (t->host.cell_size != nullptr) {
        t->host.cell_size(t->host.user, &cw, &ch);
    }
    switch (op) {
    case 14: reply(t, "\x1b[4;%u;%ut", (unsigned)ch * t->rows, (unsigned)cw * t->cols); break;
    case 16: reply(t, "\x1b[6;%u;%ut", ch, cw); break;
    case 18: reply(t, "\x1b[8;%u;%ut", t->rows, t->cols); break;
    case 22:
        if (t->title_depth < TE_TITLE_STACK) {
            (void)str_copy(t->title_stack[t->title_depth], TE_TITLE_MAX, t->title);
            t->title_depth++;
        }
        break;
    case 23:
        if (t->title_depth > 0) {
            t->title_depth--;
            (void)str_copy(t->title, sizeof t->title, t->title_stack[t->title_depth]);
            if (t->host.set_title != nullptr) {
                t->host.set_title(t->host.user, t->title);
            }
        }
        break;
    /* 20/21 (report title) are deliberately unsupported: they enable injection attacks. */
    default:
        break;
    }
}

static void csi(void *user, const vt_params *p, const uint8_t *inter, size_t ninter, uint8_t priv, uint8_t final)
{
    te_term *t = user;
    uint8_t i0 = ninter > 0 ? inter[0] : 0;
    uint16_t n1 = param_or(p, 0, 1);
    int64_t x = t->cur.x;
    int64_t y = t->cur.y;

    if (i0 == ' ' && final == 'q' && priv == 0) {          /* DECSCUSR */
        uint16_t s = param_or(p, 0, 0);
        t->modes.decscusr = (uint8_t)(s <= 6 ? s : 0);
        t->dirty = true;
        return;
    }
    if (i0 == '!' && final == 'p') {                        /* DECSTR soft reset */
        t->modes.insert = false;
        t->modes.origin = false;
        t->modes.autowrap = true;
        t->modes.cursor_visible = true;
        t->modes.app_cursor = false;
        t->modes.app_keypad = false;
        t->top = 0;
        t->bottom = (uint16_t)(t->rows - 1);
        reset_pen(t);
        memset(t->cur.charset, 0, sizeof t->cur.charset);
        t->cur.gl = 0;
        t->saved_main = t->cur;
        t->saved_alt = t->cur;
        return;
    }
    if (i0 == '$' && final == 'p') {                        /* DECRQM */
        report_decrqm(t, param_or(p, 0, 0), priv == '?');
        return;
    }
    if (ninter != 0) {
        return;
    }
    if ((priv == '>' || priv == '<' || priv == '=') && final == 'u') {
        kitty_keyboard(t, p, priv);
        return;
    }
    if (priv == '>') {
        if (final == 'c') {                                  /* DA2 */
            reply(t, "\x1b[>1;100;0c");
        } else if (final == 'q') {                           /* XTVERSION */
            reply(t, "\x1bP>|%s %s\x1b\\", APP_DISPLAY_NAME, APP_VERSION);
        }
        return;
    }
    if (priv == '=') {
        if (final == 'c') {                                  /* DA3 */
            reply(t, "\x1bP!|00000000\x1b\\");
        }
        return;
    }
    if (priv == '?') {
        switch (final) {
        case 'h':
        case 'l':
            for (size_t i = 0; i < p->n; i++) {
                set_private_mode(t, p->v[i], final == 'h');
            }
            break;
        case 'n':
            if (param_or(p, 0, 0) == 6) {
                uint16_t ry = t->modes.origin ? (uint16_t)(t->cur.y - t->top) : t->cur.y;
                reply(t, "\x1b[?%u;%uR", (unsigned)ry + 1u, (unsigned)t->cur.x + 1u);
            }
            break;
        case 'u':                                            /* kitty keyboard: query */
            reply(t, "\x1b[?%uu", (unsigned)t->modes.kitty_flags);
            break;
        default:
            break;
        }
        return;
    }
    if (priv != 0) {
        return;
    }

    switch (final) {
    case '@': insert_blanks(t, n1); break;
    case 'A': {
        int64_t lim = y >= t->top ? t->top : 0;
        cursor_set(t, x, y - n1 < lim ? lim : y - n1);
        break;
    }
    case 'B': {
        int64_t lim = y <= t->bottom ? t->bottom : (int64_t)t->rows - 1;
        cursor_set(t, x, y + n1 > lim ? lim : y + n1);
        break;
    }
    case 'C':
    case 'a': cursor_set(t, x + n1, y); break;
    case 'D': cursor_set(t, x - n1, y); break;
    case 'E': cursor_set(t, 0, y + n1); break;
    case 'F': cursor_set(t, 0, y - n1); break;
    case 'G':
    case '`': cursor_set(t, (int64_t)n1 - 1, y); break;
    case 'H':
    case 'f': {
        int64_t row = (int64_t)param_or(p, 0, 1) - 1;
        int64_t col = (int64_t)param_or(p, 1, 1) - 1;
        if (t->modes.origin) {
            row += t->top;
        }
        cursor_set(t, col, row);
        break;
    }
    case 'I': tab_forward(t, n1); break;
    case 'J': erase_display(t, param_or(p, 0, 0)); break;
    case 'K': erase_line(t, param_or(p, 0, 0)); break;
    case 'L':
        if (t->cur.y >= t->top && t->cur.y <= t->bottom) {
            scroll_down(t, t->cur.y, t->bottom, n1);
            cursor_set(t, 0, t->cur.y);
        }
        break;
    case 'M':
        if (t->cur.y >= t->top && t->cur.y <= t->bottom) {
            scroll_up(t, t->cur.y, t->bottom, n1, false);
            cursor_set(t, 0, t->cur.y);
        }
        break;
    case 'P': delete_chars(t, n1); break;
    case 'S': scroll_up(t, t->top, t->bottom, n1, true); break;
    case 'T': scroll_down(t, t->top, t->bottom, n1); break;
    case 'X': {
        uint32_t end = (uint32_t)t->cur.x + n1;
        clear_range(t, t->cur.y, t->cur.x, (uint16_t)(end > t->cols ? t->cols : end));
        break;
    }
    case 'Z': tab_backward(t, n1); break;
    case 'b': {                                              /* REP */
        if (t->last_cp == 0) {
            break;
        }
        uint32_t cap = (uint32_t)t->cols * t->rows;
        uint32_t reps = n1 > cap ? cap : n1;
        for (uint32_t i = 0; i < reps; i++) {
            print_cp(t, t->last_cp);
        }
        break;
    }
    case 'c':
        if (param_or(p, 0, 0) == 0) {
            reply(t, "\x1b[?62;22c");                        /* VT220 with ANSI colour */
        }
        break;
    case 'd': {
        int64_t row = (int64_t)n1 - 1;
        if (t->modes.origin) {
            row += t->top;
        }
        cursor_set(t, x, row);
        break;
    }
    case 'e': cursor_set(t, x, y + n1); break;
    case 'g': {
        uint16_t m = param_or(p, 0, 0);
        if (m == 0 && t->cur.x < t->cols) {
            t->tabs[t->cur.x] = 0;
        } else if (m == 3) {
            memset(t->tabs, 0, t->cols);
        }
        break;
    }
    case 'h':
    case 'l':
        for (size_t i = 0; i < p->n; i++) {
            set_ansi_mode(t, p->v[i], final == 'h');
        }
        break;
    case 'm': sgr(t, p); break;
    case 'n':
        if (param_or(p, 0, 0) == 5) {
            reply(t, "\x1b[0n");
        } else if (param_or(p, 0, 0) == 6) {
            uint16_t ry = t->modes.origin ? (uint16_t)(t->cur.y - t->top) : t->cur.y;
            reply(t, "\x1b[%u;%uR", (unsigned)ry + 1u, (unsigned)t->cur.x + 1u);
        }
        break;
    case 'r': {
        uint16_t top = (uint16_t)(param_or(p, 0, 1) - 1);
        uint16_t bot = param_or(p, 1, t->rows);
        if (bot > t->rows) {
            bot = t->rows;
        }
        bot = (uint16_t)(bot - 1);
        if (top < bot) {
            t->top = top;
            t->bottom = bot;
            cursor_set(t, 0, t->modes.origin ? top : 0);
        }
        break;
    }
    case 's': cursor_save(t); break;
    case 'u': cursor_restore(t); break;
    case 't': window_ops(t, p); break;
    default:
        LOG_D("unhandled CSI %c", final);
        break;
    }
}

/* ------------------------------------------------------------------------- */
/* ESC dispatch */

static void full_reset(te_term *t)
{
    te_cell blank = { 0 };
    memset(&t->modes, 0, sizeof t->modes);
    t->modes.autowrap = true;
    t->modes.cursor_visible = true;
    t->modes.cursor_blink = true;
    t->grid = &t->main;
    memset(&t->cur, 0, sizeof t->cur);
    t->saved_main = t->cur;
    t->saved_alt = t->cur;
    t->top = 0;
    t->bottom = (uint16_t)(t->rows - 1);
    memcpy(t->palette, t->default_palette, sizeof t->palette);
    t->fg = t->default_fg;
    t->bg = t->default_bg;
    t->cursor_color = t->default_cursor;
    tabs_reset(t, 0);
    for (uint16_t y = 0; y < t->rows; y++) {
        line_fill(&t->main.lines[y], 0, t->main.lines[y].cols, blank);
        t->main.lines[y].flags = 0;
        t->main.lines[y].input_col = 0;
        line_fill(&t->alt.lines[y], 0, t->alt.lines[y].cols, blank);
        t->alt.lines[y].flags = 0;
        t->alt.lines[y].input_col = 0;
    }
    t->view_offset = 0;
    t->title_depth = 0;
    t->last_cp = 0;
    memset(t->kitty_cur, 0, sizeof t->kitty_cur);
    images_clear(&t->img);
    memset(t->kitty_depth, 0, sizeof t->kitty_depth);
    t->dirty = true;
}

static void esc(void *user, const uint8_t *inter, size_t ninter, uint8_t final)
{
    te_term *t = user;
    if (ninter == 1) {
        uint8_t i0 = inter[0];
        if (i0 >= '(' && i0 <= '+') {                         /* designate G0..G3 */
            size_t g = (size_t)(i0 - '(');
            t->cur.charset[g] = final == '0' ? CS_DEC_GRAPHICS : CS_ASCII;
        } else if (i0 == '#' && final == '8') {              /* DECALN */
            te_cell e = { 0 };
            e.cp = 'E';
            for (uint16_t yy = 0; yy < t->rows; yy++) {
                line_fill(line_at(t, yy), 0, t->cols, e);
            }
            t->top = 0;
            t->bottom = (uint16_t)(t->rows - 1);
            cursor_set(t, 0, 0);
        }
        return;
    }
    if (ninter != 0) {
        return;
    }
    switch (final) {
    case '7': cursor_save(t); break;
    case '8': cursor_restore(t); break;
    case 'D': index_down(t); t->dirty = true; break;
    case 'E': index_down(t); t->cur.x = 0; t->dirty = true; break;
    case 'H': if (t->cur.x < t->cols) t->tabs[t->cur.x] = 1; break;
    case 'M': reverse_index(t); t->dirty = true; break;
    case 'c': full_reset(t); break;
    case '=': t->modes.app_keypad = true; break;
    case '>': t->modes.app_keypad = false; break;
    case 'n': t->cur.gl = 2; break;
    case 'o': t->cur.gl = 3; break;
    case '\\': break;                                       /* ST: string already handled */
    default:
        LOG_D("unhandled ESC %c", final);
        break;
    }
}

/* ------------------------------------------------------------------------- */
/* C0 controls */

static void execute(void *user, uint8_t c)
{
    te_term *t = user;
    switch (c) {
    case 0x07:
        if (t->host.bell != nullptr) {
            t->host.bell(t->host.user);
        }
        break;
    case 0x08:
        if (t->cur.x > 0) {
            t->cur.x--;
        }
        t->cur.pending_wrap = false;
        break;
    case 0x09: tab_forward(t, 1); break;
    case 0x0A:
    case 0x0B:
    case 0x0C:
        index_down(t);
        if (t->modes.newline) {
            t->cur.x = 0;
        }
        break;
    case 0x0D: t->cur.x = 0; t->cur.pending_wrap = false; break;
    case 0x0E: t->cur.gl = 1; break;
    case 0x0F: t->cur.gl = 0; break;
    default: break;
    }
    t->dirty = true;
}

/* ------------------------------------------------------------------------- */
/* OSC */

/* Copy src[0..len) to dst as valid UTF-8 without control characters. */
static void sanitize_utf8(char *dst, size_t dstsz, const char *src, size_t len)
{
    te_utf8_dec d = { 0 };
    size_t o = 0;
    if (dstsz == 0) {
        return;
    }
    for (size_t i = 0; i < len; i++) {
        uint32_t cp = 0;
        te_utf8_res r = utf8_feed(&d, (uint8_t)src[i], &cp);
        if (r != TE_UTF8_DONE) {
            continue;
        }
        if (cp < 0x20 || (cp >= 0x7F && cp < 0xA0)) {
            continue;
        }
        char enc[4] = { 0 };
        size_t n = utf8_encode(cp, enc, sizeof enc);
        if (n == 0 || o + n >= dstsz) {
            break;
        }
        memcpy(dst + o, enc, n);
        o += n;
    }
    dst[o] = '\0';
}

static int hexdig(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* Parse "rgb:h/h/h" (1..4 hex digits per channel) or "#rrggbb". */
static bool parse_color_spec(const char *s, size_t len, uint32_t *out)
{
    if (len == 7 && s[0] == '#') {
        uint32_t v = 0;
        for (size_t i = 1; i < 7; i++) {
            int h = hexdig(s[i]);
            if (h < 0) return false;
            v = (v << 4) | (uint32_t)h;
        }
        *out = v;
        return true;
    }
    if (len < 9 || strncmp(s, "rgb:", 4) != 0) {
        return false;
    }
    uint32_t ch[3] = { 0 };
    size_t pos = 4;
    for (size_t c = 0; c < 3; c++) {
        uint32_t v = 0;
        size_t digits = 0;
        while (pos < len && s[pos] != '/') {
            int h = hexdig(s[pos]);
            if (h < 0 || digits >= 4) return false;
            v = (v << 4) | (uint32_t)h;
            digits++;
            pos++;
        }
        if (digits == 0) return false;
        uint32_t maxv = (1u << (4 * digits)) - 1u;
        ch[c] = (v * 255u + maxv / 2u) / maxv;
        if (c < 2) {
            if (pos >= len || s[pos] != '/') return false;
            pos++;
        }
    }
    if (pos != len) {
        return false;
    }
    *out = (ch[0] << 16) | (ch[1] << 8) | ch[2];
    return true;
}

static void reply_color(te_term *t, const char *prefix, uint32_t rgb)
{
    unsigned r = (rgb >> 16) & 0xFF, g = (rgb >> 8) & 0xFF, b = rgb & 0xFF;
    reply(t, "\x1b]%s;rgb:%02x%02x/%02x%02x/%02x%02x\x1b\\", prefix, r, r, g, g, b, b);
}

/* Parse a decimal number at s[*pos]; stops at ';'. */
static bool osc_number(const char *s, size_t len, size_t *pos, uint32_t max, uint32_t *out)
{
    uint32_t v = 0;
    size_t start = *pos;
    while (*pos < len && s[*pos] >= '0' && s[*pos] <= '9') {
        v = v * 10u + (uint32_t)(s[*pos] - '0');
        if (v > max) return false;
        (*pos)++;
    }
    if (*pos == start) return false;
    *out = v;
    return true;
}

static void osc_palette(te_term *t, const char *s, size_t len)
{
    size_t pos = 0;
    while (pos < len) {
        uint32_t idx = 0;
        if (!osc_number(s, len, &pos, 255, &idx) || pos >= len || s[pos] != ';') {
            return;
        }
        pos++;
        size_t start = pos;
        while (pos < len && s[pos] != ';') pos++;
        size_t speclen = pos - start;
        if (speclen == 1 && s[start] == '?') {
            char pre[16] = { 0 };
            if (str_fmt(pre, sizeof pre, "4;%u", idx) == TE_OK) {
                reply_color(t, pre, t->palette[idx]);
            }
        } else {
            uint32_t rgb = 0;
            if (parse_color_spec(s + start, speclen, &rgb)) {
                t->palette[idx] = rgb;
                t->dirty = true;
            }
        }
        if (pos < len) pos++;
    }
}

static size_t b64_decode(const char *in, size_t len, char *out, size_t outsz)
{
    uint32_t acc = 0;
    int bits = 0;
    size_t o = 0;
    for (size_t i = 0; i < len; i++) {
        char c = in[i];
        int v = -1;
        if (c >= 'A' && c <= 'Z') v = c - 'A';
        else if (c >= 'a' && c <= 'z') v = c - 'a' + 26;
        else if (c >= '0' && c <= '9') v = c - '0' + 52;
        else if (c == '+') v = 62;
        else if (c == '/') v = 63;
        else if (c == '=') break;
        else return SIZE_MAX;
        acc = (acc << 6) | (uint32_t)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (o + 1 >= outsz) return SIZE_MAX;
            out[o++] = (char)((acc >> bits) & 0xFFu);
        }
    }
    out[o] = '\0';
    return o;
}

static void osc_clipboard(te_term *t, const char *s, size_t len)
{
    /* Format: <targets>;<base64 | ?> */
    const char *semi = memchr(s, ';', len);
    if (semi == nullptr || t->host.clipboard_set == nullptr) {
        return;
    }
    size_t off = (size_t)(semi - s) + 1;
    const char *data = s + off;
    size_t dlen = len - off;
    if (dlen == 1 && data[0] == '?') {
        return;   /* reading the clipboard is never allowed */
    }
    size_t cap = dlen / 4 * 3 + 4;
    char *buf = te_malloc(cap);
    if (buf == nullptr) {
        return;
    }
    size_t n = b64_decode(data, dlen, buf, cap);
    if (n != SIZE_MAX && n > 0) {
        t->host.clipboard_set(t->host.user, buf);
    }
    TE_FREE(buf);
}

static void osc_cwd(te_term *t, const char *s, size_t len)
{
    /* file://host/path, percent-encoded */
    if (len < 8 || strncmp(s, "file://", 7) != 0) {
        return;
    }
    const char *path = memchr(s + 7, '/', len - 7);
    if (path == nullptr) {
        return;
    }
    size_t i = (size_t)(path - s);
    size_t o = 0;
    char out[sizeof t->cwd] = { 0 };
    while (i < len) {
        char c = s[i];
        if (c == '%' && i + 2 < len) {
            int h1 = hexdig(s[i + 1]), h2 = hexdig(s[i + 2]);
            if (h1 < 0 || h2 < 0 || (h1 == 0 && h2 == 0)) {
                return;
            }
            c = (char)((h1 << 4) | h2);
            i += 2;
        }
        if (o + 1 >= sizeof out) {
            return;
        }
        out[o++] = c;
        i++;
    }
    out[o] = '\0';
    (void)str_copy(t->cwd, sizeof t->cwd, out);
}

/*
 * OSC 8 ; params ; URI. An empty URI closes the link. URIs must be printable
 * ASCII (percent-encoded), at most TE_MAX_URI bytes. Identical (id, URI) pairs
 * share one table entry; when the table is full new links are shown as text.
 */
static void osc_hyperlink(te_term *t, const char *s, size_t len)
{
    const char *semi = memchr(s, ';', len);
    if (semi == nullptr) {
        return;
    }
    size_t plen = (size_t)(semi - s);
    const char *uri = semi + 1;
    size_t ulen = len - plen - 1;
    if (ulen == 0) {
        t->cur.pen.link = 0;
        return;
    }
    if (ulen > TE_MAX_URI) {
        t->cur.pen.link = 0;
        return;
    }
    for (size_t i = 0; i < ulen; i++) {
        unsigned char c = (unsigned char)uri[i];
        if (c < 0x21 || c > 0x7E) {
            t->cur.pen.link = 0;
            return;
        }
    }
    /* Optional id=... among colon-separated params. */
    char id[64] = { 0 };
    for (size_t i = 0; i + 3 <= plen; i++) {
        if ((i == 0 || s[i - 1] == ':') && memcmp(s + i, "id=", 3) == 0) {
            size_t j = i + 3;
            size_t k = 0;
            while (j < plen && s[j] != ':' && k + 1 < sizeof id) {
                id[k++] = s[j++];
            }
            id[k] = '\0';
            break;
        }
    }
    for (uint16_t i = 1; i <= t->nlinks; i++) {
        if (strcmp(t->links[i].id, id) == 0 && strlen(t->links[i].uri) == ulen &&
            memcmp(t->links[i].uri, uri, ulen) == 0) {
            t->cur.pen.link = i;
            return;
        }
    }
    if (t->nlinks >= TE_MAX_LINKS) {
        if (!t->links_full_logged) {
            LOG_W("hyperlink table full (%u); further links are shown as plain text", TE_MAX_LINKS);
            t->links_full_logged = true;
        }
        t->cur.pen.link = 0;
        return;
    }
    char *copy = te_malloc(ulen + 1);
    if (copy == nullptr) {
        t->cur.pen.link = 0;
        return;
    }
    memcpy(copy, uri, ulen);
    copy[ulen] = '\0';
    uint16_t idx = (uint16_t)(t->nlinks + 1);
    t->links[idx].uri = copy;
    (void)str_copy(t->links[idx].id, sizeof t->links[idx].id, id);
    t->nlinks = idx;
    t->cur.pen.link = idx;
}

/*
 * Desktop notifications:
 *   OSC 9 ; body                (iTerm2; "9;<digit>;..." forms are ConEmu progress, ignored)
 *   OSC 777 ; notify ; title ; body
 *   OSC 99 ; metadata ; payload (kitty; p=body sends the body, default is the title)
 */
static void osc_notify(te_term *t, uint32_t cmd, const char *s, size_t len)
{
    if (t->host.notify == nullptr) {
        return;
    }
    char title[128] = { 0 };
    char body[256] = { 0 };
    if (cmd == 9) {
        if (len >= 2 && s[0] >= '0' && s[0] <= '9' && s[1] == ';') {
            return;
        }
        sanitize_utf8(body, sizeof body, s, len);
    } else if (cmd == 777) {
        static const char pre[] = "notify;";
        if (len < sizeof pre - 1 || memcmp(s, pre, sizeof pre - 1) != 0) {
            return;
        }
        const char *rest = s + sizeof pre - 1;
        size_t rlen = len - (sizeof pre - 1);
        const char *semi = memchr(rest, ';', rlen);
        if (semi == nullptr) {
            sanitize_utf8(title, sizeof title, rest, rlen);
        } else {
            sanitize_utf8(title, sizeof title, rest, (size_t)(semi - rest));
            sanitize_utf8(body, sizeof body, semi + 1, rlen - (size_t)(semi - rest) - 1);
        }
    } else {
        const char *semi = memchr(s, ';', len);
        if (semi == nullptr) {
            return;
        }
        size_t mlen = (size_t)(semi - s);
        bool is_body = false;
        for (size_t i = 0; i + 6 <= mlen; i++) {
            if ((i == 0 || s[i - 1] == ':') && memcmp(s + i, "p=body", 6) == 0) {
                is_body = true;
            }
        }
        sanitize_utf8(is_body ? body : title, is_body ? sizeof body : sizeof title, semi + 1, len - mlen - 1);
    }
    if (title[0] == '\0' && body[0] == '\0') {
        return;
    }
    t->host.notify(t->host.user, title, body);
}

/* OSC 133 ; <A|B|C|D> [; key=value ...] */
static void osc_shell(te_term *t, const char *s, size_t len)
{
    if (len == 0) {
        return;
    }
    te_line *l = line_at(t, t->cur.y);
    t->shell_integration = true;
    switch (s[0]) {
    case 'A':
        if (l != nullptr) {
            l->flags |= TE_LINE_PROMPT;
        }
        t->shell_input = false;
        t->shell_click_events = false;
        /* Options follow as ;key=value pairs. */
        for (size_t i = 1; i + 1 < len; i++) {
            if (s[i] != ';') {
                continue;
            }
            static const char opt[] = "click_events=1";
            size_t rest = len - i - 1;
            if (rest >= sizeof opt - 1 && memcmp(s + i + 1, opt, sizeof opt - 1) == 0 &&
                (rest == sizeof opt - 1 || s[i + sizeof opt] == ';')) {
                t->shell_click_events = true;
            }
        }
        t->prompt_count++;
        break;
    case 'B':
        if (l != nullptr) {
            l->input_col = (uint16_t)(t->cur.x + 1u);
        }
        t->shell_input = true;
        break;
    case 'C':
    case 'D':
        t->shell_input = false;
        break;
    default:
        break;
    }
}

static void osc(void *user, const char *s, size_t len)
{
    te_term *t = user;
    size_t pos = 0;
    uint32_t cmd = 0;
    if (!osc_number(s, len, &pos, 9999, &cmd)) {
        return;
    }
    const char *arg = "";
    size_t alen = 0;
    if (pos < len && s[pos] == ';') {
        arg = s + pos + 1;
        alen = len - pos - 1;
    }
    switch (cmd) {
    case 0:
    case 2:
        sanitize_utf8(t->title, sizeof t->title, arg, alen);
        if (t->host.set_title != nullptr) {
            t->host.set_title(t->host.user, t->title);
        }
        break;
    case 4: osc_palette(t, arg, alen); break;
    case 7: osc_cwd(t, arg, alen); break;
    case 8: osc_hyperlink(t, arg, alen); break;
    case 10:
    case 11:
    case 12: {
        uint32_t *target = cmd == 10 ? &t->fg : cmd == 11 ? &t->bg : &t->cursor_color;
        if (alen == 1 && arg[0] == '?') {
            char pre[8] = { 0 };
            if (str_fmt(pre, sizeof pre, "%u", cmd) == TE_OK) {
                reply_color(t, pre, *target);
            }
        } else {
            uint32_t rgb = 0;
            if (parse_color_spec(arg, alen, &rgb)) {
                *target = rgb;
                t->dirty = true;
            }
        }
        break;
    }
    case 52: osc_clipboard(t, arg, alen); break;
    case 133: osc_shell(t, arg, alen); break;
    case 9:
    case 99:
    case 777: osc_notify(t, cmd, arg, alen); break;
    case 104:
        if (alen == 0) {
            memcpy(t->palette, t->default_palette, sizeof t->palette);
        } else {
            size_t p2 = 0;
            while (p2 < alen) {
                uint32_t idx = 0;
                if (!osc_number(arg, alen, &p2, 255, &idx)) break;
                t->palette[idx] = t->default_palette[idx];
                if (p2 < alen && arg[p2] == ';') p2++;
            }
        }
        t->dirty = true;
        break;
    case 110: t->fg = t->default_fg; t->dirty = true; break;
    case 111: t->bg = t->default_bg; t->dirty = true; break;
    case 112: t->cursor_color = t->default_cursor; t->dirty = true; break;
    default:
        LOG_D("unhandled OSC %u", cmd);
        break;
    }
}

/* ------------------------------------------------------------------------- */
/* DCS */

/* ------------------------------------------------------------------------- */
/* Inline images */

static void cell_px(const te_term *t, uint16_t *cw, uint16_t *ch)
{
    *cw = 0;
    *ch = 0;
    if (t->host.cell_size != nullptr) {
        t->host.cell_size(t->host.user, cw, ch);
    }
    if (*cw == 0) *cw = 10;
    if (*ch == 0) *ch = 20;
}

/*
 * Place an image at the cursor: blank the text it covers and move the cursor
 * below it (sixel) or after it (kitty). The image stays pinned to its
 * absolute row, so it scrolls with the text.
 */
static void place_image(te_term *t, te_image *im, uint16_t cols, uint16_t rows, bool kitty, bool move_cursor)
{
    uint16_t cw = 0;
    uint16_t ch = 0;
    cell_px(t, &cw, &ch);
    uint32_t c = cols != 0 ? cols : (im->w + cw - 1u) / cw;
    uint32_t r = rows != 0 ? rows : (im->h + ch - 1u) / ch;
    uint32_t room = (uint32_t)(t->cols - t->cur.x);
    c = TE_CLAMP(c, 1u, room > 0 ? room : 1u);
    r = TE_CLAMP(r, 1u, 1000u);
    im->placed = true;
    im->alt = t->modes.alt_screen;
    im->col = t->cur.x;
    im->cols = (uint16_t)c;
    im->rows = (uint16_t)r;
    im->row = t->sb_base + t->sb_count + t->cur.y;
    uint16_t x0 = t->cur.x;
    for (uint32_t i = 0; i < r; i++) {
        clear_range(t, t->cur.y, x0, (uint16_t)TE_MIN((uint32_t)x0 + c, (uint32_t)t->cols));
        if (i + 1 < r) {
            index_down(t);
        }
    }
    if (!move_cursor) {
        /* kitty C=1: the cursor stays where the image starts. Making room above
         * moved it to the last row (scrolling if needed), so go back up. */
        uint16_t up = (uint16_t)(r - 1u);
        t->cur.y = t->cur.y >= up ? (uint16_t)(t->cur.y - up) : 0;
        t->cur.x = x0;
        t->cur.pending_wrap = false;
        t->dirty = true;
        return;
    }
    if (kitty) {
        uint32_t nx = (uint32_t)x0 + c;
        t->cur.x = (uint16_t)(nx < t->cols ? nx : t->cols - 1u);
    } else {
        index_down(t);
        t->cur.x = 0;
    }
    t->cur.pending_wrap = false;
    t->dirty = true;
}

static void sixel(te_term *t, const vt_params *p, const char *data, size_t len)
{
    uint8_t *rgba = nullptr;
    uint32_t w = 0;
    uint32_t h = 0;
    te_err rc = sixel_decode(p, data, len, t->bg, &rgba, &w, &h);
    if (rc != TE_OK) {
        LOG_D("sixel image rejected: %s", te_strerror(rc));
        return;
    }
    te_image *im = images_add(&t->img, 0, w, h, rgba);   /* takes rgba */
    if (im != nullptr) {
        place_image(t, im, 0, 0, false, true);
    }
}

static void kitty_reply(te_term *t, const te_kitty_cmd *c)
{
    if (c->id == 0) {
        return;   /* kitty only answers commands that carry an id */
    }
    if (c->err[0] != '\0') {
        if (c->quiet < 2) {
            reply(t, "\x1b_Gi=%u;%s\x1b\\", c->id, c->err);
        }
    } else if (c->quiet < 1) {
        reply(t, "\x1b_Gi=%u;OK\x1b\\", c->id);
    }
}

static void apc(void *user, const char *data, size_t len)
{
    te_term *t = user;
    if (len == 0 || data[0] != 'G') {
        return;   /* only the kitty graphics protocol uses APC here */
    }
    te_kitty_cmd c;
    kitty_command(&t->img, data + 1, len - 1, &c);
    if (!c.ready) {
        return;
    }
    if (c.err[0] != '\0') {
        TE_FREE(c.pixels);
        kitty_reply(t, &c);
        return;
    }
    switch (c.action) {
    case 't':
    case 'T': {
        te_image *im = images_add(&t->img, c.id, c.w, c.h, c.pixels);   /* takes pixels */
        c.pixels = nullptr;
        if (im == nullptr) {
            (void)str_copy(c.err, sizeof c.err, "ENOMEM:image not stored");
        } else if (c.action == 'T') {
            place_image(t, im, c.cols, c.rows, true, !c.no_move);
        }
        break;
    }
    case 'p': {
        te_image *im = images_find(&t->img, c.id);
        if (im == nullptr) {
            (void)str_copy(c.err, sizeof c.err, "ENOENT:no such image");
        } else {
            place_image(t, im, c.cols, c.rows, true, !c.no_move);
        }
        break;
    }
    case 'd':
        if (c.del == 'i' || c.del == 'I') {
            images_delete_id(&t->img, c.id);
        } else {
            images_clear(&t->img);
        }
        t->dirty = true;
        break;
    case 'q':
    default:
        break;
    }
    TE_FREE(c.pixels);
    kitty_reply(t, &c);
}

static void dcs(void *user, const vt_params *p, const uint8_t *inter, size_t ninter, uint8_t priv,
                uint8_t final, const char *data, size_t len)
{
    te_term *t = user;
    (void)priv;
    if (ninter == 0 && priv == 0 && final == 'q') {             /* sixel image */
        sixel(t, p, data, len);
        return;
    }
    if (ninter == 1 && inter[0] == '$' && final == 'q') {     /* DECRQSS */
        if (len == 1 && data[0] == 'm') {
            reply(t, "\x1bP1$r0m\x1b\\");
        } else if (len == 1 && data[0] == 'r') {
            reply(t, "\x1bP1$r%u;%ur\x1b\\", (unsigned)t->top + 1u, (unsigned)t->bottom + 1u);
        } else if (len == 2 && data[0] == ' ' && data[1] == 'q') {
            reply(t, "\x1bP1$r%u q\x1b\\", (unsigned)t->modes.decscusr);
        } else {
            reply(t, "\x1bP0$r\x1b\\");
        }
    }
}

/* ------------------------------------------------------------------------- */
/* Public API */

static void print_cb(void *user, uint32_t cp)
{
    print_cp(user, cp);
}

static const vt_callbacks g_callbacks = {
    .print = print_cb,
    .execute = execute,
    .csi = csi,
    .esc = esc,
    .osc = osc,
    .dcs = dcs,
    .apc = apc,
};

void term_set_theme(te_term *t, const te_theme *theme)
{
    theme_build_palette(theme, t->default_palette);
    memcpy(t->palette, t->default_palette, sizeof t->palette);
    t->default_fg = t->fg = theme->fg;
    t->default_bg = t->bg = theme->bg;
    t->default_cursor = t->cursor_color = theme->cursor;
    t->dirty = true;
}

te_err term_create(te_term **out, uint16_t cols, uint16_t rows, uint32_t scrollback,
                   const te_theme *theme, const te_term_host *host)
{
    if (out == nullptr || theme == nullptr || cols < 2 || rows < 1) {
        return TE_ERR_INVAL;
    }
    *out = nullptr;
    te_term *t = te_calloc(1, sizeof *t);
    if (t == nullptr) {
        return TE_ERR_NOMEM;
    }
    te_err rc = TE_OK;
    te_cell blank = { 0 };
    t->cols = cols;
    t->rows = rows;
    if (host != nullptr) {
        t->host = *host;
    }
    rc = grid_alloc(&t->main, rows, cols, blank);
    if (rc != TE_OK) goto fail;
    rc = grid_alloc(&t->alt, rows, cols, blank);
    if (rc != TE_OK) goto fail;
    t->tabs = te_calloc(cols, 1);
    if (t->tabs == nullptr) {
        rc = TE_ERR_NOMEM;
        goto fail;
    }
    if (scrollback > 0) {
        t->sb = te_calloc(scrollback, sizeof *t->sb);
        if (t->sb == nullptr) {
            rc = TE_ERR_NOMEM;
            goto fail;
        }
        t->sb_cap = scrollback;
    }
    term_set_theme(t, theme);
    full_reset(t);
    vt_parser_init(&t->parser, &g_callbacks, t);
    images_init(&t->img);
    *out = t;
    return TE_OK;

fail:
    term_destroy(&t);
    return rc;
}

void term_destroy(te_term **tp)
{
    if (tp == nullptr || *tp == nullptr) {
        return;
    }
    te_term *t = *tp;
    vt_parser_destroy(&t->parser);
    images_free(&t->img);
    for (uint16_t i = 1; i <= t->nlinks; i++) {
        TE_FREE(t->links[i].uri);
    }
    t->nlinks = 0;
    grid_free(&t->main);
    grid_free(&t->alt);
    sb_clear(t);
    TE_FREE(t->sb);
    TE_FREE(t->tabs);
    TE_FREE(*tp);
}

void term_feed(te_term *t, const uint8_t *data, size_t len)
{
    if (t == nullptr || data == nullptr) {
        return;
    }
    t->content_gen++;
    vt_parser_feed(&t->parser, data, len);
    if (t->img.n > 0) {
        images_prune(&t->img, t->sb_base);   /* images that scrolled out of history */
    }
}

size_t term_image_count(const te_term *t)
{
    return t->img.n;
}

const te_image *term_image_at(const te_term *t, size_t i)
{
    return i < t->img.n ? &t->img.items[i] : nullptr;
}

/* Resize one grid in place. Lines that fall off the top go to scrollback (main screen). */
static te_err grid_resize(te_term *t, te_grid *g, uint16_t cols, uint16_t rows, bool is_main, uint16_t *cursor_y)
{
    te_cell blank = { 0 };
    uint16_t old_rows = g->rows;
    te_line *nl = te_calloc(rows, sizeof *nl);
    if (nl == nullptr) {
        return TE_ERR_NOMEM;
    }
    /* How many top lines to drop so the cursor stays visible. */
    uint16_t shift = 0;
    if (rows < old_rows && *cursor_y >= rows) {
        shift = (uint16_t)(*cursor_y - rows + 1);
    }
    for (uint16_t i = 0; i < shift && is_main; i++) {
        (void)sb_push(t, &g->lines[i]);   /* lines not taken are freed below */
    }
    uint16_t keep = (uint16_t)TE_MIN((uint32_t)old_rows - shift, (uint32_t)rows);
    for (uint16_t i = 0; i < keep; i++) {
        nl[i] = g->lines[i + shift];
        g->lines[i + shift].cells = nullptr;
    }
    /* Free lines that no longer fit (including shifted ones the scrollback did not take). */
    for (uint16_t i = 0; i < old_rows; i++) {
        line_free(&g->lines[i]);
    }
    te_err rc = TE_OK;
    for (uint16_t i = keep; i < rows; i++) {
        if (line_alloc(&nl[i], cols, blank) != TE_OK) {
            rc = TE_ERR_NOMEM;
            nl[i].cells = nullptr;
            nl[i].cols = 0;
        }
    }
    for (uint16_t i = 0; i < keep; i++) {
        if (line_ensure(&nl[i], cols, blank) != TE_OK) {
            rc = TE_ERR_NOMEM;
        }
    }
    TE_FREE(g->lines);
    g->lines = nl;
    g->rows = rows;
    *cursor_y = (uint16_t)(*cursor_y >= shift ? *cursor_y - shift : 0);
    return rc;
}

/* ------------------------------------------------------------------------- */
/* Reflow: re-wrap the main screen and scrollback to a new width */

typedef struct rf_out {
    te_line *lines;
    size_t   n, cap;
    uint16_t cols;
} rf_out;

static void rf_free(rf_out *o)
{
    for (size_t i = 0; i < o->n; i++) {
        line_free(&o->lines[i]);
    }
    TE_FREE(o->lines);
    o->n = 0;
    o->cap = 0;
}

static te_line *rf_new_line(rf_out *o)
{
    if (o->n == o->cap) {
        size_t ncap = o->cap == 0 ? 256 : o->cap * 2;
        void *p = o->lines;
        if (te_realloc_array(&p, o->cap, ncap, sizeof(te_line)) != TE_OK) {
            return nullptr;
        }
        o->lines = p;
        o->cap = ncap;
    }
    te_line *l = &o->lines[o->n];
    te_cell blank = { 0 };
    if (line_alloc(l, o->cols, blank) != TE_OK) {
        return nullptr;
    }
    o->n++;
    return l;
}

/* Source line i (scrollback first, then the main grid). Never nullptr: out of range gives an empty line. */
static const te_line *rf_src(const te_term *t, size_t i)
{
    static const te_line empty = { .cells = nullptr, .cols = 0, .flags = 0, .input_col = 0 };
    const te_line *l = nullptr;
    if (i < t->sb_count) {
        l = sb_line(t, (uint32_t)i);
    } else if (i - t->sb_count < t->main.rows) {
        l = &t->main.lines[i - t->sb_count];
    }
    return l != nullptr ? l : &empty;
}

static bool cell_blank(const te_cell *c)
{
    return c->cp == 0 && TE_COLOR_KIND(c->bg) == 0 && (c->attrs & ~(unsigned)TE_ATTR_SPACER) == 0;
}

/* Number of cells of source line k that belong to its logical line. */
static uint16_t rf_seg_len(const te_term *t, size_t k, size_t nsrc)
{
    const te_line *l = rf_src(t, k);
    if (l->cells == nullptr) {
        return 0;
    }
    uint16_t n = l->cols;
    if ((l->flags & TE_LINE_WRAPPED) != 0) {
        /* Padding left when a wide character did not fit at the end of the row. */
        const te_line *next = k + 1 < nsrc ? rf_src(t, k + 1) : nullptr;
        if (n > 0 && l->cells[n - 1].cp == 0 && next != nullptr && next->cells != nullptr && next->cols > 0 &&
            (next->cells[0].attrs & TE_ATTR_WIDE) != 0) {
            n--;
        }
        return n;
    }
    while (n > 0 && cell_blank(&l->cells[n - 1])) {
        n--;
    }
    return n;
}

/*
 * Rebuild the main grid and scrollback at `cols` columns. Everything new is
 * allocated before anything old is freed: on failure the terminal is unchanged
 * and TE_ERR_NOMEM is returned (the caller then resizes without reflow).
 */
static te_err reflow_main(te_term *t, uint16_t cols, uint16_t rows, uint16_t *cx, uint16_t *cy)
{
    size_t nsrc_all = (size_t)t->sb_count + t->main.rows;
    size_t cur_src = (size_t)t->sb_count + *cy;
    /* Ignore empty rows below both the cursor and the last text. */
    size_t limit = cur_src + 1;
    for (size_t i = nsrc_all; i > cur_src + 1; i--) {
        const te_line *l = rf_src(t, i - 1);
        bool empty = true;
        for (uint16_t x = 0; l->cells != nullptr && x < l->cols && empty; x++) {
            empty = cell_blank(&l->cells[x]);
        }
        if (!empty) {
            limit = i;
            break;
        }
    }
    if (limit > nsrc_all) {
        limit = nsrc_all;
    }

    rf_out o = { .lines = nullptr, .n = 0, .cap = 0, .cols = cols };
    /* Where each source row's first cell lands, so inline images follow their text. */
    size_t *rowmap = te_calloc(limit > 0 ? limit : 1, sizeof *rowmap);
    size_t new_cy_line = 0;
    uint16_t new_cx = 0;
    bool cursor_set_flag = false;

    for (size_t i = 0; i < limit;) {
        /* Logical line: source lines i..j joined by soft wraps. */
        size_t j = i;
        while (j + 1 < limit && (rf_src(t, j)->flags & TE_LINE_WRAPPED) != 0) {
            j++;
        }
        te_line *dst = rf_new_line(&o);
        if (dst == nullptr) {
            goto fail;
        }
        dst->flags = (uint8_t)(rf_src(t, i)->flags & TE_LINE_PROMPT);
        uint16_t x = 0;
        uint32_t logical = 0;
        uint32_t cursor_off = UINT32_MAX;
        uint32_t input_off = UINT32_MAX;
        uint32_t seg_start = 0;
        for (size_t k = i; k <= j; k++) {
            const te_line *src = rf_src(t, k);
            uint16_t len = rf_seg_len(t, k, limit);
            if (k == cur_src) {
                cursor_off = seg_start + *cx;
            }
            if (src->input_col != 0) {
                input_off = seg_start + (uint32_t)(src->input_col - 1u);
            }
            if (rowmap != nullptr) {
                rowmap[k] = o.n - 1;
            }
            for (uint16_t sx = 0; sx < len; sx++) {
                const te_cell *c = &src->cells[sx];
                bool wide = (c->attrs & TE_ATTR_WIDE) != 0;
                if (x >= cols || (wide && x + 1u >= cols)) {
                    dst->flags |= TE_LINE_WRAPPED;
                    dst = rf_new_line(&o);
                    if (dst == nullptr) {
                        goto fail;
                    }
                    x = 0;
                }
                if (sx == 0 && rowmap != nullptr) {
                    rowmap[k] = o.n - 1;   /* the segment starts on a new output line */
                }
                if (logical == cursor_off) {
                    new_cy_line = o.n - 1;
                    new_cx = x;
                    cursor_set_flag = true;
                }
                if (logical == input_off) {
                    dst->input_col = (uint16_t)(x + 1u);
                }
                dst->cells[x++] = *c;
                logical++;
            }
            seg_start += len;
        }
        /* Cursor beyond the text (e.g. after typed spaces or on an empty line). */
        if (cursor_off != UINT32_MAX && !cursor_set_flag) {
            uint32_t extra = cursor_off - logical;
            uint32_t pos = (uint32_t)x + extra;
            while (pos >= cols) {
                dst->flags |= TE_LINE_WRAPPED;
                dst = rf_new_line(&o);
                if (dst == nullptr) {
                    goto fail;
                }
                pos -= cols;
            }
            new_cy_line = o.n - 1;
            new_cx = (uint16_t)pos;
            cursor_set_flag = true;
        }
        i = j + 1;
    }
    if (!cursor_set_flag) {
        new_cy_line = o.n > 0 ? o.n - 1 : 0;
        new_cx = 0;
    }

    /* Choose the grid window: as much text as fits, never hiding the cursor. */
    size_t grid_start = o.n > rows ? o.n - rows : 0;
    if (grid_start > new_cy_line) {
        grid_start = new_cy_line;
    }
    te_line *grid = te_calloc(rows, sizeof *grid);
    if (grid == nullptr) {
        goto fail;
    }
    te_cell blank = { 0 };
    for (uint16_t r = 0; r < rows; r++) {
        size_t src = grid_start + r;
        if (src < o.n) {
            grid[r] = o.lines[src];
            o.lines[src].cells = nullptr;
        } else if (line_alloc(&grid[r], cols, blank) != TE_OK) {
            for (uint16_t q = 0; q < r; q++) {
                if (grid_start + q < o.n) {
                    o.lines[grid_start + q] = grid[q];   /* hand back for rf_free */
                } else {
                    line_free(&grid[q]);
                }
            }
            TE_FREE(grid);
            goto fail;
        }
    }

    /* Commit: replace scrollback and the main grid. Nothing below can fail. */
    uint64_t base = t->sb_base;
    size_t out_lines = o.n;
    for (uint32_t i = 0; i < t->sb_cap; i++) {
        line_free(&t->sb[i]);
    }
    size_t keep = grid_start;
    size_t drop = keep > t->sb_cap ? keep - t->sb_cap : 0;
    for (size_t i = 0; i < drop; i++) {
        line_free(&o.lines[i]);
    }
    t->sb_head = 0;
    t->sb_count = 0;
    for (size_t i = drop; i < keep; i++) {
        t->sb[t->sb_count++] = o.lines[i];
        o.lines[i].cells = nullptr;
    }
    t->sb_base = base + drop;
    for (size_t i = grid_start + rows; i < o.n; i++) {
        line_free(&o.lines[i]);   /* below a cursor kept near the top */
    }
    TE_FREE(o.lines);
    for (uint16_t i = 0; i < t->main.rows; i++) {
        line_free(&t->main.lines[i]);
    }
    TE_FREE(t->main.lines);
    t->main.lines = grid;
    t->main.rows = rows;
    *cy = (uint16_t)(new_cy_line - grid_start);
    *cx = new_cx < cols ? new_cx : (uint16_t)(cols - 1);
    t->view_offset = 0;
    /* Move main-screen images to the rows their text went to. */
    for (size_t i = 0; i < t->img.n; i++) {
        te_image *im = &t->img.items[i];
        if (!im->placed || im->alt) {
            continue;
        }
        if (rowmap == nullptr || im->row < base || im->col >= cols) {
            im->placed = false;   /* cannot follow its text: hide it */
            continue;
        }
        uint64_t k = im->row - base;
        im->row = base + (k < limit ? (uint64_t)rowmap[k] : (uint64_t)out_lines + (k - limit));
        if ((uint32_t)im->col + im->cols > cols) {
            im->cols = (uint16_t)(cols - im->col);
        }
    }
    images_prune(&t->img, t->sb_base);
    TE_FREE(rowmap);
    return TE_OK;

fail:
    TE_FREE(rowmap);
    rf_free(&o);
    return TE_ERR_NOMEM;
}

te_err term_resize(te_term *t, uint16_t cols, uint16_t rows)
{
    if (t == nullptr || cols < 2 || rows < 1) {
        return TE_ERR_INVAL;
    }
    if (cols == t->cols && rows == t->rows) {
        return TE_OK;
    }
    void *tabs = t->tabs;
    te_err rc = te_realloc_array(&tabs, t->cols, cols, 1);
    if (rc != TE_OK) {
        return rc;
    }
    t->tabs = tabs;

    uint16_t main_y = t->grid == &t->main ? t->cur.y : t->saved_main.y;
    uint16_t alt_y = t->grid == &t->alt ? t->cur.y : t->saved_alt.y;
    uint16_t old_cols = t->cols;
    uint16_t main_x = t->grid == &t->main ? t->cur.x : t->saved_main.x;
    te_err r1 = TE_ERR_NOMEM;
    if (cols != old_cols) {
        r1 = reflow_main(t, cols, rows, &main_x, &main_y);   /* images move with their rows */
        if (r1 == TE_OK) {
            if (t->grid == &t->main) {
                t->cur.x = main_x;
            } else {
                t->saved_main.x = main_x;
            }
        } else {
            LOG_W("reflow failed (%s); resizing without re-wrapping", te_strerror(r1));
            images_drop_screen(&t->img, false);   /* without re-wrapping, images would no longer line up */
        }
    }
    t->cols = cols;    /* sb_push sizes replacement buffers by t->cols */
    if (r1 != TE_OK) {
        r1 = grid_resize(t, &t->main, cols, rows, true, &main_y);
    }
    te_err r2 = grid_resize(t, &t->alt, cols, rows, false, &alt_y);
    t->rows = rows;
    if (cols > old_cols) {
        tabs_reset(t, old_cols);
    }

    if (t->grid == &t->main) {
        t->cur.y = main_y;
        t->saved_alt.y = (uint16_t)TE_MIN(alt_y, rows - 1);
    } else {
        t->cur.y = alt_y;
        t->saved_main.y = (uint16_t)TE_MIN(main_y, rows - 1);
    }
    if (t->cur.y >= rows) t->cur.y = (uint16_t)(rows - 1);
    if (t->cur.x >= cols) t->cur.x = (uint16_t)(cols - 1);
    if (t->saved_main.x >= cols) t->saved_main.x = (uint16_t)(cols - 1);
    if (t->saved_alt.x >= cols) t->saved_alt.x = (uint16_t)(cols - 1);
    if (t->saved_main.y >= rows) t->saved_main.y = (uint16_t)(rows - 1);
    if (t->saved_alt.y >= rows) t->saved_alt.y = (uint16_t)(rows - 1);
    t->cur.pending_wrap = false;
    t->top = 0;
    t->bottom = (uint16_t)(rows - 1);
    if (t->view_offset > t->sb_count) {
        t->view_offset = t->sb_count;
    }
    t->dirty = true;
    return r1 != TE_OK ? r1 : r2;
}

const uint8_t *term_output(const te_term *t, size_t *len)
{
    *len = t->out_len;
    return t->out;
}

void term_output_consume(te_term *t, size_t n)
{
    if (n >= t->out_len) {
        t->out_len = 0;
        return;
    }
    memmove(t->out, t->out + n, t->out_len - n);
    t->out_len -= n;
}

uint16_t term_cols(const te_term *t) { return t->cols; }
uint16_t term_rows(const te_term *t) { return t->rows; }

const te_line *term_view_line(const te_term *t, uint16_t row)
{
    if (row >= t->rows) {
        return nullptr;
    }
    if (t->view_offset == 0 || t->grid != &t->main) {
        return &t->grid->lines[row];
    }
    uint32_t idx = t->sb_count - t->view_offset + row;
    if (idx < t->sb_count) {
        return sb_line(t, idx);
    }
    uint32_t gy = idx - t->sb_count;
    if (gy >= t->main.rows) {
        return nullptr;
    }
    return &t->main.lines[gy];
}

void term_scroll_view(te_term *t, int delta)
{
    if (t->grid != &t->main) {
        return;
    }
    int64_t v = (int64_t)t->view_offset + delta;
    v = TE_CLAMP(v, 0, (int64_t)t->sb_count);
    if ((uint32_t)v != t->view_offset) {
        t->view_offset = (uint32_t)v;
        t->dirty = true;
    }
}

void term_view_reset(te_term *t)
{
    if (t->view_offset != 0) {
        t->view_offset = 0;
        t->dirty = true;
    }
}

uint32_t term_view_offset(const te_term *t) { return t->view_offset; }
uint32_t term_scrollback_count(const te_term *t) { return t->sb_count; }

void term_cursor(const te_term *t, uint16_t *x, uint16_t *y)
{
    *x = t->cur.x;
    *y = t->cur.y;
}

const te_modes *term_modes(const te_term *t) { return &t->modes; }
const char *term_title(const te_term *t) { return t->title; }

uint32_t term_resolve_color(const te_term *t, uint32_t c, bool is_fg)
{
    switch (TE_COLOR_KIND(c)) {
    case 1: return t->palette[c & 0xFFu];
    case 2: return c & 0xFFFFFFu;
    default: return is_fg ? t->fg : t->bg;
    }
}

uint32_t term_default_fg(const te_term *t) { return t->fg; }
uint32_t term_default_bg(const te_term *t) { return t->bg; }
uint32_t term_cursor_color(const te_term *t) { return t->cursor_color; }

const char *term_cwd(const te_term *t)
{
    return t->cwd;
}

const char *term_link_uri(const te_term *t, uint16_t id)
{
    if (id == 0 || id > t->nlinks) {
        return nullptr;
    }
    return t->links[id].uri;
}

uint64_t term_abs_first(const te_term *t)
{
    return t->grid == &t->main ? t->sb_base : t->sb_base + t->sb_count;
}

uint64_t term_abs_end(const te_term *t)
{
    return t->sb_base + t->sb_count + t->rows;
}

uint64_t term_view_abs(const te_term *t, uint16_t row)
{
    uint32_t off = t->grid == &t->main ? t->view_offset : 0;
    return t->sb_base + t->sb_count - off + row;
}

const te_line *term_abs_line(const te_term *t, uint64_t abs)
{
    uint64_t grid0 = t->sb_base + t->sb_count;
    if (abs >= grid0) {
        uint64_t y = abs - grid0;
        return y < t->grid->rows ? &t->grid->lines[y] : nullptr;
    }
    if (t->grid != &t->main || abs < t->sb_base) {
        return nullptr;
    }
    return sb_line(t, (uint32_t)(abs - t->sb_base));
}

void term_scroll_to_abs(te_term *t, uint64_t abs, bool at_top)
{
    if (t->grid != &t->main) {
        return;
    }
    uint64_t grid0 = t->sb_base + t->sb_count;
    if (abs < t->sb_base) {
        abs = t->sb_base;
    }
    /* Already visible with the current offset? */
    uint64_t vtop = grid0 - t->view_offset;
    if (!at_top && abs >= vtop && abs < vtop + t->rows) {
        return;
    }
    int64_t want = (int64_t)grid0 - (int64_t)abs;
    if (!at_top) {
        want += t->rows / 2;
    }
    want = TE_CLAMP(want, 0, (int64_t)t->sb_count);
    if ((uint32_t)want != t->view_offset) {
        t->view_offset = (uint32_t)want;
        t->dirty = true;
    }
}

bool term_find_prompt(const te_term *t, uint64_t from, int dir, uint64_t *out)
{
    uint64_t first = term_abs_first(t);
    uint64_t end = term_abs_end(t);
    if (dir == 0 || end == first) {
        return false;
    }
    uint64_t a = from;
    for (uint64_t steps = 0; steps < end - first; steps++) {
        if (dir < 0) {
            if (a <= first) return false;
            a--;
        } else {
            if (a + 1 >= end) return false;
            a++;
        }
        const te_line *l = term_abs_line(t, a);
        if (l != nullptr && (l->flags & TE_LINE_PROMPT) != 0) {
            *out = a;
            return true;
        }
    }
    return false;
}

uint64_t term_content_gen(const te_term *t) { return t->content_gen; }

bool term_shell_integration(const te_term *t) { return t->shell_integration; }
bool term_shell_input_active(const te_term *t) { return t->shell_input; }
bool term_shell_click_events(const te_term *t) { return t->shell_click_events; }
uint32_t term_prompt_count(const te_term *t) { return t->prompt_count; }

const te_line *term_grid_line(const te_term *t, uint16_t y)
{
    return y < t->grid->rows ? &t->grid->lines[y] : nullptr;
}

bool term_is_dirty(const te_term *t) { return t->dirty; }
void term_clear_dirty(te_term *t) { t->dirty = false; }
void term_mark_dirty(te_term *t) { t->dirty = true; }
