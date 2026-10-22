/* Convert terminal cells to UTF-8 text with a byte -> (row, column) map, for regex work. */
#ifndef TE_LINETEXT_H
#define TE_LINETEXT_H

#include "common.h"
#include "vt/term.h"

typedef struct te_textpos {
    uint32_t row;    /* caller-defined row index (e.g. offset within a logical line) */
    uint16_t col;
} te_textpos;

/*
 * Append the first n cells to buf starting at byte len. map[i] receives the
 * cell of every byte written. Wide-char spacers are skipped, empty cells become
 * spaces. Stops before overflowing cap (buf stays NUL-terminated, cap >= 1).
 * Returns the new length.
 */
size_t linetext_append(const te_cell *cells, uint16_t n, uint32_t row, char *buf, te_textpos *map,
                       size_t len, size_t cap);

/* Number of cells up to and including the last non-empty one. */
uint16_t linetext_trimmed(const te_cell *cells, uint16_t n);

#endif
