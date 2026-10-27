/*
 * The few facts about the operating system the shared code needs: where home
 * and the config base are, where this program is, and what an absolute path
 * looks like. os_posix.c and os_win.c implement it. On Windows every path is
 * returned with '/' separators (C:/Users/me), which all Win32 calls accept.
 */
#ifndef TE_OS_H
#define TE_OS_H

#include <stdbool.h>
#include "common.h"

/* "/x" on POSIX; "C:/x", "C:\x" or "//server/share" on Windows. */
[[nodiscard]] bool os_path_is_absolute(const char *path);

/* $HOME (POSIX) or %USERPROFILE% (Windows); TE_ERR_NOTFOUND if unset or relative. */
[[nodiscard]] te_err os_home_dir(char *out, size_t outsz);

/* Base for per-user config: $XDG_CONFIG_HOME or ~/.config (POSIX), %APPDATA% (Windows). */
[[nodiscard]] te_err os_config_base(char *out, size_t outsz);

/* Absolute path of the running executable. */
[[nodiscard]] te_err os_self_exe(char *out, size_t outsz);

#ifdef _WIN32
/* %LOCALAPPDATA%: per-user data that stays on this machine. */
[[nodiscard]] te_err os_local_data_base(char *out, size_t outsz);
#endif

#endif
