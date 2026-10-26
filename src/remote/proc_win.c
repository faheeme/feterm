/*
 * File-transfer helper processes on Windows: not available yet. The file
 * explorer reports the failure like any other connection error.
 */
#include "remote/proc.h"

void proc_init(te_proc *p)
{
    p->pid = -1;
    p->in_fd = -1;
    p->out_fd = -1;
    p->tty_fd = -1;
}

te_err proc_spawn(te_proc *p, const char *const argv[])
{
    (void)argv;
    proc_init(p);
    return TE_ERR_PLATFORM;
}

bool proc_reap(te_proc *p, int *status)
{
    (void)p;
    if (status != nullptr) {
        *status = 1;
    }
    return true;
}

void proc_stop(te_proc *p)
{
    proc_init(p);
}
