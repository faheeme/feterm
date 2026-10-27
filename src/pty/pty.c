#include "pty/pty.h"

#include <errno.h>
#include <fcntl.h>
#include <pwd.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include "util/log.h"
#include "util/mem.h"
#include "util/os.h"
#include "util/str.h"

extern char **environ;

void pty_init(te_pty *p)
{
    p->master_fd = -1;
    p->child = -1;
}

/* Variables we set ourselves; inherited copies are dropped. */
static bool env_is_overridden(const char *kv)
{
    static const char *const names[] = {
        "TERM=", "COLORTERM=", "TERM_PROGRAM=", "TERM_PROGRAM_VERSION=",
        "COLUMNS=", "LINES=", "WINDOWID=", "TERMINFO=",
    };
    for (size_t i = 0; i < TE_ARRAY_LEN(names); i++) {
        if (strncmp(kv, names[i], strlen(names[i])) == 0) {
            return true;
        }
    }
    return false;
}

static void free_env(char **env)
{
    if (env == nullptr) {
        return;
    }
    for (size_t i = 0; env[i] != nullptr; i++) {
        TE_FREE(env[i]);
    }
    free(env);
}

/* Build the child environment in the parent (allocation is unsafe after fork). */
static te_err build_env(const char *term, char ***out)
{
    size_t n = 0;
    while (environ != nullptr && environ[n] != nullptr) {
        n++;
    }
    size_t cap = 0;
    if (ckd_add(&cap, n, (size_t)5)) { /* 4 extra variables + terminator */
        return TE_ERR_OVERFLOW;
    }
    char **env = te_calloc(cap, sizeof *env);
    if (env == nullptr) {
        return TE_ERR_NOMEM;
    }
    size_t k = 0;
    for (size_t i = 0; i < n; i++) {
        if (env_is_overridden(environ[i])) {
            continue;
        }
        env[k] = str_dup(environ[i]);
        if (env[k] == nullptr) {
            free_env(env);
            return TE_ERR_NOMEM;
        }
        k++;
    }
    char tmp[256] = { 0 };
    const char *extra[4] = { nullptr, "COLORTERM=truecolor", "TERM_PROGRAM=" APP_DISPLAY_NAME,
                             "TERM_PROGRAM_VERSION=" APP_VERSION };
    if (str_fmt(tmp, sizeof tmp, "TERM=%s", term != nullptr ? term : "xterm-256color") != TE_OK) {
        free_env(env);
        return TE_ERR_TRUNC;
    }
    extra[0] = tmp;
    for (size_t i = 0; i < TE_ARRAY_LEN(extra); i++) {
        env[k] = str_dup(extra[i]);
        if (env[k] == nullptr) {
            free_env(env);
            return TE_ERR_NOMEM;
        }
        k++;
    }
    env[k] = nullptr;
    *out = env;
    return TE_OK;
}

static const char *pick_shell(const char *configured)
{
    if (configured != nullptr && configured[0] == '/') {
        return configured;
    }
    const char *env = getenv("SHELL");
    if (env != nullptr && env[0] == '/') {
        return env;
    }
    struct passwd *pw = getpwuid(getuid());
    if (pw != nullptr && pw->pw_shell != nullptr && pw->pw_shell[0] == '/') {
        return pw->pw_shell;
    }
    return "/bin/sh";
}

/*
 * Runs in the child after fork. Only async-signal-safe calls; never returns.
 * The analyzer flags the dup2()ed stdio descriptors as leaks; they are the
 * child's stdin/stdout/stderr and are inherited by exec on purpose.
 */
#pragma GCC diagnostic push
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic ignored "-Wanalyzer-fd-leak"
#endif
[[noreturn]] static void child_exec(const char *slave_name, const char *file, char *const argv[],
                                    char *const envp[], const char *cwd, const struct winsize *ws)
{
    sigset_t all;
    sigemptyset(&all);
    sigprocmask(SIG_SETMASK, &all, nullptr);
    static const int sigs[] = { SIGCHLD, SIGINT, SIGQUIT, SIGTERM, SIGHUP, SIGPIPE, SIGALRM, SIGTSTP,
                                SIGTTIN, SIGTTOU };
    for (size_t i = 0; i < TE_ARRAY_LEN(sigs); i++) {
        signal(sigs[i], SIG_DFL);
    }
    if (setsid() < 0) {
        _exit(126);
    }
    int slave = open(slave_name, O_RDWR | O_NOCTTY);
    if (slave < 0) {
        _exit(126);
    }
    if (ioctl(slave, TIOCSCTTY, 0) < 0) {
        _exit(126);
    }
    (void)ioctl(slave, TIOCSWINSZ, ws);
    if (dup2(slave, STDIN_FILENO) < 0 || dup2(slave, STDOUT_FILENO) < 0 || dup2(slave, STDERR_FILENO) < 0) {
        _exit(126);
    }
    if (slave > STDERR_FILENO) {
        close(slave);
    }
    (void)close_range(3, ~0U, 0);
    if (cwd != nullptr && cwd[0] != '\0' && chdir(cwd) != 0) {
        /* Fall back to the inherited directory; not fatal. */
    }
    execvpe(file, argv, envp);
    _exit(127);
}
#pragma GCC diagnostic pop

te_err pty_spawn(te_pty *p, const te_pty_opts *o)
{
    if (p == nullptr || o == nullptr || p->master_fd >= 0) {
        return TE_ERR_INVAL;
    }
    te_err rc = TE_OK;
    char **env = nullptr;
    char slave_name[128] = { 0 };
    char login_name[512] = { 0 };
    const char *file = nullptr;
    const char *shell_argv[2] = { nullptr, nullptr };
    char *const *argv = nullptr;

    int fd = posix_openpt(O_RDWR | O_NOCTTY | O_CLOEXEC);
    if (fd < 0) {
        LOG_E("posix_openpt: %s", strerror(errno));
        return TE_ERR_SYS;
    }
    if (grantpt(fd) != 0 || unlockpt(fd) != 0 || ptsname_r(fd, slave_name, sizeof slave_name) != 0) {
        LOG_E("pty setup: %s", strerror(errno));
        rc = TE_ERR_SYS;
        goto fail;
    }
    slave_name[sizeof slave_name - 1] = '\0';

    /* UTF-8 aware line discipline (correct backspace over multi-byte chars). */
    struct termios tio = { 0 };
    if (tcgetattr(fd, &tio) == 0) {
        tio.c_iflag |= IUTF8;
        (void)tcsetattr(fd, TCSANOW, &tio);
    }

    rc = build_env(o->term, &env);
    if (rc != TE_OK) {
        goto fail;
    }

    if (o->argv != nullptr && o->argv[0] != nullptr) {
        file = o->argv[0];
        /* execvpe takes char *const[]; the strings themselves are never modified. */
        argv = (char *const *)(uintptr_t)o->argv;
    } else {
        file = pick_shell(o->shell);
        const char *base = strrchr(file, '/');
        base = base != nullptr ? base + 1 : file;
        /* A leading '-' in argv[0] makes the shell act as a login shell. */
        rc = str_fmt(login_name, sizeof login_name, "-%s", base);
        if (rc != TE_OK) {
            goto fail;
        }
        shell_argv[0] = login_name;
        argv = (char *const *)(uintptr_t)shell_argv;
    }

    /* Before fork: only the parent uses the master (see os_set_nonblock). */
    if (!os_set_nonblock(fd)) {
        LOG_E("fcntl O_NONBLOCK: %s", strerror(errno));
        rc = TE_ERR_SYS;
        goto fail;
    }

    struct winsize ws = { .ws_row = o->rows, .ws_col = o->cols, .ws_xpixel = o->px_w, .ws_ypixel = o->px_h };
    pid_t pid = fork();
    if (pid < 0) {
        LOG_E("fork: %s", strerror(errno));
        rc = TE_ERR_SYS;
        goto fail;
    }
    if (pid == 0) {
        close(fd);
        child_exec(slave_name, file, argv, env, o->cwd, &ws);
    }

    free_env(env);
    env = nullptr;
    p->master_fd = fd;
    p->child = pid;
    return TE_OK;

fail:
    free_env(env);
    env = nullptr;
    close(fd);
    return rc;
}

te_err pty_resize(te_pty *p, uint16_t cols, uint16_t rows, uint16_t px_w, uint16_t px_h)
{
    if (p == nullptr || p->master_fd < 0 || cols == 0 || rows == 0) {
        return TE_ERR_INVAL;
    }
    struct winsize ws = { .ws_row = rows, .ws_col = cols, .ws_xpixel = px_w, .ws_ypixel = px_h };
    if (ioctl(p->master_fd, TIOCSWINSZ, &ws) != 0) {
        return TE_ERR_SYS;
    }
    return TE_OK;
}

ssize_t pty_read(te_pty *p, void *buf, size_t len)
{
    if (p == nullptr || p->master_fd < 0 || buf == nullptr || len == 0) {
        return -1;
    }
    for (;;) {
        ssize_t n = read(p->master_fd, buf, len);
        if (n > 0) {
            return n;
        }
        if (n == 0) {
            return -1;
        }
        if (errno == EINTR) {
            continue;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return 0;
        }
        return -1; /* EIO: slave side closed, child exited */
    }
}

ssize_t pty_write(te_pty *p, const void *buf, size_t len)
{
    if (p == nullptr || p->master_fd < 0 || buf == nullptr) {
        return -1;
    }
    if (len == 0) {
        return 0;
    }
    for (;;) {
        ssize_t n = write(p->master_fd, buf, len);
        if (n >= 0) {
            return n;
        }
        if (errno == EINTR) {
            continue;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return 0;
        }
        return -1;
    }
}

bool pty_reap(te_pty *p, int *status)
{
    if (p == nullptr || p->child <= 0) {
        return true;
    }
    int st = 0;
    pid_t r = waitpid(p->child, &st, WNOHANG);
    if (r == p->child || (r < 0 && errno == ECHILD)) {
        p->child = -1;
        if (status != nullptr) {
            *status = WIFEXITED(st) ? WEXITSTATUS(st) : 128 + (WIFSIGNALED(st) ? WTERMSIG(st) : 0);
        }
        return true;
    }
    return false;
}

void pty_send_break(te_pty *p)
{
    if (p != nullptr && p->child > 0) {
        (void)kill(p->child, SIGUSR1);   /* the helper turns this into a Break */
    }
}

void pty_destroy(te_pty *p)
{
    if (p == nullptr) {
        return;
    }
    if (p->master_fd >= 0) {
        close(p->master_fd);
        p->master_fd = -1;
    }
    if (p->child > 0) {
        (void)kill(-p->child, SIGHUP);
        (void)kill(p->child, SIGHUP);
        int st = 0;
        /* Give the child a moment; then stop waiting so we never hang on exit. */
        for (int i = 0; i < 50; i++) {
            pid_t r = waitpid(p->child, &st, WNOHANG);
            if (r == p->child || (r < 0 && errno == ECHILD)) {
                p->child = -1;
                return;
            }
            struct timespec ts = { .tv_sec = 0, .tv_nsec = 10 * 1000 * 1000 };
            nanosleep(&ts, nullptr);
        }
        (void)kill(p->child, SIGKILL);
        (void)waitpid(p->child, &st, 0);
        p->child = -1;
    }
}
