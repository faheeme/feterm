#include "sessions/telnet.h"

#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/signalfd.h>
#include <sys/socket.h>
#include <termios.h>
#include <unistd.h>

#include "sessions/helper_io.h"
#include "sessions/session.h"
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

/* ------------------------------------------------------------------------- */
/* The helper process */

static int connect_to(const char *host, const char *port, char *why, size_t whysz)
{
    struct addrinfo hints = { .ai_family = AF_UNSPEC, .ai_socktype = SOCK_STREAM, .ai_flags = AI_ADDRCONFIG };
    struct addrinfo *res = nullptr;
    int gai = getaddrinfo(host, port, &hints, &res);
    if (gai != 0) {
        (void)str_copy(why, whysz, gai_strerror(gai));
        return -1;
    }
    int fd = -1;
    for (struct addrinfo *ai = res; ai != nullptr; ai = ai->ai_next) {
        fd = socket(ai->ai_family, ai->ai_socktype | SOCK_CLOEXEC | SOCK_NONBLOCK, ai->ai_protocol);
        if (fd < 0) {
            continue;
        }
        int rc = connect(fd, ai->ai_addr, ai->ai_addrlen);
        if (rc != 0 && errno == EINPROGRESS) {
            struct pollfd p = { .fd = fd, .events = POLLOUT };
            rc = poll(&p, 1, 15000);
            int err = 0;
            socklen_t len = sizeof err;
            if (rc == 1 && getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len) == 0 && err == 0) {
                rc = 0;
            } else {
                errno = rc == 0 ? ETIMEDOUT : (err != 0 ? err : errno);
                rc = -1;
            }
        }
        if (rc == 0) {
            break;
        }
        (void)str_copy(why, whysz, strerror(errno));
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    return fd;
}

static void set_keepalive(int fd, int idle)
{
    int on = 1;
    int intvl = idle / 3 > 5 ? idle / 3 : 5;
    int cnt = 9;
    (void)setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &on, sizeof on);
    (void)setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof idle);
    (void)setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &intvl, sizeof intvl);
    (void)setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &cnt, sizeof cnt);
    (void)setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &on, sizeof on);
}

int telnet_helper_main(int argc, char **argv)
{
    if (argc != 3 || !session_host_valid(argv[0])) {
        helper_say("telnet: invalid arguments\r\n");
        return 2;
    }
    long port = 0;
    long ka = 0;
    if (!str_to_long(argv[1], 1, 65535, &port) || !str_to_long(argv[2], 5, 3600, &ka)) {
        helper_say("telnet: invalid port or keepalive\r\n");
        return 2;
    }
    int sigfd = helper_signals();
    if (sigfd < 0) {
        return 2;
    }
    helper_raw_stdin();
    helper_sayf("\x1b[2mConnecting to %s port %ld...\x1b[0m\r\n", argv[0], port);
    char why[160] = "unknown error";
    int sock = connect_to(argv[0], argv[1], why, sizeof why);
    if (sock < 0) {
        helper_sayf("\x1b[31mCould not connect to %s port %ld: %s\x1b[0m\r\n", argv[0], port, why);
        close(sigfd);
        return 3;
    }
    set_keepalive(sock, (int)ka);
    int fl = fcntl(sock, F_GETFL);
    if (fl >= 0) {
        (void)fcntl(sock, F_SETFL, fl & ~O_NONBLOCK);   /* writes block; reads are driven by poll */
    }

    static te_telnet tn;
    struct winsize ws = { 0 };
    (void)ioctl(STDIN_FILENO, TIOCGWINSZ, &ws);
    telnet_init(&tn, getenv("TERM"), ws.ws_col, ws.ws_row);

    static uint8_t inbuf[16384];
    static uint8_t termbuf[16384];
    static uint8_t netbuf[2 * 16384 + 1024];
    te_tbuf term = { .data = termbuf, .cap = sizeof termbuf };
    te_tbuf net = { .data = netbuf, .cap = sizeof netbuf };
    telnet_start(&tn, &net);
    int status = 0;
    if (!helper_write_all(sock, net.data, net.len)) {
        status = 1;
    }
    net.len = 0;

    while (status == 0) {
        struct pollfd p[3] = {
            { .fd = STDIN_FILENO, .events = POLLIN },
            { .fd = sock, .events = POLLIN },
            { .fd = sigfd, .events = POLLIN },
        };
        if (poll(p, 3, -1) < 0) {
            if (errno == EINTR) continue;
            status = 1;
            break;
        }
        if ((p[2].revents & POLLIN) != 0) {
            int sig = helper_take_signal(sigfd);
            if (sig == SIGWINCH && ioctl(STDIN_FILENO, TIOCGWINSZ, &ws) == 0) {
                telnet_resize(&tn, ws.ws_col, ws.ws_row, &net);
            } else if (sig == SIGUSR1) {
                telnet_break(&net);
            }
        }
        if ((p[0].revents & (POLLIN | POLLHUP)) != 0) {
            ssize_t n = read(STDIN_FILENO, inbuf, sizeof inbuf);
            if (n <= 0) {
                break;   /* the pane closed */
            }
            telnet_tx(&tn, inbuf, (size_t)n, &net);
        }
        if ((p[1].revents & (POLLIN | POLLHUP | POLLERR)) != 0) {
            ssize_t n = read(sock, inbuf, sizeof inbuf);
            if (n == 0) {
                helper_say("\r\n\x1b[2m[Connection closed by the remote host]\x1b[0m\r\n");
                break;
            }
            if (n < 0) {
                if (errno == EINTR || errno == EAGAIN) continue;
                helper_sayf("\r\n\x1b[31m[Connection lost: %s]\x1b[0m\r\n", strerror(errno));
                status = 1;
                break;
            }
            telnet_rx(&tn, inbuf, (size_t)n, &term, &net);
            if (!helper_write_all(STDOUT_FILENO, term.data, term.len)) {
                break;
            }
            term.len = 0;
        }
        if (net.len > 0) {
            if (!helper_write_all(sock, net.data, net.len)) {
                helper_say("\r\n\x1b[31m[Connection lost while sending]\x1b[0m\r\n");
                status = 1;
            }
            net.len = 0;
        }
        net.overflow = false;
        term.overflow = false;
    }
    close(sock);
    close(sigfd);
    return status;
}
