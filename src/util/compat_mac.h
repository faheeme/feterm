/*
 * Linux calls the shared code uses, for the macOS build only (the Makefile
 * force-includes this file there; Linux and Windows never see it).
 *
 * Each wrapper keeps the Linux semantics the callers rely on: descriptors
 * come back close-on-exec, RENAME_NOREPLACE never replaces an existing name,
 * and secrets are wiped with a store the compiler may not remove. Where Linux
 * does something atomically (pipe2, SOCK_CLOEXEC) macOS needs a second call;
 * every such descriptor is also covered by the close_range() in the children.
 */
#ifndef TE_COMPAT_MAC_H
#define TE_COMPAT_MAC_H

#ifdef __APPLE__

#define __STDC_WANT_LIB_EXT1__ 1     /* memset_s */

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

extern char **environ;

/* Flag bits Linux accepts in socket()'s type; never real macOS SOCK_* values. */
#define SOCK_NONBLOCK    0x10000000
#define SOCK_CLOEXEC     0x20000000
#define RENAME_NOREPLACE 1u

static inline bool te_mac_set_fd_flags(int fd, bool cloexec, bool nonblock)
{
    if (cloexec) {
        int fl = fcntl(fd, F_GETFD);
        if (fl < 0 || fcntl(fd, F_SETFD, fl | FD_CLOEXEC) != 0) {
            return false;
        }
    }
    if (nonblock) {
        int fl = fcntl(fd, F_GETFL);
        if (fl < 0 || fcntl(fd, F_SETFL, fl | O_NONBLOCK) != 0) {
            return false;
        }
    }
    return true;
}

static inline int te_mac_pipe2(int fds[2], int flags)
{
    if ((flags & ~(O_CLOEXEC | O_NONBLOCK)) != 0) {
        errno = EINVAL;
        return -1;
    }
    if (pipe(fds) != 0) {
        return -1;
    }
    bool cloexec = (flags & O_CLOEXEC) != 0;
    bool nonblock = (flags & O_NONBLOCK) != 0;
    if (!te_mac_set_fd_flags(fds[0], cloexec, nonblock) || !te_mac_set_fd_flags(fds[1], cloexec, nonblock)) {
        int e = errno;
        (void)close(fds[0]);
        (void)close(fds[1]);
        fds[0] = -1;
        fds[1] = -1;
        errno = e;
        return -1;
    }
    return 0;
}
#define pipe2(fds, flags) te_mac_pipe2((fds), (flags))

static inline int te_mac_socket(int domain, int type, int protocol)
{
    bool cloexec = (type & SOCK_CLOEXEC) != 0;
    bool nonblock = (type & SOCK_NONBLOCK) != 0;
    int fd = socket(domain, type & ~(SOCK_CLOEXEC | SOCK_NONBLOCK), protocol);
    if (fd < 0) {
        return -1;
    }
    /* No SIGPIPE when the peer goes away: Linux callers expect EPIPE instead. */
    int one = 1;
    if (!te_mac_set_fd_flags(fd, cloexec, nonblock) ||
        setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, (socklen_t)sizeof one) != 0) {
        int e = errno;
        (void)close(fd);
        errno = e;
        return -1;
    }
    return fd;
}
#define socket(d, t, p) te_mac_socket((d), (t), (p))

/* macOS posix_openpt() does not document O_CLOEXEC, so set it afterwards. */
static inline int te_mac_posix_openpt(int flags)
{
    int fd = posix_openpt(flags & ~O_CLOEXEC);
    if (fd >= 0 && (flags & O_CLOEXEC) != 0 && !te_mac_set_fd_flags(fd, true, false)) {
        int e = errno;
        (void)close(fd);
        errno = e;
        return -1;
    }
    return fd;
}
#define posix_openpt(flags) te_mac_posix_openpt(flags)

/*
 * Only called in a freshly forked child, just before exec: close() is
 * async-signal-safe. Descriptors above 65535 would only exist after raising
 * the limit on purpose; everything FeTerm opens is close-on-exec anyway.
 */
static inline int te_mac_close_range(unsigned first, unsigned last, int flags)
{
    if (flags != 0 || first > last) {
        errno = EINVAL;
        return -1;
    }
    int table = getdtablesize();
    unsigned top = table > 0 ? (unsigned)table : 256u;
    if (top > 65536u) {
        top = 65536u;
    }
    for (unsigned fd = first; fd < top && fd <= last; fd++) {
        (void)close((int)fd);
    }
    return 0;
}
#define close_range(first, last, flags) te_mac_close_range((first), (last), (flags))

/* Also child-only: the environment is replaced just before exec. */
static inline int te_mac_execvpe(const char *file, char *const argv[], char *const envp[])
{
    environ = (char **)(uintptr_t)envp;
    return execvp(file, argv);
}
#define execvpe(file, argv, envp) te_mac_execvpe((file), (argv), (envp))

static inline int te_mac_renameat2(int olddir, const char *oldp, int newdir, const char *newp, unsigned flags)
{
    if (flags == 0) {
        return renameat(olddir, oldp, newdir, newp);
    }
    if (flags != RENAME_NOREPLACE) {
        errno = EINVAL;
        return -1;
    }
    return renameatx_np(olddir, oldp, newdir, newp, RENAME_EXCL);
}
#define renameat2(od, op, nd, np, fl) te_mac_renameat2((od), (op), (nd), (np), (fl))

static inline void te_mac_explicit_bzero(void *p, size_t n)
{
    if (p != nullptr && n > 0) {
        (void)memset_s(p, n, 0, n);
    }
}
#define explicit_bzero(p, n) te_mac_explicit_bzero((p), (n))

#endif /* __APPLE__ */
#endif
