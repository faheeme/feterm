/*
 * Regex search through the scrollback and screen (PCRE2, smart case:
 * case-insensitive unless the query has an upper-case letter). A query that
 * is not a valid regex is searched as literal text.
 */
#ifndef TE_SEARCH_H
#define TE_SEARCH_H

#include <stdbool.h>
#include "common.h"
#include "vt/term.h"

#define TE_SEARCH_MAX_QUERY   256u
#define TE_SEARCH_MAX_MATCHES 20000u

typedef struct te_search_match {
    uint64_t row;
    uint16_t col0, col1;   /* inclusive */
} te_search_match;

typedef struct te_search te_search;

[[nodiscard]] te_err search_create(te_search **out);
void search_destroy(te_search **s);

void search_open(te_search *s);
void search_close(te_search *s);
bool search_is_open(const te_search *s);

/* Editing the query re-runs the search. */
void search_append(te_search *s, const te_term *t, const char *utf8);
void search_backspace(te_search *s, const te_term *t);
const char *search_query(const te_search *s);

/* Run (or re-run after new output). The current match becomes the newest one. */
void search_run(te_search *s, const te_term *t);
/* Move to an older (dir < 0) or newer (dir > 0) match; wraps around. */
[[nodiscard]] bool search_step(te_search *s, const te_term *t, int dir);
[[nodiscard]] bool search_current(const te_search *s, te_search_match *m);

/* For rendering: is (row, col) inside a match, and is it the current one? */
[[nodiscard]] bool search_cell(const te_search *s, uint64_t row, uint16_t col, bool *current);

/* One-line status for the search bar, e.g. "3/17" or "no matches". */
const char *search_status(const te_search *s);

#endif
