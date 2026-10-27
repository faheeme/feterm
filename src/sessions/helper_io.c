#include "sessions/helper_io.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#ifdef __APPLE__
#include <sys/event.h>
#else
#include <sys/signalfd.h>
#endif
#include <termios.h>
#include <unistd.h>

bool helper_write_all(int fd, const void *data, size_t len)
{
    const uint8_t *p = data;
    size_t done = 0;
    while (done < len) {
        ssize_t n = write(fd, p + done, len - done);
        if (n > 0) {
            done += (size_t)n;
            continue;
        }
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            struct pollfd pf = { .fd = fd, .events = POLLOUT };
            if (poll(&pf, 1, 30000) <= 0) {
                return false;   /* stuck for 30 s: treat as dead */
            }
            continue;
        }
        return false;
    }
    return true;
}

void helper_say(const char *msg)
{
    (void)helper_write_all(STDOUT_FILENO, msg, strlen(msg));
}

void helper_sayf(const char *fmt, ...)
{
    char buf[512] = { 0 };
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (n > 0) {
        size_t len = (size_t)n < sizeof buf ? (size_t)n : sizeof buf - 1;
        (void)helper_write_all(STDOUT_FILENO, buf, len);
    }
}

void helper_raw_stdin(void)
{
    struct termios t = { 0 };
    if (isatty(STDIN_FILENO) && tcgetattr(STDIN_FILENO, &t) == 0) {
        cfmakeraw(&t);
        t.c_cc[VMIN] = 1;
        t.c_cc[VTIME] = 0;
        (void)tcsetattr(STDIN_FILENO, TCSANOW, &t);
    }
}

#ifdef __APPLE__
/* macOS has no signalfd: a kqueue's EVFILT_SIGNAL records the signals instead, and
 * the kqueue descriptor polls readable while any are pending. EVFILT_SIGNAL sees a
 * signal even when it is ignored, so ignoring them stops SIGUSR1 killing us. */
int helper_signals(void)
{
    static const int sigs[] = { SIGWINCH, SIGUSR1 };
    int kq = kqueue();
    if (kq < 0) {
        return -1;
    }
    int fl = fcntl(kq, F_GETFD);
    if (fl < 0 || fcntl(kq, F_SETFD, fl | FD_CLOEXEC) != 0) {
        (void)close(kq);
        return -1;
    }
    for (size_t i = 0; i < TE_ARRAY_LEN(sigs); i++) {
        struct kevent kev;
        memset(&kev, 0, sizeof kev);
        EV_SET(&kev, (uintptr_t)sigs[i], EVFILT_SIGNAL, EV_ADD, 0, 0, nullptr);
        if (signal(sigs[i], SIG_IGN) == SIG_ERR || kevent(kq, &kev, 1, nullptr, 0, nullptr) != 0) {
            (void)close(kq);
            return -1;
        }
    }
    return kq;
}

int helper_take_signal(int sigfd)
{
    struct kevent ev;
    memset(&ev, 0, sizeof ev);
    const struct timespec zero = { 0, 0 };
    if (kevent(sigfd, nullptr, 0, &ev, 1, &zero) != 1 || ev.filter != EVFILT_SIGNAL || ev.ident > 64u) {
        return 0;
    }
    return (int)ev.ident;
}
#else
int helper_signals(void)
{
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGWINCH);
    sigaddset(&set, SIGUSR1);
    if (sigprocmask(SIG_BLOCK, &set, nullptr) != 0) {
        return -1;
    }
    return signalfd(-1, &set, SFD_CLOEXEC | SFD_NONBLOCK);
}

int helper_take_signal(int sigfd)
{
    struct signalfd_siginfo si = { 0 };
    memset(&si, 0, sizeof si);
    ssize_t n = read(sigfd, &si, sizeof si);
    if (n != (ssize_t)sizeof si || si.ssi_signo > 64) {
        return 0;
    }
    return (int)si.ssi_signo;
}
#endif
