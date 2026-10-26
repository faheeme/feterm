/*
 * Background thread that waits on file descriptors (POSIX: poll) or waitable
 * HANDLEs (Windows: WaitForMultipleObjects) and posts one SDL user event per
 * ready object. Internal to src/platform/. An object fires once, then again
 * only after fdwatch_rearm().
 */
#ifndef TE_FDWATCH_H
#define TE_FDWATCH_H

#include <stdbool.h>
#include "common.h"

#ifdef _WIN32
typedef void *te_fdw_obj;      /* HANDLE */
#else
typedef int te_fdw_obj;        /* file descriptor */
#endif

typedef struct te_fdwatch te_fdwatch;

/* event_type: the registered SDL user event to post (its code is the tag). */
[[nodiscard]] te_err fdwatch_create(te_fdwatch **out, uint32_t event_type);
/* Stops the thread and frees everything (safe on nullptr). */
void fdwatch_destroy(te_fdwatch **w);
[[nodiscard]] te_err fdwatch_add(te_fdwatch *w, te_fdw_obj obj, int *tag);
void fdwatch_rearm(te_fdwatch *w, int tag);
void fdwatch_remove(te_fdwatch *w, int tag);

#endif
