#include "config/watch.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/event.h>
#include <sys/stat.h>
#include <unistd.h>

#include "config/paths.h"
#include "util/log.h"
#include "util/str.h"

/* Directory: entries added, removed or renamed. File: written in place, or replaced. */
#define DIR_NOTES  (NOTE_WRITE | NOTE_DELETE | NOTE_RENAME)
#define FILE_NOTES (NOTE_WRITE | NOTE_EXTEND | NOTE_ATTRIB | NOTE_DELETE | NOTE_RENAME)

static bool is_ini(const char *name)
{
    size_t n = strlen(name);
    return n > 4 && strcmp(name + n - 4, ".ini") == 0;
}

/* Register a kevent for fd; closes fd and returns -1 on failure. */
static int arm(int kq, int fd, uint32_t notes)
{
    if (fd < 0) {
        return -1;
    }
    struct kevent kev;
    memset(&kev, 0, sizeof kev);
    EV_SET(&kev, (uintptr_t)fd, EVFILT_VNODE, EV_ADD | EV_CLEAR, notes, 0, nullptr);
    if (kevent(kq, &kev, 1, nullptr, 0, nullptr) != 0) {
        (void)close(fd);
        return -1;
    }
    return fd;
}

static uint64_t fnv(uint64_t h, const void *data, size_t len)
{
    const unsigned char *p = data;
    for (size_t i = 0; i < len; i++) {
        h ^= p[i];
        h *= 1099511628211u;
    }
    return h;
}

static void close_files(te_watch *w)
{
    for (size_t i = 0; i < w->nfiles; i++) {
        if (w->filefd[i] >= 0) {
            (void)close(w->filefd[i]);   /* closing also removes its kevent */
        }
        w->filefd[i] = -1;
    }
    w->nfiles = 0;
}

/* Fingerprint the .ini files of every directory and (re)watch each of them. */
static uint64_t scan(te_watch *w)
{
    close_files(w);
    uint64_t h = 14695981039346656037u;
    for (size_t d = 0; d < w->ndirs; d++) {
        int dfd = dup(w->dirfd[d]);
        DIR *dir = dfd >= 0 ? fdopendir(dfd) : nullptr;
        if (dir == nullptr) {
            if (dfd >= 0) {
                (void)close(dfd);
            }
            continue;
        }
        rewinddir(dir);   /* the duplicate shares the offset left by the previous scan */
        h = fnv(h, &d, sizeof d);
        for (struct dirent *de = readdir(dir); de != nullptr; de = readdir(dir)) {
            if (!is_ini(de->d_name)) {
                continue;
            }
            struct stat st;
            memset(&st, 0, sizeof st);
            if (fstatat(w->dirfd[d], de->d_name, &st, 0) != 0 || !S_ISREG(st.st_mode)) {
                continue;
            }
            /* Readdir order is stable for an unchanged directory, so it can feed the hash. */
            h = fnv(h, de->d_name, strlen(de->d_name));
            h = fnv(h, &st.st_size, sizeof st.st_size);
            h = fnv(h, &st.st_mtimespec, sizeof st.st_mtimespec);
            h = fnv(h, &st.st_ino, sizeof st.st_ino);
            if (w->nfiles < TE_WATCH_MAX_FILES) {
                int ffd = arm(w->fd, openat(w->dirfd[d], de->d_name, O_EVTONLY | O_CLOEXEC), FILE_NOTES);
                if (ffd >= 0) {
                    w->filefd[w->nfiles++] = ffd;
                }
            }
        }
        (void)closedir(dir);   /* closes dfd too */
    }
    return h;
}

static void add_dir(te_watch *w, const char *dir)
{
    if (w->ndirs >= TE_WATCH_MAX_DIRS || dir == nullptr || dir[0] == '\0') {
        return;
    }
    int fd = arm(w->fd, open(dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC), DIR_NOTES);
    if (fd >= 0) {
        w->dirfd[w->ndirs++] = fd;
    }
}

te_err watch_init(te_watch *w, const char *extra_file)
{
    memset(w, 0, sizeof *w);
    for (size_t i = 0; i < TE_WATCH_MAX_FILES; i++) {
        w->filefd[i] = -1;
    }
    w->fd = kqueue();
    if (w->fd < 0) {
        LOG_W("live reload unavailable: kqueue: %s", strerror(errno));
        return TE_ERR_SYS;
    }
    int fl = fcntl(w->fd, F_GETFD);
    if (fl < 0 || fcntl(w->fd, F_SETFD, fl | FD_CLOEXEC) != 0) {
        watch_destroy(w);
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
    if (w->ndirs == 0) {
        watch_destroy(w);
        return TE_ERR_NOTFOUND;
    }
    w->print = scan(w);
    return TE_OK;
}

bool watch_drain(te_watch *w)
{
    if (w->fd < 0) {
        return false;
    }
    bool woke = false;
    struct kevent evs[32];
    const struct timespec zero = { 0, 0 };
    for (;;) {
        int n = kevent(w->fd, nullptr, 0, evs, (int)TE_ARRAY_LEN(evs), &zero);
        if (n <= 0) {
            break;
        }
        woke = true;
        if ((size_t)n < TE_ARRAY_LEN(evs)) {
            break;
        }
    }
    if (!woke) {
        return false;
    }
    /* Rescan: picks up new files, and re-watches files an editor replaced. */
    uint64_t print = scan(w);
    bool changed = print != w->print;
    w->print = print;
    return changed;
}

void watch_destroy(te_watch *w)
{
    close_files(w);
    for (size_t i = 0; i < w->ndirs; i++) {
        if (w->dirfd[i] >= 0) {
            (void)close(w->dirfd[i]);
        }
        w->dirfd[i] = -1;
    }
    w->ndirs = 0;
    if (w->fd >= 0) {
        (void)close(w->fd);
    }
    w->fd = -1;
}
