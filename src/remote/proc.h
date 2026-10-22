/*
 * Child processes for file transfer (ssh running the sftp subsystem, scp).
 * The child's stdin and stdout are pipes (the binary protocol); a pseudo
 * terminal is its controlling terminal and stderr, so OpenSSH's password,
 * passphrase and host-key questions arrive as text FeTerm can show in a
 * dialog. No shell is ever involved.
 */
#ifndef TE_PROC_H
#define TE_PROC_H

#include <stdbool.h>
#include <sys/types.h>
#include "common.h"

typedef struct te_proc {
    pid_t pid;        /* -1 when not running */
    int   in_fd;      /* write end of the child's stdin, -1 if closed */
    int   out_fd;     /* read end of the child's stdout, -1 if closed */
    int   tty_fd;     /* pty master: prompts and error messages, -1 if closed */
} te_proc;

void proc_init(te_proc *p);
/* Start argv[0] (looked up in PATH). All three descriptors are non-blocking. */
[[nodiscard]] te_err proc_spawn(te_proc *p, const char *const argv[]);
/* True once the child has exited; *status gets its exit code (128+signal if killed). */
bool proc_reap(te_proc *p, int *status);
/* Close everything and make sure the child is gone (SIGTERM, then SIGKILL). */
void proc_stop(te_proc *p);

#endif
