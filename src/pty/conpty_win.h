/*
 * Windows only: the pseudo console API (ConPTY, Windows 10 1809+), loaded at
 * run time so an older Windows gets a clear error instead of a missing entry
 * point. Shared by the shell panes (pty_win.c) and the file-transfer helpers
 * (remote/proc_win.c).
 */
#ifndef TE_CONPTY_WIN_H
#define TE_CONPTY_WIN_H

#include <stdbool.h>
#include "common.h"

#ifndef PSEUDOCONSOLE_INHERIT_CURSOR
#define PSEUDOCONSOLE_INHERIT_CURSOR 0x1
#endif
#ifndef PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE
#define PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE 0x00020016
#endif

typedef void *te_hpcon;

/* False (and logged once) when this Windows has no pseudo console. */
[[nodiscard]] bool conpty_load(void);
/* Only valid after conpty_load() returned true. */
[[nodiscard]] HRESULT conpty_create(COORD size, HANDLE in, HANDLE out, DWORD flags, te_hpcon *pc);
[[nodiscard]] HRESULT conpty_resize(te_hpcon pc, COORD size);
void conpty_close(te_hpcon pc);
/* Wine's ConPTY lacks some flags; it is only used for smoke tests on Linux. */
bool conpty_on_wine(void);

#endif
