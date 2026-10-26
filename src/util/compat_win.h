/*
 * POSIX calls the shared code uses, for the Windows build only (the Makefile
 * force-includes this file there; Linux never sees it).
 *
 * Directory descriptors are real C runtime descriptors wrapping a directory
 * HANDLE, so close() works on them unchanged. The *at() calls resolve names
 * against that handle's final path. O_NOFOLLOW and AT_SYMLINK_NOFOLLOW refuse
 * (or report) reparse points, the Windows form of symlinks and junctions.
 *
 * Paths are UTF-8: the application manifest makes UTF-8 the process code page,
 * and the wrappers below convert to UTF-16 for the wide Win32 calls.
 */
#ifndef TE_COMPAT_WIN_H
#define TE_COMPAT_WIN_H

#ifdef _WIN32

#ifndef _CRT_RAND_S
#define _CRT_RAND_S                  /* rand_s() for mkdtemp names */
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00          /* Windows 10: ConPTY needs 1809 or newer */
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <io.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

/* open() flags the C runtime lacks. The high bits are never real CRT flags. */
#define O_CLOEXEC    _O_NOINHERIT
#define O_NOCTTY     0
#define O_NONBLOCK   0
#define O_DIRECTORY  0x10000000
#define O_NOFOLLOW   0x20000000

#define AT_FDCWD            (-100)
#define AT_SYMLINK_NOFOLLOW 0x100
#define AT_REMOVEDIR        0x200
#define RENAME_NOREPLACE    1u

#ifndef S_IFLNK
#define S_IFLNK 0xA000
#endif
#ifndef S_ISLNK
#define S_ISLNK(m) (((m) & S_IFMT) == S_IFLNK)
#endif
#ifndef S_IRWXG
#define S_IRWXG 0070
#endif
#ifndef S_IRWXO
#define S_IRWXO 0007
#endif
#ifndef S_IWGRP
#define S_IWGRP 0020
#endif
#ifndef S_IWOTH
#define S_IWOTH 0002
#endif

#ifndef ELOOP
#define ELOOP 114
#endif

typedef int te_uid_t;

/* inotify does not exist here: the remote editor's change watch reports itself unavailable. */
#define IN_NONBLOCK    0
#define IN_CLOEXEC     0
#define IN_CLOSE_WRITE 0x8u
#define IN_MOVED_TO    0x80u
struct inotify_event {
    int      wd;
    uint32_t mask, cookie, len;
    char     name[];
};
static inline int inotify_init1(int flags)
{
    (void)flags;
    errno = ENOSYS;
    return -1;
}
static inline int inotify_add_watch(int fd, const char *path, uint32_t mask)
{
    (void)fd;
    (void)path;
    (void)mask;
    errno = ENOSYS;
    return -1;
}

int te_w_open(const char *path, int flags, ...);
int te_w_openat(int dirfd, const char *name, int flags, ...);
int te_w_fstatat(int dirfd, const char *name, struct stat *st, int flags);
int te_w_lstat(const char *path, struct stat *st);
int te_w_mkdir(const char *path);
int te_w_mkdirat(int dirfd, const char *name);
int te_w_unlinkat(int dirfd, const char *name, int flags);
int te_w_renameat(int odir, const char *oname, int ndir, const char *nname);
int te_w_renameat2(int odir, const char *oname, int ndir, const char *nname, unsigned flags);
DIR *te_w_fdopendir(int fd);
struct tm *te_w_localtime_r(const time_t *t, struct tm *out);
char *te_w_mkdtemp(char *tmpl);
int te_w_setenv(const char *name, const char *value, int overwrite);
ssize_t te_w_pread(int fd, void *buf, size_t n, off_t off);
ssize_t te_w_pwrite(int fd, const void *buf, size_t n, off_t off);
char *te_w_strcasestr(const char *hay, const char *needle);

/* UTF-8 <-> UTF-16. Return a heap string (free()) or nullptr. */
wchar_t *te_w_widen(const char *utf8);
char *te_w_narrow(const wchar_t *wide);

/* The runtime's read()/write() take an unsigned count: clamp instead of truncating. */
static inline ssize_t te_w_read(int fd, void *buf, size_t n)
{
    return _read(fd, buf, (unsigned)(n > INT_MAX ? INT_MAX : n));
}
static inline ssize_t te_w_write(int fd, const void *buf, size_t n)
{
    return _write(fd, buf, (unsigned)(n > INT_MAX ? INT_MAX : n));
}

/* Windows guards files with ACLs; the runtime fakes Unix bits as writable by everyone.
 * Report owner bits only, so FeTerm's "world-writable config" checks stay meaningful. */
static inline int te_w_fstat(int fd, struct stat *st)
{
    int r = fstat(fd, st);
    if (r == 0) {
        st->st_mode &= (unsigned short)~0077u;
    }
    return r;
}
static inline int te_w_stat(const char *path, struct stat *st)
{
    int r = stat(path, st);
    if (r == 0) {
        st->st_mode &= (unsigned short)~0077u;
    }
    return r;
}

#define fstat(fd, st)             te_w_fstat((fd), (st))
#define stat(p, st)               te_w_stat((p), (st))
#define read(fd, b, n)            te_w_read((fd), (b), (n))
#define write(fd, b, n)           te_w_write((fd), (b), (n))
#define open(...)                 te_w_open(__VA_ARGS__)
#define openat(...)               te_w_openat(__VA_ARGS__)
#define fstatat(d, n, s, f)       te_w_fstatat((d), (n), (s), (f))
#define lstat(p, s)               te_w_lstat((p), (s))
#define mkdir(p, m)               ((void)(m), te_w_mkdir(p))
#define mkdirat(d, n, m)          ((void)(m), te_w_mkdirat((d), (n)))
#define unlinkat(d, n, f)         te_w_unlinkat((d), (n), (f))
#define renameat(a, b, c, d)      te_w_renameat((a), (b), (c), (d))
#define renameat2(a, b, c, d, f)  te_w_renameat2((a), (b), (c), (d), (f))
#define fdopendir(fd)             te_w_fdopendir(fd)
#define localtime_r(t, out)       te_w_localtime_r((t), (out))
#define pread(fd, b, n, o)        te_w_pread((fd), (b), (n), (o))
#define pwrite(fd, b, n, o)       te_w_pwrite((fd), (b), (n), (o))
#define strcasestr(h, n)          te_w_strcasestr((h), (n))
#define mkdtemp(t)                te_w_mkdtemp(t)
#define setenv(n, v, o)           te_w_setenv((n), (v), (o))
#define fsync(fd)                 _commit(fd)
#define getuid()                  ((te_uid_t)0)
#define explicit_bzero(p, n)      SecureZeroMemory((p), (n))

#endif /* _WIN32 */
#endif
