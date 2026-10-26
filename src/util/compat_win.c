/* POSIX shims for the Windows build (see compat_win.h). Built only on Windows. */
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "util/mem.h"

#undef open

/* Longest path we build, in UTF-16 units (the \\?\ form allows 32767). */
#define WPATH_MAX 32768

wchar_t *te_w_widen(const char *utf8)
{
    if (utf8 == nullptr) {
        return nullptr;
    }
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8, -1, nullptr, 0);
    if (n <= 0 || n > WPATH_MAX) {
        errno = EINVAL;
        return nullptr;
    }
    wchar_t *w = te_calloc((size_t)n, sizeof *w);
    if (w == nullptr) {
        errno = ENOMEM;
        return nullptr;
    }
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8, -1, w, n) != n) {
        TE_FREE(w);
        errno = EINVAL;
        return nullptr;
    }
    return w;
}

char *te_w_narrow(const wchar_t *wide)
{
    if (wide == nullptr) {
        return nullptr;
    }
    int n = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 0 || n > 4 * WPATH_MAX) {
        errno = EINVAL;
        return nullptr;
    }
    char *s = te_calloc((size_t)n, 1);
    if (s == nullptr) {
        errno = ENOMEM;
        return nullptr;
    }
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide, -1, s, n, nullptr, nullptr) != n) {
        TE_FREE(s);
        errno = EINVAL;
        return nullptr;
    }
    return s;
}

static int errno_from_win(DWORD e)
{
    switch (e) {
    case ERROR_FILE_NOT_FOUND:
    case ERROR_PATH_NOT_FOUND:
    case ERROR_INVALID_NAME:
    case ERROR_BAD_NETPATH:      return ENOENT;
    case ERROR_ACCESS_DENIED:
    case ERROR_SHARING_VIOLATION:
    case ERROR_LOCK_VIOLATION:   return EACCES;
    case ERROR_FILE_EXISTS:
    case ERROR_ALREADY_EXISTS:   return EEXIST;
    case ERROR_DIR_NOT_EMPTY:    return ENOTEMPTY;
    case ERROR_DIRECTORY:        return ENOTDIR;
    case ERROR_NOT_ENOUGH_MEMORY:
    case ERROR_OUTOFMEMORY:      return ENOMEM;
    case ERROR_DISK_FULL:        return ENOSPC;
    case ERROR_FILENAME_EXCED_RANGE: return ENAMETOOLONG;
    default:                     return EIO;
    }
}

static int fail_win(void)
{
    errno = errno_from_win(GetLastError());
    return -1;
}

/* Wide path of an open directory descriptor (\\?\ form). Heap, or nullptr. */
static wchar_t *dir_path(int dirfd)
{
    if (dirfd == AT_FDCWD) {
        DWORD n = GetCurrentDirectoryW(0, nullptr);
        if (n == 0 || n > WPATH_MAX) {
            errno = ENOENT;
            return nullptr;
        }
        wchar_t *w = te_calloc(n, sizeof *w);
        if (w == nullptr) {
            errno = ENOMEM;
            return nullptr;
        }
        if (GetCurrentDirectoryW(n, w) == 0) {
            TE_FREE(w);
            errno = ENOENT;
            return nullptr;
        }
        return w;
    }
    intptr_t oh = _get_osfhandle(dirfd);
    if (oh == -1) {
        errno = EBADF;
        return nullptr;
    }
    HANDLE h = (HANDLE)oh;
    DWORD n = GetFinalPathNameByHandleW(h, nullptr, 0, FILE_NAME_NORMALIZED);
    if (n == 0 || n > WPATH_MAX) {
        errno = EBADF;
        return nullptr;
    }
    wchar_t *w = te_calloc((size_t)n + 1u, sizeof *w);
    if (w == nullptr) {
        errno = ENOMEM;
        return nullptr;
    }
    DWORD got = GetFinalPathNameByHandleW(h, w, n + 1u, FILE_NAME_NORMALIZED);
    if (got == 0 || got > n) {
        TE_FREE(w);
        errno = EBADF;
        return nullptr;
    }
    return w;
}

/* dir + '\' + name (UTF-8, '/' accepted as a separator). "." is the directory itself. */
static wchar_t *join_at(int dirfd, const char *name)
{
    if (name == nullptr || name[0] == '\0') {
        errno = ENOENT;
        return nullptr;
    }
    wchar_t *wn = te_w_widen(name);
    if (wn == nullptr) {
        return nullptr;
    }
    for (wchar_t *c = wn; *c != L'\0'; c++) {
        if (*c == L'/') {
            *c = L'\\';
        }
    }
    /* An absolute name ignores dirfd, as with openat(). */
    bool absolute = (wn[0] != L'\0' && wn[1] == L':') || wn[0] == L'\\';
    if (absolute) {
        return wn;
    }
    wchar_t *wd = dir_path(dirfd);
    if (wd == nullptr) {
        TE_FREE(wn);
        return nullptr;
    }
    bool self = wcscmp(wn, L".") == 0;
    size_t ld = wcslen(wd);
    size_t ln = self ? 0 : wcslen(wn);
    size_t total = ld + 1u + ln + 1u;
    if (total > WPATH_MAX) {
        TE_FREE(wd);
        TE_FREE(wn);
        errno = ENAMETOOLONG;
        return nullptr;
    }
    wchar_t *out = te_calloc(total, sizeof *out);
    if (out == nullptr) {
        TE_FREE(wd);
        TE_FREE(wn);
        errno = ENOMEM;
        return nullptr;
    }
    wmemcpy(out, wd, ld);
    if (!self) {
        out[ld] = L'\\';
        wmemcpy(out + ld + 1u, wn, ln);
    }
    TE_FREE(wd);
    TE_FREE(wn);
    return out;
}

/* Turns a HANDLE into a C runtime descriptor; closes the handle on failure. */
static int handle_fd(HANDLE h, int crt_flags)
{
    int fd = _open_osfhandle((intptr_t)h, crt_flags);
    if (fd < 0) {
        CloseHandle(h);
        errno = EMFILE;
        return -1;
    }
    return fd;
}

static int open_wide(const wchar_t *wpath, int flags, int mode)
{
    bool want_dir = (flags & O_DIRECTORY) != 0;
    bool nofollow = (flags & O_NOFOLLOW) != 0;
    int acc = flags & (_O_RDONLY | _O_WRONLY | _O_RDWR);
    DWORD access = acc == _O_WRONLY ? GENERIC_WRITE : acc == _O_RDWR ? GENERIC_READ | GENERIC_WRITE : GENERIC_READ;
    DWORD disp = OPEN_EXISTING;
    if ((flags & _O_CREAT) != 0) {
        disp = (flags & _O_EXCL) != 0 ? CREATE_NEW : (flags & _O_TRUNC) != 0 ? CREATE_ALWAYS : OPEN_ALWAYS;
    } else if ((flags & _O_TRUNC) != 0) {
        disp = TRUNCATE_EXISTING;
    }
    DWORD attrs = FILE_ATTRIBUTE_NORMAL;
    if ((flags & _O_CREAT) != 0 && (mode & _S_IWRITE) == 0) {
        attrs = FILE_ATTRIBUTE_READONLY;
    }
    DWORD fl = attrs | (want_dir ? FILE_FLAG_BACKUP_SEMANTICS : 0) | (nofollow ? FILE_FLAG_OPEN_REPARSE_POINT : 0);
    if (want_dir) {
        access = FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES | SYNCHRONIZE;
    }
    SECURITY_ATTRIBUTES sa = { .nLength = sizeof sa, .lpSecurityDescriptor = nullptr,
                               .bInheritHandle = (flags & O_CLOEXEC) == 0 };
    HANDLE h = CreateFileW(wpath, access, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, &sa, disp, fl,
                           nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        return fail_win();
    }
    BY_HANDLE_FILE_INFORMATION info;
    if (!GetFileInformationByHandle(h, &info)) {
        int e = errno_from_win(GetLastError());
        CloseHandle(h);
        errno = e;
        return -1;
    }
    bool is_dir = (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    if (nofollow && (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
        CloseHandle(h);
        errno = ELOOP;
        return -1;
    }
    if (want_dir != is_dir) {
        CloseHandle(h);
        errno = want_dir ? ENOTDIR : EISDIR;
        return -1;
    }
    int crt = _O_BINARY | (acc == _O_WRONLY ? _O_WRONLY : acc == _O_RDWR ? _O_RDWR : _O_RDONLY) |
              ((flags & O_CLOEXEC) != 0 ? _O_NOINHERIT : 0) | ((flags & _O_APPEND) != 0 ? _O_APPEND : 0);
    return handle_fd(h, crt);
}

static int open_mode(int flags, va_list ap)
{
    return (flags & _O_CREAT) != 0 ? va_arg(ap, int) : 0;
}

int te_w_open(const char *path, int flags, ...)
{
    va_list ap;
    va_start(ap, flags);
    int mode = open_mode(flags, ap);
    va_end(ap);
    wchar_t *w = join_at(AT_FDCWD, path);
    if (w == nullptr) {
        return -1;
    }
    int fd = open_wide(w, flags, mode);
    TE_FREE(w);
    return fd;
}

int te_w_openat(int dirfd, const char *name, int flags, ...)
{
    va_list ap;
    va_start(ap, flags);
    int mode = open_mode(flags, ap);
    va_end(ap);
    wchar_t *w = join_at(dirfd, name);
    if (w == nullptr) {
        return -1;
    }
    int fd = open_wide(w, flags, mode);
    TE_FREE(w);
    return fd;
}

static time_t filetime_to_unix(FILETIME ft)
{
    uint64_t t = ((uint64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
    const uint64_t epoch = 116444736000000000ull;   /* 1601-01-01 to 1970-01-01, in 100 ns */
    return t < epoch ? 0 : (time_t)((t - epoch) / 10000000ull);
}

static int stat_wide(const wchar_t *w, struct stat *st, bool nofollow)
{
    WIN32_FILE_ATTRIBUTE_DATA d;
    if (!GetFileAttributesExW(w, GetFileExInfoStandard, &d)) {
        return fail_win();
    }
    memset(st, 0, sizeof *st);
    if ((d.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0 && nofollow) {
        st->st_mode = (unsigned short)(S_IFLNK | 0777);
    } else if ((d.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
        st->st_mode = (unsigned short)(S_IFDIR | 0700);
    } else {
        st->st_mode = (unsigned short)(S_IFREG | ((d.dwFileAttributes & FILE_ATTRIBUTE_READONLY) != 0 ? 0400 : 0600));
    }
    uint64_t size = ((uint64_t)d.nFileSizeHigh << 32) | d.nFileSizeLow;
    st->st_size = (off_t)(size > (uint64_t)INT64_MAX ? INT64_MAX : size);
    st->st_mtime = filetime_to_unix(d.ftLastWriteTime);
    st->st_atime = filetime_to_unix(d.ftLastAccessTime);
    st->st_ctime = filetime_to_unix(d.ftCreationTime);
    st->st_nlink = 1;
    return 0;
}

int te_w_fstatat(int dirfd, const char *name, struct stat *st, int flags)
{
    if (st == nullptr) {
        errno = EINVAL;
        return -1;
    }
    wchar_t *w = join_at(dirfd, name);
    if (w == nullptr) {
        return -1;
    }
    int r = stat_wide(w, st, (flags & AT_SYMLINK_NOFOLLOW) != 0);
    TE_FREE(w);
    return r;
}

int te_w_lstat(const char *path, struct stat *st)
{
    return te_w_fstatat(AT_FDCWD, path, st, AT_SYMLINK_NOFOLLOW);
}

int te_w_mkdirat(int dirfd, const char *name)
{
    wchar_t *w = join_at(dirfd, name);
    if (w == nullptr) {
        return -1;
    }
    int r = CreateDirectoryW(w, nullptr) ? 0 : fail_win();
    TE_FREE(w);
    return r;
}

int te_w_mkdir(const char *path)
{
    return te_w_mkdirat(AT_FDCWD, path);
}

int te_w_unlinkat(int dirfd, const char *name, int flags)
{
    wchar_t *w = join_at(dirfd, name);
    if (w == nullptr) {
        return -1;
    }
    bool ok = (flags & AT_REMOVEDIR) != 0 ? RemoveDirectoryW(w) : DeleteFileW(w);
    int r = ok ? 0 : fail_win();
    TE_FREE(w);
    return r;
}

int te_w_renameat2(int odir, const char *oname, int ndir, const char *nname, unsigned flags)
{
    wchar_t *from = join_at(odir, oname);
    if (from == nullptr) {
        return -1;
    }
    wchar_t *to = join_at(ndir, nname);
    if (to == nullptr) {
        TE_FREE(from);
        return -1;
    }
    DWORD mf = MOVEFILE_WRITE_THROUGH | ((flags & RENAME_NOREPLACE) != 0 ? 0 : MOVEFILE_REPLACE_EXISTING);
    int r = MoveFileExW(from, to, mf) ? 0 : fail_win();
    TE_FREE(from);
    TE_FREE(to);
    return r;
}

int te_w_renameat(int odir, const char *oname, int ndir, const char *nname)
{
    return te_w_renameat2(odir, oname, ndir, nname, 0);
}

DIR *te_w_fdopendir(int fd)
{
    wchar_t *w = dir_path(fd);
    if (w == nullptr) {
        return nullptr;
    }
    /* opendir() takes a code-page path; drop the \\?\ prefix it does not understand. */
    const wchar_t *plain = wcsncmp(w, L"\\\\?\\", 4) == 0 && wcsncmp(w, L"\\\\?\\UNC\\", 8) != 0 ? w + 4 : w;
    char *u = te_w_narrow(plain);
    TE_FREE(w);
    if (u == nullptr) {
        return nullptr;
    }
    DIR *d = opendir(u);
    int e = errno;
    TE_FREE(u);
    if (d == nullptr) {
        errno = e;
        return nullptr;
    }
    /* fdopendir() takes over the descriptor. */
    (void)close(fd);
    return d;
}

struct tm *te_w_localtime_r(const time_t *t, struct tm *out)
{
    if (t == nullptr || out == nullptr || localtime_s(out, t) != 0) {
        return nullptr;
    }
    return out;
}

/* Positioned I/O: ReadFile/WriteFile with an offset in the OVERLAPPED block. */
static ssize_t rw_at(int fd, void *rbuf, const void *wbuf, size_t n, off_t off)
{
    intptr_t oh = _get_osfhandle(fd);
    if (oh == -1 || off < 0) {
        errno = oh == -1 ? EBADF : EINVAL;
        return -1;
    }
    DWORD want = (DWORD)(n > 0x40000000u ? 0x40000000u : n);
    OVERLAPPED ov = { 0 };
    uint64_t o = (uint64_t)off;
    ov.Offset = (DWORD)(o & 0xFFFFFFFFu);
    ov.OffsetHigh = (DWORD)(o >> 32);
    DWORD got = 0;
    BOOL ok = rbuf != nullptr ? ReadFile((HANDLE)oh, rbuf, want, &got, &ov)
                              : WriteFile((HANDLE)oh, wbuf, want, &got, &ov);
    if (!ok) {
        if (rbuf != nullptr && GetLastError() == ERROR_HANDLE_EOF) {
            return 0;
        }
        return fail_win();
    }
    return (ssize_t)got;
}

ssize_t te_w_pread(int fd, void *buf, size_t n, off_t off)
{
    if (buf == nullptr) {
        errno = EINVAL;
        return -1;
    }
    return rw_at(fd, buf, nullptr, n, off);
}

ssize_t te_w_pwrite(int fd, const void *buf, size_t n, off_t off)
{
    if (buf == nullptr) {
        errno = EINVAL;
        return -1;
    }
    return rw_at(fd, nullptr, buf, n, off);
}

char *te_w_strcasestr(const char *hay, const char *needle)
{
    if (hay == nullptr || needle == nullptr) {
        return nullptr;
    }
    size_t ln = strlen(needle);
    if (ln == 0) {
        return (char *)(uintptr_t)hay;
    }
    for (const char *h = hay; *h != '\0'; h++) {
        if (_strnicmp(h, needle, ln) == 0) {
            return (char *)(uintptr_t)h;
        }
    }
    return nullptr;
}

/* Replaces the trailing XXXXXX with random characters and creates the directory (0700 is the ACL default). */
char *te_w_mkdtemp(char *tmpl)
{
    size_t n = tmpl != nullptr ? strlen(tmpl) : 0;
    if (n < 6 || strcmp(tmpl + n - 6, "XXXXXX") != 0) {
        errno = EINVAL;
        return nullptr;
    }
    static const char alphabet[] = "abcdefghijklmnopqrstuvwxyz0123456789";
    for (int attempt = 0; attempt < 100; attempt++) {
        uint64_t r = ((uint64_t)GetTickCount64() << 20) ^ ((uint64_t)GetCurrentProcessId() << 8) ^
                     (uint64_t)attempt * 0x9E3779B97F4A7C15ull;
        unsigned int rnd = 0;
        if (rand_s(&rnd) == 0) {
            r ^= (uint64_t)rnd << 32 | rnd;
        }
        for (size_t i = 0; i < 6; i++) {
            tmpl[n - 6 + i] = alphabet[r % (sizeof alphabet - 1)];
            r /= sizeof alphabet - 1;
        }
        if (te_w_mkdir(tmpl) == 0) {
            return tmpl;
        }
        if (errno != EEXIST) {
            return nullptr;
        }
    }
    errno = EEXIST;
    return nullptr;
}

int te_w_setenv(const char *name, const char *value, int overwrite)
{
    if (name == nullptr || name[0] == '\0' || strchr(name, '=') != nullptr || value == nullptr) {
        errno = EINVAL;
        return -1;
    }
    if (!overwrite && getenv(name) != nullptr) {
        return 0;
    }
    return _putenv_s(name, value) == 0 ? 0 : -1;
}
