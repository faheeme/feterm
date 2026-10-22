/*
 * Application entry point: CLI, window, event loop, and the glue between
 * panes (shell sessions), tabs/splits, input and the renderer.
 */
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app/layout.h"
#include "app/pane.h"
#include "config/config.h"
#include "config/theme.h"
#include "config/watch.h"
#include "highlight/highlight.h"
#include "input/click.h"
#include "input/keybind.h"
#include "input/keys.h"
#include "input/mouse.h"
#include "platform/platform.h"
#include "render/font.h"
#include "render/renderer.h"
#include "ui/search.h"
#include "ui/selection.h"
#include "ui/url.h"
#include "util/log.h"
#include "util/mem.h"
#include "util/spawn.h"
#include "util/str.h"
#include "vt/term.h"

#define BLINK_MS           530u
#define BLINK_IDLE_MS      15000u
#define BELL_MS            150u
#define SYNC_TIMEOUT_MS    200u
#define TOAST_MS           1800u
#define RELOAD_DEBOUNCE_MS 250u
#define NOTIFY_INTERVAL_MS 2000u
#define MAX_PANES          60u      /* each pane needs one watched fd */
#define RESIZE_STEP        0.05f
#define DIVIDER_SLOP       4

typedef struct cli_opts {
    const char *const *exec_argv;   /* points into main's argv (NULL-terminated) */
    const char        *config_path;
    const char        *workdir;
    const char        *title;
    const char        *app_id;
    bool               hold;
    bool               verbose;
} cli_opts;

typedef struct app {
    cli_opts      cli;
    te_config     cfg;
    te_theme      theme;
    te_platform  *plat;
    te_font      *font;
    te_renderer  *ren;
    te_layout     layout;
    uint32_t      next_pane_id;
    te_urlfinder *urls;

    float         scale;
    float         font_pt;
    int           width, height;
    bool          running;
    bool          focused;
    bool          suppress_text;
    bool          need_render;
    float         wheel_accum;
    int           exit_status;

    uint64_t      last_input_ms;
    bool          cursor_on;
    uint64_t      last_bell_ms;
    uint64_t      last_notify_ms;

    char          toast[128];
    uint64_t      toast_until_ms;

    /* Mouse */
    te_pane      *mouse_pane;        /* pane that received the press */
    bool          press_valid;       /* primary button down, not dragged yet */
    uint16_t      press_col, press_row;
    uint32_t      press_mods;
    bool          sel_pending;
    bool          link_click;
    uint8_t       buttons_held;
    bool          mouse_reported;
    uint16_t      last_mouse_col, last_mouse_row;
    te_url_hit    hover;
    bool          hover_valid;
    te_pane      *hover_pane;
    te_node      *drag_split;        /* divider being dragged */

    /* Paste confirmation */
    char         *pending_paste;
    uint32_t      paste_pane_id;
    char          modal[160];

    te_watch      watch;
    int           watch_tag;
    uint64_t      reload_due_ms;

    char          tab_title_buf[TE_MAX_TABS][96];
    const char   *tab_titles[TE_MAX_TABS];
} app;

/* ------------------------------------------------------------------------- */
/* CLI */

static void usage(FILE *fp)
{
    fprintf(fp,
            "Usage: %s [options] [-e command [args...]]\n"
            "  -e, --exec CMD...     run CMD instead of the login shell (must be last)\n"
            "  -d, --directory DIR   start in DIR\n"
            "  -c, --config FILE     use FILE instead of ~/.config/%s/config.ini\n"
            "  -T, --title TITLE     initial window title\n"
            "      --class ID        application id / WM_CLASS\n"
            "      --hold            keep panes open after their command exits\n"
            "      --verbose         debug logging\n"
            "  -v, --version         print version\n"
            "  -h, --help            this help\n",
            APP_NAME, APP_NAME);
}

/* Returns 0 to continue, 1 to exit successfully, -1 on error. */
static int parse_cli(int argc, char **argv, cli_opts *o)
{
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        bool has_next = i + 1 < argc;
        if (strcmp(a, "-e") == 0 || strcmp(a, "--exec") == 0) {
            if (!has_next) {
                fprintf(stderr, "%s: %s needs a command\n", APP_NAME, a);
                return -1;
            }
            o->exec_argv = (const char *const *)&argv[i + 1];
            return 0;
        } else if ((strcmp(a, "-d") == 0 || strcmp(a, "--directory") == 0) && has_next) {
            o->workdir = argv[++i];
        } else if ((strcmp(a, "-c") == 0 || strcmp(a, "--config") == 0) && has_next) {
            o->config_path = argv[++i];
        } else if ((strcmp(a, "-T") == 0 || strcmp(a, "--title") == 0) && has_next) {
            o->title = argv[++i];
        } else if (strcmp(a, "--class") == 0 && has_next) {
            o->app_id = argv[++i];
        } else if (strcmp(a, "--hold") == 0) {
            o->hold = true;
        } else if (strcmp(a, "--verbose") == 0) {
            o->verbose = true;
        } else if (strcmp(a, "-v") == 0 || strcmp(a, "--version") == 0) {
            printf("%s %s\n", APP_DISPLAY_NAME, APP_VERSION);
            return 1;
        } else if (strcmp(a, "-h") == 0 || strcmp(a, "--help") == 0) {
            usage(stdout);
            return 1;
        } else {
            fprintf(stderr, "%s: unknown or incomplete option '%s'\n", APP_NAME, a);
            usage(stderr);
            return -1;
        }
    }
    return 0;
}

/* ------------------------------------------------------------------------- */
/* Small helpers */

static te_pane *focus_pane(const app *a)
{
    return layout_focus(&a->layout);
}

static void show_toast(app *a, const char *msg)
{
    if (str_copy(a->toast, sizeof a->toast, msg) == TE_ERR_INVAL) {
        return;
    }
    a->toast_until_ms = plat_ticks_ms() + TOAST_MS;
    a->need_render = true;
}

static void note_input(app *a)
{
    a->last_input_ms = plat_ticks_ms();
    a->cursor_on = true;
    a->need_render = true;
}

static int padding_px(const app *a)
{
    return (int)lroundf((float)a->cfg.padding * a->scale);
}

static int tab_bar_height(const app *a)
{
    bool show = a->cfg.tab_bar == TE_TABBAR_ALWAYS || (a->cfg.tab_bar == TE_TABBAR_AUTO && a->layout.ntabs > 1);
    return show ? (int)lroundf((float)font_metrics(a->font)->cell_h * 1.6f) : 0;
}

static void grid_for_rect(const app *a, int w, int h, uint16_t *cols, uint16_t *rows)
{
    const te_font_metrics *m = font_metrics(a->font);
    int pad = padding_px(a);
    int c = (w - 2 * pad) / (m->cell_w > 0 ? m->cell_w : 1);
    int r = (h - 2 * pad) / (m->cell_h > 0 ? m->cell_h : 1);
    *cols = (uint16_t)TE_CLAMP(c, 2, 2000);
    *rows = (uint16_t)TE_CLAMP(r, 1, 1000);
}

static const char *pane_title(const te_pane *p)
{
    const char *t = term_title(p->term);
    return t != nullptr && t[0] != '\0' ? t : "shell";
}

static void update_window_title(app *a)
{
    te_pane *p = focus_pane(a);
    if (p != nullptr) {
        plat_set_title(a->plat, a->cli.title != nullptr && term_title(p->term)[0] == '\0' ? a->cli.title : pane_title(p));
    }
}

/* Recompute pane rectangles and resize every terminal to its new grid. */
static void relayout(app *a)
{
    int bar = tab_bar_height(a);
    layout_compute(&a->layout, 0, bar, a->width, a->height - bar);
    te_pane *all[MAX_PANES] = { 0 };
    size_t n = layout_all_panes(&a->layout, all, TE_ARRAY_LEN(all));
    int pad = padding_px(a);
    for (size_t i = 0; i < n; i++) {
        te_pane *p = all[i];
        int r[4] = { 0, bar, a->width, a->height - bar };
        if (!layout_pane_rect(&a->layout, p, r)) {
            continue;
        }
        int x = r[0], y = r[1], w = r[2], h = r[3];
        p->x = x;
        p->y = y;
        p->w = w;
        p->h = h;
        uint16_t cols = 0;
        uint16_t rows = 0;
        grid_for_rect(a, w, h, &cols, &rows);
        (void)pane_resize(p, cols, rows, (uint16_t)TE_CLAMP(w - 2 * pad, 0, UINT16_MAX),
                          (uint16_t)TE_CLAMP(h - 2 * pad, 0, UINT16_MAX));
    }
    a->hover_valid = false;
    a->need_render = true;
}

static void apply_size(app *a)
{
    int w = 0;
    int h = 0;
    plat_drawable_size(a->plat, &w, &h);
    if (w <= 0 || h <= 0) {
        return;
    }
    a->width = w;
    a->height = h;
    relayout(a);
}

[[nodiscard]] static te_err load_font(app *a)
{
    float px = a->font_pt * 96.0f / 72.0f * a->scale;
    te_font *nf = nullptr;
    te_err rc = font_create(&nf, a->cfg.font_family, px, a->cfg.line_height);
    if (rc != TE_OK) {
        return rc;
    }
    if (a->ren != nullptr) {
        renderer_set_font(a->ren, nf);
    }
    font_destroy(&a->font);
    a->font = nf;
    return TE_OK;
}

static void zoom(app *a, float delta, bool reset)
{
    float old = a->font_pt;
    a->font_pt = reset ? a->cfg.font_size : TE_CLAMP(a->font_pt + delta, 5.0f, 72.0f);
    if (a->font_pt == old) {
        return;
    }
    if (load_font(a) != TE_OK) {
        a->font_pt = old;
        LOG_W("could not load font at %.1fpt", (double)a->font_pt);
        return;
    }
    apply_size(a);
}

/* ------------------------------------------------------------------------- */
/* Terminal host callbacks (user = the pane) */

static void host_title(void *user, const char *utf8)
{
    te_pane *p = user;
    app *a = p->owner;
    (void)utf8;
    if (p == focus_pane(a)) {
        update_window_title(a);
    }
    a->need_render = true;   /* tab titles */
}

static void host_bell(void *user)
{
    te_pane *p = user;
    app *a = p->owner;
    uint64_t now = plat_ticks_ms();
    if (now - a->last_bell_ms < 100) {
        return;   /* rate-limit bell storms */
    }
    a->last_bell_ms = now;
    if (a->cfg.bell == TE_BELL_NONE) {
        return;
    }
    p->bell_until_ms = now + BELL_MS;
    a->need_render = true;
    if (a->cfg.bell == TE_BELL_URGENT && !a->focused) {
        plat_bell(a->plat);
    }
}

static void host_clipboard(void *user, const char *utf8)
{
    te_pane *p = user;
    app *a = p->owner;
    if (a->cfg.osc52_write) {
        plat_clipboard_set(a->plat, utf8);
    }
}

static void host_cell_size(void *user, uint16_t *w, uint16_t *h)
{
    te_pane *p = user;
    app *a = p->owner;
    const te_font_metrics *m = font_metrics(a->font);
    *w = (uint16_t)TE_CLAMP(m->cell_w, 0, UINT16_MAX);
    *h = (uint16_t)TE_CLAMP(m->cell_h, 0, UINT16_MAX);
}

static void host_notify(void *user, const char *title, const char *body)
{
    te_pane *p = user;
    app *a = p->owner;
    if (a->cfg.notifications == TE_NOTIFY_NEVER) {
        return;
    }
    if (a->cfg.notifications == TE_NOTIFY_UNFOCUSED && a->focused && p == focus_pane(a)) {
        return;   /* you are looking at it already */
    }
    uint64_t now = plat_ticks_ms();
    if (a->last_notify_ms != 0 && now - a->last_notify_ms < NOTIFY_INTERVAL_MS) {
        return;   /* rate limit: a program cannot flood the desktop */
    }
    a->last_notify_ms = now;
    const char *t = title != nullptr && title[0] != '\0' ? title : pane_title(p);
    char app_arg[64] = { 0 };
    if (str_fmt(app_arg, sizeof app_arg, "--app-name=%s", APP_NAME) != TE_OK) {
        return;
    }
    /* "--" ends option parsing: text starting with '-' cannot become an option. */
    const char *argv[] = { "notify-send", app_arg, "--", t, body != nullptr ? body : "", nullptr };
    if (spawn_detached(argv) != TE_OK) {
        LOG_D("notify-send unavailable");
    }
    if (!a->focused) {
        plat_bell(a->plat);
    }
}

/* ------------------------------------------------------------------------- */
/* Panes */

static te_pane *pane_by_tag(const app *a, int tag)
{
    te_pane *all[MAX_PANES] = { 0 };
    size_t n = layout_all_panes(&a->layout, all, TE_ARRAY_LEN(all));
    for (size_t i = 0; i < n; i++) {
        if (all[i]->tag == tag) {
            return all[i];
        }
    }
    return nullptr;
}

static te_pane *pane_by_id(const app *a, uint32_t id)
{
    te_pane *all[MAX_PANES] = { 0 };
    size_t n = layout_all_panes(&a->layout, all, TE_ARRAY_LEN(all));
    for (size_t i = 0; i < n; i++) {
        if (all[i]->id == id) {
            return all[i];
        }
    }
    return nullptr;
}

/* Start a shell (or the -e command for the very first pane) in cwd. */
static te_pane *spawn_pane(app *a, const char *cwd, const char *const *argv)
{
    if (layout_count_panes(&a->layout) >= MAX_PANES) {
        show_toast(a, "Too many panes");
        return nullptr;
    }
    te_pane *f = focus_pane(a);
    int w = f != nullptr ? f->w : a->width;
    int h = f != nullptr ? f->h : a->height;
    uint16_t cols = 0;
    uint16_t rows = 0;
    grid_for_rect(a, w, h, &cols, &rows);
    te_pane_spawn sp = {
        .id = ++a->next_pane_id,
        .owner = a,
        .cfg = &a->cfg,
        .theme = &a->theme,
        .host = {
            .set_title = host_title,
            .bell = host_bell,
            .clipboard_set = host_clipboard,
            .cell_size = host_cell_size,
            .notify = host_notify,
        },
        .argv = argv,
        .cwd = cwd,
        .cols = cols,
        .rows = rows,
        .px_w = (uint16_t)TE_CLAMP(w, 0, UINT16_MAX),
        .px_h = (uint16_t)TE_CLAMP(h, 0, UINT16_MAX),
    };
    te_pane *p = nullptr;
    te_err rc = pane_create(&p, &sp);
    if (rc != TE_OK) {
        LOG_E("cannot start a shell: %s", te_strerror(rc));
        show_toast(a, "Could not start a shell");
        return nullptr;
    }
    if (plat_watch_fd(a->plat, p->pty.master_fd, &p->tag) != TE_OK) {
        LOG_E("cannot watch the pty");
        pane_destroy(&p);
        return nullptr;
    }
    return p;
}

static void inherit_cwd(app *a, char *out, size_t outsz, const char **cwd)
{
    *cwd = nullptr;
    te_pane *f = focus_pane(a);
    if (f != nullptr && pane_cwd(f, out, outsz) == TE_OK) {
        *cwd = out;
    }
}

static void close_pane(app *a, te_pane *p)
{
    if (p == nullptr) {
        return;
    }
    if (p->exited) {
        a->exit_status = p->status;
    }
    if (a->mouse_pane == p) a->mouse_pane = nullptr;
    if (a->hover_pane == p) { a->hover_pane = nullptr; a->hover_valid = false; }
    a->drag_split = nullptr;
    if (a->pending_paste != nullptr && a->paste_pane_id == p->id) {
        TE_FREE(a->pending_paste);
        a->modal[0] = '\0';
    }
    plat_unwatch_fd(a->plat, p->tag);
    bool empty = layout_remove(&a->layout, p);
    pane_destroy(&p);
    if (empty) {
        a->running = false;
        return;
    }
    relayout(a);
    update_window_title(a);
}

static void new_tab(app *a)
{
    char buf[4096] = { 0 };
    const char *cwd = nullptr;
    inherit_cwd(a, buf, sizeof buf, &cwd);
    te_pane *p = spawn_pane(a, cwd, nullptr);
    if (p == nullptr) {
        return;
    }
    if (layout_add_tab(&a->layout, p) != TE_OK) {
        show_toast(a, "Too many tabs");
        plat_unwatch_fd(a->plat, p->tag);
        pane_destroy(&p);
        return;
    }
    relayout(a);
    update_window_title(a);
}

static void split(app *a, bool side_by_side)
{
    char buf[4096] = { 0 };
    const char *cwd = nullptr;
    inherit_cwd(a, buf, sizeof buf, &cwd);
    te_pane *p = spawn_pane(a, cwd, nullptr);
    if (p == nullptr) {
        return;
    }
    if (layout_split(&a->layout, p, side_by_side) != TE_OK) {
        show_toast(a, "Too many panes in this tab");
        plat_unwatch_fd(a->plat, p->tag);
        pane_destroy(&p);
        return;
    }
    relayout(a);
    update_window_title(a);
}

/* ------------------------------------------------------------------------- */
/* Clipboard and paste */

static void paste_now(app *a, te_pane *p, const char *text)
{
    size_t len = 0;
    char *buf = keys_prepare_paste(text, term_modes(p->term)->bracketed_paste, a->cfg.paste_sanitize, &len);
    if (buf != nullptr) {
        term_view_reset(p->term);
        pane_queue(p, buf, len);
        TE_FREE(buf);
        note_input(a);
    }
}

/* Takes ownership of text. Multi-line pastes into a program without bracketed paste ask first. */
static void paste_text(app *a, char *text)
{
    te_pane *p = focus_pane(a);
    if (text == nullptr || p == nullptr) {
        TE_FREE(text);
        return;
    }
    size_t n = strlen(text);
    size_t newlines = 0;
    for (size_t i = 0; i < n; i++) {
        newlines += text[i] == '\n';
    }
    if (n > 0 && text[n - 1] == '\n' && newlines > 0) {
        newlines--;   /* a single trailing newline is the common copy artefact */
    }
    if (a->cfg.confirm_paste && !term_modes(p->term)->bracketed_paste && newlines > 0 && !p->exited) {
        TE_FREE(a->pending_paste);
        a->pending_paste = text;
        a->paste_pane_id = p->id;
        (void)str_fmt(a->modal, sizeof a->modal,
                      "Paste %zu lines? Each line runs as it arrives.   Enter: paste    Esc: cancel", newlines + 1);
        a->need_render = true;
        return;
    }
    paste_now(a, p, text);
    TE_FREE(text);
}

static void modal_answer(app *a, bool yes)
{
    te_pane *p = pane_by_id(a, a->paste_pane_id);
    if (yes && a->pending_paste != nullptr && p != nullptr) {
        paste_now(a, p, a->pending_paste);
    }
    TE_FREE(a->pending_paste);
    a->modal[0] = '\0';
    a->need_render = true;
}

static void copy_selection(app *a, te_pane *p, bool clipboard)
{
    if (p == nullptr) {
        return;
    }
    size_t len = 0;
    char *text = sel_text(&p->sel, p->term, &len);
    if (text == nullptr) {
        if (clipboard) {
            show_toast(a, p->sel.active ? "Selection too large to copy" : "Nothing selected");
        }
        return;
    }
    if (clipboard) {
        plat_clipboard_set(a->plat, text);
        char msg[64] = { 0 };
        (void)str_fmt(msg, sizeof msg, "Copied %zu bytes", len);
        show_toast(a, msg);
    } else {
        plat_primary_set(a->plat, text);
    }
    TE_FREE(text);
}

/* ------------------------------------------------------------------------- */
/* Search */

static void search_follow(app *a, te_pane *p)
{
    te_search_match m = { 0 };
    if (search_current(p->search, &m)) {
        term_scroll_to_abs(p->term, m.row, false);
    }
    a->need_render = true;
}

static void search_toggle(app *a, te_pane *p)
{
    if (p == nullptr || p->search == nullptr) {
        show_toast(a, "Search unavailable");
        return;
    }
    if (!search_is_open(p->search)) {
        search_open(p->search);
        search_run(p->search, p->term);
    } else if (search_step(p->search, p->term, -1)) {
        search_follow(a, p);
    }
    a->need_render = true;
}

/* Keys while the search bar is open. Returns true if the key produces no text event. */
static bool search_key(app *a, te_pane *p, const te_event *ev)
{
    uint32_t mods = ev->key.mods;
    switch (ev->key.key) {
    case TE_KEY_ESCAPE:
        search_close(p->search);
        term_view_reset(p->term);
        a->need_render = true;
        return true;
    case TE_KEY_ENTER:
        if (search_step(p->search, p->term, (mods & TE_MOD_SHIFT) != 0 ? 1 : -1)) {
            search_follow(a, p);
        }
        return true;
    case TE_KEY_UP:
        if (search_step(p->search, p->term, -1)) {
            search_follow(a, p);
        }
        return true;
    case TE_KEY_DOWN:
        if (search_step(p->search, p->term, 1)) {
            search_follow(a, p);
        }
        return true;
    case TE_KEY_BACKSPACE:
        search_backspace(p->search, p->term);
        search_follow(a, p);
        return true;
    case TE_KEY_CHAR:
        if (keymap_lookup(&a->cfg.keys, ev->key.key, ev->key.cp, mods) == TE_ACT_SEARCH) {
            search_toggle(a, p);
            return true;
        }
        return (mods & (TE_MOD_CTRL | TE_MOD_ALT | TE_MOD_SUPER)) != 0;   /* plain keys arrive as text */
    default:
        return true;
    }
}

/* ------------------------------------------------------------------------- */
/* Configuration and themes */

static void apply_theme_to_panes(app *a)
{
    te_pane *all[MAX_PANES] = { 0 };
    size_t n = layout_all_panes(&a->layout, all, TE_ARRAY_LEN(all));
    for (size_t i = 0; i < n; i++) {
        term_set_theme(all[i]->term, &a->theme);
    }
    a->need_render = true;
}

static void reload_config(app *a)
{
    te_config nc;
    config_defaults(&nc);
    if (config_load(&nc, a->cli.config_path) != TE_OK) {
        show_toast(a, "Config has errors: kept the previous settings");
        return;
    }
    te_theme th;
    if (theme_load(&th, nc.theme) != TE_OK) {
        theme_defaults(&th);
    }
    te_config old = a->cfg;
    bool font_changed = strcmp(old.font_family, nc.font_family) != 0 || old.font_size != nc.font_size ||
                        old.line_height != nc.line_height;
    a->cfg = nc;
    a->theme = th;
    apply_theme_to_panes(a);
    if (font_changed) {
        float old_pt = a->font_pt;
        if (old.font_size != nc.font_size) {
            a->font_pt = nc.font_size;
        }
        if (load_font(a) != TE_OK) {
            LOG_W("font '%s' could not be loaded; keeping the previous font", nc.font_family);
            (void)str_copy(a->cfg.font_family, sizeof a->cfg.font_family, old.font_family);
            a->cfg.line_height = old.line_height;
            a->font_pt = old_pt;
        }
    }
    te_pane *all[MAX_PANES] = { 0 };
    size_t n = layout_all_panes(&a->layout, all, TE_ARRAY_LEN(all));
    for (size_t i = 0; i < n; i++) {
        pane_setup_highlight(all[i], &a->cfg);
    }
    apply_size(a);
    show_toast(a, "Configuration reloaded");
}

static void theme_next(app *a)
{
    static char names[TE_THEME_LIST_MAX][128];
    size_t n = theme_list(names, TE_THEME_LIST_MAX);
    if (n == 0) {
        show_toast(a, "No themes installed");
        return;
    }
    size_t next = 0;
    for (size_t i = 0; i < n; i++) {
        if (strcmp(names[i], a->theme.name) == 0) {
            next = (i + 1) % n;
            break;
        }
    }
    te_theme th;
    if (theme_load(&th, names[next]) != TE_OK) {
        show_toast(a, "Theme could not be loaded");
        return;
    }
    a->theme = th;
    (void)str_copy(a->cfg.theme, sizeof a->cfg.theme, names[next]);
    apply_theme_to_panes(a);
    char msg[160] = { 0 };
    (void)str_fmt(msg, sizeof msg, "Theme: %s", names[next]);
    show_toast(a, msg);
}

/* ------------------------------------------------------------------------- */
/* Actions (keyboard shortcuts) */

static void jump_prompt(app *a, te_pane *p, int dir)
{
    if (!term_shell_integration(p->term)) {
        show_toast(a, "Prompt jumping needs shell integration (OSC 133)");
        return;
    }
    uint64_t found = 0;
    uint64_t from = term_view_abs(p->term, 0);
    if (!term_find_prompt(p->term, from, dir, &found)) {
        if (dir > 0) {
            term_view_reset(p->term);
        }
        return;
    }
    if (dir > 0 && found >= term_abs_end(p->term) - term_rows(p->term)) {
        term_view_reset(p->term);
    } else {
        term_scroll_to_abs(p->term, found, true);
    }
}

static void do_action(app *a, te_action act)
{
    te_pane *p = focus_pane(a);
    if (p == nullptr) {
        return;
    }
    te_term *t = p->term;
    uint16_t page = term_rows(t) > 1 ? (uint16_t)(term_rows(t) - 1) : 1;
    switch (act) {
    case TE_ACT_COPY:             copy_selection(a, p, true); break;
    case TE_ACT_PASTE:            paste_text(a, plat_clipboard_get(a->plat)); break;
    case TE_ACT_PASTE_PRIMARY:    paste_text(a, plat_primary_get(a->plat)); break;
    case TE_ACT_ZOOM_IN:          zoom(a, 1.0f, false); break;
    case TE_ACT_ZOOM_OUT:         zoom(a, -1.0f, false); break;
    case TE_ACT_ZOOM_RESET:       zoom(a, 0.0f, true); break;
    case TE_ACT_SCROLL_PAGE_UP:   term_scroll_view(t, page); break;
    case TE_ACT_SCROLL_PAGE_DOWN: term_scroll_view(t, -(int)page); break;
    case TE_ACT_SCROLL_LINE_UP:   term_scroll_view(t, 1); break;
    case TE_ACT_SCROLL_LINE_DOWN: term_scroll_view(t, -1); break;
    case TE_ACT_SCROLL_TOP:
        term_scroll_view(t, (int)TE_MIN(term_scrollback_count(t), (uint32_t)INT32_MAX));
        break;
    case TE_ACT_SCROLL_BOTTOM:    term_view_reset(t); break;
    case TE_ACT_SEARCH:           search_toggle(a, p); break;
    case TE_ACT_HIGHLIGHT_CYCLE:
        show_toast(a, p->hl != nullptr ? hl_cycle(p->hl) : "Highlighting unavailable");
        break;
    case TE_ACT_RELOAD_CONFIG:    reload_config(a); break;
    case TE_ACT_PREV_PROMPT:      jump_prompt(a, p, -1); break;
    case TE_ACT_NEXT_PROMPT:      jump_prompt(a, p, 1); break;
    case TE_ACT_NEW_TAB:          new_tab(a); break;
    case TE_ACT_CLOSE_PANE:       close_pane(a, p); break;
    case TE_ACT_NEXT_TAB:         layout_cycle_tab(&a->layout, 1); update_window_title(a); break;
    case TE_ACT_PREV_TAB:         layout_cycle_tab(&a->layout, -1); update_window_title(a); break;
    case TE_ACT_GOTO_TAB_1: case TE_ACT_GOTO_TAB_2: case TE_ACT_GOTO_TAB_3:
    case TE_ACT_GOTO_TAB_4: case TE_ACT_GOTO_TAB_5: case TE_ACT_GOTO_TAB_6:
    case TE_ACT_GOTO_TAB_7: case TE_ACT_GOTO_TAB_8: case TE_ACT_GOTO_TAB_9:
        layout_select_tab(&a->layout, (size_t)(act - TE_ACT_GOTO_TAB_1));
        update_window_title(a);
        break;
    case TE_ACT_SPLIT_RIGHT:      split(a, true); break;
    case TE_ACT_SPLIT_DOWN:       split(a, false); break;
    case TE_ACT_FOCUS_NEXT_PANE:  layout_cycle_focus(&a->layout, 1); update_window_title(a); break;
    case TE_ACT_FOCUS_PREV_PANE:  layout_cycle_focus(&a->layout, -1); update_window_title(a); break;
    case TE_ACT_RESIZE_LEFT:      if (layout_resize(&a->layout, -1, 0, RESIZE_STEP)) relayout(a); break;
    case TE_ACT_RESIZE_RIGHT:     if (layout_resize(&a->layout, 1, 0, RESIZE_STEP)) relayout(a); break;
    case TE_ACT_RESIZE_UP:        if (layout_resize(&a->layout, 0, -1, RESIZE_STEP)) relayout(a); break;
    case TE_ACT_RESIZE_DOWN:      if (layout_resize(&a->layout, 0, 1, RESIZE_STEP)) relayout(a); break;
    case TE_ACT_TOGGLE_ZOOM:      layout_toggle_zoom(&a->layout); relayout(a); break;
    case TE_ACT_THEME_NEXT:       theme_next(a); break;
    case TE_ACT_NONE:
    case TE_ACT_COUNT:
    default:
        break;
    }
    a->need_render = true;
}

/* ------------------------------------------------------------------------- */
/* Keyboard */

static void on_key(app *a, const te_event *ev)
{
    te_pane *p = focus_pane(a);
    if (p == nullptr) {
        return;
    }
    te_keyev kev = !ev->key.down ? TE_KEYEV_RELEASE : (ev->key.repeat ? TE_KEYEV_REPEAT : TE_KEYEV_PRESS);
    if (kev == TE_KEYEV_RELEASE) {
        /* Releases only matter to programs that asked for them (kitty flag 2). */
        if ((term_modes(p->term)->kitty_flags & 2u) != 0 && a->pending_paste == nullptr &&
            !search_is_open(p->search)) {
            char buf[64] = { 0 };
            size_t n = keys_encode_ev(ev->key.key, ev->key.cp, ev->key.mods, kev, term_modes(p->term), buf, sizeof buf);
            if (n > 0) {
                pane_queue(p, buf, n);
            }
        }
        return;
    }
    a->suppress_text = false;
    if (a->pending_paste != nullptr) {
        modal_answer(a, ev->key.key == TE_KEY_ENTER);
        a->suppress_text = true;
        return;
    }
    if (search_is_open(p->search)) {
        a->suppress_text = search_key(a, p, ev);
        return;
    }
    uint32_t mods = ev->key.mods & (TE_MOD_SHIFT | TE_MOD_CTRL | TE_MOD_ALT | TE_MOD_SUPER);
    te_action act = keymap_lookup(&a->cfg.keys, ev->key.key, ev->key.cp, mods);
    if (act != TE_ACT_NONE) {
        do_action(a, act);
        a->suppress_text = true;
        return;
    }
    char buf[64] = { 0 };
    size_t n = keys_encode_ev(ev->key.key, ev->key.cp, ev->key.mods, kev, term_modes(p->term), buf, sizeof buf);
    if (n > 0) {
        term_view_reset(p->term);
        pane_queue(p, buf, n);
        a->suppress_text = true;
        note_input(a);
    }
}

static void on_text(app *a, const te_event *ev)
{
    te_pane *p = focus_pane(a);
    if (a->suppress_text) {
        a->suppress_text = false;
        return;
    }
    if (a->pending_paste != nullptr || p == nullptr) {
        return;
    }
    size_t n = strnlen(ev->text.text, sizeof ev->text.text);
    if (n == 0) {
        return;
    }
    if (search_is_open(p->search)) {
        search_append(p->search, p->term, ev->text.text);
        search_follow(a, p);
        return;
    }
    term_view_reset(p->term);
    pane_queue(p, ev->text.text, n);
    note_input(a);
}

/* ------------------------------------------------------------------------- */
/* Mouse */

static bool pixel_to_cell(const app *a, const te_pane *p, float px, float py, uint16_t *col, uint16_t *row)
{
    const te_font_metrics *m = font_metrics(a->font);
    float pad = (float)padding_px(a);
    float fx = (px - (float)p->x - pad) / (float)m->cell_w;
    float fy = (py - (float)p->y - pad) / (float)m->cell_h;
    if (!(fx >= 0.0f && fy >= 0.0f)) {
        return false;
    }
    if (fx >= (float)term_cols(p->term) || fy >= (float)term_rows(p->term)) {
        return false;
    }
    *col = (uint16_t)fx;
    *row = (uint16_t)fy;
    return true;
}

/* Like pixel_to_cell, but positions outside the pane snap to its edge. */
static void pixel_to_cell_clamped(const app *a, const te_pane *p, float px, float py, uint16_t *col, uint16_t *row)
{
    const te_font_metrics *m = font_metrics(a->font);
    float pad = (float)padding_px(a);
    float fx = (px - (float)p->x - pad) / (float)m->cell_w;
    float fy = (py - (float)p->y - pad) / (float)m->cell_h;
    *col = (uint16_t)TE_CLAMP(fx, 0.0f, (float)term_cols(p->term) - 1.0f);
    *row = (uint16_t)TE_CLAMP(fy, 0.0f, (float)term_rows(p->term) - 1.0f);
}

static void click_position(app *a, te_pane *p, uint16_t col, uint16_t row)
{
    const te_modes *m = term_modes(p->term);
    if (!a->cfg.click_to_position || p->exited || m->alt_screen || m->mouse != TE_MOUSE_NONE ||
        term_view_offset(p->term) != 0) {
        return;
    }
    uint16_t top = 0;
    uint16_t bottom = 0;
    click_input_block(p->term, &top, &bottom);
    int prompt_end = -1;
    const te_line *l = term_grid_line(p->term, top);
    if (p->hl != nullptr && l != nullptr && l->cells != nullptr && l->input_col == 0) {
        uint16_t n = l->cols < term_cols(p->term) ? l->cols : term_cols(p->term);
        prompt_end = hl_prompt_end(p->hl, l->cells, n);
    }
    te_click_opts co = { .emacs_keys = a->cfg.click_emacs_keys, .prompt_end = prompt_end };
    char buf[16384];
    size_t n = click_to_position(p->term, col, row, &co, buf, sizeof buf);
    if (n > 0) {
        pane_queue(p, buf, n);
        note_input(a);
    }
}

/* Mouse events go to the program when it asked for them, unless Shift is held. */
static bool reporting(const te_pane *p, uint32_t mods)
{
    return p != nullptr && term_modes(p->term)->mouse != TE_MOUSE_NONE && (mods & TE_MOD_SHIFT) == 0 && !p->exited;
}

static void report_mouse(app *a, te_pane *p, te_mouse_action act, int button, uint32_t mods, float px, float py)
{
    const te_modes *m = term_modes(p->term);
    if (!mouse_wants(m, act, a->buttons_held != 0)) {
        return;
    }
    uint16_t col = 0;
    uint16_t row = 0;
    pixel_to_cell_clamped(a, p, px, py, &col, &row);
    if (act == TE_MOUSE_MOVE && m->mouse_enc != TE_MOUSE_ENC_SGR_PIXELS && a->mouse_reported &&
        col == a->last_mouse_col && row == a->last_mouse_row) {
        return;   /* only report motion into a new cell */
    }
    float pad = (float)padding_px(a);
    uint32_t gx = (uint32_t)TE_CLAMP(px - (float)p->x - pad, 0.0f, 65535.0f);
    uint32_t gy = (uint32_t)TE_CLAMP(py - (float)p->y - pad, 0.0f, 65535.0f);
    char buf[64] = { 0 };
    size_t n = mouse_encode(m, act, button, mods, col, row, gx, gy, buf, sizeof buf);
    if (n > 0) {
        pane_queue(p, buf, n);
    }
    a->last_mouse_col = col;
    a->last_mouse_row = row;
    a->mouse_reported = true;
}

static te_selpos sel_pos(const te_pane *p, uint16_t col, uint16_t row)
{
    return (te_selpos){ .row = term_view_abs(p->term, row), .col = col };
}

static void update_hover(app *a, te_pane *p, float px, float py, uint32_t mods)
{
    uint16_t col = 0;
    uint16_t row = 0;
    bool want = p != nullptr && (mods & TE_MOD_CTRL) != 0 && a->urls != nullptr &&
                pixel_to_cell(a, p, px, py, &col, &row);
    bool had = a->hover_valid;
    a->hover_valid = want && url_at(a->urls, p->term, term_view_abs(p->term, row), col, &a->hover);
    a->hover_pane = a->hover_valid ? p : nullptr;
    te_pointer ptr = a->hover_valid ? TE_POINTER_HAND : TE_POINTER_TEXT;
    if (layout_divider_at(&a->layout, (int)px, (int)py, DIVIDER_SLOP) != nullptr || py < (float)tab_bar_height(a)) {
        ptr = TE_POINTER_DEFAULT;
    }
    plat_set_pointer(a->plat, ptr);
    if (had || a->hover_valid) {
        a->need_render = true;
    }
}

static void open_link_at(app *a, te_pane *p, uint16_t col, uint16_t row)
{
    te_url_hit hit;
    if (a->urls == nullptr || !url_at(a->urls, p->term, term_view_abs(p->term, row), col, &hit)) {
        return;
    }
    char msg[96] = { 0 };
    if (url_open(hit.uri) == TE_OK) {
        (void)str_fmt(msg, sizeof msg, "Opening %.70s%s", hit.uri, strlen(hit.uri) > 70 ? "..." : "");
    } else {
        (void)str_copy(msg, sizeof msg, "Link not opened (unsupported scheme)");
    }
    show_toast(a, msg);
}

static int map_button(uint8_t sdl_button)
{
    switch (sdl_button) {
    case 1: return TE_MB_LEFT;
    case 2: return TE_MB_MIDDLE;
    case 3: return TE_MB_RIGHT;
    default: return -1;
    }
}

static te_chrome make_chrome(app *a)
{
    for (size_t i = 0; i < a->layout.ntabs; i++) {
        const te_tab *t = &a->layout.tabs[i];
        const char *title = t->focus != nullptr ? pane_title(t->focus) : "";
        if (t->npanes > 1) {
            (void)str_fmt(a->tab_title_buf[i], sizeof a->tab_title_buf[i], "%s [%zu]", title, t->npanes);
        } else {
            (void)str_copy(a->tab_title_buf[i], sizeof a->tab_title_buf[i], title);
        }
        a->tab_titles[i] = a->tab_title_buf[i];
    }
    return (te_chrome){
        .win_w = a->width,
        .win_h = a->height,
        .tab_titles = a->tab_titles,
        .ntabs = a->layout.ntabs,
        .active_tab = a->layout.active,
        .tab_bar_h = tab_bar_height(a),
        .bar_bg = a->theme.ansi[0],
        .bar_fg = a->theme.fg,
        .active_bg = a->theme.bg,
        .active_fg = a->theme.fg,
        .accent = a->theme.cursor,
        .divider = a->theme.selection_bg,
        .toast_fg = a->theme.selection_fg,
        .toast_bg = a->theme.selection_bg,
        .dim_bg = a->theme.bg,
        .scale = a->scale,
    };
}

static void on_button(app *a, const te_event *ev)
{
    int b = map_button(ev->button.button);
    uint32_t mods = ev->button.mods;
    if (b < 0) {
        return;
    }
    uint8_t bit = (uint8_t)(1u << (unsigned)b);
    int ix = (int)ev->button.x;
    int iy = (int)ev->button.y;

    if (ev->button.down) {
        /* Tab bar: select a tab (middle click closes it). */
        if (iy < tab_bar_height(a)) {
            te_chrome c = make_chrome(a);
            int ti = renderer_tab_at(a->ren, &c, ix, iy);
            if (ti >= 0) {
                layout_select_tab(&a->layout, (size_t)ti);
                if (b == TE_MB_MIDDLE) {
                    te_pane *list[TE_MAX_PANES_PER_TAB] = { 0 };
                    size_t n = layout_tab_panes(&a->layout.tabs[ti], list, TE_ARRAY_LEN(list));
                    for (size_t i = 0; i < n && a->running; i++) {
                        close_pane(a, list[i]);
                    }
                }
                update_window_title(a);
                a->need_render = true;
            }
            return;
        }
        /* Divider: start a drag. */
        if (b == TE_MB_LEFT) {
            te_node *d = layout_divider_at(&a->layout, ix, iy, DIVIDER_SLOP);
            if (d != nullptr) {
                a->drag_split = d;
                return;
            }
        }
        te_pane *hit = layout_pane_at(&a->layout, ix, iy);
        if (hit != nullptr && hit != focus_pane(a)) {
            layout_set_focus(&a->layout, hit);
            update_window_title(a);
            a->need_render = true;
        }
        a->mouse_pane = hit;
    }
    if (!ev->button.down && a->drag_split != nullptr && b == TE_MB_LEFT) {
        a->drag_split = nullptr;
        relayout(a);
        return;
    }
    te_pane *p = a->mouse_pane;
    if (p == nullptr) {
        return;
    }
    if (reporting(p, mods)) {
        if (ev->button.down) {
            a->buttons_held |= bit;
            report_mouse(a, p, TE_MOUSE_PRESS, b, mods, ev->button.x, ev->button.y);
        } else {
            a->buttons_held &= (uint8_t)~bit;
            report_mouse(a, p, TE_MOUSE_RELEASE, b, mods, ev->button.x, ev->button.y);
        }
        return;
    }
    if (!ev->button.down) {
        a->buttons_held &= (uint8_t)~bit;
    } else {
        a->buttons_held |= bit;
    }
    uint16_t col = 0;
    uint16_t row = 0;
    bool inside = pixel_to_cell(a, p, ev->button.x, ev->button.y, &col, &row);

    if (b == TE_MB_MIDDLE) {
        if (ev->button.down && inside) {
            paste_text(a, plat_primary_get(a->plat));
        }
        return;
    }
    if (b != TE_MB_LEFT) {
        return;
    }
    if (ev->button.down) {
        if (!inside) {
            return;
        }
        a->press_col = col;
        a->press_row = row;
        a->press_mods = mods;
        a->link_click = (mods & TE_MOD_CTRL) != 0;
        a->press_valid = ev->button.clicks == 1 && mods == 0;
        a->sel_pending = false;
        if (a->link_click) {
            return;
        }
        if (ev->button.clicks >= 3) {
            sel_start(&p->sel, p->term, sel_pos(p, col, row), TE_SEL_LINE, a->cfg.word_chars);
        } else if (ev->button.clicks == 2) {
            sel_start(&p->sel, p->term, sel_pos(p, col, row), TE_SEL_WORD, a->cfg.word_chars);
        } else {
            sel_clear(&p->sel);
            a->sel_pending = true;   /* becomes a selection once the pointer moves */
        }
        a->need_render = true;
        return;
    }
    /* Release */
    if (a->link_click) {
        a->link_click = false;
        if (inside && col == a->press_col && row == a->press_row) {
            open_link_at(a, p, col, row);
        }
        return;
    }
    if (p->sel.dragging) {
        sel_finish(&p->sel);
        if (p->sel.active && a->cfg.copy_on_select) {
            copy_selection(a, p, false);
        }
    } else if (a->press_valid && inside && col == a->press_col && row == a->press_row) {
        click_position(a, p, col, row);
    }
    a->sel_pending = false;
    a->press_valid = false;
    a->need_render = true;
}

static void on_motion(app *a, const te_event *ev)
{
    uint32_t mods = ev->motion.mods;
    if (a->drag_split != nullptr) {
        layout_drag(a->drag_split, (int)ev->motion.x, (int)ev->motion.y);
        relayout(a);
        return;
    }
    te_pane *under = layout_pane_at(&a->layout, (int)ev->motion.x, (int)ev->motion.y);
    update_hover(a, under, ev->motion.x, ev->motion.y, mods);
    bool left_down = (ev->motion.buttons & 1u) != 0;
    te_pane *p = left_down || a->buttons_held != 0 ? a->mouse_pane : under;
    if (p == nullptr) {
        return;
    }
    if (reporting(p, mods)) {
        int b = TE_MB_NONE;
        for (int i = 0; i < 3; i++) {
            if ((a->buttons_held & (1u << (unsigned)i)) != 0) {
                b = i;
                break;
            }
        }
        report_mouse(a, p, TE_MOUSE_MOVE, b, mods, ev->motion.x, ev->motion.y);
        return;
    }
    if (!left_down || p != a->mouse_pane) {
        return;
    }
    uint16_t col = 0;
    uint16_t row = 0;
    pixel_to_cell_clamped(a, p, ev->motion.x, ev->motion.y, &col, &row);
    if (col != a->press_col || row != a->press_row) {
        a->press_valid = false;
    }
    /* Dragging past the top or bottom edge of the pane scrolls its history. */
    if (ev->motion.y < (float)p->y) {
        term_scroll_view(p->term, 1);
    } else if (ev->motion.y > (float)(p->y + p->h)) {
        term_scroll_view(p->term, -1);
    }
    if (a->sel_pending && (col != a->press_col || row != a->press_row)) {
        te_sel_mode mode = (a->press_mods & TE_MOD_ALT) != 0 ? TE_SEL_BLOCK : TE_SEL_STREAM;
        sel_start(&p->sel, p->term, sel_pos(p, a->press_col, a->press_row), mode, a->cfg.word_chars);
        a->sel_pending = false;
    }
    if (p->sel.dragging) {
        sel_update(&p->sel, p->term, sel_pos(p, col, row), a->cfg.word_chars);
        a->need_render = true;
    }
}

static void on_wheel(app *a, const te_event *ev)
{
    te_pane *p = layout_pane_at(&a->layout, (int)ev->wheel.x, (int)ev->wheel.y);
    if (p == nullptr) {
        return;
    }
    a->wheel_accum += ev->wheel.dy * 3.0f;
    int lines = (int)a->wheel_accum;
    if (lines == 0) {
        return;
    }
    a->wheel_accum -= (float)lines;
    const te_modes *m = term_modes(p->term);
    int count = lines > 0 ? lines : -lines;
    if (reporting(p, ev->wheel.mods)) {
        for (int i = 0; i < count && i < 10; i++) {
            report_mouse(a, p, TE_MOUSE_PRESS, lines > 0 ? TE_MB_WHEEL_UP : TE_MB_WHEEL_DOWN, ev->wheel.mods,
                         ev->wheel.x, ev->wheel.y);
        }
        return;
    }
    if (m->alt_screen) {
        /* Alternate scroll: turn the wheel into arrow keys for less, man, vim... */
        char buf[16] = { 0 };
        size_t n = keys_encode(lines > 0 ? TE_KEY_UP : TE_KEY_DOWN, 0, 0, m, buf, sizeof buf);
        for (int i = 0; i < count && i < 50; i++) {
            pane_queue(p, buf, n);
        }
        return;
    }
    term_scroll_view(p->term, lines);
    a->need_render = true;
}

static void on_focus(app *a, bool focused)
{
    a->focused = focused;
    te_pane *p = focus_pane(a);
    if (p != nullptr && term_modes(p->term)->focus_events && !p->exited) {
        pane_queue(p, focused ? "\x1b[I" : "\x1b[O", 3);
    }
    if (!focused) {
        a->buttons_held = 0;
        a->hover_valid = false;
        a->drag_split = nullptr;
    }
    note_input(a);
}

static void handle_event(app *a, const te_event *ev)
{
    switch (ev->type) {
    case TE_EV_QUIT:         a->running = false; break;
    case TE_EV_RESIZE:       apply_size(a); break;
    case TE_EV_SCALE: {
        float s = plat_content_scale(a->plat);
        if (s != a->scale) {
            float old = a->scale;
            a->scale = s;
            if (load_font(a) != TE_OK) {
                a->scale = old;
            }
            apply_size(a);
        }
        break;
    }
    case TE_EV_EXPOSE:       a->need_render = true; break;
    case TE_EV_FOCUS:        on_focus(a, ev->focus.focused); break;
    case TE_EV_KEY:          on_key(a, ev); break;
    case TE_EV_TEXT:         on_text(a, ev); break;
    case TE_EV_MOUSE_WHEEL:  on_wheel(a, ev); break;
    case TE_EV_MOUSE_BUTTON: on_button(a, ev); break;
    case TE_EV_MOUSE_MOTION: on_motion(a, ev); break;
    case TE_EV_FD_READY:
        if (ev->fd.tag == a->watch_tag && a->watch_tag >= 0) {
            if (watch_drain(&a->watch)) {
                a->reload_due_ms = plat_ticks_ms() + RELOAD_DEBOUNCE_MS;
            }
            plat_fd_rearm(a->plat, a->watch_tag);
        } else {
            te_pane *p = pane_by_tag(a, ev->fd.tag);
            if (p != nullptr) {
                p->fd_ready = true;
            }
        }
        break;
    case TE_EV_NONE:
    default:
        break;
    }
}

/* ------------------------------------------------------------------------- */
/* pty I/O for all panes */

static void service_panes(app *a)
{
    te_pane *all[MAX_PANES] = { 0 };
    size_t n = layout_all_panes(&a->layout, all, TE_ARRAY_LEN(all));
    for (size_t i = 0; i < n && a->running; i++) {
        te_pane *p = all[i];
        if (p->fd_ready) {
            p->fd_ready = false;
            bool alive = pane_read(p, a->cli.hold);
            if (alive) {
                plat_fd_rearm(a->plat, p->tag);
            } else if (!a->cli.hold) {
                close_pane(a, p);   /* the shell exited: its pane goes away */
                continue;
            }
            a->need_render = true;
        }
        pane_flush(p);
    }
}

/* ------------------------------------------------------------------------- */
/* Rendering */

static bool cursor_blinks(const app *a, const te_pane *p)
{
    uint8_t s = term_modes(p->term)->decscusr;
    if (s != 0) {
        return (s & 1u) != 0;   /* DECSCUSR odd values blink */
    }
    return a->cfg.cursor_blink && term_modes(p->term)->cursor_blink;
}

static int update_blink(app *a, uint64_t now)
{
    te_pane *p = focus_pane(a);
    if (p == nullptr || !a->focused || !cursor_blinks(a, p) || now - a->last_input_ms > BLINK_IDLE_MS) {
        if (!a->cursor_on) {
            a->cursor_on = true;
            a->need_render = true;
        }
        return -1;
    }
    uint64_t phase = (now - a->last_input_ms) / BLINK_MS;
    bool on = (phase & 1u) == 0;
    if (on != a->cursor_on) {
        a->cursor_on = on;
        a->need_render = true;
    }
    uint64_t next = (phase + 1u) * BLINK_MS + a->last_input_ms;
    return (int)(next > now ? next - now : 1);
}

static bool visible_dirty(const app *a, bool *holding, uint64_t now)
{
    te_pane *vis[TE_MAX_PANES_PER_TAB] = { 0 };
    size_t n = layout_visible_panes(&a->layout, vis, TE_ARRAY_LEN(vis));
    bool dirty = false;
    *holding = false;
    for (size_t i = 0; i < n; i++) {
        te_pane *p = vis[i];
        dirty = dirty || term_is_dirty(p->term) || p->bell_until_ms > now;
        if (term_modes(p->term)->sync_output) {
            if (p->sync_since_ms == 0) {
                p->sync_since_ms = now;
            }
            if (now - p->sync_since_ms < SYNC_TIMEOUT_MS) {
                *holding = true;   /* synchronized update in progress */
            }
        } else {
            p->sync_since_ms = 0;
        }
    }
    return dirty;
}

static void render_pane(app *a, te_pane *p, bool focused_pane, bool several, uint64_t now)
{
    /* A switch to or from the alternate screen invalidates the selection. */
    bool alt = term_modes(p->term)->alt_screen;
    if (alt != p->last_alt) {
        sel_clear(&p->sel);
        p->last_alt = alt;
    }
    sel_validate(&p->sel, p->term);
    const char *bar = nullptr;
    if (search_is_open(p->search)) {
        (void)str_fmt(p->bar, sizeof p->bar,
                      "Find: %s\xe2\x96\x8f   %s      Enter: older   Shift+Enter: newer   Esc: close",
                      search_query(p->search), search_status(p->search));
        bar = p->bar;
    }
    te_render_opts o = {
        .padding = padding_px(a),
        .opacity = a->cfg.opacity,
        .cursor_shape = a->cfg.cursor_shape,
        .bold_is_bright = a->cfg.bold_is_bright,
        .cursor_text = a->theme.cursor_text,
        .hl = p->hl,
        .hl_alt_screen = a->cfg.hl_alt_screen,
        .toast_fg = a->theme.selection_fg,
        .toast_bg = a->theme.selection_bg,
        .sel = &p->sel,
        .sel_fg = a->theme.selection_fg,
        .sel_bg = a->theme.selection_bg,
        .search = search_is_open(p->search) ? p->search : nullptr,
        .search_fg = a->theme.search_fg,
        .search_bg = a->theme.search_bg,
        .search_cur_bg = a->theme.search_current_bg,
        .hover = a->hover_valid && a->hover_pane == p ? &a->hover : nullptr,
    };
    float bell = p->bell_until_ms > now ? (float)(p->bell_until_ms - now) / (float)BELL_MS : 0.0f;
    te_frame f = {
        .width = p->w,
        .height = p->h,
        .x = p->x,
        .y = p->y,
        .focused = a->focused && focused_pane,
        .cursor_on = a->cursor_on || !focused_pane,
        .bell = bell,
        .scale = a->scale,
        .bar = bar,
        .dim = several && !focused_pane ? a->cfg.inactive_dim : 0.0f,
        .animate_cursor = a->cfg.cursor_animate && focused_pane,
        .anim_key = p->id,
        .now_ms = now,
    };
    if (p->w > 0 && p->h > 0) {
        (void)renderer_draw(a->ren, p->term, &o, &f);
    }
    term_clear_dirty(p->term);
    if (bell > 0.0f) {
        a->need_render = true;
    }
    /* Auto-detection switched profile while drawing: redraw with the new rules. */
    if (p->hl != nullptr && hl_take_changed(p->hl)) {
        a->need_render = true;
        if (hl_mode(p->hl) == TE_HL_AUTO && hl_active_name(p->hl) != nullptr) {
            char msg[128] = { 0 };
            (void)str_fmt(msg, sizeof msg, "Highlighting: %s", hl_active_name(p->hl));
            show_toast(a, msg);
        }
    }
}

static void render(app *a, uint64_t now)
{
    a->need_render = false;
    renderer_begin(a->ren, a->width, a->height, a->theme.bg, a->cfg.opacity);
    te_pane *vis[TE_MAX_PANES_PER_TAB] = { 0 };
    size_t n = layout_visible_panes(&a->layout, vis, TE_ARRAY_LEN(vis));
    te_pane *focus = focus_pane(a);
    for (size_t i = 0; i < n; i++) {
        render_pane(a, vis[i], vis[i] == focus, n > 1, now);
    }
    int dividers[2 * TE_MAX_PANES_PER_TAB][4];
    te_chrome c = make_chrome(a);
    c.ndividers = layout_dividers(&a->layout, dividers, TE_ARRAY_LEN(dividers));
    c.dividers = (const int (*)[4])dividers;
    c.toast = now < a->toast_until_ms ? a->toast : nullptr;
    c.modal = a->pending_paste != nullptr ? a->modal : nullptr;
    renderer_chrome(a->ren, &c);
    if (renderer_end(a->ren) == TE_OK) {
        plat_swap(a->plat);
    }
    if (renderer_animating(a->ren)) {
        a->need_render = true;
    }
}

/* ------------------------------------------------------------------------- */
/* Main */

static void app_destroy(app *a)
{
    te_pane *all[MAX_PANES] = { 0 };
    size_t n = layout_all_panes(&a->layout, all, TE_ARRAY_LEN(all));
    for (size_t i = 0; i < n; i++) {
        if (a->plat != nullptr) {
            plat_unwatch_fd(a->plat, all[i]->tag);
        }
        pane_destroy(&all[i]);
    }
    layout_destroy(&a->layout);
    url_destroy(&a->urls);
    watch_destroy(&a->watch);
    TE_FREE(a->pending_paste);
    spawn_reap();
    renderer_destroy(&a->ren);
    font_destroy(&a->font);
    plat_destroy(&a->plat);
}

static int min_timeout(int cur, int cand)
{
    return (cur < 0 || cand < cur) ? cand : cur;
}

static void run(app *a)
{
    while (a->running) {
        uint64_t now = plat_ticks_ms();
        int timeout = update_blink(a, now);
        bool holding = false;
        bool want_render = visible_dirty(a, &holding, now) || a->need_render;
        if (want_render && holding) {
            timeout = min_timeout(timeout, (int)SYNC_TIMEOUT_MS / 4);   /* never spin while held */
        } else if (want_render) {
            timeout = 0;
        }
        te_pane *all[MAX_PANES] = { 0 };
        size_t np = layout_all_panes(&a->layout, all, TE_ARRAY_LEN(all));
        for (size_t i = 0; i < np; i++) {
            if (all[i]->wq_len > 0) {
                timeout = min_timeout(timeout, 10);
            }
        }
        if (a->reload_due_ms != 0) {
            if (now >= a->reload_due_ms) {
                a->reload_due_ms = 0;
                reload_config(a);
            } else {
                timeout = min_timeout(timeout, (int)(a->reload_due_ms - now));
            }
        }
        spawn_reap();
        if (a->toast_until_ms > now) {
            timeout = min_timeout(timeout, (int)(a->toast_until_ms - now) + 1);
        } else if (a->toast_until_ms != 0) {
            a->toast_until_ms = 0;
            a->need_render = true;
        }
        te_event ev = { 0 };
        if (plat_wait_event(a->plat, &ev, timeout)) {
            handle_event(a, &ev);
            while (a->running && plat_wait_event(a->plat, &ev, 0)) {
                handle_event(a, &ev);
            }
        }
        if (!a->running) {
            break;
        }
        service_panes(a);
        if (!a->running) {
            break;
        }
        now = plat_ticks_ms();
        bool dirty = visible_dirty(a, &holding, now);
        if (holding) {
            continue;   /* synchronized update in progress: hold the frame */
        }
        if (a->need_render || dirty) {
            render(a, now);
        }
    }
}

int main(int argc, char **argv)
{
    app a = { 0 };
    a.watch.fd = -1;
    a.watch_tag = -1;
    int rc = parse_cli(argc, argv, &a.cli);
    if (rc != 0) {
        return rc > 0 ? EXIT_SUCCESS : EXIT_FAILURE;
    }
    te_log_set_level(a.cli.verbose ? TE_LOG_DEBUG : TE_LOG_INFO);
    (void)signal(SIGPIPE, SIG_IGN);

    config_defaults(&a.cfg);
    if (config_load(&a.cfg, a.cli.config_path) != TE_OK && a.cli.config_path != nullptr) {
        return EXIT_FAILURE;
    }
    if (theme_load(&a.theme, a.cfg.theme) != TE_OK) {
        theme_defaults(&a.theme);
    }
    layout_init(&a.layout, 2);

    int exit_code = EXIT_FAILURE;
    te_err err = TE_OK;
    te_platform_opts po = {
        .title = a.cli.title != nullptr ? a.cli.title : APP_DISPLAY_NAME,
        .app_id = a.cli.app_id,
        .width = 800,
        .height = 600,
        .transparent = a.cfg.opacity < 1.0f,
    };
    err = plat_create(&a.plat, &po);
    if (err != TE_OK) {
        LOG_E("cannot open a window: %s", te_strerror(err));
        goto out;
    }
    a.scale = plat_content_scale(a.plat);
    a.font_pt = a.cfg.font_size;
    a.layout.divider = (int)lroundf(2.0f * a.scale);
    err = load_font(&a);
    if (err != TE_OK) {
        LOG_E("cannot load font '%s': %s", a.cfg.font_family, te_strerror(err));
        goto out;
    }
    err = renderer_create(&a.ren, a.font);
    if (err != TE_OK) {
        LOG_E("cannot initialise the renderer: %s", te_strerror(err));
        goto out;
    }

    const te_font_metrics *m = font_metrics(a.font);
    int pad = padding_px(&a);
    a.width = (int)a.cfg.cols * m->cell_w + 2 * pad;
    a.height = (int)a.cfg.rows * m->cell_h + 2 * pad;
    plat_set_size_px(a.plat, a.width, a.height);

    te_pane *first = spawn_pane(&a, a.cli.workdir, a.cli.exec_argv);
    if (first == nullptr) {
        goto out;
    }
    if (layout_add_tab(&a.layout, first) != TE_OK) {
        pane_destroy(&first);
        goto out;
    }
    if (url_create(&a.urls) != TE_OK) {
        LOG_W("link detection unavailable");
    }
    if (watch_init(&a.watch, a.cli.config_path) == TE_OK &&
        plat_watch_fd(a.plat, a.watch.fd, &a.watch_tag) != TE_OK) {
        a.watch_tag = -1;
        watch_destroy(&a.watch);
    }

    plat_show(a.plat);
    a.running = true;
    a.focused = true;
    a.cursor_on = true;
    a.last_input_ms = plat_ticks_ms();
    apply_size(&a);
    run(&a);
    /* `-e cmd` passes the command's exit status on. */
    exit_code = a.exit_status;

out:
    app_destroy(&a);
    return exit_code;
}
