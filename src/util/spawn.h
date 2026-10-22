/* Launch helper programs (URL opener) without a shell, and reap them. */
#ifndef TE_SPAWN_H
#define TE_SPAWN_H

#include "common.h"

/*
 * Start argv[0] (looked up in PATH) detached: new session, stdio on
 * /dev/null, default signal handling. Never uses a shell. The child is
 * reaped later by spawn_reap().
 */
[[nodiscard]] te_err spawn_detached(const char *const argv[]);

/* Collect exited helpers (call periodically). */
void spawn_reap(void);

#endif
