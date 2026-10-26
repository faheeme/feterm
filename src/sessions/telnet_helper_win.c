/*
 * The Telnet helper on Windows (in feterm-helper.exe): the protocol in
 * telnet.c between the pane's pseudo console and a Winsock socket.
 */
#include <winsock2.h>
#include <ws2tcpip.h>
#include <mstcpip.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sessions/helper_con.h"
#include "sessions/session.h"
#include "sessions/telnet.h"
#include "util/str.h"

#define CONNECT_TIMEOUT_MS 15000

/* A short reason for a Winsock error, worded like strerror() on Linux. */
static void sock_error(int e, char *why, size_t whysz)
{
    static const struct { int code; const char *text; } known[] = {
        { WSAETIMEDOUT, "Connection timed out" },
        { WSAECONNREFUSED, "Connection refused" },
        { WSAEHOSTUNREACH, "No route to host" },
        { WSAENETUNREACH, "Network is unreachable" },
        { WSAECONNRESET, "Connection reset by peer" },
        { WSAECONNABORTED, "Connection aborted" },
        { WSAENETDOWN, "Network is down" },
        { WSAEADDRNOTAVAIL, "Address not available" },
        { WSAHOST_NOT_FOUND, "Host not found" },
        { WSATRY_AGAIN, "Temporary failure in name resolution" },
        { WSANO_DATA, "No address for this name" },
    };
    for (size_t i = 0; i < TE_ARRAY_LEN(known); i++) {
        if (known[i].code == e) {
            (void)str_copy(why, whysz, known[i].text);
            return;
        }
    }
    (void)str_fmt(why, whysz, "Windows socket error %d", e);
}

/*
 * Connects to each address in turn, CONNECT_TIMEOUT_MS each. The socket is
 * non-blocking from here on: WSAEventSelect makes it so, and *ev (its event)
 * is what the main loop waits on.
 */
static SOCKET connect_to(const char *host, const char *port, WSAEVENT *ev, char *why, size_t whysz)
{
    ADDRINFOA hints = { .ai_family = AF_UNSPEC, .ai_socktype = SOCK_STREAM, .ai_flags = AI_ADDRCONFIG };
    ADDRINFOA *res = nullptr;
    int gai = getaddrinfo(host, port, &hints, &res);
    if (gai != 0) {
        sock_error(gai, why, whysz);
        return INVALID_SOCKET;
    }
    SOCKET s = INVALID_SOCKET;
    for (ADDRINFOA *ai = res; ai != nullptr; ai = ai->ai_next) {
        s = WSASocketW(ai->ai_family, ai->ai_socktype, ai->ai_protocol, nullptr, 0, WSA_FLAG_NO_HANDLE_INHERIT);
        if (s == INVALID_SOCKET) {
            continue;
        }
        int err = 0;
        if (WSAEventSelect(s, *ev, FD_CONNECT) != 0) {
            err = WSAGetLastError();
        } else if (connect(s, ai->ai_addr, (int)ai->ai_addrlen) != 0) {
            err = WSAGetLastError();
            if (err == WSAEWOULDBLOCK) {
                WSANETWORKEVENTS ne = { 0 };
                DWORD r = WSAWaitForMultipleEvents(1, ev, FALSE, CONNECT_TIMEOUT_MS, FALSE);
                if (r != WSA_WAIT_EVENT_0) {
                    err = WSAETIMEDOUT;
                } else if (WSAEnumNetworkEvents(s, *ev, &ne) != 0) {
                    err = WSAGetLastError();
                } else {
                    err = (ne.lNetworkEvents & FD_CONNECT) != 0 ? ne.iErrorCode[FD_CONNECT_BIT] : WSAETIMEDOUT;
                }
            }
        }
        if (err == 0) {
            break;
        }
        sock_error(err, why, whysz);
        (void)WSAEventSelect(s, *ev, 0);
        closesocket(s);
        s = INVALID_SOCKET;
        (void)WSAResetEvent(*ev);
    }
    freeaddrinfo(res);
    return s;
}

static void set_keepalive(SOCKET s, int idle)
{
    BOOL on = TRUE;
    (void)setsockopt(s, SOL_SOCKET, SO_KEEPALIVE, (const char *)&on, sizeof on);
    (void)setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char *)&on, sizeof on);
    struct tcp_keepalive ka = {
        .onoff = 1,
        .keepalivetime = (ULONG)idle * 1000u,
        .keepaliveinterval = (ULONG)(idle / 3 > 5 ? idle / 3 : 5) * 1000u,
    };
    DWORD ret = 0;
    (void)WSAIoctl(s, SIO_KEEPALIVE_VALS, &ka, sizeof ka, nullptr, 0, &ret, nullptr, nullptr);
}

/* Sends everything, waiting up to 30 s whenever the socket is full. */
static bool send_all(SOCKET s, const uint8_t *p, size_t len)
{
    while (len > 0) {
        int n = send(s, (const char *)p, (int)TE_MIN(len, (size_t)65536), 0);
        if (n > 0) {
            p += n;
            len -= (size_t)n;
            continue;
        }
        if (n == SOCKET_ERROR && WSAGetLastError() == WSAEWOULDBLOCK) {
            fd_set wr;
            FD_ZERO(&wr);
            FD_SET(s, &wr);
            struct timeval tv = { .tv_sec = 30, .tv_usec = 0 };
            if (select(0, nullptr, &wr, nullptr, &tv) != 1) {
                return false;   /* stuck for 30 s: treat as dead */
            }
            continue;
        }
        return false;
    }
    return true;
}

int telnet_helper_main(int argc, char **argv)
{
    te_hcon con;
    if (!hcon_open(&con)) {
        fputs("telnet: must run inside a FeTerm pane\n", stderr);
        return 2;
    }
    if (argc != 3 || !session_host_valid(argv[0])) {
        hcon_say(&con, "telnet: invalid arguments\r\n");
        hcon_close(&con);
        return 2;
    }
    long port = 0;
    long ka = 0;
    if (!str_to_long(argv[1], 1, 65535, &port) || !str_to_long(argv[2], 5, 3600, &ka)) {
        hcon_say(&con, "telnet: invalid port or keepalive\r\n");
        hcon_close(&con);
        return 2;
    }
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        hcon_say(&con, "telnet: networking is unavailable\r\n");
        hcon_close(&con);
        return 2;
    }
    int status = 0;
    SOCKET sock = INVALID_SOCKET;
    WSAEVENT sev = WSACreateEvent();
    if (sev == WSA_INVALID_EVENT) {
        status = 1;
        goto out;
    }
    hcon_sayf(&con, "\x1b[2mConnecting to %s port %ld...\x1b[0m\r\n", argv[0], port);
    char why[160] = "unknown error";
    sock = connect_to(argv[0], argv[1], &sev, why, sizeof why);
    if (sock == INVALID_SOCKET) {
        hcon_sayf(&con, "\x1b[31mCould not connect to %s port %ld: %s\x1b[0m\r\n", argv[0], port, why);
        status = 3;
        goto out;
    }
    set_keepalive(sock, (int)ka);
    if (WSAEventSelect(sock, sev, FD_READ | FD_CLOSE) != 0) {
        hcon_say(&con, "\x1b[31mtelnet: cannot watch the connection\x1b[0m\r\n");
        status = 1;
        goto out;
    }

    static te_telnet tn;
    telnet_init(&tn, getenv("TERM"), con.cols, con.rows);
    static uint8_t inbuf[16384];
    static uint8_t termbuf[16384];
    static uint8_t netbuf[2 * 16384 + 1024];
    te_tbuf term = { .data = termbuf, .cap = sizeof termbuf };
    te_tbuf net = { .data = netbuf, .cap = sizeof netbuf };
    telnet_start(&tn, &net);
    if (!send_all(sock, net.data, net.len)) {
        status = 1;
    }
    net.len = 0;

    HANDLE hs[3] = { con.in, sev, con.brk };
    DWORD nh = con.brk != nullptr ? 3 : 2;
    bool closed = false;
    while (status == 0 && !closed) {
        DWORD r = WaitForMultipleObjects(nh, hs, FALSE, INFINITE);
        if (r == WAIT_OBJECT_0 + 2) {
            telnet_break(&net);
        } else if (r == WAIT_OBJECT_0) {
            bool resized = false;
            ssize_t n = hcon_read(&con, inbuf, sizeof inbuf, &resized);
            if (n < 0) {
                break;   /* the pane closed */
            }
            if (resized) {
                telnet_resize(&tn, con.cols, con.rows, &net);
            }
            if (n > 0) {
                telnet_tx(&tn, inbuf, (size_t)n, &net);
            }
        } else if (r == WAIT_OBJECT_0 + 1) {
            WSANETWORKEVENTS ne = { 0 };
            if (WSAEnumNetworkEvents(sock, sev, &ne) != 0) {
                status = 1;
                break;
            }
            /* Read what is there; FD_CLOSE can come with the last data. */
            for (;;) {
                int n = recv(sock, (char *)inbuf, (int)sizeof inbuf, 0);
                if (n > 0) {
                    telnet_rx(&tn, inbuf, (size_t)n, &term, &net);
                    if (!hcon_write(&con, term.data, term.len)) {
                        closed = true;
                    }
                    term.len = 0;
                    continue;
                }
                if (n == 0) {
                    hcon_say(&con, "\r\n\x1b[2m[Connection closed by the remote host]\x1b[0m\r\n");
                    closed = true;
                } else if (WSAGetLastError() != WSAEWOULDBLOCK) {
                    sock_error(WSAGetLastError(), why, sizeof why);
                    hcon_sayf(&con, "\r\n\x1b[31m[Connection lost: %s]\x1b[0m\r\n", why);
                    status = 1;
                } else if ((ne.lNetworkEvents & FD_CLOSE) != 0) {
                    hcon_say(&con, "\r\n\x1b[2m[Connection closed by the remote host]\x1b[0m\r\n");
                    closed = true;
                }
                break;
            }
        } else {
            status = 1;
            break;
        }
        if (net.len > 0 && status == 0 && !closed) {
            if (!send_all(sock, net.data, net.len)) {
                hcon_say(&con, "\r\n\x1b[31m[Connection lost while sending]\x1b[0m\r\n");
                status = 1;
            }
        }
        net.len = 0;
        net.overflow = false;
        term.overflow = false;
    }
out:
    if (sev != WSA_INVALID_EVENT) {
        WSACloseEvent(sev);
    }
    if (sock != INVALID_SOCKET) {
        closesocket(sock);
    }
    WSACleanup();
    hcon_close(&con);
    return status;
}
