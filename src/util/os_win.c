#include "util/os.h"

#include <windows.h>

#include <stdlib.h>
#include <string.h>

#include "util/compat_win.h"
#include "util/mem.h"
#include "util/str.h"

/* Copies a UTF-16 path into out as UTF-8 with '/' separators. */
static te_err put_path(const wchar_t *w, char *out, size_t outsz)
{
    char *u = te_w_narrow(w);
    if (u == nullptr) {
        return errno == ENOMEM ? TE_ERR_NOMEM : TE_ERR_INVAL;
    }
    for (char *c = u; *c != '\0'; c++) {
        if (*c == '\\') {
            *c = '/';
        }
    }
    te_err rc = str_copy(out, outsz, u);
    TE_FREE(u);
    if (rc == TE_OK && !os_path_is_absolute(out)) {
        out[0] = '\0';
        return TE_ERR_NOTFOUND;
    }
    return rc;
}

static te_err env_path(const wchar_t *name, char *out, size_t outsz)
{
    const wchar_t *v = _wgetenv(name);
    if (v == nullptr || v[0] == L'\0') {
        return TE_ERR_NOTFOUND;
    }
    return put_path(v, out, outsz);
}

bool os_path_is_absolute(const char *path)
{
    if (path == nullptr) {
        return false;
    }
    bool drive = ((path[0] >= 'A' && path[0] <= 'Z') || (path[0] >= 'a' && path[0] <= 'z')) && path[1] == ':' &&
                 (path[2] == '/' || path[2] == '\\');
    bool unc = (path[0] == '/' || path[0] == '\\') && (path[1] == '/' || path[1] == '\\');
    return drive || unc;
}

te_err os_home_dir(char *out, size_t outsz)
{
    return env_path(L"USERPROFILE", out, outsz);
}

te_err os_config_base(char *out, size_t outsz)
{
    return env_path(L"APPDATA", out, outsz);
}

te_err os_local_data_base(char *out, size_t outsz)
{
    return env_path(L"LOCALAPPDATA", out, outsz);
}

te_err os_self_exe(char *out, size_t outsz)
{
    if (out == nullptr || outsz < 2) {
        return TE_ERR_INVAL;
    }
    /* Long paths: grow until the name fits (32767 is the Win32 maximum). */
    for (DWORD cap = MAX_PATH; cap <= 32768u; cap *= 2u) {
        wchar_t *w = te_calloc(cap, sizeof *w);
        if (w == nullptr) {
            return TE_ERR_NOMEM;
        }
        DWORD n = GetModuleFileNameW(nullptr, w, cap);
        if (n == 0) {
            TE_FREE(w);
            return TE_ERR_SYS;
        }
        if (n < cap) {
            te_err rc = put_path(w, out, outsz);
            TE_FREE(w);
            return rc;
        }
        TE_FREE(w);
    }
    return TE_ERR_TRUNC;
}

void os_mark_from_internet(const char *path)
{
    if (path == nullptr || path[0] == '\0') {
        return;
    }
    /* The Mark-of-the-Web lives in an alternate data stream "<file>:Zone.Identifier".
     * ZoneId=3 is the Internet zone, so SmartScreen and apps warn before running it. */
    char stream[2048] = { 0 };
    if (str_fmt(stream, sizeof stream, "%s:Zone.Identifier", path) != TE_OK) {
        return;
    }
    wchar_t *w = te_w_widen(stream);
    if (w == nullptr) {
        return;
    }
    HANDLE h = CreateFileW(w, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    TE_FREE(w);
    if (h == INVALID_HANDLE_VALUE) {
        return;   /* best-effort: e.g. a filesystem without ADS support */
    }
    static const char zone[] = "[ZoneTransfer]\r\nZoneId=3\r\n";
    DWORD written = 0;
    (void)WriteFile(h, zone, (DWORD)(sizeof zone - 1), &written, nullptr);
    CloseHandle(h);
}

/* a + b in a new buffer, or nullptr. */
static wchar_t *wcs_join(const wchar_t *a, size_t alen, const wchar_t *b)
{
    size_t blen = wcslen(b);
    size_t total = 0;
    if (ckd_add(&total, alen, blen) || ckd_add(&total, total, (size_t)1)) {
        return nullptr;
    }
    wchar_t *r = te_calloc(total, sizeof *r);
    if (r != nullptr) {
        memcpy(r, a, alen * sizeof *r);
        memcpy(r + alen, b, blen * sizeof *r);
        r[total - 1] = L'\0';
    }
    return r;
}

/* The real directory entry for w (no wildcards). false if missing. */
static bool find_entry(const wchar_t *w, WIN32_FIND_DATAW *fd)
{
    HANDLE h = FindFirstFileW(w, fd);
    if (h == INVALID_HANDLE_VALUE) {
        return false;
    }
    FindClose(h);
    fd->cFileName[MAX_PATH - 1] = L'\0';
    return true;
}

/* A real folder: not a file, and not a junction or link (renaming one of those
 * would not move what it points at). */
static bool plain_dir(const WIN32_FIND_DATAW *fd)
{
    return (fd->dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0 &&
           (fd->dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0;
}

void os_fix_dir_case(const char *path)
{
    if (path == nullptr || path[0] == '\0') {
        return;
    }
    wchar_t *w = te_w_widen(path);
    if (w == nullptr) {
        return;
    }
    for (wchar_t *c = w; *c != L'\0'; c++) {
        if (*c == L'/') {
            *c = L'\\';
        }
    }
    const wchar_t *leaf = wcsrchr(w, L'\\');
    leaf = leaf != nullptr ? leaf + 1 : w;
    size_t parent = (size_t)(leaf - w);
    wchar_t *tmp = nullptr;
    wchar_t *from = nullptr;
    WIN32_FIND_DATAW fd = { 0 };

    /* FindFirstFileW treats * and ? as wildcards: only look up a plain name. */
    if (leaf[0] == L'\0' || wcspbrk(leaf, L"*?") != nullptr) {
        goto out;
    }
    tmp = wcs_join(w, wcslen(w), L".case-rename");
    if (tmp == nullptr) {
        goto out;
    }
    /* An earlier start stopped between the two renames below: finish it. */
    if (!find_entry(w, &fd) && find_entry(tmp, &fd) && plain_dir(&fd)) {
        (void)MoveFileW(tmp, w);
    }
    if (!find_entry(w, &fd) || !plain_dir(&fd) || wcscmp(fd.cFileName, leaf) == 0 ||
        _wcsicmp(fd.cFileName, leaf) != 0) {
        goto out;   /* missing, not a folder, already right, or (8.3 alias) not the same name */
    }
    /* Rename from the folder's current spelling. NTFS accepts a rename that only
     * changes case; where that is a no-op (Wine), go through a temporary name. */
    from = wcs_join(w, parent, fd.cFileName);
    if (from == nullptr) {
        goto out;
    }
    (void)MoveFileW(from, w);
    if (find_entry(w, &fd) && wcscmp(fd.cFileName, leaf) != 0 && MoveFileW(from, tmp)) {
        (void)MoveFileW(tmp, w);   /* if this fails, the next start finishes it */
    }
out:
    TE_FREE(from);
    TE_FREE(tmp);
    TE_FREE(w);
}

uint32_t os_clipboard_serial(void)
{
    return (uint32_t)GetClipboardSequenceNumber();
}

void os_harden_process(void)
{
    /* Drop the current directory from the DLL search order so a planted SDL3.dll,
     * pcre2 or freetype next to our working directory cannot be loaded instead of
     * the real one. Resolve the call at run time: it exists on every supported
     * Windows 10 target, but this keeps the binary tolerant of older SDKs. */
    HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
    if (k32 != nullptr) {
        typedef BOOL(WINAPI * set_default_dll_dirs_fn)(DWORD);
        FARPROC raw = GetProcAddress(k32, "SetDefaultDllDirectories");
        if (raw != nullptr) {
            set_default_dll_dirs_fn set_dirs = nullptr;
            memcpy(&set_dirs, &raw, sizeof set_dirs);   /* FARPROC -> typed fn without a cast warning */
            (void)set_dirs(LOAD_LIBRARY_SEARCH_SYSTEM32 | LOAD_LIBRARY_SEARCH_USER_DIRS |
                           LOAD_LIBRARY_SEARCH_APPLICATION_DIR);
        }
    }
    (void)SetDllDirectoryW(L"");   /* also removes the current directory for implicit loads */
    /* Move the current directory to after the system directories for SearchPath(). */
    (void)SetSearchPathMode(BASE_SEARCH_PATH_ENABLE_SAFE_SEARCHMODE | BASE_SEARCH_PATH_PERMANENT);
}

bool os_hostname(char *out, size_t outsz)
{
    if (out == nullptr || outsz == 0) {
        return false;
    }
    wchar_t w[256] = { 0 };
    DWORD n = (DWORD)(sizeof w / sizeof *w);
    if (!GetComputerNameExW(ComputerNameDnsHostname, w, &n)) {
        out[0] = '\0';
        return false;
    }
    char *u = te_w_narrow(w);
    if (u == nullptr) {
        out[0] = '\0';
        return false;
    }
    te_err rc = str_copy(out, outsz, u);
    TE_FREE(u);
    if (rc != TE_OK) {
        out[0] = '\0';
        return false;
    }
    return out[0] != '\0';
}
