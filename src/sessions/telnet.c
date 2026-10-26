#include "sessions/telnet.h"

#include <string.h>

#include "util/str.h"

enum { ST_DATA, ST_IAC, ST_WILL, ST_WONT, ST_DO, ST_DONT, ST_SB, ST_SB_IAC };
enum { Q_NO = 0, Q_YES = 1, Q_WANTYES = 2, Q_WANTNO = 3 };

#define TTYPE_IS   0
#define TTYPE_SEND 1

static void put(te_tbuf *b, uint8_t c)
{
    if (b->len < b->cap) {
        b->data[b->len++] = c;
    } else {
        b->overflow = true;
    }
}

static void put3(te_tbuf *b, uint8_t a, uint8_t c, uint8_t d)
{
    if (b->cap - b->len < 3) {   /* all or nothing: never half a command */
        b->overflow = true;
        return;
    }
    put(b, a);
    put(b, c);
    put(b, d);
}

/* A byte inside a sub-negotiation: 255 must be doubled. */
static void put_sb_byte(te_tbuf *b, uint8_t c)
{
    put(b, c);
    if (c == TN_IAC) {
        put(b, TN_IAC);
    }
}

void telnet_init(te_telnet *t, const char *ttype, uint16_t cols, uint16_t rows)
{
    memset(t, 0, sizeof *t);
    t->cols = cols;
    t->rows = rows;
    /* TTYPE values are conventionally upper case ASCII. */
    const char *src = ttype != nullptr && ttype[0] != '\0' ? ttype : "XTERM-256COLOR";
    size_t k = 0;
    for (size_t i = 0; src[i] != '\0' && k + 1 < sizeof t->ttype; i++) {
        unsigned char c = (unsigned char)src[i];
        if (c > 0x20 && c < 0x7F) {
            t->ttype[k++] = (char)(c >= 'a' && c <= 'z' ? c - 32 : c);
        }
    }
    t->ttype[k] = '\0';
}

static bool we_support(uint8_t opt)
{
    return opt == TN_OPT_TTYPE || opt == TN_OPT_NAWS || opt == TN_OPT_BINARY || opt == TN_OPT_SGA;
}

static bool he_may(uint8_t opt)
{
    return opt == TN_OPT_ECHO || opt == TN_OPT_SGA || opt == TN_OPT_BINARY;
}

static void send_naws(const te_telnet *t, te_tbuf *net)
{
    if (net->cap - net->len < 14) {
        net->overflow = true;
        return;
    }
    put(net, TN_IAC);
    put(net, TN_SB);
    put(net, TN_OPT_NAWS);
    put_sb_byte(net, (uint8_t)(t->cols >> 8));
    put_sb_byte(net, (uint8_t)(t->cols & 0xFFu));
    put_sb_byte(net, (uint8_t)(t->rows >> 8));
    put_sb_byte(net, (uint8_t)(t->rows & 0xFFu));
    put(net, TN_IAC);
    put(net, TN_SE);
}

void telnet_start(te_telnet *t, te_tbuf *net)
{
    /* Ask for character mode with server echo, and offer our type and size. */
    t->him[TN_OPT_SGA] = Q_WANTYES;
    put3(net, TN_IAC, TN_DO, TN_OPT_SGA);
    t->us[TN_OPT_NAWS] = Q_WANTYES;
    put3(net, TN_IAC, TN_WILL, TN_OPT_NAWS);
    t->us[TN_OPT_TTYPE] = Q_WANTYES;
    put3(net, TN_IAC, TN_WILL, TN_OPT_TTYPE);
}

/* RFC 1143: the server asks us to enable (DO) or disable (DONT) an option of ours. */
static void on_do(te_telnet *t, uint8_t opt, bool enable, te_tbuf *net)
{
    uint8_t *q = &t->us[opt];
    if (enable) {
        if (*q == Q_YES) {
            return;
        }
        if (*q == Q_WANTYES) {
            *q = Q_YES;
        } else if (*q == Q_WANTNO) {
            *q = Q_NO;   /* we asked to stop; the answer does not change our mind */
            return;
        } else if (we_support(opt)) {
            *q = Q_YES;
            put3(net, TN_IAC, TN_WILL, opt);
        } else {
            put3(net, TN_IAC, TN_WONT, opt);
            return;
        }
        if (opt == TN_OPT_NAWS) {
            send_naws(t, net);
        }
    } else {
        if (*q == Q_NO) {
            return;
        }
        bool was_yes = *q == Q_YES;
        *q = Q_NO;
        if (was_yes) {
            put3(net, TN_IAC, TN_WONT, opt);
        }
    }
}

/* The server offers (WILL) or withdraws (WONT) an option of its own. */
static void on_will(te_telnet *t, uint8_t opt, bool enable, te_tbuf *net)
{
    uint8_t *q = &t->him[opt];
    if (enable) {
        if (*q == Q_YES) {
            return;
        }
        if (*q == Q_WANTYES) {
            *q = Q_YES;
        } else if (*q == Q_WANTNO) {
            *q = Q_NO;
        } else if (he_may(opt)) {
            *q = Q_YES;
            put3(net, TN_IAC, TN_DO, opt);
        } else {
            put3(net, TN_IAC, TN_DONT, opt);
        }
    } else {
        if (*q == Q_NO) {
            return;
        }
        bool was_yes = *q == Q_YES;
        *q = Q_NO;
        if (was_yes) {
            put3(net, TN_IAC, TN_DONT, opt);
        }
    }
}

static void on_sb(te_telnet *t, te_tbuf *net)
{
    if (t->sb_overflow || t->sb_len < 2) {
        return;
    }
    if (t->sb[0] == TN_OPT_TTYPE && t->sb[1] == TTYPE_SEND && t->us[TN_OPT_TTYPE] == Q_YES) {
        size_t n = strlen(t->ttype);
        if (net->cap - net->len < n + 6) {
            net->overflow = true;
            return;
        }
        put(net, TN_IAC);
        put(net, TN_SB);
        put(net, TN_OPT_TTYPE);
        put(net, TTYPE_IS);
        for (size_t i = 0; i < n; i++) {
            put(net, (uint8_t)t->ttype[i]);
        }
        put(net, TN_IAC);
        put(net, TN_SE);
    }
}

void telnet_rx(te_telnet *t, const uint8_t *in, size_t n, te_tbuf *term, te_tbuf *net)
{
    for (size_t i = 0; i < n; i++) {
        uint8_t c = in[i];
        switch (t->state) {
        case ST_DATA:
            if (c == TN_IAC) {
                t->state = ST_IAC;
            } else if (c == 0 && t->last_cr && t->him[TN_OPT_BINARY] != Q_YES) {
                t->last_cr = false;   /* CR NUL: the NUL is padding */
            } else {
                put(term, c);
                t->last_cr = c == '\r';
            }
            break;
        case ST_IAC:
            t->state = ST_DATA;
            switch (c) {
            case TN_IAC:  put(term, TN_IAC); t->last_cr = false; break;
            case TN_WILL: t->state = ST_WILL; break;
            case TN_WONT: t->state = ST_WONT; break;
            case TN_DO:   t->state = ST_DO; break;
            case TN_DONT: t->state = ST_DONT; break;
            case TN_SB:
                t->state = ST_SB;
                t->sb_len = 0;
                t->sb_overflow = false;
                break;
            default: break;   /* NOP, GA, DM, AYT...: nothing to do */
            }
            break;
        case ST_WILL: on_will(t, c, true, net);  t->state = ST_DATA; break;
        case ST_WONT: on_will(t, c, false, net); t->state = ST_DATA; break;
        case ST_DO:   on_do(t, c, true, net);    t->state = ST_DATA; break;
        case ST_DONT: on_do(t, c, false, net);   t->state = ST_DATA; break;
        case ST_SB:
            if (c == TN_IAC) {
                t->state = ST_SB_IAC;
            } else if (t->sb_len < sizeof t->sb) {
                t->sb[t->sb_len++] = c;
            } else {
                t->sb_overflow = true;
            }
            break;
        case ST_SB_IAC:
            if (c == TN_SE) {
                on_sb(t, net);
                t->state = ST_DATA;
            } else if (c == TN_IAC) {
                if (t->sb_len < sizeof t->sb) {
                    t->sb[t->sb_len++] = TN_IAC;
                } else {
                    t->sb_overflow = true;
                }
                t->state = ST_SB;
            } else {
                t->state = ST_DATA;   /* malformed: abandon the sub-negotiation */
            }
            break;
        default:
            t->state = ST_DATA;
            break;
        }
    }
}

void telnet_tx(const te_telnet *t, const uint8_t *in, size_t n, te_tbuf *net)
{
    bool binary = t->us[TN_OPT_BINARY] == Q_YES;
    for (size_t i = 0; i < n; i++) {
        uint8_t c = in[i];
        if (c == TN_IAC) {
            if (net->cap - net->len < 2) {
                net->overflow = true;
                return;
            }
            put(net, TN_IAC);
            put(net, TN_IAC);
        } else if (c == '\r' && !binary) {
            if (net->cap - net->len < 2) {
                net->overflow = true;
                return;
            }
            put(net, '\r');
            put(net, (i + 1 < n && in[i + 1] == '\n') ? (uint8_t)'\n' : 0);
            if (i + 1 < n && in[i + 1] == '\n') {
                i++;
            }
        } else {
            put(net, c);
        }
    }
}

void telnet_resize(te_telnet *t, uint16_t cols, uint16_t rows, te_tbuf *net)
{
    t->cols = cols;
    t->rows = rows;
    if (t->us[TN_OPT_NAWS] == Q_YES) {
        send_naws(t, net);
    }
}

void telnet_break(te_tbuf *net)
{
    if (net->cap - net->len < 2) {
        net->overflow = true;
        return;
    }
    put(net, TN_IAC);
    put(net, TN_BRK);
}

bool telnet_remote_echo(const te_telnet *t)
{
    return t->him[TN_OPT_ECHO] == Q_YES;
}
