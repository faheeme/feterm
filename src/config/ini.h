/*
 * Minimal bounded INI parser.
 *   [section]          section names up to INI_MAX_NAME-1 bytes
 *   key = value        keys up to INI_MAX_NAME-1, values up to INI_MAX_LINE-1
 *   ; or # comment     only at the start of a line (values may contain ; and #,
 *                      which regular expressions need)
 * Values are passed on literally (only surrounding whitespace is trimmed);
 * quotes are not interpreted, so a regex like "[^"]*" survives intact.
 */
#ifndef TE_INI_H
#define TE_INI_H

#include "common.h"

#define INI_MAX_LINE   4096
#define INI_MAX_NAME   128
#define INI_MAX_FILE   (1024 * 1024)

/* Return TE_OK to continue; any other value stops parsing and is returned. */
typedef te_err (*ini_handler)(void *user, const char *section, const char *key,
                              const char *value, const char *path, unsigned line);

/* Parse the file. Malformed lines are logged and skipped. */
[[nodiscard]] te_err ini_parse_file(const char *path, ini_handler cb, void *user);
/* Parse name inside the directory dirfd, refusing symlinks. display names the file in messages. */
[[nodiscard]] te_err ini_parse_at(int dirfd, const char *name, const char *display, ini_handler cb, void *user);

#endif
