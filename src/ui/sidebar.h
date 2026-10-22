/*
 * The sidebar: a collapsible panel (left by default, or right) with tabs at
 * the top. The Sessions tab is a SecureCRT-style folder tree of saved
 * sessions with a filter box, drag-to-move, and right-click menus; the Files
 * tab is the remote file explorer.
 *
 * The sidebar owns its menus and dialogs. While one is open it captures all
 * input (sidebar_captures). Right-click only ever opens a menu; nothing is
 * changed until a menu item or dialog button is chosen, and deleting always
 * asks first. The application is reached only through te_sidebar_host.
 */
#ifndef TE_SIDEBAR_H
#define TE_SIDEBAR_H

#include <stdbool.h>
#include "common.h"
#include "platform/platform.h"
#include "sessions/store.h"
#include "ui/form.h"
#include "ui/menu.h"
#include "ui/paint.h"

typedef enum { TE_SB_SESSIONS, TE_SB_FILES } te_sb_tab;

typedef struct te_sidebar_host {
    void *user;
    /* Open a session in a new tab, or split the current pane. */
    void (*connect)(void *user, const te_session *s, const te_session_eff *eff, bool split);
    void (*toast)(void *user, const char *msg);
    void (*copy)(void *user, const char *text);
} te_sidebar_host;

typedef struct te_sidebar {
    te_store         store;
    bool             store_ok;
    te_sidebar_host  host;

    bool             visible;
    bool             right;
    float            width_logical;     /* configured or dragged width */
    te_sb_tab        tab;
    bool             focused;           /* keyboard goes to the sidebar */
    bool             layout_changed;    /* visibility, side or width changed: relayout */

    /* Geometry (physical pixels), from sidebar_layout. */
    float            win_w, win_h, x, w, cell_w, cell_h, scale;

    /* Session tree */
    int             *vis;               /* visible node indices */
    size_t           nvis, viscap;
    int              sel;               /* node index, -1 none */
    int              hover;             /* node index under the pointer, -1 none */
    size_t           scroll;            /* first visible row */
    char             filter[96];
    char             sel_key[TE_SESS_FOLDER_MAX + TE_SESS_NAME_MAX + 4];

    /* Mouse */
    bool             pressed;
    float            press_x, press_y;
    int              press_node;
    bool             dragging;
    int              drop_folder;       /* node index, -1 top level, -2 none */
    bool             resizing;
    float            mouse_x, mouse_y;

    /* Overlays */
    te_menu          menu;
    int              menu_node;         /* -1: empty space */
    te_form          form;
    int              form_node;
    char             form_folder[TE_SESS_FOLDER_MAX + 1];
    char             form_name[TE_SESS_NAME_MAX + 1];
    te_session       form_session;      /* the session being edited */

    /* Folder list for the dialogs' Folder choice */
    char           (*folder_paths)[TE_SESS_FOLDER_MAX + 1];
    const char     **folder_choices;
    size_t           nfolders;
} te_sidebar;

/* Loads the session tree from sessions_root. A failure still gives a usable (empty) sidebar. */
void sidebar_init(te_sidebar *sb, const char *sessions_root, const te_sidebar_host *host);
void sidebar_destroy(te_sidebar *sb);
/* Apply [sidebar] settings. visible is only taken at startup (first_time). */
void sidebar_configure(te_sidebar *sb, bool visible, bool right, float width_logical, bool first_time);
/* Window size and font metrics for this frame; call before input and drawing. */
void sidebar_layout(te_sidebar *sb, const te_painter *p, float win_w, float win_h);
/* Width the sidebar takes (the collapsed strip when hidden) and the x where the terminal starts. */
float sidebar_width(const te_sidebar *sb);
float sidebar_term_x(const te_sidebar *sb);
bool sidebar_take_layout_change(te_sidebar *sb);

bool sidebar_captures(const te_sidebar *sb);           /* a menu or dialog is open */
bool sidebar_contains(const te_sidebar *sb, float x, float y);
bool sidebar_on_edge(const te_sidebar *sb, float x, float y);
bool sidebar_focused(const te_sidebar *sb);

/* Input. Each returns true if the sidebar used the event. button: 0 left, 1 middle, 2 right. */
bool sidebar_button(te_sidebar *sb, float x, float y, int button, bool down, uint8_t clicks);
bool sidebar_motion(te_sidebar *sb, float x, float y, bool left_down);
bool sidebar_wheel(te_sidebar *sb, float x, float y, float dy);
bool sidebar_key(te_sidebar *sb, te_key key, uint32_t cp, uint32_t mods);
bool sidebar_text(te_sidebar *sb, const char *utf8);

void sidebar_toggle(te_sidebar *sb);
void sidebar_show_tab(te_sidebar *sb, te_sb_tab tab);
void sidebar_blur(te_sidebar *sb);
void sidebar_quick_connect(te_sidebar *sb);
/* Re-read the session folder (e.g. on config reload). */
void sidebar_reload(te_sidebar *sb);

void sidebar_draw(const te_sidebar *sb, const te_painter *p, const te_ui_colors *c);

/* Exposed for tests: rebuild the visible list and parse a quick-connect target. */
void sidebar_rebuild(te_sidebar *sb);
[[nodiscard]] bool quick_parse(te_sess_type type, const char *target, te_session *out);

#endif
