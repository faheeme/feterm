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

/*
 * Process-wide security hardening, called once at startup. On Windows it drops
 * the current directory from the DLL and executable search order, so a file
 * planted beside FeTerm's working directory cannot be loaded or run in place of
 * a system one (DLL/exe planting). A no-op on POSIX.
 */
void os_harden_process(void);

/* This machine's host name (short or FQDN) into out. false if unavailable. */
[[nodiscard]] bool os_hostname(char *out, size_t outsz);

/*
 * Tag a file that just arrived from a remote host with the Mark-of-the-Web, so
 * the OS treats it as untrusted (SmartScreen/zone warnings). Writes a
 * Zone.Identifier stream on Windows and the com.apple.quarantine attribute on
 * macOS; a no-op on Linux. Best-effort: failure is ignored, since it only adds
 * a warning and never blocks the download.
 */
void os_mark_from_internet(const char *path);

#ifndef _WIN32
/*
 * Make fd non-blocking. Set a pty master's flag before fork(): on macOS the
 * pty driver can refuse it (ENOTTY) while the child is opening the other end.
 * On macOS an ENOTTY whose flag nonetheless took effect counts as success, as
 * the kernel records O_NONBLOCK before asking the driver and reads honour it.
 */
[[nodiscard]] bool os_set_nonblock(int fd);
#endif

#ifdef _WIN32
/* %LOCALAPPDATA%: per-user data that stays on this machine. */
[[nodiscard]] te_err os_local_data_base(char *out, size_t outsz);
#endif

#endif
