/*
 * Click-to-position: clicking inside the line being typed moves the cursor
 * there by sending the same Left/Right keys a person would press. Works with
 * readline (bash), zsh, fish, and network-device CLIs (Cisco, Arista,
 * Juniper) over ssh/telnet/serial, since all of them accept arrow keys.
 */
#ifndef TE_CLICK_H
#define TE_CLICK_H

#include <stdbool.h>
#include "common.h"
#include "vt/term.h"

/* Rows [top, bottom] of the soft-wrapped input block containing the cursor. */
void click_input_block(const te_term *t, uint16_t *top, uint16_t *bottom);

typedef struct te_click_opts {
    bool emacs_keys;      /* send Ctrl-B / Ctrl-F instead of arrow keys */
    int  prompt_end;      /* column where input starts on the block's top row; -1 unknown */
} te_click_opts;

/*
 * Compute the bytes to send for a click at grid cell (col, row). Returns the
 * number of bytes written to out (0: the click is not a cursor move).
 * The caller decides when a click qualifies (primary button, no drag, mouse
 * reporting off, main screen, live view).
 */
[[nodiscard]] size_t click_to_position(const te_term *t, uint16_t col, uint16_t row,
                                       const te_click_opts *o, char *out, size_t outsz);

#endif
