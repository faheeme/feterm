/* Windows only: building a CreateProcessW command line from an argv array. */
#ifndef TE_WINARGS_H
#define TE_WINARGS_H

#include "common.h"

/*
 * Join argv (UTF-8, NULL-terminated) into one UTF-16 command line, each
 * argument quoted so the C runtime's parser gives back exactly that argv.
 * *out is heap (free()) on TE_OK.
 */
[[nodiscard]] te_err win_cmdline(const char *const argv[], wchar_t **out);

#endif
