/*
 * The Files tab: the remote file explorer of the focused SSH session. It
 * shows the current folder, a toolbar and the transfer queue, and drives a
 * te_remote. Like the Sessions tab it shares the sidebar's menu and dialog:
 * right-click only ever opens a menu, destructive actions always ask first,
 * and nothing is transferred until a menu item or button is chosen.
 */
#ifndef TE_FILES_H
#define TE_FILES_H

#include <stdbool.h>
#include "common.h"
#include "platform/platform.h"
#include "remote/remote.h"
#include "ui/form.h"
#include "ui/menu.h"
#include "ui/paint.h"

#define TE_FILES_MENU_BASE 1000    /* menu ids and dialog purposes at or above this belong here */

typedef struct te_files_host {
    void *user;
    /* Ask the platform for a file dialog; the answer comes back through files_dialog_result. */
    void (*dialog)(void *user, te_dialog_kind kind, int request);
    void (*toast)(void *user, const char *msg);
    void (*copy)(void *user, const char *text);
} te_files_host;

typedef struct te_files {
    te_files_host host;
    te_menu      *menu;            /* the sidebar's */
    te_form      *form;
    te_remote    *remote;          /* borrowed: the focused pane's connection, or nullptr */
    bool          ssh_pane;        /* the focused pane is an SSH session */
    bool          show_hidden;
    bool          follow;          /* follow the remote shell's folder */
    char          download_dir[1024];   /* expanded */

    /* Rows: indices into the remote's entries, after hiding dot files. */
    size_t       *rows;
    size_t        nrows, caprows;
    bool         *marked;
    uint32_t      gen_seen;
    const te_remote *remote_seen;
    int           sel;             /* row, -1 none */
    int           anchor;          /* for Shift+click ranges */
    int           hover;
    size_t        scroll;

    /* Geometry (physical pixels) from files_layout. */
    float         x, y, w, h, cell_w, cell_h, scale, win_w, win_h;
    float         mouse_x, mouse_y;

    /* What a menu or dialog is about. */
    char        **target;          /* names, owned */
    size_t        ntarget;
    int           dialog_kind;     /* pending file dialog purpose */
    char        **pending_paths;   /* uploads waiting for a conflict answer, owned */
    size_t        npending;
    te_remote    *prompt_remote;   /* whose login question the dialog is answering */
} te_files;

void files_init(te_files *fv, te_menu *menu, te_form *form, const te_files_host *host);
void files_destroy(te_files *fv);
void files_set_remote(te_files *fv, te_remote *r, bool ssh_pane);
void files_set_download_dir(te_files *fv, const char *dir);
void files_layout(te_files *fv, const te_painter *p, float x, float y, float w, float h, float win_w, float win_h);

bool files_button(te_files *fv, float x, float y, int button, bool down, uint8_t clicks, uint32_t mods);
void files_motion(te_files *fv, float x, float y);
bool files_wheel(te_files *fv, float dy);
bool files_key(te_files *fv, te_key key, uint32_t mods);

/* Menu choices and dialog results routed here by the sidebar (ids >= TE_FILES_MENU_BASE). */
void files_menu(te_files *fv, int id);
void files_submit(te_files *fv);
void files_cancel(te_files *fv);

/* From the application. */
void files_prompt(te_files *fv, te_remote *r, te_prompt_kind kind, const char *text);
void files_dialog_result(te_files *fv, int request, char **paths, size_t n, bool failed, const char *error);
void files_drop(te_files *fv, const char *path);
/* The remote shell reported a new folder (OSC 7). */
void files_follow(te_files *fv, const char *path);

void files_draw(const te_files *fv, const te_painter *p, const te_ui_colors *c);

/* "1.5M" style size (exposed for tests). */
void files_human_size(uint64_t bytes, char *out, size_t outsz);

#endif
