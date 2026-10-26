/*
 * Handle watcher thread (Windows): WaitForMultipleObjects on every armed
 * HANDLE plus a wake event. Removing a handle waits until the thread has
 * rebuilt its wait list, because closing a handle that another thread is
 * waiting on is undefined on Windows.
 */
#include "platform/fdwatch.h"

#include <SDL3/SDL.h>

#include "platform/platform.h"
#include "util/log.h"
#include "util/mem.h"

/* One wait slot is the wake event. */
#define SLOTS (TE_PLAT_MAX_FDS < MAXIMUM_WAIT_OBJECTS ? TE_PLAT_MAX_FDS : MAXIMUM_WAIT_OBJECTS - 1)

struct te_fdwatch {
    uint32_t           event_type;
    SDL_Thread        *thread;
    HANDLE             wake;            /* auto-reset */
    SRWLOCK            lock;            /* guards everything below */
    CONDITION_VARIABLE acked_cv;
    HANDLE             obj[SLOTS];      /* nullptr: slot free */
    bool               armed[SLOTS];
    int                nobj;
    uint64_t           gen;             /* bumped by every remove */
    uint64_t           acked;           /* the generation the thread's wait list reflects */
    bool               quit;
    bool               running;
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
    InitializeSRWLock(&w->lock);
    InitializeConditionVariable(&w->acked_cv);
    w->wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (w->wake == nullptr) {
        TE_FREE(w);
        return TE_ERR_SYS;
    }
    *out = w;
    return TE_OK;
}

static int SDLCALL watch_thread(void *arg)
{
    te_fdwatch *w = arg;
    for (;;) {
        HANDLE hs[SLOTS + 1] = { 0 };
        int tags[SLOTS + 1] = { 0 };
        DWORD n = 0;
        hs[n] = w->wake;
        tags[n++] = -1;
        AcquireSRWLockExclusive(&w->lock);
        if (w->quit) {
            w->running = false;
            WakeAllConditionVariable(&w->acked_cv);
            ReleaseSRWLockExclusive(&w->lock);
            return 0;
        }
        for (int i = 0; i < w->nobj && i < SLOTS; i++) {
            if (w->armed[i] && w->obj[i] != nullptr) {
                hs[n] = w->obj[i];
                tags[n++] = i;
            }
        }
        w->acked = w->gen;
        WakeAllConditionVariable(&w->acked_cv);
        ReleaseSRWLockExclusive(&w->lock);

        DWORD r = WaitForMultipleObjects(n, hs, FALSE, INFINITE);
        if (r == WAIT_FAILED) {
            LOG_W("WaitForMultipleObjects failed (error %lu)", (unsigned long)GetLastError());
            Sleep(50);   /* a handle went bad between rebuilds; never spin */
            continue;
        }
        DWORD k = r >= WAIT_ABANDONED_0 && r < WAIT_ABANDONED_0 + n ? r - WAIT_ABANDONED_0 : r - WAIT_OBJECT_0;
        if (k >= n || tags[k] < 0) {
            continue;   /* woken to rebuild the list */
        }
        int tag = tags[k];
        AcquireSRWLockExclusive(&w->lock);
        bool still = tag < w->nobj && w->obj[tag] == hs[k] && w->armed[tag];
        if (still) {
            w->armed[tag] = false;   /* until the main thread has drained it and re-armed */
        }
        ReleaseSRWLockExclusive(&w->lock);
        if (!still) {
            continue;
        }
        SDL_Event ev;
        SDL_zero(ev);
        ev.type = w->event_type;
        ev.user.code = tag;
        if (!SDL_PushEvent(&ev)) {
            LOG_W("SDL_PushEvent failed: %s", SDL_GetError());
            AcquireSRWLockExclusive(&w->lock);
            if (tag < w->nobj && w->obj[tag] == hs[k]) {
                w->armed[tag] = true;
            }
            ReleaseSRWLockExclusive(&w->lock);
        }
    }
}

void fdwatch_destroy(te_fdwatch **wp)
{
    if (wp == nullptr || *wp == nullptr) {
        return;
    }
    te_fdwatch *w = *wp;
    if (w->thread != nullptr) {
        AcquireSRWLockExclusive(&w->lock);
        w->quit = true;
        ReleaseSRWLockExclusive(&w->lock);
        (void)SetEvent(w->wake);
        SDL_WaitThread(w->thread, nullptr);
        w->thread = nullptr;
    }
    if (w->wake != nullptr) {
        CloseHandle(w->wake);
        w->wake = nullptr;
    }
    TE_FREE(*wp);
}

te_err fdwatch_add(te_fdwatch *w, te_fdw_obj obj, int *tag)
{
    if (w == nullptr || obj == nullptr || obj == INVALID_HANDLE_VALUE || tag == nullptr) {
        return TE_ERR_INVAL;
    }
    AcquireSRWLockExclusive(&w->lock);
    int n = -1;
    for (int i = 0; i < w->nobj; i++) {
        if (w->obj[i] == nullptr) {
            n = i;   /* reuse a freed slot */
            break;
        }
    }
    if (n < 0) {
        if (w->nobj >= SLOTS) {
            ReleaseSRWLockExclusive(&w->lock);
            return TE_ERR_RANGE;
        }
        n = w->nobj++;
    }
    w->obj[n] = obj;
    w->armed[n] = true;
    bool start = w->thread == nullptr;
    if (start) {
        w->running = true;
    }
    ReleaseSRWLockExclusive(&w->lock);
    *tag = n;
    if (start) {
        w->thread = SDL_CreateThread(watch_thread, "fd-watch", w);
        if (w->thread == nullptr) {
            LOG_E("SDL_CreateThread: %s", SDL_GetError());
            AcquireSRWLockExclusive(&w->lock);
            w->obj[n] = nullptr;
            w->armed[n] = false;
            w->running = false;
            ReleaseSRWLockExclusive(&w->lock);
            return TE_ERR_PLATFORM;
        }
    } else {
        (void)SetEvent(w->wake);
    }
    return TE_OK;
}

void fdwatch_rearm(te_fdwatch *w, int tag)
{
    if (w == nullptr) {
        return;
    }
    AcquireSRWLockExclusive(&w->lock);
    bool ok = tag >= 0 && tag < w->nobj && w->obj[tag] != nullptr;
    if (ok) {
        w->armed[tag] = true;
    }
    ReleaseSRWLockExclusive(&w->lock);
    if (ok) {
        (void)SetEvent(w->wake);
    }
}

void fdwatch_remove(te_fdwatch *w, int tag)
{
    if (w == nullptr) {
        return;
    }
    AcquireSRWLockExclusive(&w->lock);
    if (tag < 0 || tag >= w->nobj || w->obj[tag] == nullptr) {
        ReleaseSRWLockExclusive(&w->lock);
        return;
    }
    w->obj[tag] = nullptr;
    w->armed[tag] = false;
    uint64_t want = ++w->gen;
    (void)SetEvent(w->wake);
    /* The caller closes the handle next: wait until the thread no longer waits on it. */
    while (w->running && w->acked < want) {
        if (!SleepConditionVariableSRW(&w->acked_cv, &w->lock, 1000, 0)) {
            LOG_W("handle watcher did not answer; still waiting");
        }
    }
    ReleaseSRWLockExclusive(&w->lock);
}
