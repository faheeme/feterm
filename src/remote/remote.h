/*
 * The remote side of the Files tab: one SFTP connection per SSH session,
 * started as `ssh ... -s HOST sftp`. When the session shares its login
 * (OpenSSH ControlMaster) this reuses it with no second password; otherwise
 * ssh's questions (password, passphrase, host key) are passed to the host
 * as prompts and the answers written straight back, never stored.
 *
 * Folder listings, transfers and recursive operations are asynchronous: the
 * host watches the descriptors this module asks for and calls
 * remote_fd_ready / remote_service. Transfers run one at a time from a
 * queue, each pipelined (up to 16 requests of 32 KiB in flight).
 *
 * Downloads go to a hidden temporary file next to the destination and are
 * renamed into place when complete; an existing file is only replaced when
 * the caller chose that. Uploads never follow symlinks on the remote side
 * because they create files by name through SFTP. Every server reply is
 * parsed by sftp_proto with strict bounds.
 *
 * When the server has no SFTP subsystem (Cisco IOS), the state becomes
 * TE_RS_NO_SFTP and single files can still be copied with scp -O.
 */
#ifndef TE_REMOTE_H
#define TE_REMOTE_H

#include <stdbool.h>
#include "common.h"
#include "sessions/session.h"
#include "sessions/store.h"

#define TE_REMOTE_PATH_MAX    4096
#define TE_REMOTE_MAX_ENTRIES 20000     /* entries shown per folder */
#define TE_REMOTE_MAX_QUEUE   20000     /* queued transfer steps */

typedef enum { TE_RS_IDLE, TE_RS_CONNECTING, TE_RS_READY, TE_RS_NO_SFTP, TE_RS_FAILED } te_remote_state;
typedef enum { TE_PROMPT_SECRET, TE_PROMPT_TEXT, TE_PROMPT_CONFIRM } te_prompt_kind;
typedef enum { TE_CONFLICT_REPLACE, TE_CONFLICT_KEEP_BOTH, TE_CONFLICT_SKIP } te_conflict;

typedef enum {
    TE_XK_DOWNLOAD, TE_XK_UPLOAD,          /* shown in the transfer list */
    TE_XK_SCAN_DOWN, TE_XK_SCAN_DELETE,    /* folder walks */
    TE_XK_MKDIR, TE_XK_REMOVE, TE_XK_RMDIR,
    TE_XK_SCP_DOWN, TE_XK_SCP_UP,
} te_xfer_kind;

typedef enum { TE_XS_QUEUED, TE_XS_ACTIVE, TE_XS_DONE, TE_XS_FAILED, TE_XS_CANCELLED, TE_XS_SKIPPED } te_xfer_state;

typedef struct te_rentry {
    char     *name;
    uint64_t  size;
    uint32_t  perm;       /* mode bits including the type */
    uint32_t  mtime;
    uint32_t  uid, gid;
    bool      dir, link, has_size, has_time, has_perm;
} te_rentry;

typedef struct te_xfer {
    te_xfer_kind  kind;
    te_xfer_state state;
    char         *local;      /* owned */
    char         *remote;     /* owned */
    uint64_t      size, done;
    uint64_t      started_ms, finished_ms;
    te_conflict   conflict;
    bool          edit;       /* part of "edit locally": no prompts, open/watch afterwards */
    uint8_t       depth;      /* folder walk depth */
    char          error[128];
} te_xfer;

typedef struct te_remote te_remote;

typedef struct te_remote_host {
    void *user;
    bool (*watch)(void *user, int fd, int *tag);
    void (*unwatch)(void *user, int tag);
    void (*rearm)(void *user, int tag);
    void (*changed)(void *user);                         /* redraw */
    void (*prompt)(void *user, te_remote *r, te_prompt_kind kind, const char *text);
    void (*toast)(void *user, const char *msg);
    void (*edit_open)(void *user, const char *local_path);
} te_remote_host;

typedef struct te_remote_opts {
    const char *control_dir;   /* shared-login sockets ("" or nullptr: none) */
    const char *edit_dir;      /* private folder for edit-locally copies (created 0700) */
    int         keepalive;
} te_remote_opts;

[[nodiscard]] te_err remote_create(te_remote **out, const te_session *s, const te_session_eff *eff,
                                   const te_remote_opts *o, const te_remote_host *h);
void remote_destroy(te_remote **r);

/* Start (or restart after a failure) the SFTP connection. */
void remote_connect(te_remote *r);
/* A watched descriptor is readable. Returns false if the tag is not this remote's. */
bool remote_fd_ready(te_remote *r, int tag);
/* Write pending requests, notice an exited child, start queued work. */
void remote_service(te_remote *r);
/* True while there is output waiting to be written (poll again soon). */
bool remote_busy(const te_remote *r);

/* Answer the current prompt; nullptr cancels the login. The buffer is wiped by the caller. */
void remote_answer(te_remote *r, const char *answer);
bool remote_prompting(const te_remote *r);

te_remote_state remote_state(const te_remote *r);
const char *remote_error(const te_remote *r);
const char *remote_title(const te_remote *r);         /* "user@host" */
const char *remote_cwd(const te_remote *r);
bool remote_loading(const te_remote *r);
size_t remote_entries(const te_remote *r, const te_rentry **out);
uint32_t remote_generation(const te_remote *r);        /* changes whenever the listing does */

/* Browse: absolute path, or relative to the current folder ("..", "sub"). */
void remote_list(te_remote *r, const char *path);
void remote_refresh(te_remote *r);
void remote_home(te_remote *r);

/* Operations on entries of the current folder (names, not paths). */
void remote_mkdir(te_remote *r, const char *name);
void remote_rename(te_remote *r, const char *from, const char *to);
void remote_chmod(te_remote *r, const char *name, uint32_t mode);
void remote_delete(te_remote *r, const char *const *names, size_t n);    /* folders recursively */
void remote_download(te_remote *r, const char *const *names, size_t n, const char *local_dir, te_conflict c);
/* Upload local files or folders into the current folder. */
void remote_upload(te_remote *r, const char *const *paths, size_t n, te_conflict c);
void remote_edit(te_remote *r, const char *name);
/* Names among these local paths that already exist in the current folder. */
size_t remote_upload_conflicts(const te_remote *r, const char *const *paths, size_t n);

/* SCP fallback for servers without SFTP. */
void remote_scp_upload(te_remote *r, const char *local, const char *remote_path);
void remote_scp_download(te_remote *r, const char *remote_path, const char *local_dir);

size_t remote_transfers(const te_remote *r, const te_xfer **out);
void remote_cancel_all(te_remote *r);
void remote_retry_failed(te_remote *r);
void remote_clear_finished(te_remote *r);

/* Edit-locally watcher: returns its descriptor tag ownership via remote_fd_ready. */

/* Pure helpers (exposed for tests). */
[[nodiscard]] te_err remote_join(const char *dir, const char *name, char *out, size_t outsz);
/* A safe local file name from a remote one: no '/', no control characters, not "." or "..". */
[[nodiscard]] bool remote_local_name(const char *remote_name, char *out, size_t outsz);
/* "name (2).ext" style alternative that does not exist in dirfd. */
[[nodiscard]] te_err remote_unique_name(int dirfd, const char *name, char *out, size_t outsz);
/* Quote a remote path for scp -O (the remote shell parses it); false if it cannot be made safe. */
[[nodiscard]] bool remote_scp_quote(const char *path, char *out, size_t outsz);

#endif
