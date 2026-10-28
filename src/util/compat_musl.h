/*
 * Linux calls the shared code uses that musl (Alpine Linux) does not provide,
 * for musl builds only (the Makefile force-includes this file when the
 * compiler targets *-musl; glibc, Windows and macOS never see it).
 *
 * The system headers are included first, so a later musl that declares
 * these functions itself cannot clash with the macros below.
 */
#ifndef TE_COMPAT_MUSL_H
#define TE_COMPAT_MUSL_H

#include <errno.h>
#include <stdio.h>
#include <sys/syscall.h>
#include <unistd.h>

/*
 * Only called in a freshly forked child, just before exec: syscall() and
 * close() are async-signal-safe. Kernels before 5.9 have no close_range and
 * answer ENOSYS; then each descriptor is closed in turn. Descriptors above
 * 65535 would only exist after raising the limit on purpose; everything
 * FeTerm opens is close-on-exec anyway.
 */
static inline int te_musl_close_range(unsigned first, unsigned last, int flags)
{
    if (flags != 0 || first > last) {
        errno = EINVAL;
        return -1;
    }
#ifdef SYS_close_range
    if (syscall(SYS_close_range, first, last, 0u) == 0) {
        return 0;
    }
    if (errno != ENOSYS) {
        return -1;
    }
#endif
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
#define close_range(first, last, flags) te_musl_close_range((first), (last), (flags))

/*
 * musl added renameat2() in 1.2.6 (Alpine 3.24); older releases get the raw
 * system call. There is deliberately no fallback to rename-then-check: callers
 * rely on RENAME_NOREPLACE being atomic, so a kernel or filesystem without it
 * fails (ENOSYS/EINVAL) instead of possibly replacing a file.
 */
#ifndef RENAME_NOREPLACE
#define RENAME_NOREPLACE (1u << 0)

static inline int te_musl_renameat2(int olddir, const char *oldp, int newdir, const char *newp, unsigned flags)
{
#ifdef SYS_renameat2
    return (int)syscall(SYS_renameat2, olddir, oldp, newdir, newp, flags);
#else
    if (flags == 0) {
        return renameat(olddir, oldp, newdir, newp);
    }
    errno = ENOSYS;
    return -1;
#endif
}
#define renameat2(od, op, nd, np, fl) te_musl_renameat2((od), (op), (nd), (np), (fl))
#endif

#endif
