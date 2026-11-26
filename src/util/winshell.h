/*
 * Windows default shell: the [terminal] shell value is a preset name (pwsh,
 * cmd, gitbash, wsl:Ubuntu, ...) or a program path. This part only parses;
 * finding the program is in pty_win.c. Portable, so it is tested everywhere.
 */
#ifndef TE_WINSHELL_H
#define TE_WINSHELL_H

#include <stddef.h>
#include "common.h"

typedef enum {
    TE_WSH_AUTO,         /* empty or "auto": pwsh, else Windows PowerShell, else cmd */
    TE_WSH_PWSH,         /* PowerShell 7+ */
    TE_WSH_POWERSHELL,   /* Windows PowerShell 5.1 */
    TE_WSH_CMD,
    TE_WSH_GITBASH,
    TE_WSH_WSL,          /* optional distribution after "wsl:" */
    TE_WSH_MSYS2,
    TE_WSH_CYGWIN,
    TE_WSH_NU,           /* Nushell */
    TE_WSH_PATH,         /* anything else: a program to run */
    TE_WSH_INVALID,      /* "wsl:" with an unusable distribution name */
} te_wsh_kind;

#define TE_WSH_DISTRO_MAX 64
#define TE_WSH_MAX_ARGS   16

/* Parse value; for "wsl:<name>" the name goes to distro (else distro is ""). */
te_wsh_kind winshell_parse(const char *value, char *distro, size_t distro_sz);
/* The preset's config name ("pwsh", "gitbash", ...), or "" for AUTO/PATH/INVALID. */
const char *winshell_name(te_wsh_kind k);

/*
 * Split shell_args into words: blanks separate, "double quotes" group, no
 * escapes and nothing is expanded. At most TE_WSH_MAX_ARGS words.
 */
typedef struct te_wsh_args {
    char        buf[1024];
    const char *argv[TE_WSH_MAX_ARGS];
    size_t      argc;
} te_wsh_args;
[[nodiscard]] te_err winshell_split_args(const char *s, te_wsh_args *out);

#endif
