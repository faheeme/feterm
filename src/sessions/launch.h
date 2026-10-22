/*
 * Turning a saved session into the command a pane runs. Everything is an
 * argv array handed to exec: no shell ever parses session fields (the one
 * exception is a local session's own command, which the user typed for
 * their own machine and which runs via /bin/sh -c like `-e` would).
 *
 *   SSH     system OpenSSH, with keepalives and (optionally) connection
 *           sharing, so the file explorer can reuse the login
 *   Telnet  this binary in --telnet-helper mode
 *   Serial  this binary in --serial-helper mode
 *   Local   the login shell or the session's command
 */
#ifndef TE_LAUNCH_H
#define TE_LAUNCH_H

#include <stdbool.h>
#include "common.h"
#include "sessions/session.h"
#include "sessions/store.h"

#define TE_LAUNCH_ARGV_MAX 32

typedef struct te_launch_opts {
    const char *self_exe;       /* absolute path of this program (for the helpers) */
    const char *control_dir;    /* private dir for SSH control sockets; nullptr disables sharing */
    int         keepalive;      /* seconds between SSH/TCP keepalives */
} te_launch_opts;

typedef struct te_launch {
    const char *argv[TE_LAUNCH_ARGV_MAX + 1];   /* NULL-terminated; argc 0 = login shell */
    size_t      argc;
    char        cwd[TE_SESS_PATH_MAX];          /* "" = inherit */
    char        storage[8192];                  /* backing for the argv strings */
    size_t      used;
} te_launch;

[[nodiscard]] te_err launch_build(const te_session *s, const te_session_eff *eff, const te_launch_opts *o,
                                  te_launch *out);

/* $XDG_RUNTIME_DIR/<app> (or /tmp/<app>-<uid>), created 0700 and checked to be ours. */
[[nodiscard]] te_err launch_control_dir(char *out, size_t outsz);
/* Absolute path of the running executable. */
[[nodiscard]] te_err launch_self_exe(char *out, size_t outsz);
/* Expand a leading "~/" with $HOME. */
[[nodiscard]] te_err launch_expand_home(const char *in, char *out, size_t outsz);

#endif
