#include "config/watch.h"

#include <errno.h>
#include <string.h>
#include <sys/inotify.h>
#include <unistd.h>

#include "config/paths.h"
#include "util/log.h"
#include "util/str.h"

static void add_dir(te_watch *w, const char *dir)
{
    if (w->nwd >= TE_WATCH_MAX_DIRS || dir == nullptr || dir[0] == '\0') {
        return;
    }
    int wd = inotify_add_watch(w->fd, dir, IN_CLOSE_WRITE | IN_MOVED_TO | IN_CREATE | IN_DELETE | IN_ONLYDIR);
    if (wd >= 0) {
        w->wd[w->nwd++] = wd;
    }
}

te_err watch_init(te_watch *w, const char *extra_file)
{
    memset(w, 0, sizeof *w);
    w->fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (w->fd < 0) {
        LOG_W("live reload unavailable: inotify: %s", strerror(errno));
        return TE_ERR_SYS;
    }
    char base[TE_PATH_MAX] = { 0 };
    char sub[TE_PATH_MAX] = { 0 };
    if (paths_config_dir(base, sizeof base) == TE_OK) {
        add_dir(w, base);
        if (str_fmt(sub, sizeof sub, "%s/themes", base) == TE_OK) add_dir(w, sub);
        if (str_fmt(sub, sizeof sub, "%s/highlight", base) == TE_OK) add_dir(w, sub);
    }
    if (extra_file != nullptr && str_copy(sub, sizeof sub, extra_file) == TE_OK) {
        char *slash = strrchr(sub, '/');
        if (slash != nullptr && slash != sub) {
            *slash = '\0';
            add_dir(w, sub);
        } else if (slash == nullptr) {
            add_dir(w, ".");
        }
    }
    if (w->nwd == 0) {
        watch_destroy(w);
        return TE_ERR_NOTFOUND;
    }
    return TE_OK;
}

bool watch_drain(te_watch *w)
{
    if (w->fd < 0) {
        return false;
    }
    bool changed = false;
    _Alignas(struct inotify_event) char buf[4096];
    for (;;) {
        ssize_t n = read(w->fd, buf, sizeof buf);
        if (n <= 0) {
            break;
        }
        size_t off = 0;
        while (off + sizeof(struct inotify_event) <= (size_t)n) {
            const struct inotify_event *ev = (const struct inotify_event *)(const void *)(buf + off);
            size_t step = sizeof(struct inotify_event) + ev->len;
            if (step > (size_t)n - off) {
                break;   /* truncated record: never read past the buffer */
            }
            if (ev->len > 0) {
                size_t nl = strnlen(ev->name, ev->len);
                if (nl > 4 && strncmp(ev->name + nl - 4, ".ini", 4) == 0) {
                    changed = true;
                }
            }
            off += step;
        }
    }
    return changed;
}

void watch_destroy(te_watch *w)
{
    if (w->fd >= 0) {
        close(w->fd);
    }
    w->fd = -1;
    w->nwd = 0;
}
