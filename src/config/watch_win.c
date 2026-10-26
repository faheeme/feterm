/*
 * Live reload on Windows: a change notification per watched directory. A
 * thread waits on all of them and sets one event, which the platform's
 * handle watcher reports like the inotify descriptor on Linux. Any change in
 * those directories counts (Windows does not say which file changed).
 */
#include "config/watch.h"

#include <string.h>

#include "config/paths.h"
#include "util/log.h"
#include "util/mem.h"
#include "util/str.h"

static void add_dir(te_watch *w, const char *dir)
{
    if (w->ndirs >= TE_WATCH_MAX_DIRS || dir == nullptr || dir[0] == '\0') {
        return;
    }
    wchar_t *wd = te_w_widen(dir);
    if (wd == nullptr) {
        return;
    }
    HANDLE h = FindFirstChangeNotificationW(wd, FALSE, FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_LAST_WRITE);
    TE_FREE(wd);
    if (h != INVALID_HANDLE_VALUE && h != nullptr) {
        w->dirs[w->ndirs++] = h;
    }
}

static DWORD WINAPI watch_main(void *arg)
{
    te_watch *w = arg;
    HANDLE hs[TE_WATCH_MAX_DIRS + 1] = { 0 };
    DWORD n = 0;
    hs[n++] = w->stop;
    for (size_t i = 0; i < w->ndirs; i++) {
        hs[n++] = w->dirs[i];
    }
    for (;;) {
        DWORD r = WaitForMultipleObjects(n, hs, FALSE, INFINITE);
        if (r == WAIT_OBJECT_0 || r == WAIT_FAILED) {
            return 0;
        }
        DWORD k = r - WAIT_OBJECT_0;
        if (k == 0 || k >= n) {
            continue;
        }
        (void)SetEvent(w->changed);
        if (!FindNextChangeNotification(hs[k])) {
            return 0;   /* the directory went away; stop watching */
        }
    }
}

te_err watch_init(te_watch *w, const char *extra_file)
{
    memset(w, 0, sizeof *w);
    char base[TE_PATH_MAX] = { 0 };
    char sub[TE_PATH_MAX] = { 0 };
    if (paths_config_dir(base, sizeof base) == TE_OK) {
        add_dir(w, base);
        if (str_fmt(sub, sizeof sub, "%s/themes", base) == TE_OK) add_dir(w, sub);
        if (str_fmt(sub, sizeof sub, "%s/highlight", base) == TE_OK) add_dir(w, sub);
    }
    if (extra_file != nullptr && str_copy(sub, sizeof sub, extra_file) == TE_OK) {
        char *slash = strrchr(sub, '/');
        char *bslash = strrchr(sub, '\\');
        if (bslash != nullptr && (slash == nullptr || bslash > slash)) {
            slash = bslash;
        }
        if (slash != nullptr && slash != sub) {
            *slash = '\0';
            add_dir(w, sub);
        } else if (slash == nullptr) {
            add_dir(w, ".");
        }
    }
    if (w->ndirs == 0) {
        watch_destroy(w);
        return TE_ERR_NOTFOUND;
    }
    w->changed = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    w->stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    w->thread = w->changed != nullptr && w->stop != nullptr ? CreateThread(nullptr, 0, watch_main, w, 0, nullptr)
                                                            : nullptr;
    if (w->thread == nullptr) {
        LOG_W("live reload unavailable (error %lu)", (unsigned long)GetLastError());
        watch_destroy(w);
        return TE_ERR_SYS;
    }
    return TE_OK;
}

void *watch_handle(const te_watch *w)
{
    return w != nullptr ? w->changed : nullptr;
}

bool watch_drain(te_watch *w)
{
    if (w->changed == nullptr || WaitForSingleObject(w->changed, 0) != WAIT_OBJECT_0) {
        return false;
    }
    (void)ResetEvent(w->changed);
    return true;
}

void watch_destroy(te_watch *w)
{
    if (w->thread != nullptr) {
        (void)SetEvent(w->stop);
        (void)WaitForSingleObject(w->thread, INFINITE);
        CloseHandle(w->thread);
        w->thread = nullptr;
    }
    for (size_t i = 0; i < w->ndirs; i++) {
        if (w->dirs[i] != nullptr) {
            FindCloseChangeNotification(w->dirs[i]);
            w->dirs[i] = nullptr;
        }
    }
    w->ndirs = 0;
    HANDLE *evs[] = { &w->changed, &w->stop };
    for (size_t i = 0; i < TE_ARRAY_LEN(evs); i++) {
        if (*evs[i] != nullptr) {
            CloseHandle(*evs[i]);
            *evs[i] = nullptr;
        }
    }
}
