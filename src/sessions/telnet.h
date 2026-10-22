/*
 * Built-in Telnet client (RFC 854/855) for old network gear and terminal
 * servers. The protocol is a pure state machine (testable without a
 * network); telnet_helper_main() runs it between the pane's pty and a TCP
 * socket when FeTerm is started as `feterm --telnet-helper HOST PORT KEEPALIVE`.
 *
 * Negotiated: ECHO and SGA (character-at-a-time with remote echo), BINARY,
 * TTYPE (sends $TERM) and NAWS (window size). Everything else is refused.
 * Option state follows RFC 1143's Q method, so a hostile server cannot make
 * the client loop. Sub-negotiations longer than TE_TELNET_SB_MAX are dropped.
 */
#ifndef TE_TELNET_H
#define TE_TELNET_H

#include <stdbool.h>
#include "common.h"

#define TE_TELNET_SB_MAX 512

enum {
    TN_IAC = 255, TN_DONT = 254, TN_DO = 253, TN_WONT = 252, TN_WILL = 251, TN_SB = 250, TN_BRK = 243,
    TN_SE = 240, TN_NOP = 241,
    TN_OPT_BINARY = 0, TN_OPT_ECHO = 1, TN_OPT_SGA = 3, TN_OPT_TTYPE = 24, TN_OPT_NAWS = 31,
};

typedef struct te_tbuf {
    uint8_t *data;
    size_t   len, cap;
    bool     overflow;     /* something did not fit and was dropped */
} te_tbuf;

typedef struct te_telnet {
    uint8_t  state;
    uint8_t  opt;
    uint8_t  us[256];       /* our side of each option: 0 no, 1 yes, 2 want yes, 3 want no */
    uint8_t  him[256];      /* the server's side */
    uint8_t  sb[TE_TELNET_SB_MAX];
    size_t   sb_len;
    bool     sb_overflow;
    bool     last_cr;       /* previous data byte was CR (a following NUL is padding) */
    uint16_t cols, rows;
    char     ttype[64];
} te_telnet;

void telnet_init(te_telnet *t, const char *ttype, uint16_t cols, uint16_t rows);
/* Opening negotiation the client sends after connecting. */
void telnet_start(te_telnet *t, te_tbuf *net);
/* Bytes from the server: data goes to term, protocol replies to net. */
void telnet_rx(te_telnet *t, const uint8_t *in, size_t n, te_tbuf *term, te_tbuf *net);
/* Keystrokes from the user: escaped and CR-padded into net. */
void telnet_tx(const te_telnet *t, const uint8_t *in, size_t n, te_tbuf *net);
/* New window size; sends NAWS if the server agreed to it. */
void telnet_resize(te_telnet *t, uint16_t cols, uint16_t rows, te_tbuf *net);
void telnet_break(te_tbuf *net);
bool telnet_remote_echo(const te_telnet *t);

/* Entry point for --telnet-helper. argv: host, port, keepalive seconds. */
int telnet_helper_main(int argc, char **argv);

#endif
