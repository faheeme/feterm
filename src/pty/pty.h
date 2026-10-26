/* Pseudo-terminal creation and the child shell process. */
#ifndef TE_PTY_H
#define TE_PTY_H

#include <stdbool.h>
#include <sys/types.h>
#include "common.h"

#ifdef _WIN32
/* Windows: a ConPTY with its reader and exit-watcher threads (pty_win.c). */
typedef struct te_conpty te_conpty;
typedef struct te_pty {
    te_conpty *con;    /* nullptr when closed */
} te_pty;
/* Event that is set while output is waiting (for plat_watch_handle). */
void *pty_ready_handle(const te_pty *p);
#else
typedef struct te_pty {
    int   master_fd;   /* -1 when closed */
    pid_t child;       /* -1 when no child */
} te_pty;
#endif

typedef struct te_pty_opts {
    const char  *const *argv;  /* nullptr: run the user's login shell */
    const char  *shell;        /* overrides $SHELL when argv is nullptr; may be nullptr/empty */
    const char  *cwd;          /* may be nullptr */
    const char  *term;         /* TERM value for the child */
    uint16_t     cols, rows;
    uint16_t     px_w, px_h;
} te_pty_opts;

void pty_init(te_pty *p);
[[nodiscard]] te_err pty_spawn(te_pty *p, const te_pty_opts *o);
[[nodiscard]] te_err pty_resize(te_pty *p, uint16_t cols, uint16_t rows, uint16_t px_w, uint16_t px_h);

/* Non-blocking read; returns bytes read, 0 if nothing available, -1 on EOF/error (child gone). */
[[nodiscard]] ssize_t pty_read(te_pty *p, void *buf, size_t len);
/* Non-blocking write; returns bytes written (may be short or 0), -1 on fatal error. */
[[nodiscard]] ssize_t pty_write(te_pty *p, const void *buf, size_t len);

/* Ask a --serial-helper or --telnet-helper child to send a Break. */
void pty_send_break(te_pty *p);
/* Reap the child if it exited. Returns true if it has exited; *status gets the exit code. */
bool pty_reap(te_pty *p, int *status);

/* Close the master, send SIGHUP to the child's process group and reap it. */
void pty_destroy(te_pty *p);

#endif
