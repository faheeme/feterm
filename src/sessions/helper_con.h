/*
 * Windows only: the console side of feterm-helper.exe, which runs the Telnet
 * and serial clients inside a pane's pseudo console. Keys arrive as UTF-8 VT
 * input, output goes out as UTF-8 VT, resizes come from console input
 * events, and FeTerm asks for a Break through a named event (pty_send_break).
 */
#ifndef TE_HELPER_CON_H
#define TE_HELPER_CON_H

#include <stdbool.h>
#include "common.h"

typedef struct te_hcon {
    void    *in;           /* console input (waitable) */
    void    *out;          /* console output */
    void    *brk;          /* auto-reset event: FeTerm asks for a Break */
    uint16_t cols, rows;
    uint16_t surrogate;    /* high half of a UTF-16 pair split across reads */
} te_hcon;

/* Name of the Break event for helper process pid (also used by pty_win.c). */
[[nodiscard]] te_err hcon_break_name(unsigned long pid, wchar_t *out, size_t outsz);

/* Raw UTF-8 VT input and output, and the Break event. False if this is not a console. */
[[nodiscard]] bool hcon_open(te_hcon *c);
void hcon_close(te_hcon *c);

/* Write everything to the console. */
bool hcon_write(te_hcon *c, const void *data, size_t len);
void hcon_say(te_hcon *c, const char *msg);
void hcon_sayf(te_hcon *c, const char *fmt, ...) TE_PRINTF(2, 3);

/*
 * Take the waiting console input: typed bytes (UTF-8) into buf, and *resized
 * set if the window size changed (c->cols/c->rows updated). Returns the
 * number of bytes, 0 if only other events were waiting, -1 on error.
 */
[[nodiscard]] ssize_t hcon_read(te_hcon *c, uint8_t *buf, size_t cap, bool *resized);

#endif
