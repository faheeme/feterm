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

/* pwsh if installed, else Windows PowerShell; nullptr means cmd. Heap UTF-8 path. */
static char *find_powershell(void)
{
    wchar_t found[MAX_PATH] = { 0 };
    /* Resolve against PATH only. Passing nullptr would let SearchPathW search the
     * current directory, where a planted pwsh.exe could shadow the real one. */
    DWORD plen = GetEnvironmentVariableW(L"PATH", nullptr, 0);
    if (plen > 1) {
        wchar_t *path = te_calloc(plen, sizeof *path);
        if (path != nullptr && GetEnvironmentVariableW(L"PATH", path, plen) == plen - 1) {
            DWORD n = SearchPathW(path, L"pwsh.exe", nullptr, MAX_PATH, found, nullptr);
            if (n > 0 && n < MAX_PATH) {
                TE_FREE(path);
                return te_w_narrow(found);
            }
        }
        TE_FREE(path);
    }
    wchar_t sys[MAX_PATH] = { 0 };
    UINT s = GetSystemDirectoryW(sys, MAX_PATH);
    if (s == 0 || s >= MAX_PATH ||
        _snwprintf(found, MAX_PATH, L"%ls\\WindowsPowerShell\\v1.0\\powershell.exe", sys) <= 0) {
        return nullptr;
    }
    found[MAX_PATH - 1] = L'\0';
    return GetFileAttributesW(found) != INVALID_FILE_ATTRIBUTES ? te_w_narrow(found) : nullptr;
}

/* The command line: argv, else the configured shell, else PowerShell or cmd. */
static te_err build_cmdline(const te_pty_opts *o, wchar_t **out)
{
    if (o->argv != nullptr && o->argv[0] != nullptr) {
        return win_cmdline(o->argv, out);
    }
    if (o->shell != nullptr && o->shell[0] != '\0') {
        const char *argv[] = { o->shell, nullptr };
        return win_cmdline(argv, out);
    }
    char *ps = find_powershell();
    if (ps != nullptr) {
        const char *argv[] = { ps, "-NoLogo", nullptr };
        te_err rc = win_cmdline(argv, out);
        TE_FREE(ps);
        return rc;
    }
    const char *comspec = getenv("ComSpec");
    const char *argv[] = { comspec != nullptr && comspec[0] != '\0' ? comspec : "cmd.exe", nullptr };
    return win_cmdline(argv, out);
}

/* Variables we set ourselves; inherited copies are dropped. */
static bool env_is_overridden(const wchar_t *kv)
{
    static const wchar_t *const names[] = {
        L"TERM=", L"COLORTERM=", L"TERM_PROGRAM=", L"TERM_PROGRAM_VERSION=", L"TERMINFO=",
    };
    for (size_t i = 0; i < TE_ARRAY_LEN(names); i++) {
        if (_wcsnicmp(kv, names[i], wcslen(names[i])) == 0) {
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

/* The inherited environment plus ours, sorted as CreateProcess expects. Heap block, or nullptr. */
static wchar_t *build_env(const char *term)
{
    char tval[256] = { 0 };
    if (str_fmt(tval, sizeof tval, "TERM=%s", term != nullptr && term[0] != '\0' ? term : "xterm-256color") != TE_OK) {
        return nullptr;
    }
    const char *extra_u8[4] = { tval, "COLORTERM=truecolor", "TERM_PROGRAM=" APP_DISPLAY_NAME,
                                "TERM_PROGRAM_VERSION=" APP_VERSION };
    wchar_t *extra[4] = { nullptr, nullptr, nullptr, nullptr };
    wchar_t *block = nullptr;
    const wchar_t **list = nullptr;
    wchar_t *inherited = GetEnvironmentStringsW();
    if (inherited == nullptr) {
        return nullptr;
    }
    for (size_t i = 0; i < TE_ARRAY_LEN(extra); i++) {
        extra[i] = te_w_widen(extra_u8[i]);
        if (extra[i] == nullptr) {
            goto out;
        }
    }
    size_t count = TE_ARRAY_LEN(extra);
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
        if (!env_is_overridden(e)) {
            list[k++] = e;
            total += wcslen(e) + 1;
        }
    }
    for (size_t i = 0; i < TE_ARRAY_LEN(extra) && k < count; i++) {
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

    rc = build_cmdline(o, &wcmd);
    if (rc != TE_OK) {
        goto fail;
    }
    rc = TE_ERR_SYS;
    env = build_env(o->term);
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
