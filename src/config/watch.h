/* Watcher for live reload of config, themes and highlight profiles (inotify; watch_win.c on Windows, watch_mac.c on macOS). */
#ifndef TE_WATCH_H
#define TE_WATCH_H

#include <stdbool.h>
#include "common.h"

#define TE_WATCH_MAX_DIRS 8

#ifdef _WIN32
/* Windows: one change notification per directory, merged by a thread into one event. */
typedef struct te_watch {
    void  *changed;                 /* manual-reset event; nullptr when unavailable */
    void  *dirs[TE_WATCH_MAX_DIRS]; /* FindFirstChangeNotification handles */
    size_t ndirs;
    void  *stop;
    void  *thread;
} te_watch;
/* The event to wait on (plat_watch_handle), or nullptr. */
void *watch_handle(const te_watch *w);
#elif defined(__APPLE__)
#define TE_WATCH_MAX_FILES 64
/* macOS: a kqueue watching each directory and the .ini files in it. kqueue does
 * not name the changed file, so a fingerprint of the .ini files decides. */
typedef struct te_watch {
    int      fd;                    /* the kqueue; -1 when unavailable */
    int      dirfd[TE_WATCH_MAX_DIRS];
    size_t   ndirs;
    int      filefd[TE_WATCH_MAX_FILES];
    size_t   nfiles;
    uint64_t print;                 /* fingerprint of every watched .ini (name, size, mtime, inode) */
} te_watch;
#else
typedef struct te_watch {
    int fd;                         /* -1 when unavailable */
    int wd[TE_WATCH_MAX_DIRS];
    size_t nwd;
} te_watch;
#endif

/* Watch the user config dir and its themes/ and highlight/ subdirectories, plus extra_file's directory. */
[[nodiscard]] te_err watch_init(te_watch *w, const char *extra_file);
/* Drain pending events; true if any watched .ini file changed. */
[[nodiscard]] bool watch_drain(te_watch *w);
void watch_destroy(te_watch *w);

#endif
