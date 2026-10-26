/* fd watcher thread: polls every armed fd plus a wake pipe (POSIX). */
#include "platform/fdwatch.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdatomic.h>
#include <unistd.h>

#include <SDL3/SDL.h>

#include "platform/platform.h"
#include "util/log.h"
#include "util/mem.h"

struct te_fdwatch {
    uint32_t       event_type;
    SDL_Thread    *thread;
    atomic_int     fds[TE_PLAT_MAX_FDS];     /* -1: slot free */
    atomic_bool    armed[TE_PLAT_MAX_FDS];
    atomic_int     nfds;
    int            wake_pipe[2];
    atomic_bool    quit;
};

te_err fdwatch_create(te_fdwatch **out, uint32_t event_type)
{
    if (out == nullptr) {
        return TE_ERR_INVAL;
    }
    *out = nullptr;
    te_fdwatch *w = te_calloc(1, sizeof *w);
    if (w == nullptr) {
        return TE_ERR_NOMEM;
    }
    w->event_type = event_type;
    w->wake_pipe[0] = -1;
    w->wake_pipe[1] = -1;
    atomic_init(&w->quit, false);
    atomic_init(&w->nfds, 0);
    for (size_t i = 0; i < TE_PLAT_MAX_FDS; i++) {
        atomic_init(&w->fds[i], -1);
        atomic_init(&w->armed[i], false);
    }
    *out = w;
    return TE_OK;
}

static void wake_thread(te_fdwatch *w)
{
    if (w->wake_pipe[1] >= 0) {
        ssize_t n = write(w->wake_pipe[1], "w", 1);
        (void)n;   /* the pipe is non-blocking; a full pipe already means "wake up" */
    }
}

void fdwatch_destroy(te_fdwatch **wp)
{
    if (wp == nullptr || *wp == nullptr) {
        return;
    }
    te_fdwatch *w = *wp;
    if (w->thread != nullptr) {
        atomic_store(&w->quit, true);
        wake_thread(w);
        SDL_WaitThread(w->thread, nullptr);
        w->thread = nullptr;
    }
    for (size_t i = 0; i < 2; i++) {
        if (w->wake_pipe[i] >= 0) {
            close(w->wake_pipe[i]);
            w->wake_pipe[i] = -1;
        }
    }
    TE_FREE(*wp);
}

static int SDLCALL fd_thread(void *arg)
{
    te_fdwatch *w = arg;
    while (!atomic_load(&w->quit)) {
        struct pollfd pfd[TE_PLAT_MAX_FDS + 1];
        int tags[TE_PLAT_MAX_FDS + 1] = { 0 };
        nfds_t n = 0;
        pfd[n] = (struct pollfd){ .fd = w->wake_pipe[0], .events = POLLIN, .revents = 0 };
        tags[n++] = -1;
        int count = atomic_load(&w->nfds);
        for (int i = 0; i < count && i < TE_PLAT_MAX_FDS; i++) {
            if (atomic_load(&w->armed[i]) && atomic_load(&w->fds[i]) >= 0) {
                pfd[n] = (struct pollfd){ .fd = atomic_load(&w->fds[i]), .events = POLLIN, .revents = 0 };
                tags[n++] = i;
            }
        }
        int r = poll(pfd, n, -1);
        if (r < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }
        for (nfds_t k = 0; k < n; k++) {
            if (pfd[k].revents == 0) {
                continue;
            }
            if (tags[k] < 0) {
                char drain[64];
                while (read(w->wake_pipe[0], drain, sizeof drain) > 0) {
                }
                continue;
            }
            /* Disarm until the main thread has drained this fd and re-armed it. */
            atomic_store(&w->armed[tags[k]], false);
            SDL_Event ev;
            SDL_zero(ev);
            ev.type = w->event_type;
            ev.user.code = tags[k];
            if (!SDL_PushEvent(&ev)) {
                LOG_W("SDL_PushEvent failed: %s", SDL_GetError());
                atomic_store(&w->armed[tags[k]], true);
            }
        }
    }
    return 0;
}

te_err fdwatch_add(te_fdwatch *w, te_fdw_obj fd, int *tag)
{
    if (w == nullptr || fd < 0 || tag == nullptr) {
        return TE_ERR_INVAL;
    }
    int count = atomic_load(&w->nfds);
    int n = -1;
    for (int i = 0; i < count; i++) {
        if (atomic_load(&w->fds[i]) < 0) {
            n = i;   /* reuse a freed slot */
            break;
        }
    }
    if (n < 0) {
        if (count >= TE_PLAT_MAX_FDS) {
            return TE_ERR_RANGE;
        }
        n = count;
    }
    if (w->wake_pipe[0] < 0 && pipe2(w->wake_pipe, O_CLOEXEC | O_NONBLOCK) != 0) {
        return TE_ERR_SYS;
    }
    atomic_store(&w->fds[n], fd);
    atomic_store(&w->armed[n], true);
    if (n == count) {
        atomic_store(&w->nfds, n + 1);
    }
    *tag = n;
    if (w->thread == nullptr) {
        w->thread = SDL_CreateThread(fd_thread, "fd-watch", w);
        if (w->thread == nullptr) {
            LOG_E("SDL_CreateThread: %s", SDL_GetError());
            atomic_store(&w->fds[n], -1);
            return TE_ERR_PLATFORM;
        }
    } else {
        wake_thread(w);
    }
    return TE_OK;
}

void fdwatch_rearm(te_fdwatch *w, int tag)
{
    if (w == nullptr || tag < 0 || tag >= atomic_load(&w->nfds)) {
        return;
    }
    atomic_store(&w->armed[tag], true);
    wake_thread(w);
}

void fdwatch_remove(te_fdwatch *w, int tag)
{
    if (w == nullptr || tag < 0 || tag >= atomic_load(&w->nfds)) {
        return;
    }
    atomic_store(&w->armed[tag], false);
    atomic_store(&w->fds[tag], -1);
    wake_thread(w);
}
