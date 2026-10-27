/*
 * Child processes for file transfer (ssh running the sftp subsystem, scp).
 * The child's stdin and stdout are pipes (the binary protocol); a terminal is
 * where OpenSSH asks its password, passphrase and host-key questions and
 * prints its errors, as text FeTerm can show in a dialog. No shell is ever
 * involved.
 *
 * POSIX: the terminal is a pseudo terminal (controlling terminal and stderr).
 * Windows (proc_win.c): it is a pseudo console (ConPTY), with stderr on a
 * pipe merged into the same text; reader and writer threads move the data
 * through bounded buffers and set an event while anything is waiting.
 */
#ifndef TE_PROC_H
#define TE_PROC_H

#include <stdbool.h>
#include <sys/types.h>
#include "common.h"

#ifdef _WIN32
typedef void *te_proc_obj;     /* waitable HANDLE: set while data or end of file is waiting */
typedef struct te_wproc te_wproc;
typedef struct te_proc {
    te_wproc *w;               /* nullptr when not started or stopped */
} te_proc;
#else
typedef int te_proc_obj;       /* file descriptor */
typedef struct te_proc {
    pid_t pid;        /* -1 when not running */
    int   in_fd;      /* write end of the child's stdin, -1 if closed */
    int   out_fd;     /* read end of the child's stdout, -1 if closed */
    int   tty_fd;     /* pty master: prompts and error messages, -1 if closed */
} te_proc;
#endif

void proc_init(te_proc *p);
/* Start argv[0] (looked up in PATH). Every channel is non-blocking. */
[[nodiscard]] te_err proc_spawn(te_proc *p, const char *const argv[]);
/* Why the last proc_spawn failed, in words for the user ("" after a success). */
const char *proc_spawn_error(void);
/* True once the child has exited; *status gets its exit code (128+signal if killed). */
bool proc_reap(te_proc *p, int *status);
/* Close everything and make sure the child is gone. */
void proc_stop(te_proc *p);

/* Started and not yet reaped. */
bool proc_running(const te_proc *p);
/* Channels still open (each closes at end of file or by proc_close_*). */
bool proc_in_open(const te_proc *p);
bool proc_out_open(const te_proc *p);
bool proc_tty_open(const te_proc *p);

/* What the host watches for proc_read_out / proc_read_tty. */
te_proc_obj proc_out_obj(const te_proc *p);
te_proc_obj proc_tty_obj(const te_proc *p);

/*
 * Non-blocking I/O. Reads return the bytes read, 0 if nothing is waiting,
 * -1 at end of file or on error. Writes return the bytes taken (possibly
 * fewer than len), 0 if the channel is full, -1 on error.
 */
[[nodiscard]] ssize_t proc_write_in(te_proc *p, const void *buf, size_t len);
[[nodiscard]] ssize_t proc_read_out(te_proc *p, void *buf, size_t len);
[[nodiscard]] ssize_t proc_read_tty(te_proc *p, void *buf, size_t len);
/* An answer typed at the terminal; '\n' is Enter. */
[[nodiscard]] ssize_t proc_write_tty(te_proc *p, const void *buf, size_t len);
void proc_close_in(te_proc *p);
void proc_close_tty(te_proc *p);

#endif
