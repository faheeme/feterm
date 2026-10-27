#include "remote/proc.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include "util/log.h"
#include "util/str.h"

static char spawn_error[200];

const char *proc_spawn_error(void)
{
    return spawn_error;
}

void proc_init(te_proc *p)
{
    p->pid = -1;
    p->in_fd = -1;
    p->out_fd = -1;
    p->tty_fd = -1;
}

static void close_fd(int *fd)
{
    if (*fd >= 0) {
        close(*fd);
        *fd = -1;
    }
}

static bool set_nonblock(int fd)
{
    int fl = fcntl(fd, F_GETFL);
    return fl >= 0 && fcntl(fd, F_SETFL, fl | O_NONBLOCK) == 0;
}

/*
 * Runs in the child after fork: only async-signal-safe calls; never returns.
 * stdin/stdout/stderr are dup2()ed into place on purpose (the analyzer would
 * otherwise flag them as leaked).
 */
#pragma GCC diagnostic push
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic ignored "-Wanalyzer-fd-leak"
#endif
[[noreturn]] static void child(const char *slave, int in_r, int out_w, char *const argv[])
{
    sigset_t none;
    sigemptyset(&none);
    sigprocmask(SIG_SETMASK, &none, nullptr);
    static const int sigs[] = { SIGCHLD, SIGINT, SIGQUIT, SIGTERM, SIGHUP, SIGPIPE, SIGTSTP, SIGTTIN, SIGTTOU };
    for (size_t i = 0; i < TE_ARRAY_LEN(sigs); i++) {
        signal(sigs[i], SIG_DFL);
    }
    if (setsid() < 0) {
        _exit(126);
    }
    int s = open(slave, O_RDWR | O_NOCTTY);
    if (s < 0 || ioctl(s, TIOCSCTTY, 0) < 0) {
        _exit(126);
    }
    if (dup2(in_r, STDIN_FILENO) < 0 || dup2(out_w, STDOUT_FILENO) < 0 || dup2(s, STDERR_FILENO) < 0) {
        _exit(126);
    }
    (void)close_range(3, ~0U, 0);
    execvp(argv[0], argv);
    _exit(127);
}
#pragma GCC diagnostic pop

te_err proc_spawn(te_proc *p, const char *const argv[])
{
    if (p == nullptr || argv == nullptr || argv[0] == nullptr || p->pid > 0) {
        return TE_ERR_INVAL;
    }
    spawn_error[0] = '\0';
    int in_pipe[2] = { -1, -1 };
    int out_pipe[2] = { -1, -1 };
    int master = -1;
    char slave[128] = { 0 };
    te_err rc = TE_ERR_SYS;
    if (pipe2(in_pipe, O_CLOEXEC) != 0 || pipe2(out_pipe, O_CLOEXEC) != 0) {
        goto fail;
    }
    master = posix_openpt(O_RDWR | O_NOCTTY | O_CLOEXEC);
    if (master < 0 || grantpt(master) != 0 || unlockpt(master) != 0 || ptsname_r(master, slave, sizeof slave) != 0) {
        goto fail;
    }
    slave[sizeof slave - 1] = '\0';
    struct winsize ws = { .ws_row = 24, .ws_col = 120 };
    (void)ioctl(master, TIOCSWINSZ, &ws);
    pid_t pid = fork();
    if (pid < 0) {
        goto fail;
    }
    if (pid == 0) {
        /* execvp takes char *const[]; the strings are never modified. */
        child(slave, in_pipe[0], out_pipe[1], (char *const *)(uintptr_t)argv);
    }
    close_fd(&in_pipe[0]);
    close_fd(&out_pipe[1]);
    p->pid = pid;
    p->in_fd = in_pipe[1];
    p->out_fd = out_pipe[0];
    p->tty_fd = master;
    if (!set_nonblock(p->in_fd) || !set_nonblock(p->out_fd) || !set_nonblock(p->tty_fd)) {
        proc_stop(p);
        return TE_ERR_SYS;
    }
    return TE_OK;

fail:
    (void)str_fmt(spawn_error, sizeof spawn_error, "%s", strerror(errno));
    LOG_W("cannot start %s: %s", argv[0], spawn_error);
    close_fd(&in_pipe[0]);
    close_fd(&in_pipe[1]);
    close_fd(&out_pipe[0]);
    close_fd(&out_pipe[1]);
    close_fd(&master);
    return rc;
}

bool proc_reap(te_proc *p, int *status)
{
    if (p->pid <= 0) {
        return true;
    }
    int st = 0;
    pid_t r = waitpid(p->pid, &st, WNOHANG);
    if (r == p->pid || (r < 0 && errno == ECHILD)) {
        p->pid = -1;
        if (status != nullptr) {
            *status = WIFEXITED(st) ? WEXITSTATUS(st) : 128 + (WIFSIGNALED(st) ? WTERMSIG(st) : 0);
        }
        return true;
    }
    return false;
}

void proc_stop(te_proc *p)
{
    close_fd(&p->in_fd);
    close_fd(&p->out_fd);
    close_fd(&p->tty_fd);
    if (p->pid <= 0) {
        return;
    }
    (void)kill(p->pid, SIGTERM);
    for (int i = 0; i < 30; i++) {
        if (proc_reap(p, nullptr)) {
            return;
        }
        struct timespec ts = { .tv_sec = 0, .tv_nsec = 10 * 1000 * 1000 };
        nanosleep(&ts, nullptr);
    }
    (void)kill(p->pid, SIGKILL);
    (void)waitpid(p->pid, nullptr, 0);
    p->pid = -1;
}

bool proc_running(const te_proc *p)
{
    return p->pid > 0;
}

bool proc_in_open(const te_proc *p)
{
    return p->in_fd >= 0;
}

bool proc_out_open(const te_proc *p)
{
    return p->out_fd >= 0;
}

bool proc_tty_open(const te_proc *p)
{
    return p->tty_fd >= 0;
}

te_proc_obj proc_out_obj(const te_proc *p)
{
    return p->out_fd;
}

te_proc_obj proc_tty_obj(const te_proc *p)
{
    return p->tty_fd;
}

/* read(2) in the convention of proc.h: 0 means "nothing yet", -1 end of file or error. */
static ssize_t fd_read(int fd, void *buf, size_t len)
{
    if (fd < 0) {
        return -1;
    }
    ssize_t n = read(fd, buf, len);
    if (n > 0) {
        return n;
    }
    return n < 0 && (errno == EAGAIN || errno == EINTR) ? 0 : -1;
}

static ssize_t fd_write(int fd, const void *buf, size_t len)
{
    if (fd < 0) {
        return -1;
    }
    ssize_t n = write(fd, buf, len);
    if (n >= 0) {
        return n;
    }
    return errno == EAGAIN || errno == EINTR ? 0 : -1;
}

ssize_t proc_write_in(te_proc *p, const void *buf, size_t len)
{
    return fd_write(p->in_fd, buf, len);
}

ssize_t proc_read_out(te_proc *p, void *buf, size_t len)
{
    return fd_read(p->out_fd, buf, len);
}

ssize_t proc_read_tty(te_proc *p, void *buf, size_t len)
{
    return fd_read(p->tty_fd, buf, len);
}

ssize_t proc_write_tty(te_proc *p, const void *buf, size_t len)
{
    return fd_write(p->tty_fd, buf, len);
}

void proc_close_in(te_proc *p)
{
    close_fd(&p->in_fd);
}

void proc_close_tty(te_proc *p)
{
    close_fd(&p->tty_fd);
}
