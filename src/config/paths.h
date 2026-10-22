/* Locating configuration and data files. All names derive from APP_NAME. */
#ifndef TE_PATHS_H
#define TE_PATHS_H

#include <stdbool.h>
#include "common.h"

#define TE_PATH_MAX 4096

/* $XDG_CONFIG_HOME/APP_NAME, or $HOME/.config/APP_NAME. */
[[nodiscard]] te_err paths_config_dir(char *out, size_t outsz);

/*
 * Look for a relative data path (e.g. "themes/nord.ini") in, in order:
 *   1. the user config directory
 *   2. <directory of the executable>/data  (running from a source tree)
 *   3. APP_DATADIR (install location)
 */
[[nodiscard]] te_err paths_find_data(const char *rel, char *out, size_t outsz);

/*
 * Fill dirs with the data search directories, lowest priority first
 * (install dir, source-tree data dir, user config dir), so later entries
 * override earlier ones. Returns the number of directories written.
 */
size_t paths_data_dirs(char dirs[][TE_PATH_MAX], size_t max);

/* True if name is a safe single path component: [A-Za-z0-9._-], no leading dot. */
[[nodiscard]] bool paths_is_safe_name(const char *name);

#endif
