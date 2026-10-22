/*
 * A pane is one shell session: terminal state, pty, write queue, and the
 * per-session helpers (highlighter with its own device detection, search,
 * selection). Tabs and splits arrange panes (see layout.h).
 */
#ifndef TE_PANE_H
#define TE_PANE_H

#include <stdbool.h>
#include "common.h"
#include "config/config.h"
#include "config/theme.h"
#include "highlight/highlight.h"
#include "pty/pty.h"
#include "sessions/launch.h"
#include "ui/search.h"
#include "ui/selection.h"
#include "vt/term.h"

#define TE_PANE_WRITE_MAX (8u * 1024u * 1024u)

/* A pane opened from a saved (or quick-connect) session. */
typedef struct te_pane_session {
    te_session      s;
    te_session_eff  eff;
    te_launch       launch;         /* the command, kept for reconnecting */
} te_pane_session;

typedef struct te_pane {
    uint32_t        id;
    void           *owner;          /* the application (for host callbacks) */
    te_term        *term;
    te_pty          pty;
    te_highlighter *hl;             /* nullptr if unavailable */
    te_search      *search;         /* nullptr if unavailable */
    te_selection    sel;
    uint8_t        *wq;             /* bytes waiting to be written to the pty */
    size_t          wq_len, wq_cap;
    int             tag;            /* platform fd-watch tag, -1 when not watched */
    bool            fd_ready;
    bool            exited;
    int             status;
    uint32_t        prompt_count;
    uint64_t        sync_since_ms;
    bool            last_alt;
    uint64_t        bell_until_ms;
    int             x, y, w, h;     /* rectangle in the window, pixels */
    char            bar[TE_SEARCH_MAX_QUERY + 128];

    /* Sessions (nullptr for a plain shell) */
    te_pane_session *sess;          /* owned */
    uint64_t        started_ms;     /* when the current connection was started */
    uint64_t        last_input_ms;  /* last keystroke or paste from the user */
    uint64_t        last_anti_ms;   /* last anti-idle byte */
    bool            disconnected;   /* the connection dropped; the pane waits to reconnect */
    uint64_t        reconnect_at_ms;/* automatic reconnect due (0: waiting for the user) */
    uint32_t        reconnect_tries;
} te_pane;

typedef struct te_pane_spawn {
    uint32_t            id;
    void               *owner;
    const te_config    *cfg;
    const te_theme     *theme;
    te_term_host        host;       /* callbacks; .user is set to the pane */
    const char *const  *argv;       /* nullptr: login shell */
    const char         *cwd;        /* may be nullptr */
    uint16_t            cols, rows, px_w, px_h;
} te_pane_spawn;

[[nodiscard]] te_err pane_create(te_pane **out, const te_pane_spawn *sp);
/* Start the pane's command again in a fresh pty, keeping the terminal and its history. */
[[nodiscard]] te_err pane_respawn(te_pane *p, const te_config *cfg, const char *const *argv, const char *cwd);
void pane_destroy(te_pane **p);

/* (Re)create the highlighter from the config. Failure leaves p->hl nullptr. */
void pane_setup_highlight(te_pane *p, const te_config *cfg);

void pane_queue(te_pane *p, const void *data, size_t len);
void pane_flush(te_pane *p);

/*
 * Read pending pty output (time-boxed) into the terminal and queue its
 * replies. Returns false once the child has exited (p->exited, p->status set).
 * With hold, a "[process exited]" note is shown.
 */
bool pane_read(te_pane *p, bool hold);

[[nodiscard]] te_err pane_resize(te_pane *p, uint16_t cols, uint16_t rows, uint16_t px_w, uint16_t px_h);

/* The shell's working directory: OSC 7 if reported, else /proc/<pid>/cwd. */
[[nodiscard]] te_err pane_cwd(const te_pane *p, char *out, size_t outsz);

#endif
