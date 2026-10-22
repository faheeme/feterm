#include "input/click.h"

#include <string.h>

#include "util/str.h"

#define CLICK_MAX_MOVES 4096u

static const te_cell *cell_at(const te_term *t, uint16_t row, uint16_t col)
{
    const te_line *l = term_grid_line(t, row);
    if (l == nullptr || l->cells == nullptr || col >= l->cols) {
        return nullptr;
    }
    return &l->cells[col];
}

void click_input_block(const te_term *t, uint16_t *top, uint16_t *bottom)
{
    uint16_t cx = 0;
    uint16_t cy = 0;
    term_cursor(t, &cx, &cy);
    uint16_t rows = term_rows(t);
    uint16_t a = cy;
    uint16_t b = cy;
    while (a > 0) {
        const te_line *prev = term_grid_line(t, (uint16_t)(a - 1));
        if (prev == nullptr || (prev->flags & TE_LINE_WRAPPED) == 0) {
            break;
        }
        a--;
    }
    while (b + 1 < rows) {
        const te_line *cur = term_grid_line(t, b);
        if (cur == nullptr || (cur->flags & TE_LINE_WRAPPED) == 0) {
            break;
        }
        b++;
    }
    *top = a;
    *bottom = b;
}

/* Is the cell at linear position p (relative to the block top) a typed character? */
static bool is_char_cell(const te_term *t, uint16_t top, uint32_t p)
{
    uint16_t cols = term_cols(t);
    uint32_t row = (uint32_t)top + p / cols;
    if (row > UINT16_MAX) {
        return false;
    }
    const te_cell *c = cell_at(t, (uint16_t)row, (uint16_t)(p % cols));
    if (c == nullptr || (c->attrs & TE_ATTR_SPACER) != 0) {
        return false;   /* second half of a wide character: no key press */
    }
    /* cp == 0 inside the input is wrap padding (a wide char moved to the next row). */
    return c->cp != 0;
}

size_t click_to_position(const te_term *t, uint16_t col, uint16_t row, const te_click_opts *o,
                         char *out, size_t outsz)
{
    if (t == nullptr || o == nullptr || out == nullptr || outsz < 16) {
        return 0;
    }
    out[0] = '\0';
    uint16_t cols = term_cols(t);
    uint16_t rows = term_rows(t);
    if (col >= cols || row >= rows || term_view_offset(t) != 0) {
        return 0;
    }
    uint16_t top = 0;
    uint16_t bottom = 0;
    click_input_block(t, &top, &bottom);
    if (row < top || row > bottom) {
        return 0;
    }

    /* The shell asked to receive clicks itself (OSC 133;A;click_events=1). */
    if (term_shell_click_events(t) && term_shell_input_active(t)) {
        te_err rc = str_fmt(out, outsz, "\x1b[<0;%u;%uM\x1b[<0;%u;%um", (unsigned)col + 1u,
                            (unsigned)row + 1u, (unsigned)col + 1u, (unsigned)row + 1u);
        return rc == TE_OK ? strlen(out) : 0;
    }
    /* With shell integration we know when a command (not the shell) owns the input. */
    if (term_shell_integration(t) && !term_shell_input_active(t)) {
        return 0;
    }

    uint16_t cx = 0;
    uint16_t cy = 0;
    term_cursor(t, &cx, &cy);
    uint32_t block_cells = ((uint32_t)bottom - top + 1u) * cols;
    uint32_t cursor = ((uint32_t)cy - top) * cols + cx;

    /* Where typed input starts: OSC 133;B mark, else the recognised prompt, else column 0. */
    uint32_t start = 0;
    bool have_start = false;
    for (uint16_t y = top; y <= bottom && !have_start; y++) {
        const te_line *l = term_grid_line(t, y);
        if (l != nullptr && l->input_col != 0) {
            start = ((uint32_t)y - top) * cols + (uint32_t)(l->input_col - 1u);
            have_start = true;
        }
    }
    if (!have_start && o->prompt_end > 0 && o->prompt_end < cols) {
        start = (uint32_t)o->prompt_end;
    }
    if (start > cursor) {
        start = cursor;   /* the cursor is always inside the input */
    }

    /* Where typed input ends: just past the last visible character, or the cursor. */
    uint32_t end = cursor;
    for (uint32_t p = block_cells; p > 0; p--) {
        uint32_t q = p - 1;
        uint32_t r = (uint32_t)top + q / cols;
        const te_cell *c = r <= UINT16_MAX ? cell_at(t, (uint16_t)r, (uint16_t)(q % cols)) : nullptr;
        if (c != nullptr && c->cp != 0 && c->cp != ' ') {
            uint32_t past = (c->attrs & TE_ATTR_WIDE) != 0 ? q + 2u : q + 1u;
            if (past > end) {
                end = past;
            }
            break;
        }
    }

    /* Clicking the right half of a wide character means the character itself. */
    uint16_t tcol = col;
    const te_cell *tc = cell_at(t, row, col);
    if (tc != nullptr && (tc->attrs & TE_ATTR_SPACER) != 0 && col > 0) {
        tcol = (uint16_t)(col - 1);
    }
    uint32_t target = ((uint32_t)row - top) * cols + tcol;
    target = TE_CLAMP(target, start, end);
    if (target == cursor) {
        return 0;
    }

    bool left = target < cursor;
    uint32_t lo = left ? target : cursor;
    uint32_t hi = left ? cursor : target;
    uint32_t moves = 0;
    /* One key press per character: wide-char halves and wrap padding don't count. */
    for (uint32_t p = lo; p < hi; p++) {
        if (is_char_cell(t, top, p)) {
            moves++;
        }
    }

    const char *seq = nullptr;
    if (o->emacs_keys) {
        seq = left ? "\x02" : "\x06";
    } else if (term_modes(t)->app_cursor) {
        seq = left ? "\x1bOD" : "\x1bOC";
    } else {
        seq = left ? "\x1b[D" : "\x1b[C";
    }
    size_t sl = strlen(seq);
    size_t n = 0;
    for (uint32_t i = 0; i < moves && i < CLICK_MAX_MOVES && n + sl < outsz; i++) {
        memcpy(out + n, seq, sl);
        n += sl;
    }
    out[n] = '\0';
    return n;
}
