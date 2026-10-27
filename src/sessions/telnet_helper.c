/* The --telnet-helper process (POSIX): the protocol in telnet.c between the pane's pty and a TCP socket. */
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
#include <sys/socket.h>
#include <termios.h>
#include <unistd.h>

#include "sessions/helper_io.h"
#include "sessions/session.h"
#include "util/str.h"

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
