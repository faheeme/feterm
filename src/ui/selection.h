/*
 * Text selection. Positions use absolute line numbers (see term_view_abs),
 * so a selection stays on its text while output scrolls the screen.
 */
#ifndef TE_SELECTION_H
#define TE_SELECTION_H

#include <stdbool.h>
#include "common.h"
#include "vt/term.h"

#define TE_SEL_MAX_TEXT (16u * 1024u * 1024u)

typedef enum { TE_SEL_STREAM, TE_SEL_WORD, TE_SEL_LINE, TE_SEL_BLOCK } te_sel_mode;

typedef struct te_selpos {
    uint64_t row;
    uint16_t col;
} te_selpos;

typedef struct te_selection {
    te_sel_mode mode;
    te_selpos   anchor;      /* where the drag started */
    te_selpos   head;        /* where it is now */
    te_selpos   a0, a1;      /* word/line modes: extent of the unit under the anchor */
    te_selpos   h0, h1;      /* word/line modes: extent of the unit under the head */
    bool        active;      /* something is selected */
    bool        dragging;
} te_selection;

void sel_clear(te_selection *s);
void sel_start(te_selection *s, const te_term *t, te_selpos p, te_sel_mode mode, const char *word_chars);
void sel_update(te_selection *s, const te_term *t, te_selpos p, const char *word_chars);
void sel_finish(te_selection *s);

/* Normalised range, both ends inclusive. False if nothing is selected. */
[[nodiscard]] bool sel_range(const te_selection *s, te_selpos *start, te_selpos *end);
[[nodiscard]] bool sel_contains(const te_selection *s, uint64_t row, uint16_t col);

/* Drop the selection if any of its lines are no longer stored. */
void sel_validate(te_selection *s, const te_term *t);

/*
 * Selected text as UTF-8: soft-wrapped lines are joined, hard line ends become
 * '\n', trailing blanks are trimmed. Heap string (caller frees) or nullptr.
 */
[[nodiscard]] char *sel_text(const te_selection *s, const te_term *t, size_t *len);

#endif
