/*
 * Pseudo console (ConPTY, Windows 10 1809+) and the shell in it.
 *
 * Two threads per pane:
 *   reader  copies the console's output into a bounded ring buffer and sets
 *           `ready` while anything (or end of file) is waiting there;
 *   waiter  waits for the shell to exit, then closes the pseudo console:
 *           ConPTY never ends its output pipe on its own, closing it does.
 * The shell runs in a Job Object, so closing the pane also ends whatever the
 * shell started.
 */
#include "pty/pty.h"

#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "pty/conpty_win.h"
#include "sessions/helper_con.h"
#include "util/log.h"
#include "util/mem.h"
#include "util/os.h"
#include "util/str.h"
#include "util/winargs.h"
#include "util/winshell.h"

#define OUT_CAP      (1u << 20)   /* output buffered before the reader waits for the UI */

struct te_conpty {
    te_hpcon           hpc;          /* guarded by lock; nullptr once closed */
    HANDLE             in_w;         /* we write keystrokes here */
    HANDLE             out_r;        /* the reader thread reads output here */
    HANDLE             proc;
    HANDLE             job;
    HANDLE             ready;        /* manual-reset: output or end of file is waiting */
    HANDLE             stop;         /* manual-reset: the pane is closing */
    HANDLE             reader;
    HANDLE             waiter;
    SRWLOCK            lock;
    CONDITION_VARIABLE space;        /* the UI took output out of the buffer */
    uint8_t           *buf;          /* ring buffer of OUT_CAP bytes */
    size_t             head, len;
    bool               eof;
    bool               closing;      /* nobody reads any more: the reader drops output */
    bool               reaped;
    int                status;
};

void pty_init(te_pty *p)
{
    p->con = nullptr;
}

void *pty_ready_handle(const te_pty *p)
{
    return p != nullptr && p->con != nullptr ? p->con->ready : nullptr;
}

/* ------------------------------------------------------------------------- */
/* Command line and environment */

/* exe's full path from PATH only, never the current directory (a planted copy
 * there could shadow the real one). Heap UTF-8, or nullptr. */
static char *search_path_exe(const wchar_t *exe)
{
    char *res = nullptr;
    DWORD plen = GetEnvironmentVariableW(L"PATH", nullptr, 0);
    if (plen <= 1) {
        return nullptr;
    }
    wchar_t *path = te_calloc(plen, sizeof *path);
    if (path != nullptr && GetEnvironmentVariableW(L"PATH", path, plen) == plen - 1) {
        wchar_t found[MAX_PATH] = { 0 };
        DWORD n = SearchPathW(path, exe, nullptr, MAX_PATH, found, nullptr);
        if (n > 0 && n < MAX_PATH) {
            res = te_w_narrow(found);
        }
    }
    TE_FREE(path);
    return res;
}

/* base\rel if that file exists. base: an environment variable's value (L"ProgramFiles"),
 * or nullptr for the System32 folder. Heap UTF-8, or nullptr. */
static char *existing_under(const wchar_t *base_var, const wchar_t *rel)
{
    wchar_t base[MAX_PATH] = { 0 };
    if (base_var == nullptr) {
        UINT s = GetSystemDirectoryW(base, MAX_PATH);
        if (s == 0 || s >= MAX_PATH) {
            return nullptr;
        }
    } else {
        DWORD s = GetEnvironmentVariableW(base_var, base, MAX_PATH);
        if (s == 0 || s >= MAX_PATH) {
            return nullptr;
        }
    }
    wchar_t full[MAX_PATH] = { 0 };
    int n = _snwprintf(full, MAX_PATH, L"%ls\\%ls", base, rel);
    if (n <= 0 || n >= MAX_PATH) {
        return nullptr;
    }
    DWORD at = GetFileAttributesW(full);
    return at != INVALID_FILE_ATTRIBUTES && (at & FILE_ATTRIBUTE_DIRECTORY) == 0 ? te_w_narrow(full) : nullptr;
}

/* Git for Windows' bin\bash.exe: next to the git.exe on PATH (Git\cmd or Git\mingw64\bin), else the usual folders. */
static char *find_git_bash(void)
{
    char *git = search_path_exe(L"git.exe");
    if (git != nullptr) {
        static const char *const ups[] = { "\\..\\bin\\bash.exe", "\\..\\..\\bin\\bash.exe" };
        char *slash = strrchr(git, '\\');
        if (slash != nullptr) {
            *slash = '\0';
            for (size_t i = 0; i < TE_ARRAY_LEN(ups); i++) {
                char cand[MAX_PATH * 3] = { 0 };
                wchar_t *w = nullptr;
                if (str_fmt(cand, sizeof cand, "%s%s", git, ups[i]) == TE_OK && (w = te_w_widen(cand)) != nullptr) {
                    wchar_t full[MAX_PATH] = { 0 };
                    DWORD n = GetFullPathNameW(w, MAX_PATH, full, nullptr);
                    DWORD at = n > 0 && n < MAX_PATH ? GetFileAttributesW(full) : INVALID_FILE_ATTRIBUTES;
                    TE_FREE(w);
                    if (at != INVALID_FILE_ATTRIBUTES && (at & FILE_ATTRIBUTE_DIRECTORY) == 0) {
                        TE_FREE(git);
                        return te_w_narrow(full);
                    }
                }
            }
        }
        TE_FREE(git);
    }
    char *b = existing_under(L"ProgramFiles", L"Git\\bin\\bash.exe");
    return b != nullptr ? b : existing_under(L"LOCALAPPDATA", L"Programs\\Git\\bin\\bash.exe");
}

/* pwsh if installed, else Windows PowerShell; nullptr means cmd. Heap UTF-8 path. */
static char *find_powershell(void)
{
    char *ps = search_path_exe(L"pwsh.exe");
    if (ps == nullptr) {
        ps = existing_under(L"ProgramFiles", L"PowerShell\\7\\pwsh.exe");
    }
    return ps != nullptr ? ps : existing_under(nullptr, L"WindowsPowerShell\\v1.0\\powershell.exe");
}

/* cmd: %ComSpec%, else System32\cmd.exe. Heap UTF-8 path. */
static char *find_cmd(void)
{
    const char *comspec = getenv("ComSpec");
    if (comspec != nullptr && comspec[0] != '\0') {
        return str_dup(comspec);
    }
    char *c = existing_under(nullptr, L"cmd.exe");
    return c != nullptr ? c : str_dup("cmd.exe");
}

/* The default shell to start: a program, its arguments and extra environment. */
typedef struct win_shell {
    char        *exe;                   /* heap */
    const char  *args[TE_WSH_MAX_ARGS + 2];
    size_t       nargs;
    const char  *env[2];                /* "NAME=value" literals, may be nullptr */
    char         distro[TE_WSH_DISTRO_MAX];
    te_wsh_args  user;                  /* shell_args, for a shell given as a path */
} win_shell;

/* Find the program for a preset. False when it is not installed (exe stays nullptr). */
static bool resolve_preset(te_wsh_kind k, win_shell *w)
{
    switch (k) {
    case TE_WSH_AUTO:
        w->exe = find_powershell();
        if (w->exe != nullptr) {
            w->args[w->nargs++] = "-NoLogo";
        } else {
            w->exe = find_cmd();
        }
        return w->exe != nullptr;
    case TE_WSH_PWSH:
        w->exe = search_path_exe(L"pwsh.exe");
        if (w->exe == nullptr) {
            w->exe = existing_under(L"ProgramFiles", L"PowerShell\\7\\pwsh.exe");
        }
        w->args[w->nargs++] = "-NoLogo";
        break;
    case TE_WSH_POWERSHELL:
        w->exe = existing_under(nullptr, L"WindowsPowerShell\\v1.0\\powershell.exe");
        w->args[w->nargs++] = "-NoLogo";
        break;
    case TE_WSH_CMD:
        w->exe = find_cmd();
        break;
    case TE_WSH_GITBASH:
        w->exe = find_git_bash();
        w->args[w->nargs++] = "--login";
        w->args[w->nargs++] = "-i";
        w->env[0] = "CHERE_INVOKING=1";   /* start in the pane's folder, not $HOME */
        break;
    case TE_WSH_WSL:
        w->exe = existing_under(nullptr, L"wsl.exe");
        if (w->distro[0] != '\0') {
            w->args[w->nargs++] = "-d";
            w->args[w->nargs++] = w->distro;
        }
        break;
    case TE_WSH_MSYS2:
        w->exe = existing_under(L"SystemDrive", L"msys64\\usr\\bin\\bash.exe");
        w->args[w->nargs++] = "--login";
        w->args[w->nargs++] = "-i";
        w->env[0] = "MSYSTEM=UCRT64";
        w->env[1] = "CHERE_INVOKING=1";
        break;
    case TE_WSH_CYGWIN:
        w->exe = existing_under(L"SystemDrive", L"cygwin64\\bin\\bash.exe");
        w->args[w->nargs++] = "--login";
        w->args[w->nargs++] = "-i";
        w->env[0] = "CHERE_INVOKING=1";
        break;
    case TE_WSH_NU:
        w->exe = search_path_exe(L"nu.exe");
        if (w->exe == nullptr) {
            w->exe = existing_under(L"ProgramFiles", L"nu\\bin\\nu.exe");
        }
        break;
    case TE_WSH_PATH:
    case TE_WSH_INVALID:
        return false;
    }
    return w->exe != nullptr;
}

/* The configured shell, falling back to auto when a preset is not installed. */
static te_err resolve_shell(const char *shell, const char *shell_args, win_shell *w)
{
    memset(w, 0, sizeof *w);
    te_wsh_kind k = winshell_parse(shell, w->distro, sizeof w->distro);
    if (k == TE_WSH_PATH) {
        w->exe = str_dup(shell);
        if (w->exe == nullptr) {
            return TE_ERR_NOMEM;
        }
        if (winshell_split_args(shell_args, &w->user) != TE_OK) {
            LOG_W("ignoring shell_args: unbalanced quote or too many words");
            w->user.argc = 0;
        }
        for (size_t i = 0; i < w->user.argc && w->nargs < TE_WSH_MAX_ARGS; i++) {
            w->args[w->nargs++] = w->user.argv[i];
        }
        return TE_OK;
    }
    if (resolve_preset(k, w)) {
        return TE_OK;
    }
    if (k != TE_WSH_AUTO) {
        LOG_W("shell '%s' is not installed; using the default", shell);
    }
    TE_FREE(w->exe);
    memset(w, 0, sizeof *w);
    return resolve_preset(TE_WSH_AUTO, w) ? TE_OK : TE_ERR_NOTFOUND;
}

bool pty_shell_found(const char *shell)
{
    win_shell w;
    char distro[TE_WSH_DISTRO_MAX] = { 0 };
    te_wsh_kind k = winshell_parse(shell, distro, sizeof distro);
    if (k == TE_WSH_AUTO || k == TE_WSH_PATH) {
        return true;   /* a path is reported by CreateProcess when the pane starts */
    }
    memset(&w, 0, sizeof w);
    bool ok = k != TE_WSH_INVALID && resolve_preset(k, &w);
    TE_FREE(w.exe);
    return ok;
}

/* The command line: argv, else the configured shell. env gets the shell's extra variables. */
static te_err build_cmdline(const te_pty_opts *o, wchar_t **out, const char *env[2])
{
    env[0] = nullptr;
    env[1] = nullptr;
    if (o->argv != nullptr && o->argv[0] != nullptr) {
        return win_cmdline(o->argv, out);
    }
    win_shell w;
    te_err rc = resolve_shell(o->shell, o->shell_args, &w);
    if (rc != TE_OK) {
        return rc;
    }
    const char *argv[TE_WSH_MAX_ARGS + 4] = { 0 };
    size_t n = 0;
    argv[n++] = w.exe;
    for (size_t i = 0; i < w.nargs && n + 1 < TE_ARRAY_LEN(argv); i++) {
        argv[n++] = w.args[i];
    }
    argv[n] = nullptr;
    rc = win_cmdline(argv, out);
    env[0] = w.env[0];
    env[1] = w.env[1];
    TE_FREE(w.exe);
    return rc;
}

/* Variables we set ourselves (the fixed ones, plus extra[0..nextra)); inherited copies are dropped. */
static bool env_is_overridden(const wchar_t *kv, wchar_t *const *extra, size_t nextra)
{
    static const wchar_t *const names[] = {
        L"TERM=", L"COLORTERM=", L"TERM_PROGRAM=", L"TERM_PROGRAM_VERSION=", L"TERMINFO=",
    };
    for (size_t i = 0; i < TE_ARRAY_LEN(names); i++) {
        if (_wcsnicmp(kv, names[i], wcslen(names[i])) == 0) {
            return true;
        }
    }
    for (size_t i = 0; i < nextra; i++) {
        const wchar_t *eq = extra[i] != nullptr ? wcschr(extra[i], L'=') : nullptr;
        if (eq != nullptr && _wcsnicmp(kv, extra[i], (size_t)(eq - extra[i]) + 1) == 0) {
            return true;
        }
    }
    return false;
}

static int env_cmp(const void *a, const void *b)
{
    const wchar_t *const *x = a;
    const wchar_t *const *y = b;
    return _wcsicmp(*x, *y);
}

/* The inherited environment plus ours and the shell's (shell_env, may hold nullptrs),
 * sorted as CreateProcess expects. Heap block, or nullptr. */
static wchar_t *build_env(const char *term, const char *const shell_env[2])
{
    char tval[256] = { 0 };
    if (str_fmt(tval, sizeof tval, "TERM=%s", term != nullptr && term[0] != '\0' ? term : "xterm-256color") != TE_OK) {
        return nullptr;
    }
    const char *extra_u8[6] = { tval, "COLORTERM=truecolor", "TERM_PROGRAM=" APP_DISPLAY_NAME,
                                "TERM_PROGRAM_VERSION=" APP_VERSION, shell_env[0], shell_env[1] };
    wchar_t *extra[6] = { nullptr, nullptr, nullptr, nullptr, nullptr, nullptr };
    size_t nextra = 4;
    for (size_t i = 4; i < TE_ARRAY_LEN(extra_u8); i++) {
        if (extra_u8[i] != nullptr) {
            extra_u8[nextra++] = extra_u8[i];   /* pack the shell's variables after ours */
        }
    }
    wchar_t *block = nullptr;
    const wchar_t **list = nullptr;
    wchar_t *inherited = GetEnvironmentStringsW();
    if (inherited == nullptr) {
        return nullptr;
    }
    for (size_t i = 0; i < nextra; i++) {
        extra[i] = te_w_widen(extra_u8[i]);
        if (extra[i] == nullptr) {
            goto out;
        }
    }
    size_t count = nextra;
    for (const wchar_t *e = inherited; *e != L'\0'; e += wcslen(e) + 1) {
        count++;
    }
    list = te_calloc(count, sizeof *list);
    if (list == nullptr) {
        goto out;
    }
    size_t k = 0;
    size_t total = 1;   /* the block's final terminator */
    for (const wchar_t *e = inherited; *e != L'\0' && k < count; e += wcslen(e) + 1) {
        if (!env_is_overridden(e, extra + 4, nextra - 4)) {
            list[k++] = e;
            total += wcslen(e) + 1;
        }
    }
    for (size_t i = 0; i < nextra && k < count; i++) {
        list[k++] = extra[i];
        total += wcslen(extra[i]) + 1;
    }
    qsort(list, k, sizeof *list, env_cmp);
    block = te_calloc(total, sizeof *block);
    if (block == nullptr) {
        goto out;
    }
    size_t at = 0;
    for (size_t i = 0; i < k; i++) {
        size_t n = wcslen(list[i]) + 1;
        if (at + n >= total) {   /* cannot happen: total counted every entry */
            TE_FREE(block);
            goto out;
        }
        wmemcpy(block + at, list[i], n);
        at += n;
    }
    block[at] = L'\0';
out:
    TE_FREE(list);
    for (size_t i = 0; i < TE_ARRAY_LEN(extra); i++) {
        TE_FREE(extra[i]);
    }
    FreeEnvironmentStringsW(inherited);
    return block;
}

/* ------------------------------------------------------------------------- */
/* Threads */

static void close_console(te_conpty *c)
{
    AcquireSRWLockExclusive(&c->lock);
    te_hpcon h = c->hpc;
    c->hpc = nullptr;
    ReleaseSRWLockExclusive(&c->lock);
    if (h != nullptr) {
        conpty_close(h);   /* may wait for the reader to drain the last output */
    }
}

static DWORD WINAPI reader_main(void *arg)
{
    te_conpty *c = arg;
    static_assert(OUT_CAP >= 16384, "the ring must hold a whole read");
    uint8_t chunk[16384];
    for (;;) {
        DWORD got = 0;
        if (!ReadFile(c->out_r, chunk, sizeof chunk, &got, nullptr) || got == 0) {
            break;   /* the pseudo console was closed */
        }
        size_t off = 0;
        AcquireSRWLockExclusive(&c->lock);
        while (off < got && !c->closing) {
            while (c->len == OUT_CAP && !c->closing) {
                (void)SleepConditionVariableSRW(&c->space, &c->lock, INFINITE, 0);
            }
            if (c->closing) {
                break;
            }
            size_t tail = (c->head + c->len) % OUT_CAP;
            size_t n = got - off;
            n = TE_MIN(n, OUT_CAP - c->len);
            n = TE_MIN(n, OUT_CAP - tail);
            memcpy(c->buf + tail, chunk + off, n);
            c->len += n;
            off += n;
            (void)SetEvent(c->ready);
        }
        ReleaseSRWLockExclusive(&c->lock);
    }
    AcquireSRWLockExclusive(&c->lock);
    c->eof = true;
    (void)SetEvent(c->ready);
    ReleaseSRWLockExclusive(&c->lock);
    return 0;
}

static DWORD WINAPI waiter_main(void *arg)
{
    te_conpty *c = arg;
    HANDLE hs[2] = { c->proc, c->stop };
    if (WaitForMultipleObjects(2, hs, FALSE, INFINITE) == WAIT_OBJECT_0) {
        close_console(c);   /* the shell is gone: end the output so the reader sees end of file */
    }
    return 0;
}

/* ------------------------------------------------------------------------- */

static void free_conpty(te_conpty *c)
{
    HANDLE *hs[] = { &c->in_w, &c->out_r, &c->proc, &c->job, &c->ready, &c->stop, &c->reader, &c->waiter };
    for (size_t i = 0; i < TE_ARRAY_LEN(hs); i++) {
        if (*hs[i] != nullptr && *hs[i] != INVALID_HANDLE_VALUE) {
            CloseHandle(*hs[i]);
        }
        *hs[i] = nullptr;
    }
    TE_FREE(c->buf);
    free(c);
}

static COORD console_size(uint16_t cols, uint16_t rows)
{
    COORD sz = { .X = (SHORT)TE_CLAMP(cols, 1, SHRT_MAX), .Y = (SHORT)TE_CLAMP(rows, 1, SHRT_MAX) };
    return sz;
}

te_err pty_spawn(te_pty *p, const te_pty_opts *o)
{
    if (p == nullptr || o == nullptr || p->con != nullptr) {
        return TE_ERR_INVAL;
    }
    if (!conpty_load()) {
        return TE_ERR_PLATFORM;
    }
    te_err rc = TE_ERR_SYS;
    wchar_t *wcmd = nullptr;
    wchar_t *wcwd = nullptr;
    wchar_t *env = nullptr;
    HANDLE in_r = nullptr;
    HANDLE out_w = nullptr;
    LPPROC_THREAD_ATTRIBUTE_LIST attrs = nullptr;
    PROCESS_INFORMATION pi = { 0 };

    te_conpty *c = te_calloc(1, sizeof *c);
    if (c == nullptr) {
        return TE_ERR_NOMEM;
    }
    InitializeSRWLock(&c->lock);
    InitializeConditionVariable(&c->space);
    c->buf = te_malloc(OUT_CAP);
    c->ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    c->stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (c->buf == nullptr || c->ready == nullptr || c->stop == nullptr) {
        rc = c->buf == nullptr ? TE_ERR_NOMEM : TE_ERR_SYS;
        goto fail;
    }

    const char *shell_env[2] = { nullptr, nullptr };
    rc = build_cmdline(o, &wcmd, shell_env);
    if (rc != TE_OK) {
        goto fail;
    }
    rc = TE_ERR_SYS;
    env = build_env(o->term, shell_env);
    if (env == nullptr) {
        rc = TE_ERR_NOMEM;
        goto fail;
    }
    /* The requested directory, else home (as a login shell on Linux starts there). */
    char home[1024] = { 0 };
    const char *dir = o->cwd != nullptr && o->cwd[0] != '\0' ? o->cwd
                      : os_home_dir(home, sizeof home) == TE_OK ? home : nullptr;
    if (dir != nullptr) {
        wcwd = te_w_widen(dir);
        DWORD at = wcwd != nullptr ? GetFileAttributesW(wcwd) : INVALID_FILE_ATTRIBUTES;
        if (at == INVALID_FILE_ATTRIBUTES || (at & FILE_ATTRIBUTE_DIRECTORY) == 0) {
            TE_FREE(wcwd);   /* fall back to the inherited directory; not fatal */
        }
    }

    if (!CreatePipe(&in_r, &c->in_w, nullptr, 0) || !CreatePipe(&c->out_r, &out_w, nullptr, 0)) {
        LOG_E("CreatePipe failed (error %lu)", (unsigned long)GetLastError());
        goto fail;
    }
    /* Inherit the cursor: ConPTY asks where it is (ESC[6n) and starts drawing there, below
     * anything already on screen (the welcome banner), instead of repainting from the top. */
    DWORD pc_flags = conpty_on_wine() ? 0 : PSEUDOCONSOLE_INHERIT_CURSOR;
    HRESULT hr = conpty_create(console_size(o->cols, o->rows), in_r, out_w, pc_flags, &c->hpc);
    /* The console holds its own copies of its pipe ends. */
    CloseHandle(in_r);
    in_r = nullptr;
    CloseHandle(out_w);
    out_w = nullptr;
    if (FAILED(hr)) {
        LOG_E("CreatePseudoConsole failed (0x%08lx)", (unsigned long)hr);
        c->hpc = nullptr;
        goto fail;
    }

    SIZE_T asz = 0;
    (void)InitializeProcThreadAttributeList(nullptr, 1, 0, &asz);
    attrs = asz > 0 ? te_calloc(1, asz) : nullptr;
    if (attrs == nullptr || !InitializeProcThreadAttributeList(attrs, 1, 0, &asz) ||
        !UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE, c->hpc, sizeof c->hpc, nullptr,
                                   nullptr)) {
        LOG_E("cannot attach the pseudo console (error %lu)", (unsigned long)GetLastError());
        goto fail;
    }

    c->job = CreateJobObjectW(nullptr, nullptr);
    if (c->job != nullptr) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION lim = { 0 };
        lim.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE | JOB_OBJECT_LIMIT_BREAKAWAY_OK;
        if (!SetInformationJobObject(c->job, JobObjectExtendedLimitInformation, &lim, sizeof lim)) {
            CloseHandle(c->job);
            c->job = nullptr;
        }
    }

    STARTUPINFOEXW si = { 0 };
    si.StartupInfo.cb = sizeof si;
    /* Empty standard handles: the child must use the pseudo console, never ours
     * (FeTerm started from a console window has real ones). */
    si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    si.lpAttributeList = attrs;
    if (!CreateProcessW(nullptr, wcmd, nullptr, nullptr, FALSE,
                        EXTENDED_STARTUPINFO_PRESENT | CREATE_UNICODE_ENVIRONMENT | CREATE_SUSPENDED, env, wcwd,
                        &si.StartupInfo, &pi)) {
        DWORD e = GetLastError();
        LOG_E("cannot start the shell (error %lu)", (unsigned long)e);
        rc = e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND ? TE_ERR_NOTFOUND : TE_ERR_CHILD;
        goto fail;
    }
    if (c->job != nullptr && !AssignProcessToJobObject(c->job, pi.hProcess)) {
        LOG_D("AssignProcessToJobObject failed (error %lu)", (unsigned long)GetLastError());
    }
    (void)ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);
    pi.hThread = nullptr;
    c->proc = pi.hProcess;
    pi.hProcess = nullptr;

    c->reader = CreateThread(nullptr, 0, reader_main, c, 0, nullptr);
    c->waiter = c->reader != nullptr ? CreateThread(nullptr, 0, waiter_main, c, 0, nullptr) : nullptr;
    if (c->reader == nullptr || c->waiter == nullptr) {
        LOG_E("CreateThread failed (error %lu)", (unsigned long)GetLastError());
        p->con = c;
        pty_destroy(p);
        c = nullptr;
        rc = TE_ERR_SYS;
        goto cleanup;
    }
    p->con = c;
    c = nullptr;
    rc = TE_OK;
    goto cleanup;

fail:
    if (c != nullptr) {
        close_console(c);
        free_conpty(c);   /* no threads were started yet */
        c = nullptr;
    }
cleanup:
    if (attrs != nullptr) {
        DeleteProcThreadAttributeList(attrs);
        TE_FREE(attrs);
    }
    if (in_r != nullptr) {
        CloseHandle(in_r);
    }
    if (out_w != nullptr) {
        CloseHandle(out_w);
    }
    TE_FREE(wcmd);
    TE_FREE(wcwd);
    TE_FREE(env);
    return rc;
}

te_err pty_resize(te_pty *p, uint16_t cols, uint16_t rows, uint16_t px_w, uint16_t px_h)
{
    (void)px_w;
    (void)px_h;
    if (p == nullptr || p->con == nullptr) {
        return TE_ERR_INVAL;
    }
    te_conpty *c = p->con;
    te_err rc = TE_OK;
    AcquireSRWLockExclusive(&c->lock);
    if (c->hpc != nullptr && FAILED(conpty_resize(c->hpc, console_size(cols, rows)))) {
        rc = TE_ERR_SYS;
    }
    ReleaseSRWLockExclusive(&c->lock);
    return rc;
}

ssize_t pty_read(te_pty *p, void *buf, size_t len)
{
    if (p == nullptr || p->con == nullptr || buf == nullptr) {
        return -1;
    }
    te_conpty *c = p->con;
    AcquireSRWLockExclusive(&c->lock);
    if (c->len == 0) {
        bool eof = c->eof;
        if (!eof) {
            (void)ResetEvent(c->ready);
        }
        ReleaseSRWLockExclusive(&c->lock);
        return eof ? -1 : 0;
    }
    size_t n = TE_MIN(len, c->len);
    n = TE_MIN(n, OUT_CAP - c->head);   /* the contiguous part; the caller reads again */
    n = TE_MIN(n, (size_t)INT32_MAX);
    memcpy(buf, c->buf + c->head, n);
    c->head = (c->head + n) % OUT_CAP;
    c->len -= n;
    if (c->len == 0 && !c->eof) {
        (void)ResetEvent(c->ready);
    }
    WakeConditionVariable(&c->space);
    ReleaseSRWLockExclusive(&c->lock);
    return (ssize_t)n;
}

ssize_t pty_write(te_pty *p, const void *buf, size_t len)
{
    if (p == nullptr || p->con == nullptr || p->con->in_w == nullptr || buf == nullptr) {
        return -1;
    }
    DWORD want = (DWORD)TE_MIN(len, (size_t)65536);
    DWORD put = 0;
    if (!WriteFile(p->con->in_w, buf, want, &put, nullptr)) {
        return -1;
    }
    return (ssize_t)put;
}

bool pty_reap(te_pty *p, int *status)
{
    if (p == nullptr || p->con == nullptr || p->con->proc == nullptr) {
        return true;
    }
    te_conpty *c = p->con;
    if (!c->reaped) {
        if (WaitForSingleObject(c->proc, 0) != WAIT_OBJECT_0) {
            return false;
        }
        DWORD code = 0;
        if (!GetExitCodeProcess(c->proc, &code)) {
            code = 1;
        }
        c->status = code <= 255 ? (int)code : 255;   /* NTSTATUS crash codes count as failure */
        c->reaped = true;
    }
    if (status != nullptr) {
        *status = c->status;
    }
    return true;
}

void pty_send_break(te_pty *p)
{
    if (p == nullptr || p->con == nullptr || p->con->proc == nullptr) {
        return;
    }
    /* feterm-helper.exe waits on an event named after its process id. */
    wchar_t name[64] = { 0 };
    if (hcon_break_name(GetProcessId(p->con->proc), name, TE_ARRAY_LEN(name)) != TE_OK) {
        return;
    }
    HANDLE ev = OpenEventW(EVENT_MODIFY_STATE, FALSE, name);
    if (ev == nullptr) {
        return;   /* not a helper (a shell or ssh): nothing to send to */
    }
    (void)SetEvent(ev);
    CloseHandle(ev);
}

void pty_destroy(te_pty *p)
{
    if (p == nullptr || p->con == nullptr) {
        return;
    }
    te_conpty *c = p->con;
    p->con = nullptr;
    if (c->stop != nullptr) {
        (void)SetEvent(c->stop);
    }
    AcquireSRWLockExclusive(&c->lock);
    c->closing = true;
    WakeAllConditionVariable(&c->space);
    ReleaseSRWLockExclusive(&c->lock);

    close_console(c);   /* console programs get CTRL_CLOSE_EVENT */
    if (c->in_w != nullptr) {
        CloseHandle(c->in_w);
        c->in_w = nullptr;
    }
    if (c->proc != nullptr && WaitForSingleObject(c->proc, 500) == WAIT_TIMEOUT && c->job == nullptr) {
        (void)TerminateProcess(c->proc, 1);
    }
    if (c->job != nullptr) {
        (void)TerminateJobObject(c->job, 1);   /* the shell and everything it started */
    }
    bool stuck = false;
    if (c->reader != nullptr && WaitForSingleObject(c->reader, 2000) == WAIT_TIMEOUT) {
        (void)CancelSynchronousIo(c->reader);
        stuck = WaitForSingleObject(c->reader, 1000) == WAIT_TIMEOUT;
    }
    if (c->waiter != nullptr && WaitForSingleObject(c->waiter, 2000) == WAIT_TIMEOUT) {
        stuck = true;
    }
    if (stuck) {
        /* A thread still uses c: leaking it is safer than freeing it under that thread. */
        LOG_W("a pseudo console thread did not stop; leaving its memory in place");
        return;
    }
    free_conpty(c);
}
