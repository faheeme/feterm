#include "ui/selection.h"

#include <string.h>

#include "util/mem.h"
#include "util/utf8.h"

void sel_clear(te_selection *s)
{
    memset(s, 0, sizeof *s);
}

static bool pos_less(te_selpos a, te_selpos b)
{
    return a.row < b.row || (a.row == b.row && a.col < b.col);
}

static const te_cell *cell_at(const te_term *t, uint64_t row, uint16_t col)
{
    const te_line *l = term_abs_line(t, row);
    if (l == nullptr || l->cells == nullptr || col >= l->cols) {
        return nullptr;
    }
    return &l->cells[col];
}

static bool is_word_cell(const te_term *t, uint64_t row, uint16_t col, const char *word_chars)
{
    const te_cell *c = cell_at(t, row, col);
    if (c == nullptr) {
        return false;
    }
    if ((c->attrs & TE_ATTR_SPACER) != 0) {
        return col > 0 && is_word_cell(t, row, (uint16_t)(col - 1), word_chars);
    }
    uint32_t cp = c->cp;
    if (cp == 0 || cp == ' ') {
        return false;
    }
    if (cp >= 0x80) {
        return true;   /* letters of other scripts, CJK, emoji */
    }
    if ((cp >= 'a' && cp <= 'z') || (cp >= 'A' && cp <= 'Z') || (cp >= '0' && cp <= '9')) {
        return true;
    }
    return word_chars != nullptr && cp != 0 && strchr(word_chars, (int)cp) != nullptr;
}

static void word_extent(const te_term *t, te_selpos p, const char *wc, te_selpos *a, te_selpos *b)
{
    const te_line *l = term_abs_line(t, p.row);
    uint16_t cols = l != nullptr ? l->cols : 0;
    *a = p;
    *b = p;
    if (!is_word_cell(t, p.row, p.col, wc)) {
        return;   /* clicking on a space selects just that cell */
    }
    while (a->col > 0 && is_word_cell(t, p.row, (uint16_t)(a->col - 1), wc)) {
        a->col--;
    }
    while (b->col + 1 < cols && is_word_cell(t, p.row, (uint16_t)(b->col + 1), wc)) {
        b->col++;
    }
}

/* A whole logical line: extend across soft wraps in both directions. */
static void line_extent(const te_term *t, te_selpos p, te_selpos *a, te_selpos *b)
{
    uint64_t first = term_abs_first(t);
    uint64_t end = term_abs_end(t);
    uint64_t r0 = p.row;
    while (r0 > first) {
        const te_line *prev = term_abs_line(t, r0 - 1);
        if (prev == nullptr || (prev->flags & TE_LINE_WRAPPED) == 0) {
            break;
        }
        r0--;
    }
    uint64_t r1 = p.row;
    while (r1 + 1 < end) {
        const te_line *cur = term_abs_line(t, r1);
        if (cur == nullptr || (cur->flags & TE_LINE_WRAPPED) == 0) {
            break;
        }
        r1++;
    }
    const te_line *last = term_abs_line(t, r1);
    a->row = r0;
    a->col = 0;
    b->row = r1;
    b->col = (uint16_t)(last != nullptr && last->cols > 0 ? last->cols - 1 : 0);
}

void sel_start(te_selection *s, const te_term *t, te_selpos p, te_sel_mode mode, const char *wc)
{
    sel_clear(s);
    s->mode = mode;
    s->anchor = p;
    s->head = p;
    s->dragging = true;
    if (mode == TE_SEL_WORD) {
        word_extent(t, p, wc, &s->a0, &s->a1);
        s->active = true;
    } else if (mode == TE_SEL_LINE) {
        line_extent(t, p, &s->a0, &s->a1);
        s->active = true;
    }
    s->h0 = s->a0;
    s->h1 = s->a1;
}

void sel_update(te_selection *s, const te_term *t, te_selpos p, const char *wc)
{
    s->head = p;
    if (s->mode == TE_SEL_WORD) {
        word_extent(t, p, wc, &s->h0, &s->h1);
    } else if (s->mode == TE_SEL_LINE) {
        line_extent(t, p, &s->h0, &s->h1);
    } else {
        s->active = s->head.row != s->anchor.row || s->head.col != s->anchor.col;
    }
}

void sel_finish(te_selection *s)
{
    s->dragging = false;
}

bool sel_range(const te_selection *s, te_selpos *start, te_selpos *end)
{
    if (!s->active) {
        return false;
    }
    te_selpos a = s->anchor;
    te_selpos b = s->head;
    if (s->mode == TE_SEL_BLOCK) {
        start->row = TE_MIN(a.row, b.row);
        end->row = TE_MAX(a.row, b.row);
        start->col = TE_MIN(a.col, b.col);
        end->col = TE_MAX(a.col, b.col);
        return true;
    }
    if (pos_less(b, a)) {
        te_selpos tmp = a;
        a = b;
        b = tmp;
    }
    *start = a;
    *end = b;
    /* Word and line modes cover whole units at both ends. */
    if (s->mode == TE_SEL_WORD || s->mode == TE_SEL_LINE) {
        *start = pos_less(s->a0, s->h0) ? s->a0 : s->h0;
        *end = pos_less(s->a1, s->h1) ? s->h1 : s->a1;
    }
    return true;
}

bool sel_contains(const te_selection *s, uint64_t row, uint16_t col)
{
    te_selpos a = { 0 };
    te_selpos b = { 0 };
    if (!sel_range(s, &a, &b) || row < a.row || row > b.row) {
        return false;
    }
    if (s->mode == TE_SEL_BLOCK) {
        return col >= a.col && col <= b.col;
    }
    if (s->mode == TE_SEL_LINE) {
        return true;
    }
    if (row == a.row && col < a.col) {
        return false;
    }
    if (row == b.row && col > b.col) {
        return false;
    }
    return true;
}

void sel_validate(te_selection *s, const te_term *t)
{
    te_selpos a = { 0 };
    te_selpos b = { 0 };
    if (sel_range(s, &a, &b) && (a.row < term_abs_first(t) || b.row >= term_abs_end(t))) {
        sel_clear(s);
    }
}

typedef struct tbuf {
    char  *p;
    size_t len, cap;
    bool   overflow;
} tbuf;

static void tb_put(tbuf *b, const char *s, size_t n)
{
    if (b->overflow) {
        return;
    }
    if (n > TE_SEL_MAX_TEXT - b->len) {
        b->overflow = true;
        return;
    }
    if (b->len + n + 1 > b->cap) {
        size_t ncap = b->cap == 0 ? 1024 : b->cap;
        while (ncap < b->len + n + 1) {
            ncap *= 2;
        }
        void *p = b->p;
        if (te_realloc_array(&p, b->cap, ncap, 1) != TE_OK) {
            b->overflow = true;
            return;
        }
        b->p = p;
        b->cap = ncap;
    }
    memcpy(b->p + b->len, s, n);
    b->len += n;
    b->p[b->len] = '\0';
}

static void tb_trim_spaces(tbuf *b, size_t floor)
{
    while (b->len > floor && b->p[b->len - 1] == ' ') {
        b->p[--b->len] = '\0';
    }
}

char *sel_text(const te_selection *s, const te_term *t, size_t *len)
{
    te_selpos a = { 0 };
    te_selpos b = { 0 };
    *len = 0;
    if (!sel_range(s, &a, &b)) {
        return nullptr;
    }
    tbuf tb = { 0 };
    for (uint64_t r = a.row; r <= b.row; r++) {
        const te_line *l = term_abs_line(t, r);
        size_t row_start = tb.len;
        bool wrapped = l != nullptr && (l->flags & TE_LINE_WRAPPED) != 0 && s->mode != TE_SEL_BLOCK;
        if (l != nullptr && l->cells != nullptr && l->cols > 0) {
            uint16_t c0 = (s->mode == TE_SEL_BLOCK || r == a.row) ? a.col : 0;
            uint16_t c1 = (s->mode == TE_SEL_BLOCK || r == b.row) ? b.col : (uint16_t)(l->cols - 1);
            if (s->mode == TE_SEL_LINE) {
                c0 = 0;
                c1 = (uint16_t)(l->cols - 1);
            }
            if (c1 >= l->cols) {
                c1 = (uint16_t)(l->cols - 1);
            }
            for (uint16_t x = c0; x <= c1 && x < l->cols; x++) {
                const te_cell *c = &l->cells[x];
                if ((c->attrs & TE_ATTR_SPACER) != 0) {
                    continue;
                }
                uint32_t cps[1 + TE_CELL_MAX_COMBINING] = { c->cp != 0 ? c->cp : ' ', c->comb[0], c->comb[1] };
                for (size_t k = 0; k < TE_ARRAY_LEN(cps); k++) {
                    if (k > 0 && cps[k] == 0) {
                        break;
                    }
                    char enc[4] = { 0 };
                    size_t n = utf8_encode(cps[k], enc, sizeof enc);
                    if (n > 0 && n <= sizeof enc) {
                        tb_put(&tb, enc, n);
                    }
                }
            }
        }
        /* Trailing blanks never matter, except inside a soft-wrapped run. */
        if (!wrapped || r == b.row) {
            tb_trim_spaces(&tb, row_start);
        }
        if (r < b.row && !wrapped) {
            tb_put(&tb, "\n", 1);
        }
        if (tb.overflow) {
            break;
        }
    }
    if (tb.overflow || tb.p == nullptr) {
        TE_FREE(tb.p);
        return nullptr;
    }
    *len = tb.len;
    return tb.p;
}
