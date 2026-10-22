/*
 * Saved sessions on disk, SecureCRT style: one INI file per session in a
 * folder tree under <config dir>/sessions. A folder may hold _folder.ini
 * with defaults its sessions inherit.
 *
 * Every path is opened one component at a time with O_NOFOLLOW, so a symlink
 * planted in the tree can never redirect a read, write or delete. Files are
 * written atomically (temporary file, fsync, rename) with mode 0600, and
 * folders are created 0700.
 *
 * The in-memory tree is a pre-order list: each folder is followed by its
 * contents (sub-folders first, then sessions, each sorted by name).
 */
#ifndef TE_STORE_H
#define TE_STORE_H

#include <stdbool.h>
#include "common.h"
#include "config/paths.h"
#include "sessions/session.h"

#define TE_STORE_MAX_NODES 10000

typedef struct te_store_node {
    bool                folder;
    bool                expanded;     /* folders: shown open in the tree */
    int                 parent;       /* node index, -1 at the top level */
    uint8_t             depth;
    char                name[TE_SESS_NAME_MAX + 1];
    char                path[TE_SESS_FOLDER_MAX + 1];  /* folder: its own path; session: its folder */
    te_folder_defaults  fdef;         /* folders only */
    te_session         *sess;         /* sessions only (owned) */
} te_store_node;

typedef struct te_store {
    char           root[TE_PATH_MAX];
    te_store_node *nodes;
    size_t         len, cap;
    bool           truncated;          /* hit TE_STORE_MAX_NODES or the depth limit */
} te_store;

/* Open (creating if needed) the session tree at root and load it. */
[[nodiscard]] te_err store_open(te_store *st, const char *root);
/* Re-read everything; folders that were expanded stay expanded. */
[[nodiscard]] te_err store_reload(te_store *st);
void store_close(te_store *st);

/* Index of the folder node with this path, or of the session, or -1. */
int store_find_folder(const te_store *st, const char *path);
int store_find_session(const te_store *st, const char *folder, const char *name);
bool store_session_exists(const te_store *st, const char *folder, const char *name);

/*
 * Save s into s->folder. When old_name is not nullptr this replaces the
 * session old_folder/old_name (an edit or rename); otherwise it must not
 * exist yet. Reloads the tree on success.
 */
[[nodiscard]] te_err store_save_session(te_store *st, const te_session *s, const char *old_folder, const char *old_name);
[[nodiscard]] te_err store_delete_session(te_store *st, const char *folder, const char *name);
[[nodiscard]] te_err store_move_session(te_store *st, const char *folder, const char *name, const char *to_folder);
[[nodiscard]] te_err store_create_folder(te_store *st, const char *parent, const char *name);
[[nodiscard]] te_err store_rename_folder(te_store *st, const char *path, const char *new_name);
/* Deletes the folder with all its sessions and sub-folders. Refuses if it holds files FeTerm did not create. */
[[nodiscard]] te_err store_delete_folder(te_store *st, const char *path);
[[nodiscard]] te_err store_save_folder_defaults(te_store *st, const char *path, const te_folder_defaults *d);

/* Effective colour, anti-idle and user of a session after folder inheritance. */
typedef struct te_session_eff {
    te_tag_color color;
    int32_t      anti_idle;      /* TE_ANTI_IDLE_INHERIT: use the global setting */
    char         user[TE_SESS_USER_MAX + 1];
} te_session_eff;
void store_effective(const te_store *st, const te_session *s, te_session_eff *out);

/* Join folder and name into "a/b/name" (or "name"). */
[[nodiscard]] te_err store_join(const char *folder, const char *name, char *out, size_t outsz);

#endif
