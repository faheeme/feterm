#include "util/spawn.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "util/log.h"

#define SPAWN_MAX_CHILDREN 16

extern char **environ;

static pid_t g_children[SPAWN_MAX_CHILDREN];
static size_t g_nchildren;

void spawn_reap(void)
{
    for (size_t i = 0; i < g_nchildren;) {
        int st = 0;
        pid_t r = waitpid(g_children[i], &st, WNOHANG);
        if (r == g_children[i] || (r < 0 && errno == ECHILD)) {
            g_children[i] = g_children[--g_nchildren];
            continue;
        }
        i++;
    }
}

te_err spawn_detached(const char *const argv[])
{
    if (argv == nullptr || argv[0] == nullptr) {
        return TE_ERR_INVAL;
    }
    spawn_reap();
    if (g_nchildren >= SPAWN_MAX_CHILDREN) {
        LOG_W("too many helper programs still running; not starting %s", argv[0]);
        return TE_ERR_RANGE;
    }
    te_err rc = TE_ERR_SYS;
    posix_spawnattr_t attr;
    posix_spawn_file_actions_t fa;
    bool attr_ok = false;
    bool fa_ok = false;
    if (posix_spawnattr_init(&attr) != 0) {
        return TE_ERR_SYS;
    }
    attr_ok = true;
    if (posix_spawn_file_actions_init(&fa) != 0) {
        goto out;
    }
    fa_ok = true;

    sigset_t none;
    sigset_t all;
    sigemptyset(&none);
    sigfillset(&all);
    short flags = POSIX_SPAWN_SETSID | POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF;
    if (posix_spawnattr_setflags(&attr, flags) != 0 || posix_spawnattr_setsigmask(&attr, &none) != 0 ||
        posix_spawnattr_setsigdefault(&attr, &all) != 0) {
        goto out;
    }
    for (int fd = 0; fd <= 2; fd++) {
        if (posix_spawn_file_actions_addopen(&fa, fd, "/dev/null", fd == 0 ? O_RDONLY : O_WRONLY, 0) != 0) {
            goto out;
        }
    }
    pid_t pid = -1;
    /* posix_spawnp takes char *const[]; the strings are not modified. */
    int err = posix_spawnp(&pid, argv[0], &fa, &attr, (char *const *)(uintptr_t)argv, environ);
    if (err != 0) {
        LOG_W("cannot run %s: %s", argv[0], strerror(err));
        goto out;
    }
    g_children[g_nchildren++] = pid;
    rc = TE_OK;

out:
    if (fa_ok) {
        (void)posix_spawn_file_actions_destroy(&fa);
    }
    if (attr_ok) {
        (void)posix_spawnattr_destroy(&attr);
    }
    return rc;
}
