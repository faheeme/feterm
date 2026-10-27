/*
 * File-transfer helper processes on Windows (ssh.exe and scp.exe from the
 * OpenSSH client that ships with Windows).
 *
 * Win32-OpenSSH asks its questions on the console, not on stdin, so the
 * child gets a pseudo console of its own for those, while its stdin and
 * stdout are pipes carrying the SFTP protocol and its stderr is a third pipe.
 * Threads, all sharing one lock:
 *   out     stdout -> the `out` ring, `out_ready` set while data or EOF waits
 *   con     console output, stripped of escape sequences -> the `tty` ring
 *   err     stderr -> the `tty` ring as well (`tty_ready`)
 *   in      the `in` ring -> stdin, so writing never blocks the UI
 *   waiter  closes the pseudo console once the child exits, which is what
 *           ends its output pipe
 * The child runs in a Job Object, so stopping it also ends anything it started.
 */
#include "remote/proc.h"

#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "pty/conpty_win.h"
#include "util/log.h"
#include "util/mem.h"
#include "util/str.h"
#include "util/winargs.h"

#define RING_CAP   (1u << 20)   /* per channel; a full ring makes its thread wait */
#define CHUNK      16384u
#define CON_COLS   200          /* wide, so ssh's lines arrive unwrapped */
#define CON_ROWS   24

typedef struct ring {
    uint8_t *buf;               /* RING_CAP bytes */
    size_t   head, len;
} ring;

/* What the console thread is in the middle of (escape sequences are dropped). */
typedef enum { VT_TEXT, VT_ESC, VT_CSI, VT_OSC, VT_STR, VT_STR_ESC } vt_state;

typedef struct vt_filter {
    vt_state st;
    uint32_t param;             /* first CSI parameter, saturated */
    bool     in_first;          /* still reading the first parameter */
    uint32_t row;               /* where the console last put the cursor */
} vt_filter;

struct te_wproc {
    SRWLOCK            lock;
    CONDITION_VARIABLE cv;      /* ring space, stdin data or closing */
    te_hpcon           hpc;     /* guarded by lock; nullptr once closed */
    HANDLE             con_in;  /* answers are typed here */
    HANDLE             con_out;
    HANDLE             err_r;
    HANDLE             out_r;
    HANDLE             in_w;    /* owned by the `in` thread once it runs */
    HANDLE             proc, job, stop;
    HANDLE             out_ready, tty_ready;
    HANDLE             t_out, t_con, t_err, t_in, t_wait;
    ring               out, tty, in;
    int                tty_sources; /* con and err threads still feeding `tty` */
    bool               out_eof, in_closed, in_failed, tty_closed;
    bool               closing, reaped;
    int                status;
};

static size_t ring_space(const ring *r)
{
    return RING_CAP - r->len;
}

/* Copies up to n bytes in; returns how many fit. */
static size_t ring_put(ring *r, const uint8_t *src, size_t n)
{
    size_t done = 0;
    while (done < n && r->len < RING_CAP) {
        size_t tail = (r->head + r->len) % RING_CAP;
        size_t k = TE_MIN(n - done, RING_CAP - r->len);
        k = TE_MIN(k, RING_CAP - tail);
        memcpy(r->buf + tail, src + done, k);
        r->len += k;
        done += k;
    }
    return done;
}

static size_t ring_get(ring *r, uint8_t *dst, size_t n)
{
    size_t done = 0;
    while (done < n && r->len > 0) {
        size_t k = TE_MIN(n - done, r->len);
        k = TE_MIN(k, RING_CAP - r->head);
        memcpy(dst + done, r->buf + r->head, k);
        r->head = (r->head + k) % RING_CAP;
        r->len -= k;
        done += k;
    }
    if (r->len == 0) {
        r->head = 0;
    }
    return done;
}

/* ------------------------------------------------------------------------- */
/* Console text */

static size_t vt_emit(uint8_t *out, size_t at, size_t cap, uint8_t c)
{
    if (at < cap) {
        out[at++] = c;
    }
    return at;
}

/*
 * Keeps the text a person would read and drops escape sequences. A cursor
 * jump to another row becomes a line break and a cursor-forward becomes
 * spaces, which is how ConPTY draws them, so the last line still reads as
 * ssh printed it. Returns the bytes written to out (at most cap).
 */
static size_t vt_strip(vt_filter *f, const uint8_t *in, size_t n, uint8_t *out, size_t cap)
{
    size_t k = 0;
    for (size_t i = 0; i < n; i++) {
        uint8_t c = in[i];
        switch (f->st) {
        case VT_TEXT:
            if (c == 0x1B) {
                f->st = VT_ESC;
            } else if (c == '\n') {
                f->row++;
                k = vt_emit(out, k, cap, c);
            } else if (c >= 0x20 || c == '\r' || c == '\t') {
                k = vt_emit(out, k, cap, c);
            }
            break;
        case VT_ESC:
            if (c == '[') {
                f->st = VT_CSI;
                f->param = 0;
                f->in_first = true;
            } else if (c == ']') {
                f->st = VT_OSC;
            } else if (c == 'P' || c == 'X' || c == '^' || c == '_') {
                f->st = VT_STR;
            } else {
                f->st = VT_TEXT;   /* a two-byte sequence */
            }
            break;
        case VT_CSI:
            if (c >= '0' && c <= '9') {
                if (f->in_first && f->param < 100000u) {
                    f->param = f->param * 10u + (uint32_t)(c - '0');
                }
            } else if (c == ';' || c == ':') {
                f->in_first = false;
            } else if (c >= 0x40 && c <= 0x7E) {
                uint32_t p = f->param == 0 ? 1u : f->param;
                if ((c == 'H' || c == 'f') && p != f->row) {
                    f->row = p;
                    k = vt_emit(out, k, cap, '\n');
                } else if (c == 'C') {
                    for (uint32_t s = 0; s < TE_MIN(p, 8u); s++) {   /* enough to keep words apart */
                        k = vt_emit(out, k, cap, ' ');
                    }
                }
                f->st = VT_TEXT;
            } else if (c < 0x20 || c > 0x3F) {
                f->st = c == 0x1B ? VT_ESC : VT_TEXT;   /* malformed: resynchronise */
            }
            break;
        case VT_OSC:
            if (c == 0x07) {
                f->st = VT_TEXT;
            } else if (c == 0x1B) {
                f->st = VT_STR_ESC;
            }
            break;
        case VT_STR:
            if (c == 0x1B) {
                f->st = VT_STR_ESC;
            }
            break;
        case VT_STR_ESC:
            f->st = c == '\\' ? VT_TEXT : (c == 0x1B ? VT_STR_ESC : VT_STR);
            break;
        }
    }
    return k;
}

/* ------------------------------------------------------------------------- */
/* Threads */

static void close_console(te_wproc *w)
{
    AcquireSRWLockExclusive(&w->lock);
    te_hpcon h = w->hpc;
    w->hpc = nullptr;
    ReleaseSRWLockExclusive(&w->lock);
    if (h != nullptr) {
        conpty_close(h);   /* may wait for the console thread to drain the last output */
    }
}

/* Moves n bytes into ring r, waiting for space; drops them once the process is closing. */
static void feed(te_wproc *w, ring *r, HANDLE ready, const uint8_t *data, size_t n)
{
    size_t off = 0;
    AcquireSRWLockExclusive(&w->lock);
    while (off < n && !w->closing) {
        while (ring_space(r) == 0 && !w->closing) {
            (void)SleepConditionVariableSRW(&w->cv, &w->lock, INFINITE, 0);
        }
        off += ring_put(r, data + off, n - off);
        (void)SetEvent(ready);
    }
    ReleaseSRWLockExclusive(&w->lock);
}

static void tty_source_done(te_wproc *w)
{
    AcquireSRWLockExclusive(&w->lock);
    if (w->tty_sources > 0) {
        w->tty_sources--;
    }
    if (w->tty_sources == 0) {
        (void)SetEvent(w->tty_ready);   /* end of file is waiting */
    }
    ReleaseSRWLockExclusive(&w->lock);
}

static DWORD WINAPI out_main(void *arg)
{
    te_wproc *w = arg;
    uint8_t chunk[CHUNK] = { 0 };
    for (;;) {
        DWORD got = 0;
        if (!ReadFile(w->out_r, chunk, sizeof chunk, &got, nullptr) || got == 0) {
            break;
        }
        feed(w, &w->out, w->out_ready, chunk, got);
    }
    AcquireSRWLockExclusive(&w->lock);
    w->out_eof = true;
    (void)SetEvent(w->out_ready);
    ReleaseSRWLockExclusive(&w->lock);
    return 0;
}

static DWORD WINAPI con_main(void *arg)
{
    te_wproc *w = arg;
    vt_filter f = { .st = VT_TEXT, .row = 1 };
    uint8_t chunk[4096] = { 0 };
    uint8_t text[3 * 4096] = { 0 };   /* a 3-byte cursor-forward becomes at most 8 spaces */
    for (;;) {
        DWORD got = 0;
        if (!ReadFile(w->con_out, chunk, sizeof chunk, &got, nullptr) || got == 0) {
            break;
        }
        size_t n = vt_strip(&f, chunk, got, text, sizeof text);
        if (n > 0) {
            feed(w, &w->tty, w->tty_ready, text, n);
        }
    }
    SecureZeroMemory(chunk, sizeof chunk);
    SecureZeroMemory(text, sizeof text);
    tty_source_done(w);
    return 0;
}

static DWORD WINAPI err_main(void *arg)
{
    te_wproc *w = arg;
    uint8_t chunk[4096] = { 0 };
    for (;;) {
        DWORD got = 0;
        if (!ReadFile(w->err_r, chunk, sizeof chunk, &got, nullptr) || got == 0) {
            break;
        }
        feed(w, &w->tty, w->tty_ready, chunk, got);
    }
    tty_source_done(w);
    return 0;
}

static DWORD WINAPI in_main(void *arg)
{
    te_wproc *w = arg;
    uint8_t chunk[CHUNK] = { 0 };
    for (;;) {
        AcquireSRWLockExclusive(&w->lock);
        while (w->in.len == 0 && !w->in_closed && !w->closing) {
            (void)SleepConditionVariableSRW(&w->cv, &w->lock, INFINITE, 0);
        }
        if (w->closing || (w->in.len == 0 && w->in_closed)) {
            ReleaseSRWLockExclusive(&w->lock);
            break;
        }
        size_t n = ring_get(&w->in, chunk, sizeof chunk);
        WakeAllConditionVariable(&w->cv);
        ReleaseSRWLockExclusive(&w->lock);
        size_t off = 0;
        while (off < n) {
            DWORD put = 0;
            if (!WriteFile(w->in_w, chunk + off, (DWORD)(n - off), &put, nullptr) || put == 0) {
                AcquireSRWLockExclusive(&w->lock);
                w->in_failed = true;
                ReleaseSRWLockExclusive(&w->lock);
                goto out;
            }
            off += put;
        }
    }
out:
    /* Closing stdin is how the child learns there is no more input. */
    CloseHandle(w->in_w);
    w->in_w = nullptr;
    return 0;
}

static DWORD WINAPI wait_main(void *arg)
{
    te_wproc *w = arg;
    HANDLE hs[2] = { w->proc, w->stop };
    if (WaitForMultipleObjects(2, hs, FALSE, INFINITE) == WAIT_OBJECT_0) {
        close_console(w);   /* the child is gone: end the console output */
    }
    return 0;
}

/* ------------------------------------------------------------------------- */
/* Start and stop */

static void close_handle(HANDLE *h)
{
    if (*h != nullptr && *h != INVALID_HANDLE_VALUE) {
        CloseHandle(*h);
    }
    *h = nullptr;
}

static void free_wproc(te_wproc *w)
{
    HANDLE *hs[] = { &w->con_in, &w->con_out, &w->err_r, &w->out_r, &w->in_w, &w->proc, &w->job, &w->stop,
                     &w->out_ready, &w->tty_ready, &w->t_out, &w->t_con, &w->t_err, &w->t_in, &w->t_wait };
    for (size_t i = 0; i < TE_ARRAY_LEN(hs); i++) {
        close_handle(hs[i]);
    }
    if (w->tty.buf != nullptr) {
        SecureZeroMemory(w->tty.buf, RING_CAP);   /* prompts and what was typed at them */
    }
    TE_FREE(w->out.buf);
    TE_FREE(w->tty.buf);
    TE_FREE(w->in.buf);
    free(w);
}

static char spawn_error[200];

/* Records (and logs) which step failed and Windows' reason for it. */
static void set_error(const char *what, DWORD code)
{
    char sys[128] = { 0 };
    DWORD n = FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, code, 0, sys,
                             (DWORD)sizeof sys, nullptr);
    if (n >= sizeof sys) {
        n = 0;
    }
    sys[n] = '\0';
    while (n > 0 && (sys[n - 1] == '\r' || sys[n - 1] == '\n' || sys[n - 1] == ' ' || sys[n - 1] == '.')) {
        sys[--n] = '\0';
    }
    if (n > 0) {
        (void)str_fmt(spawn_error, sizeof spawn_error, "%s: %s (error %lu)", what, sys, (unsigned long)code);
    } else {
        (void)str_fmt(spawn_error, sizeof spawn_error, "%s (error 0x%08lx)", what, (unsigned long)code);
    }
    LOG_W("%s", spawn_error);
}

const char *proc_spawn_error(void)
{
    return spawn_error;
}

/* Full path of the OpenSSH program argv0 ("ssh" or "scp"): the one Windows ships, else PATH. */
static te_err find_program(const char *argv0, wchar_t *out, DWORD cap)
{
    wchar_t exe[64] = { 0 };
    size_t len = strnlen(argv0, 32);
    if (len == 0 || len >= 32 || strpbrk(argv0, "/\\:") != nullptr ||
        _snwprintf(exe, TE_ARRAY_LEN(exe), L"%hs.exe", argv0) <= 0) {
        return TE_ERR_INVAL;
    }
    exe[TE_ARRAY_LEN(exe) - 1] = L'\0';
    wchar_t sys[MAX_PATH] = { 0 };
    UINT s = GetSystemDirectoryW(sys, MAX_PATH);
    if (s > 0 && s < MAX_PATH && _snwprintf(out, cap, L"%ls\\OpenSSH\\%ls", sys, exe) > 0) {
        out[cap - 1] = L'\0';
        DWORD at = GetFileAttributesW(out);
        if (at != INVALID_FILE_ATTRIBUTES && (at & FILE_ATTRIBUTE_DIRECTORY) == 0) {
            return TE_OK;
        }
    }
    /* PATH only: never the current directory. */
    const wchar_t *path = _wgetenv(L"PATH");
    if (path != nullptr && path[0] != L'\0') {
        DWORD n = SearchPathW(path, exe, nullptr, cap, out, nullptr);
        if (n > 0 && n < cap) {
            return TE_OK;
        }
    }
    out[0] = L'\0';
    return TE_ERR_NOTFOUND;
}

/* A pipe whose child end is inheritable and whose own end is not. */
static bool make_pipe(HANDLE *child_end, HANDLE *own_end, bool child_reads)
{
    SECURITY_ATTRIBUTES sa = { .nLength = sizeof sa, .bInheritHandle = TRUE };
    HANDLE r = nullptr;
    HANDLE wr = nullptr;
    if (!CreatePipe(&r, &wr, &sa, 0)) {
        return false;
    }
    *child_end = child_reads ? r : wr;
    *own_end = child_reads ? wr : r;
    if (!SetHandleInformation(*own_end, HANDLE_FLAG_INHERIT, 0)) {
        CloseHandle(r);
        CloseHandle(wr);
        *child_end = nullptr;
        *own_end = nullptr;
        return false;
    }
    return true;
}

void proc_init(te_proc *p)
{
    p->w = nullptr;
}

te_err proc_spawn(te_proc *p, const char *const argv[])
{
    spawn_error[0] = '\0';
    if (p == nullptr || argv == nullptr || argv[0] == nullptr || p->w != nullptr) {
        (void)str_copy(spawn_error, sizeof spawn_error, "internal error (bad arguments)");
        return TE_ERR_INVAL;
    }
    if (!conpty_load()) {
        (void)str_copy(spawn_error, sizeof spawn_error, "this Windows has no pseudo console (needs 10 1809+)");
        return TE_ERR_PLATFORM;
    }
    wchar_t app[MAX_PATH] = { 0 };
    te_err rc = find_program(argv[0], app, MAX_PATH);
    if (rc != TE_OK) {
        (void)str_fmt(spawn_error, sizeof spawn_error, "%s.exe not found in System32\\OpenSSH or PATH", argv[0]);
        LOG_W("%s", spawn_error);
        return rc;
    }
    wchar_t *wcmd = nullptr;
    rc = win_cmdline(argv, &wcmd);
    if (rc != TE_OK) {
        (void)str_fmt(spawn_error, sizeof spawn_error, "cannot build the command line (%s)", te_strerror(rc));
        return rc;
    }
    rc = TE_ERR_SYS;
    HANDLE child_in = nullptr;
    HANDLE child_out = nullptr;
    HANDLE child_err = nullptr;
    HANDLE pc_in = nullptr;
    HANDLE pc_out = nullptr;
    LPPROC_THREAD_ATTRIBUTE_LIST attrs = nullptr;
    PROCESS_INFORMATION pi = { 0 };

    te_wproc *w = te_calloc(1, sizeof *w);
    if (w == nullptr) {
        TE_FREE(wcmd);
        (void)str_copy(spawn_error, sizeof spawn_error, "out of memory");
        return TE_ERR_NOMEM;
    }
    InitializeSRWLock(&w->lock);
    InitializeConditionVariable(&w->cv);
    w->tty_sources = 2;
    w->out.buf = te_malloc(RING_CAP);
    w->tty.buf = te_malloc(RING_CAP);
    w->in.buf = te_malloc(RING_CAP);
    w->stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    w->out_ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    w->tty_ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (w->out.buf == nullptr || w->tty.buf == nullptr || w->in.buf == nullptr) {
        rc = TE_ERR_NOMEM;
        (void)str_copy(spawn_error, sizeof spawn_error, "out of memory");
        goto fail;
    }
    if (w->stop == nullptr || w->out_ready == nullptr || w->tty_ready == nullptr) {
        set_error("CreateEvent", GetLastError());
        goto fail;
    }

    /* The console's own pipes are not inherited: the pseudo console holds them. */
    if (!CreatePipe(&pc_in, &w->con_in, nullptr, 0) || !CreatePipe(&w->con_out, &pc_out, nullptr, 0) ||
        !make_pipe(&child_in, &w->in_w, true) || !make_pipe(&child_out, &w->out_r, false) ||
        !make_pipe(&child_err, &w->err_r, false)) {
        set_error("CreatePipe", GetLastError());
        goto fail;
    }
    /* No PSEUDOCONSOLE_INHERIT_CURSOR: nothing would answer its cursor query. */
    COORD size = { .X = CON_COLS, .Y = CON_ROWS };
    HRESULT hr = conpty_create(size, pc_in, pc_out, 0, &w->hpc);
    close_handle(&pc_in);
    close_handle(&pc_out);
    if (FAILED(hr)) {
        set_error("CreatePseudoConsole", (DWORD)hr);
        w->hpc = nullptr;
        goto fail;
    }

    /* Two attributes: the console, and exactly which handles the child inherits. */
    HANDLE inherit[3] = { child_in, child_out, child_err };
    SIZE_T asz = 0;
    (void)InitializeProcThreadAttributeList(nullptr, 2, 0, &asz);
    attrs = asz > 0 ? te_calloc(1, asz) : nullptr;
    if (attrs == nullptr || !InitializeProcThreadAttributeList(attrs, 2, 0, &asz)) {
        TE_FREE(attrs);
        set_error("InitializeProcThreadAttributeList", GetLastError());
        goto fail;
    }
    if (!UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE, w->hpc, sizeof w->hpc, nullptr,
                                   nullptr) ||
        !UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherit, sizeof inherit, nullptr,
                                   nullptr)) {
        set_error("UpdateProcThreadAttribute", GetLastError());
        goto fail;
    }

    w->job = CreateJobObjectW(nullptr, nullptr);
    if (w->job != nullptr) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION lim = { 0 };
        lim.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (!SetInformationJobObject(w->job, JobObjectExtendedLimitInformation, &lim, sizeof lim)) {
            close_handle(&w->job);
        }
    }

    STARTUPINFOEXW si = { 0 };
    si.StartupInfo.cb = sizeof si;
    si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    si.StartupInfo.hStdInput = child_in;
    si.StartupInfo.hStdOutput = child_out;
    si.StartupInfo.hStdError = child_err;
    si.lpAttributeList = attrs;
    if (!CreateProcessW(app, wcmd, nullptr, nullptr, TRUE, EXTENDED_STARTUPINFO_PRESENT | CREATE_SUSPENDED, nullptr,
                        nullptr, &si.StartupInfo, &pi)) {
        DWORD e = GetLastError();
        set_error("CreateProcess", e);
        rc = e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND ? TE_ERR_NOTFOUND : TE_ERR_CHILD;
        goto fail;
    }
    if (w->job != nullptr && !AssignProcessToJobObject(w->job, pi.hProcess)) {
        LOG_D("AssignProcessToJobObject failed (error %lu)", (unsigned long)GetLastError());
    }
    (void)ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);
    w->proc = pi.hProcess;
    /* Only the child holds these now: its exit ends our reads. */
    close_handle(&child_in);
    close_handle(&child_out);
    close_handle(&child_err);

    w->t_out = CreateThread(nullptr, 0, out_main, w, 0, nullptr);
    w->t_con = w->t_out != nullptr ? CreateThread(nullptr, 0, con_main, w, 0, nullptr) : nullptr;
    w->t_err = w->t_con != nullptr ? CreateThread(nullptr, 0, err_main, w, 0, nullptr) : nullptr;
    w->t_in = w->t_err != nullptr ? CreateThread(nullptr, 0, in_main, w, 0, nullptr) : nullptr;
    w->t_wait = w->t_in != nullptr ? CreateThread(nullptr, 0, wait_main, w, 0, nullptr) : nullptr;
    p->w = w;
    w = nullptr;
    if (p->w->t_wait == nullptr) {
        set_error("CreateThread", GetLastError());
        proc_stop(p);   /* stops whichever threads did start */
        rc = TE_ERR_SYS;
    } else {
        rc = TE_OK;
    }
    goto cleanup;

fail:
    close_console(w);
    free_wproc(w);   /* no threads were started yet */
    w = nullptr;
cleanup:
    if (attrs != nullptr) {
        DeleteProcThreadAttributeList(attrs);
        TE_FREE(attrs);
    }
    close_handle(&pc_in);
    close_handle(&pc_out);
    close_handle(&child_in);
    close_handle(&child_out);
    close_handle(&child_err);
    TE_FREE(wcmd);
    return rc;
}

bool proc_reap(te_proc *p, int *status)
{
    if (p == nullptr || p->w == nullptr || p->w->proc == nullptr) {
        return true;
    }
    te_wproc *w = p->w;
    if (!w->reaped) {
        if (WaitForSingleObject(w->proc, 0) != WAIT_OBJECT_0) {
            return false;
        }
        DWORD code = 0;
        if (!GetExitCodeProcess(w->proc, &code)) {
            code = 1;
        }
        w->status = code <= 255 ? (int)code : 255;   /* NTSTATUS crash codes count as failure */
        w->reaped = true;
    }
    if (status != nullptr) {
        *status = w->status;
    }
    return true;
}

/* Waits for a thread; true if it is still running afterwards. */
static bool join(HANDLE t, DWORD ms)
{
    if (t == nullptr || WaitForSingleObject(t, ms) != WAIT_TIMEOUT) {
        return false;
    }
    (void)CancelSynchronousIo(t);
    return WaitForSingleObject(t, 1000) == WAIT_TIMEOUT;
}

void proc_stop(te_proc *p)
{
    if (p == nullptr || p->w == nullptr) {
        return;
    }
    te_wproc *w = p->w;
    p->w = nullptr;
    (void)SetEvent(w->stop);
    AcquireSRWLockExclusive(&w->lock);
    w->closing = true;
    WakeAllConditionVariable(&w->cv);
    ReleaseSRWLockExclusive(&w->lock);

    close_console(w);   /* console programs get CTRL_CLOSE_EVENT */
    if (w->proc != nullptr && WaitForSingleObject(w->proc, 300) == WAIT_TIMEOUT && w->job == nullptr) {
        (void)TerminateProcess(w->proc, 1);
    }
    if (w->job != nullptr) {
        (void)TerminateJobObject(w->job, 1);
    }
    bool stuck = false;
    HANDLE ts[] = { w->t_in, w->t_out, w->t_con, w->t_err };
    for (size_t i = 0; i < TE_ARRAY_LEN(ts); i++) {
        stuck = join(ts[i], 2000) || stuck;
    }
    if (w->t_wait != nullptr && WaitForSingleObject(w->t_wait, 2000) == WAIT_TIMEOUT) {
        stuck = true;
    }
    if (stuck) {
        /* A thread still uses w: leaking it is safer than freeing it under that thread. */
        LOG_W("a file-transfer helper thread did not stop; leaving its memory in place");
        return;
    }
    free_wproc(w);
}

/* ------------------------------------------------------------------------- */
/* I/O */

bool proc_running(const te_proc *p)
{
    return p->w != nullptr && !p->w->reaped;
}

bool proc_in_open(const te_proc *p)
{
    if (p->w == nullptr) {
        return false;
    }
    AcquireSRWLockShared(&p->w->lock);
    bool open = !p->w->in_closed && !p->w->in_failed;
    ReleaseSRWLockShared(&p->w->lock);
    return open;
}

bool proc_out_open(const te_proc *p)
{
    return p->w != nullptr;
}

bool proc_tty_open(const te_proc *p)
{
    return p->w != nullptr && !p->w->tty_closed;
}

te_proc_obj proc_out_obj(const te_proc *p)
{
    return p->w != nullptr ? p->w->out_ready : nullptr;
}

te_proc_obj proc_tty_obj(const te_proc *p)
{
    return p->w != nullptr ? p->w->tty_ready : nullptr;
}

/* Reads from a ring that `ready` reports on; eof says no more will come. */
static ssize_t read_ring(te_wproc *w, ring *r, HANDLE ready, bool eof_flag_out, void *buf, size_t len)
{
    AcquireSRWLockExclusive(&w->lock);
    bool eof = eof_flag_out ? w->out_eof : w->tty_sources == 0;
    size_t n = ring_get(r, buf, TE_MIN(len, (size_t)INT32_MAX));
    if (n > 0) {
        WakeAllConditionVariable(&w->cv);
    }
    if (r->len == 0 && !eof) {
        (void)ResetEvent(ready);
    }
    ReleaseSRWLockExclusive(&w->lock);
    if (n > 0) {
        return (ssize_t)n;
    }
    return eof ? -1 : 0;
}

ssize_t proc_read_out(te_proc *p, void *buf, size_t len)
{
    if (p == nullptr || p->w == nullptr || buf == nullptr) {
        return -1;
    }
    return read_ring(p->w, &p->w->out, p->w->out_ready, true, buf, len);
}

ssize_t proc_read_tty(te_proc *p, void *buf, size_t len)
{
    if (p == nullptr || p->w == nullptr || buf == nullptr || p->w->tty_closed) {
        return -1;
    }
    return read_ring(p->w, &p->w->tty, p->w->tty_ready, false, buf, len);
}

ssize_t proc_write_in(te_proc *p, const void *buf, size_t len)
{
    if (p == nullptr || p->w == nullptr || buf == nullptr) {
        return -1;
    }
    te_wproc *w = p->w;
    AcquireSRWLockExclusive(&w->lock);
    if (w->in_closed || w->in_failed) {
        ReleaseSRWLockExclusive(&w->lock);
        return -1;
    }
    size_t n = ring_put(&w->in, buf, TE_MIN(len, (size_t)INT32_MAX));
    if (n > 0) {
        WakeAllConditionVariable(&w->cv);
    }
    ReleaseSRWLockExclusive(&w->lock);
    return (ssize_t)n;
}

ssize_t proc_write_tty(te_proc *p, const void *buf, size_t len)
{
    if (p == nullptr || p->w == nullptr || p->w->con_in == nullptr || buf == nullptr) {
        return -1;
    }
    /* The console's Enter is '\r'. Small pieces, so a password is never copied far. */
    const uint8_t *src = buf;
    uint8_t piece[256] = { 0 };
    size_t take = TE_MIN(len, sizeof piece);
    for (size_t i = 0; i < take; i++) {
        piece[i] = src[i] == '\n' ? '\r' : src[i];
    }
    DWORD put = 0;
    BOOL ok = WriteFile(p->w->con_in, piece, (DWORD)take, &put, nullptr);
    SecureZeroMemory(piece, sizeof piece);
    return ok ? (ssize_t)put : -1;
}

void proc_close_in(te_proc *p)
{
    if (p == nullptr || p->w == nullptr) {
        return;
    }
    AcquireSRWLockExclusive(&p->w->lock);
    p->w->in_closed = true;   /* the `in` thread closes the pipe once the ring is empty */
    WakeAllConditionVariable(&p->w->cv);
    ReleaseSRWLockExclusive(&p->w->lock);
}

void proc_close_tty(te_proc *p)
{
    if (p != nullptr && p->w != nullptr) {
        p->w->tty_closed = true;
    }
}
