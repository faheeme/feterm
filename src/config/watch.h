/* inotify watcher for live reload of config, themes and highlight profiles. */
#ifndef TE_WATCH_H
#define TE_WATCH_H

#include <stdbool.h>
#include "common.h"

#define TE_WATCH_MAX_DIRS 8

typedef struct te_watch {
    int fd;                         /* -1 when unavailable */
    int wd[TE_WATCH_MAX_DIRS];
    size_t nwd;
} te_watch;

/* Watch the user config dir and its themes/ and highlight/ subdirectories, plus extra_file's directory. */
[[nodiscard]] te_err watch_init(te_watch *w, const char *extra_file);
/* Drain pending events; true if any watched .ini file changed. */
[[nodiscard]] bool watch_drain(te_watch *w);
void watch_destroy(te_watch *w);

#endif
