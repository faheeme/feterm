/*
 * Application entry point: CLI, window, event loop, and the glue between
 * panes (shell sessions), tabs/splits, input and the renderer.
 */
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "app/banner.h"
#include "app/layout.h"
#include "app/pane.h"
#include "config/config.h"
#include "config/paths.h"
#include "config/theme.h"
#include "config/watch.h"
#include "highlight/highlight.h"
#include "input/click.h"
#include "input/keybind.h"
#include "input/keys.h"
#include "input/mouse.h"
#include "platform/platform.h"
#include "render/font.h"
#include "remote/remote.h"
#include "render/renderer.h"
#include "sessions/launch.h"
#include "sessions/serial.h"
#include "sessions/telnet.h"
#include "ui/files.h"
#include "ui/paint.h"
#include "ui/search.h"
#include "ui/selection.h"
#include "ui/sidebar.h"
#include "ui/url.h"
#include "util/log.h"
#include "util/mem.h"
#include "util/os.h"
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
#define RECONNECT_MIN_MS   2000u
#define RECONNECT_MAX_MS   60000u
#define STABLE_SESSION_MS  120000u   /* a connection this old resets the reconnect backoff */

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
    te_font      *ui_font;      /* sidebar, menus, dialogs: the configured size, never zoomed */
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

    float         opacity;           /* cfg.opacity, or 1.0 when the desktop cannot show it */

    char          tab_title_buf[TE_MAX_TABS][96];
    const char   *tab_titles[TE_MAX_TABS];
    uint32_t      tab_colors[TE_MAX_TABS];

    /* Sidebar and sessions */
    te_sidebar    sb;
    bool          sb_ready;
    char          self_exe[TE_PATH_MAX];
    char          control_dir[TE_PATH_MAX];   /* "" when SSH connection sharing is unavailable */

    /* File browser connections, one per SSH pane that has shown the Files tab */
    struct {
        uint32_t   pane_id;
        te_remote *r;
        char       last_cwd[1024];   /* last folder the remote shell reported (OSC 7) */
    } remotes[MAX_PANES];
    char          edit_root[TE_PATH_MAX];      /* private folder for edit-locally copies */
    char          download_dir[1024];          /* expanded [files] download_dir */
} app;

static void drop_remote(app *a, uint32_t pane_id);

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
    LOG_D("toast: %s", a->toast);
}

static void note_input(app *a)
{
    a->last_input_ms = plat_ticks_ms();
    te_pane *f = layout_focus(&a->layout);
    if (f != nullptr) {
        f->last_input_ms = a->last_input_ms;   /* anti-idle counts from real typing only */
    }
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
    if (t != nullptr && t[0] != '\0') {
#ifdef _WIN32
        /* ConPTY titles a console with its program's full path: show just "pwsh.exe". */
        size_t n = strlen(t);
        const char *slash = strrchr(t, '\\');
        if (slash != nullptr && n > 4 && _stricmp(t + n - 4, ".exe") == 0 && os_path_is_absolute(t)) {
            return slash + 1;
        }
#endif
        return t;
    }
    return p->sess != nullptr ? p->sess->s.name : "shell";
}

/* Tab label: a session's name, so tabs stay recognisable whatever the remote prompt sets. */
static const char *tab_label(const te_pane *p)
{
    return p->sess != nullptr ? p->sess->s.name : pane_title(p);
}

static void update_window_title(app *a)
{
    te_pane *p = focus_pane(a);
    if (p != nullptr) {
        plat_set_title(a->plat, a->cli.title != nullptr && term_title(p->term)[0] == '\0' ? a->cli.title : pane_title(p));
    }
}

/* The sidebar's place in the window for the current size and font. */
static void layout_sidebar(app *a)
{
    if (!a->sb_ready || a->ren == nullptr) {
        return;
    }
    te_painter p;
    renderer_painter(a->ren, a->scale, &p);
    sidebar_layout(&a->sb, &p, (float)a->width, (float)a->height);
}

/* Horizontal extent of the terminal area (everything but the sidebar). */
static void term_area(const app *a, int *x, int *w)
{
    int sx = a->sb_ready ? (int)sidebar_term_x(&a->sb) : 0;
    int sw = a->sb_ready ? (int)sidebar_width(&a->sb) : 0;
    *x = sx;
    *w = a->width - sw > 1 ? a->width - sw : 1;
}

/* Recompute pane rectangles and resize every terminal to its new grid. */
static void relayout(app *a)
{
    layout_sidebar(a);
    int bar = tab_bar_height(a);
    int ax = 0;
    int aw = 0;
    term_area(a, &ax, &aw);
    layout_compute(&a->layout, ax, bar, aw, a->height - bar);
    te_pane *all[MAX_PANES] = { 0 };
    size_t n = layout_all_panes(&a->layout, all, TE_ARRAY_LEN(all));
    int pad = padding_px(a);
    for (size_t i = 0; i < n; i++) {
        te_pane *p = all[i];
        int r[4] = { ax, bar, aw, a->height - bar };
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

/* The UI font follows the configured size and the DPI, never the zoom. */
[[nodiscard]] static te_err load_ui_font(app *a)
{
    float px = a->cfg.font_size * 96.0f / 72.0f * a->scale;
    te_font *nf = nullptr;
    te_err rc = font_create(&nf, a->cfg.font_family, px, a->cfg.line_height);
    if (rc != TE_OK) {
        return rc;
    }
    if (a->ren != nullptr) {
        renderer_set_ui_font(a->ren, nf);
    }
    font_destroy(&a->ui_font);
    a->ui_font = nf;
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

/* The pane's output: the pty descriptor (POSIX) or the ConPTY reader's ready event (Windows). */
static te_err watch_pty(app *a, te_pane *p)
{
#ifdef _WIN32
    return plat_watch_handle(a->plat, pty_ready_handle(&p->pty), &p->tag);
#else
    return plat_watch_fd(a->plat, p->pty.master_fd, &p->tag);
#endif
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
    if (watch_pty(a, p) != TE_OK) {
        LOG_E("cannot watch the pty");
        pane_destroy(&p);
        return nullptr;
    }
    return p;
}

/* The welcome banner at the top of a new local shell (before its first prompt). */
static void show_banner(app *a, te_pane *p)
{
    if (!a->cfg.banner || p == nullptr) {
        return;
    }
    const te_font_metrics *m = font_metrics(a->font);
    int cw = m->cell_w > 0 ? m->cell_w : 8;
    int ch = m->cell_h > 0 ? m->cell_h : 16;
    uint16_t rows = 2;
    /* A square logo two lines tall: as many columns as that height in cells. */
    uint16_t cols = (uint16_t)TE_CLAMP((rows * ch + cw / 2) / cw, 2, 8);
    const uint8_t *rgba = nullptr;
    size_t len = 0;
    int px = rows * ch > 64 ? 128 : 64;   /* sharp on HiDPI, small otherwise */
    if (!plat_logo_rgba(px, &rgba, &len) && !plat_logo_rgba(64, &rgba, &len)) {
        rgba = nullptr;
    }
    uint32_t side = rgba != nullptr ? (uint32_t)(len == 128u * 128u * 4u ? 128 : 64) : 0;
    char *text = nullptr;
    size_t n = 0;
    if (banner_build(rgba, len, side, cols, rows, &text, &n) == TE_OK) {
        term_feed(p->term, (const uint8_t *)text, n);
        TE_FREE(text);
    }
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
    drop_remote(a, p->id);   /* its file browser connection goes with it */
    if (p->tag >= 0) {
        plat_unwatch_fd(a->plat, p->tag);
    }
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
    show_banner(a, p);   /* after its final size: a width change would drop the logo */
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
    show_banner(a, p);
    update_window_title(a);
}

/* ------------------------------------------------------------------------- */
/* Sessions */

static void apply_session_highlight(te_pane *p, const te_session *s)
{
    if (p->hl == nullptr || s->highlight[0] == '\0' || strcmp(s->highlight, "auto") == 0) {
        return;   /* the configured default (usually auto-detect) */
    }
    te_err rc = strcmp(s->highlight, "off") == 0 ? hl_set_mode(p->hl, TE_HL_OFF, nullptr)
                                                 : hl_set_mode(p->hl, TE_HL_PINNED, s->highlight);
    if (rc != TE_OK) {
        LOG_W("highlight profile '%s' not found; using auto-detection", s->highlight);
    }
    (void)hl_take_changed(p->hl);
}

static void host_sb_toast(void *user, const char *msg)
{
    show_toast(user, msg);
}

static void host_sb_copy(void *user, const char *text)
{
    app *a = user;
    plat_clipboard_set(a->plat, text);
    show_toast(a, "Copied");
}

static void host_sb_connect(void *user, const te_session *s, const te_session_eff *eff, bool split_pane)
{
    app *a = user;
    char msg[160] = { 0 };
    te_pane_session *ps = te_calloc(1, sizeof *ps);
    if (ps == nullptr) {
        show_toast(a, "Out of memory");
        return;
    }
    ps->s = *s;
    ps->eff = *eff;
    te_launch_opts lo = {
        .self_exe = a->self_exe[0] != '\0' ? a->self_exe : nullptr,
        .control_dir = a->control_dir[0] != '\0' ? a->control_dir : nullptr,
        .keepalive = a->cfg.keepalive,
    };
    te_err rc = launch_build(s, eff, &lo, &ps->launch);
    if (rc != TE_OK) {
        (void)str_fmt(msg, sizeof msg, "Cannot start %s: %s", s->name, te_strerror(rc));
        show_toast(a, msg);
        TE_FREE(ps);
        return;
    }
    const char *const *argv = ps->launch.argc > 0 ? ps->launch.argv : nullptr;
    te_pane *p = spawn_pane(a, ps->launch.cwd[0] != '\0' ? ps->launch.cwd : nullptr, argv);
    if (p == nullptr) {
        TE_FREE(ps);
        return;
    }
    p->sess = ps;   /* the pane owns it now; argv points into it, for reconnecting */
    ps = nullptr;
    bool banner = s->type == TE_SESS_LOCAL && s->command[0] == '\0';   /* a saved local shell starts like any other */
    p->started_ms = plat_ticks_ms();
    p->last_input_ms = p->started_ms;
    apply_session_highlight(p, s);
    te_err lr = split_pane && focus_pane(a) != nullptr ? layout_split(&a->layout, p, true) : layout_add_tab(&a->layout, p);
    if (lr != TE_OK) {
        show_toast(a, split_pane ? "Too many panes in this tab" : "Too many tabs");
        if (p->tag >= 0) {
            plat_unwatch_fd(a->plat, p->tag);
        }
        pane_destroy(&p);
        return;
    }
    sidebar_blur(&a->sb);
    relayout(a);
    if (banner) {
        show_banner(a, p);
    }
    update_window_title(a);
}

static void pane_note(te_pane *p, const char *msg)
{
    term_feed(p->term, (const uint8_t *)msg, strlen(msg));
}

static void reconnect_pane(app *a, te_pane *p)
{
    if (p == nullptr || p->sess == nullptr || !p->disconnected) {
        return;
    }
    pane_note(p, "\r\n\x1b[2m[Reconnecting...]\x1b[0m\r\n");
    const te_launch *l = &p->sess->launch;
    te_err rc = pane_respawn(p, &a->cfg, l->argc > 0 ? l->argv : nullptr, l->cwd[0] != '\0' ? l->cwd : nullptr);
    if (rc == TE_OK && watch_pty(a, p) != TE_OK) {
        p->tag = -1;
        rc = TE_ERR_PLATFORM;
    }
    if (rc != TE_OK) {
        char msg[160] = { 0 };
        (void)str_fmt(msg, sizeof msg, "\x1b[31m[Reconnect failed: %s. Press Enter to try again.]\x1b[0m\r\n",
                      te_strerror(rc));
        pane_note(p, msg);
        p->exited = true;
        p->reconnect_at_ms = 0;
        a->need_render = true;
        return;
    }
    p->disconnected = false;
    p->reconnect_at_ms = 0;
    p->started_ms = plat_ticks_ms();
    p->last_input_ms = p->started_ms;
    a->need_render = true;
}

/* A session's connection ended with an error: offer (or schedule) a reconnect. */
static void on_disconnect(app *a, te_pane *p)
{
    uint64_t now = plat_ticks_ms();
    if (now - p->started_ms > STABLE_SESSION_MS) {
        p->reconnect_tries = 0;
    }
    if (p->tag >= 0) {
        plat_unwatch_fd(a->plat, p->tag);
        p->tag = -1;
    }
    p->disconnected = true;
    char msg[200] = { 0 };
    if (a->cfg.reconnect == TE_RECONNECT_AUTO) {
        uint32_t shift = p->reconnect_tries < 5 ? p->reconnect_tries : 5;
        uint64_t delay = (uint64_t)RECONNECT_MIN_MS << shift;
        if (delay > RECONNECT_MAX_MS) {
            delay = RECONNECT_MAX_MS;
        }
        p->reconnect_at_ms = now + delay;
        p->reconnect_tries++;
        (void)str_fmt(msg, sizeof msg,
                      "\r\n\x1b[33m[Connection lost (exit %d). Reconnecting in %u s: Enter to retry now, Esc to stop.]\x1b[0m\r\n",
                      p->status, (unsigned)(delay / 1000u));
    } else {
        p->reconnect_at_ms = 0;
        (void)str_fmt(msg, sizeof msg,
                      "\r\n\x1b[33m[Connection lost (exit %d). Press Enter to reconnect, or Ctrl+Shift+W to close.]\x1b[0m\r\n",
                      p->status);
    }
    pane_note(p, msg);
    a->need_render = true;
}

/* Keys for a pane whose connection dropped. Returns true if handled. */
static bool disconnected_key(app *a, te_pane *p, te_key key)
{
    if (!p->disconnected) {
        return false;
    }
    if (key == TE_KEY_ENTER) {
        p->reconnect_tries = 0;
        reconnect_pane(a, p);
    } else if (key == TE_KEY_ESCAPE && p->reconnect_at_ms != 0) {
        p->reconnect_at_ms = 0;
        pane_note(p, "\x1b[2m[Automatic reconnect stopped. Press Enter to reconnect.]\x1b[0m\r\n");
        a->need_render = true;
    }
    return true;   /* nothing else reaches a dead connection */
}

/* Keep-awake bytes for sessions the user has not typed into for a while.
 * Returns the milliseconds until the next one is due, or -1. */
static int service_anti_idle(app *a, uint64_t now)
{
    if (a->cfg.anti_idle_len == 0) {
        return -1;
    }
    int timeout = -1;
    te_pane *all[MAX_PANES] = { 0 };
    size_t n = layout_all_panes(&a->layout, all, TE_ARRAY_LEN(all));
    for (size_t i = 0; i < n; i++) {
        te_pane *p = all[i];
        if (p->sess == nullptr || p->exited || p->disconnected) {
            continue;
        }
        int32_t secs = p->sess->eff.anti_idle == TE_ANTI_IDLE_INHERIT ? a->cfg.anti_idle : p->sess->eff.anti_idle;
        if (secs <= 0) {
            continue;
        }
        uint64_t last = TE_MAX(TE_MAX(p->last_input_ms, p->last_anti_ms), p->started_ms);
        uint64_t due = last + (uint64_t)secs * 1000u;
        if (now >= due) {
            /* Never type into a full-screen program (vim, less, top): there the byte
             * could mean something, and those programs are not idle sessions. */
            if (!term_modes(p->term)->alt_screen) {
                pane_queue(p, a->cfg.anti_idle_bytes, a->cfg.anti_idle_len);
            }
            p->last_anti_ms = now;
            due = now + (uint64_t)secs * 1000u;
        }
        uint64_t wait = due - now;
        int w = wait > (uint64_t)INT32_MAX ? INT32_MAX : (int)wait;
        timeout = timeout < 0 || w < timeout ? w : timeout;
    }
    return timeout;
}

static void send_break(app *a)
{
    te_pane *p = focus_pane(a);
    if (p == nullptr || p->exited) {
        return;
    }
    te_sess_type t = p->sess != nullptr ? p->sess->s.type : TE_SESS_LOCAL;
    if (t == TE_SESS_SERIAL || t == TE_SESS_TELNET) {
        pty_send_break(&p->pty);
        show_toast(a, "Break sent");
    } else if (t == TE_SESS_SSH) {
        show_toast(a, "SSH: press Enter, then type ~B to send a Break");
    } else {
        show_toast(a, "Break is for serial and Telnet sessions");
    }
}

/* ------------------------------------------------------------------------- */
/* File browser */

static void host_files_dialog(void *user, te_dialog_kind kind, int request)
{
    app *a = user;
    static char home[1024];   /* outlives the dialog, which may read it later */
    const char *def = a->download_dir;
    if (kind != TE_DIALOG_CHOOSE_FOLDER) {
        def = os_home_dir(home, sizeof home) == TE_OK ? home : nullptr;
    }
    plat_file_dialog(a->plat, kind, def, request);
}

static bool host_r_watch(void *user, te_proc_obj obj, int *tag)
{
    app *a = user;
#ifdef _WIN32
    return plat_watch_handle(a->plat, obj, tag) == TE_OK;
#else
    return plat_watch_fd(a->plat, obj, tag) == TE_OK;
#endif
}

static void host_r_unwatch(void *user, int tag)
{
    app *a = user;
    plat_unwatch_fd(a->plat, tag);
}

static void host_r_rearm(void *user, int tag)
{
    app *a = user;
    plat_fd_rearm(a->plat, tag);
}

static void host_r_changed(void *user)
{
    app *a = user;
    a->need_render = true;
}

static void host_r_prompt(void *user, te_remote *r, te_prompt_kind kind, const char *text)
{
    app *a = user;
    layout_sidebar(a);
    files_prompt(&a->sb.files, r, kind, text);
    a->need_render = true;
}

/* Extensions the desktop "open" handler would run rather than edit. A download
 * is named by the (untrusted) remote server, so we never hand one of these to the
 * default handler; the user must point [files] editor at a real editor first. */
static bool path_is_executable_type(const char *path)
{
    const char *slash = strrchr(path, '/');
    const char *base = slash != nullptr ? slash + 1 : path;
    const char *dot = strrchr(base, '.');
    if (dot == nullptr || dot[1] == '\0') {
        return false;
    }
    static const char *const ext[] = {
        "exe", "com", "bat", "cmd", "scr", "pif", "msi", "msp", "cpl", "hta", "jar",
        "js",  "jse", "vbs", "vbe", "wsf", "wsh", "ps1", "psm1", "lnk", "url", "reg",
        "inf", "scf", "msc", "sct", "desktop", "application", "appref-ms", "gadget",
        "dll", "so",  "dylib", "run",
    };
    for (size_t i = 0; i < TE_ARRAY_LEN(ext); i++) {
        if (strcasecmp(dot + 1, ext[i]) == 0) {
            return true;
        }
    }
    return false;
}

/* Open a downloaded copy in the user's editor (or the desktop default), without a shell. */
static void host_r_edit_open(void *user, const char *path)
{
    app *a = user;
    char buf[1024] = { 0 };
    const char *argv[10] = { 0 };
    size_t n = 0;
    if (a->cfg.editor[0] != '\0' && str_copy(buf, sizeof buf, a->cfg.editor) == TE_OK) {
        char *save = nullptr;
        for (char *tok = strtok_r(buf, " ", &save); tok != nullptr && n < 8; tok = strtok_r(nullptr, " ", &save)) {
            argv[n++] = tok;
        }
    }
    /* With no editor configured we fall back to the desktop "open" handler, which
     * would *execute* a program or script. Refuse rather than run a file the remote
     * side chose its name for (e.g. a disguised .exe/.desktop). */
    if (n == 0 && path_is_executable_type(path)) {
        show_toast(a, "Not opening an executable file; set [files] editor in config.ini to edit it");
        return;
    }
    if (n == 0) {
        argv[n++] = "xdg-open";
    }
    argv[n++] = path;
    argv[n] = nullptr;
    if (spawn_detached(argv) != TE_OK) {
        show_toast(a, "Could not start the editor; set [files] editor in config.ini");
    }
}

static te_remote *remote_for(const app *a, uint32_t pane_id)
{
    for (size_t i = 0; i < MAX_PANES; i++) {
        if (a->remotes[i].r != nullptr && a->remotes[i].pane_id == pane_id) {
            return a->remotes[i].r;
        }
    }
    return nullptr;
}

static void drop_remote(app *a, uint32_t pane_id)
{
    for (size_t i = 0; i < MAX_PANES; i++) {
        if (a->remotes[i].r != nullptr && a->remotes[i].pane_id == pane_id) {
            if (a->sb.files.remote == a->remotes[i].r) {
                files_set_remote(&a->sb.files, nullptr, false);
            }
            if (a->sb.files.prompt_remote == a->remotes[i].r) {
                a->sb.files.prompt_remote = nullptr;
            }
            remote_destroy(&a->remotes[i].r);
            a->remotes[i].pane_id = 0;
            a->remotes[i].last_cwd[0] = '\0';
        }
    }
}

/* Point the Files tab at the focused pane's connection, opening it on first use. */
static void update_files(app *a)
{
    if (!a->sb_ready) {
        return;
    }
    te_pane *p = focus_pane(a);
    bool ssh = p != nullptr && p->sess != nullptr && p->sess->s.type == TE_SESS_SSH && !p->disconnected;
    te_remote *r = ssh ? remote_for(a, p->id) : nullptr;
    bool showing = a->sb.visible && a->sb.tab == TE_SB_FILES;
    if (ssh && r == nullptr && showing) {
        size_t slot = MAX_PANES;
        for (size_t i = 0; i < MAX_PANES && slot == MAX_PANES; i++) {
            if (a->remotes[i].r == nullptr) slot = i;
        }
        char edit_dir[TE_PATH_MAX] = { 0 };
        if (a->edit_root[0] != '\0') {
            (void)str_fmt(edit_dir, sizeof edit_dir, "%s/%u", a->edit_root, (unsigned)p->id);
        }
        te_remote_opts ro = { .control_dir = a->control_dir, .edit_dir = edit_dir, .keepalive = a->cfg.keepalive };
        te_remote_host rh = {
            .user = a, .watch = host_r_watch, .unwatch = host_r_unwatch, .rearm = host_r_rearm,
            .changed = host_r_changed, .prompt = host_r_prompt, .toast = host_sb_toast, .edit_open = host_r_edit_open,
        };
        if (slot < MAX_PANES && remote_create(&a->remotes[slot].r, &p->sess->s, &p->sess->eff, &ro, &rh) == TE_OK) {
            a->remotes[slot].pane_id = p->id;
            a->remotes[slot].last_cwd[0] = '\0';
            r = a->remotes[slot].r;
            remote_connect(r);
        }
    }
    files_set_remote(&a->sb.files, r, ssh);
    /* Follow the remote shell's folder when it reports one (OSC 7). */
    if (r != nullptr && a->cfg.follow_terminal && remote_state(r) == TE_RS_READY) {
        for (size_t i = 0; i < MAX_PANES; i++) {
            if (a->remotes[i].r == r) {
                const char *cwd = term_cwd(p->term);
                if (cwd != nullptr && cwd[0] == '/' && strcmp(cwd, a->remotes[i].last_cwd) != 0) {
                    (void)str_copy(a->remotes[i].last_cwd, sizeof a->remotes[i].last_cwd, cwd);
                    files_follow(&a->sb.files, cwd);
                }
            }
        }
    }
}

static void apply_files_config(app *a)
{
    char dir[1024] = { 0 };
    if (launch_expand_home(a->cfg.download_dir, dir, sizeof dir) != TE_OK || !os_path_is_absolute(dir)) {
        (void)launch_expand_home("~/Downloads", dir, sizeof dir);
    }
    (void)str_copy(a->download_dir, sizeof a->download_dir, dir);
    if (mkdir(a->download_dir, 0755) != 0 && errno != EEXIST) {
        LOG_W("cannot create the download folder %s: %s", a->download_dir, strerror(errno));
    }
    if (a->sb_ready) {
        files_set_download_dir(&a->sb.files, a->download_dir);
    }
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

/* Works out the opacity to draw with: the configured value, unless nothing on
 * this desktop can composite a translucent window (X11 without a compositor). */
static void update_opacity(app *a)
{
    float want = TE_CLAMP(a->cfg.opacity, 0.1f, 1.0f);
    bool available = want >= 1.0f || plat_transparency_available(a->plat);
    float old = a->opacity;
    a->opacity = available ? want : 1.0f;
    if (!available && old != 1.0f) {
        LOG_I("no compositor running: transparency off (opacity %.2f ignored)", (double)want);
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
        } else if (load_ui_font(a) != TE_OK) {
            LOG_W("could not load the sidebar font; keeping the previous one");
        }
    }
    te_pane *all[MAX_PANES] = { 0 };
    size_t n = layout_all_panes(&a->layout, all, TE_ARRAY_LEN(all));
    for (size_t i = 0; i < n; i++) {
        pane_setup_highlight(all[i], &a->cfg);
    }
    update_opacity(a);
    apply_files_config(a);
    if (a->sb_ready) {
        sidebar_configure(&a->sb, a->cfg.sidebar_visible, a->cfg.sidebar_right, a->cfg.sidebar_width, false);
        sidebar_reload(&a->sb);
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
    case TE_ACT_TOGGLE_SIDEBAR:   sidebar_toggle(&a->sb); break;
    case TE_ACT_SIDEBAR_SESSIONS: sidebar_show_tab(&a->sb, TE_SB_SESSIONS); break;
    case TE_ACT_SIDEBAR_FILES:    sidebar_show_tab(&a->sb, TE_SB_FILES); break;
    case TE_ACT_QUICK_CONNECT:    layout_sidebar(a); sidebar_quick_connect(&a->sb); break;
    case TE_ACT_SEND_BREAK:       send_break(a); break;
    case TE_ACT_NONE:
    case TE_ACT_COUNT:
    default:
        break;
    }
    if (sidebar_take_layout_change(&a->sb)) {
        relayout(a);
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
    uint32_t kmods = ev->key.mods & (TE_MOD_SHIFT | TE_MOD_CTRL | TE_MOD_ALT | TE_MOD_SUPER);
    /* Printable keys without Ctrl/Alt arrive again as text; only those may produce text. */
    bool printable = ev->key.key == TE_KEY_CHAR && (kmods & (TE_MOD_CTRL | TE_MOD_ALT | TE_MOD_SUPER)) == 0;
    if (sidebar_captures(&a->sb)) {
        /* A menu or dialog is open: it gets everything (paste goes into the field). */
        te_action pa = keymap_lookup(&a->cfg.keys, ev->key.key, ev->key.cp, kmods);
        if (pa == TE_ACT_PASTE || pa == TE_ACT_PASTE_PRIMARY) {
            char *clip = pa == TE_ACT_PASTE ? plat_clipboard_get(a->plat) : plat_primary_get(a->plat);
            if (clip != nullptr) {
                (void)sidebar_text(&a->sb, clip);
                TE_FREE(clip);
            }
        } else {
            (void)sidebar_key(&a->sb, ev->key.key, ev->key.cp, ev->key.mods);
        }
        a->suppress_text = !printable;
        a->need_render = true;
        return;
    }
    if (sidebar_focused(&a->sb)) {
        te_action sa = keymap_lookup(&a->cfg.keys, ev->key.key, ev->key.cp, kmods);
        if (sa != TE_ACT_NONE) {
            do_action(a, sa);
            a->suppress_text = true;
            return;
        }
        if (sidebar_key(&a->sb, ev->key.key, ev->key.cp, ev->key.mods)) {
            a->suppress_text = !printable;
            a->need_render = true;
            if (sidebar_take_layout_change(&a->sb)) {
                relayout(a);
            }
            return;
        }
    }
    if (a->pending_paste != nullptr) {
        modal_answer(a, ev->key.key == TE_KEY_ENTER);
        a->suppress_text = true;
        return;
    }
    if (search_is_open(p->search)) {
        a->suppress_text = search_key(a, p, ev);
        return;
    }
    te_action act = keymap_lookup(&a->cfg.keys, ev->key.key, ev->key.cp, kmods);
    if (act != TE_ACT_NONE) {
        do_action(a, act);
        a->suppress_text = true;
        return;
    }
    if (disconnected_key(a, p, ev->key.key)) {
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
    if (sidebar_focused(&a->sb)) {
        if (sidebar_text(&a->sb, ev->text.text)) {
            a->need_render = true;
            return;
        }
    }
    if (a->pending_paste != nullptr || p == nullptr || p->disconnected) {
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
    char prev[TE_MAX_URI + 1] = { 0 };
    if (had) {
        (void)str_copy(prev, sizeof prev, a->hover.uri);
    }
    a->hover_valid = want && url_at(a->urls, p->term, term_view_abs(p->term, row), col, &a->hover);
    a->hover_pane = a->hover_valid ? p : nullptr;
    /* Show the real destination before the click: an OSC 8 hyperlink can display
     * any text while pointing somewhere else, so the link text is not trustworthy. */
    if (a->hover_valid && strcmp(prev, a->hover.uri) != 0) {
        char msg[128] = { 0 };
        bool cut = strnlen(a->hover.uri, sizeof a->hover.uri) > 96;
        (void)str_fmt(msg, sizeof msg, "%.96s%s  —  Ctrl+click to open", a->hover.uri, cut ? "..." : "");
        show_toast(a, msg);
    }
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
        const char *title = t->focus != nullptr ? tab_label(t->focus) : "";
        a->tab_colors[i] = t->focus != nullptr && t->focus->sess != nullptr ? tag_color_rgb(t->focus->sess->eff.color) : 0;
        if (t->npanes > 1) {
            (void)str_fmt(a->tab_title_buf[i], sizeof a->tab_title_buf[i], "%s [%zu]", title, t->npanes);
        } else {
            (void)str_copy(a->tab_title_buf[i], sizeof a->tab_title_buf[i], title);
        }
        a->tab_titles[i] = a->tab_title_buf[i];
    }
    int ax = 0;
    int aw = 0;
    term_area(a, &ax, &aw);
    return (te_chrome){
        .win_w = a->width,
        .win_h = a->height,
        .tab_titles = a->tab_titles,
        .ntabs = a->layout.ntabs,
        .active_tab = a->layout.active,
        .tab_bar_h = tab_bar_height(a),
        .area_x = ax,
        .area_w = aw,
        .tab_colors = a->tab_colors,
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

    /* The sidebar first: its menus and dialogs, drags, resizing, or a click inside it. */
    bool sb_busy = sidebar_captures(&a->sb) || a->sb.pressed || a->sb.resizing;
    if (sb_busy || (ev->button.down && a->buttons_held == 0 &&
                    (sidebar_contains(&a->sb, ev->button.x, ev->button.y) ||
                     sidebar_on_edge(&a->sb, ev->button.x, ev->button.y)))) {
        int sbtn = b == TE_MB_LEFT ? 0 : (b == TE_MB_MIDDLE ? 1 : 2);
        (void)sidebar_button(&a->sb, ev->button.x, ev->button.y, sbtn, ev->button.down, ev->button.clicks,
                             ev->button.mods);
        if (sidebar_take_layout_change(&a->sb)) {
            relayout(a);
        }
        a->need_render = true;
        return;
    }
    if (ev->button.down) {
        sidebar_blur(&a->sb);   /* a click in the terminal gives it the keyboard back */
    }

    int area_x = 0;
    int area_w = 0;
    term_area(a, &area_x, &area_w);
    if (ev->button.down) {
        /* Tab bar: select a tab (middle click closes it). */
        if (iy < tab_bar_height(a) && ix >= area_x && ix < area_x + area_w) {
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
    bool left = (ev->motion.buttons & 1u) != 0;
    if (sidebar_captures(&a->sb) || a->sb.pressed || a->sb.resizing) {
        (void)sidebar_motion(&a->sb, ev->motion.x, ev->motion.y, left);
        if (sidebar_take_layout_change(&a->sb)) {
            relayout(a);
        }
        a->need_render = true;
        return;
    }
    if (a->drag_split == nullptr && a->buttons_held == 0) {
        int old_hover = a->sb.hover;
        bool in = sidebar_motion(&a->sb, ev->motion.x, ev->motion.y, left);
        if (old_hover != a->sb.hover) {
            a->need_render = true;
        }
        if (in || sidebar_on_edge(&a->sb, ev->motion.x, ev->motion.y)) {
            plat_set_pointer(a->plat, TE_POINTER_DEFAULT);
            if (in) a->need_render = true;   /* toolbar hover */
            return;
        }
    }
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
    if (sidebar_wheel(&a->sb, ev->wheel.x, ev->wheel.y, ev->wheel.dy)) {
        a->need_render = true;
        return;
    }
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
        a->sb.pressed = false;     /* a release we will never see */
        a->sb.dragging = false;
        a->sb.resizing = false;
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
            } else if (load_ui_font(a) != TE_OK) {
                LOG_W("could not reload the sidebar font for the new scale");
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
    case TE_EV_DROP:
        if (ev->drop.path != nullptr) {
            te_pane *fp = focus_pane(a);
            if (fp != nullptr && fp->sess != nullptr && fp->sess->s.type == TE_SESS_SSH) {
                if (!(a->sb.visible && a->sb.tab == TE_SB_FILES)) {
                    sidebar_show_tab(&a->sb, TE_SB_FILES);
                    if (sidebar_take_layout_change(&a->sb)) relayout(a);
                }
                update_files(a);
                files_drop(&a->sb.files, ev->drop.path);
            } else {
                show_toast(a, "Drop files onto an SSH session to upload them");
            }
            a->need_render = true;
        }
        break;
    case TE_EV_DIALOG:
        files_dialog_result(&a->sb.files, ev->dialog.request, ev->dialog.paths, ev->dialog.n, ev->dialog.failed,
                            ev->dialog.error);
        a->need_render = true;
        break;
    case TE_EV_FD_READY:
        for (size_t i = 0; i < MAX_PANES; i++) {
            if (a->remotes[i].r != nullptr && remote_fd_ready(a->remotes[i].r, ev->fd.tag)) {
                a->need_render = true;
                return;
            }
        }
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
            } else if (p->sess != nullptr && p->status != 0 && a->cfg.reconnect != TE_RECONNECT_NEVER) {
                on_disconnect(a, p);   /* a session dropped: keep the pane and offer to reconnect */
            } else if (!a->cli.hold) {
                close_pane(a, p);   /* the shell exited (or the user logged out): its pane goes away */
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
        .opacity = a->opacity,
        .transparent_cells = a->cfg.transparent_cells,
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
    renderer_begin(a->ren, a->width, a->height, a->theme.bg, a->opacity);
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
    if (a->sb_ready) {
        const te_theme *t = &a->theme;
        te_ui_colors uc = {
            .bg = ui_mix(t->bg, 0x000000, 0.18f),
            .bg_alt = ui_mix(t->bg, t->fg, 0.07f),
            .fg = t->fg,
            .muted = ui_mix(t->fg, t->bg, 0.45f),
            .accent = t->cursor,
            .sel_bg = t->selection_bg,
            .sel_fg = t->selection_fg,
            .hover = ui_mix(t->bg, t->fg, 0.12f),
            .border = ui_mix(t->bg, t->fg, 0.18f),
            .danger = t->ansi[1],
        };
        te_painter painter;
        renderer_painter(a->ren, a->scale, &painter);
        layout_sidebar(a);
        renderer_ui_begin(a->ren);
        sidebar_draw(&a->sb, &painter, &uc);
        renderer_ui_end(a->ren);
    }
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
    for (size_t i = 0; i < MAX_PANES; i++) {
        remote_destroy(&a->remotes[i].r);   /* before the platform goes: it unwatches */
    }
    layout_destroy(&a->layout);
    if (a->sb_ready) {
        sidebar_destroy(&a->sb);
        a->sb_ready = false;
    }
    url_destroy(&a->urls);
    watch_destroy(&a->watch);
    TE_FREE(a->pending_paste);
    spawn_reap();
    renderer_destroy(&a->ren);
    font_destroy(&a->ui_font);
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
        int ai = service_anti_idle(a, now);
        if (ai >= 0) {
            timeout = min_timeout(timeout, ai);
        }
        for (size_t i = 0; i < np; i++) {
            te_pane *rp = all[i];
            if (rp->disconnected && rp->reconnect_at_ms != 0) {
                if (now >= rp->reconnect_at_ms) {
                    reconnect_pane(a, rp);
                } else {
                    timeout = min_timeout(timeout, (int)(rp->reconnect_at_ms - now));
                }
            }
        }
        if (a->toast_until_ms > now) {
            timeout = min_timeout(timeout, (int)(a->toast_until_ms - now) + 1);
        } else if (a->toast_until_ms != 0) {
            a->toast_until_ms = 0;
            a->need_render = true;
        }
        for (size_t i = 0; i < MAX_PANES; i++) {
            if (a->remotes[i].r != nullptr) {
                remote_service(a->remotes[i].r);
                if (remote_busy(a->remotes[i].r)) {
                    timeout = min_timeout(timeout, 10);
                }
            }
        }
        te_event ev = { 0 };
        if (plat_wait_event(a->plat, &ev, timeout)) {
            handle_event(a, &ev);
            plat_event_free(&ev);
            while (a->running && plat_wait_event(a->plat, &ev, 0)) {
                handle_event(a, &ev);
                plat_event_free(&ev);
            }
        }
        update_files(a);
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
    os_harden_process();   /* drop the current dir from the DLL/exe search order (Windows) */
#ifndef _WIN32
    /* Helper modes: the built-in Telnet and serial clients run inside a pane
     * (on Windows they are feterm-helper.exe, a console program). */
    if (argc >= 2 && strcmp(argv[1], "--telnet-helper") == 0) {
        return telnet_helper_main(argc - 2, argv + 2);
    }
    if (argc >= 2 && strcmp(argv[1], "--serial-helper") == 0) {
        return serial_helper_main(argc - 2, argv + 2);
    }
#endif
    static app a;   /* large (sidebar state); zero-initialised */
#ifndef _WIN32
    a.watch.fd = -1;   /* the Windows watch is all nullptrs when unused */
#endif
    a.watch_tag = -1;
    int rc = parse_cli(argc, argv, &a.cli);
    if (rc != 0) {
        return rc > 0 ? EXIT_SUCCESS : EXIT_FAILURE;
    }
    te_log_set_level(a.cli.verbose ? TE_LOG_DEBUG : TE_LOG_INFO);
#ifdef SIGPIPE
    (void)signal(SIGPIPE, SIG_IGN);
#endif

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
    };
    err = plat_create(&a.plat, &po);
    if (err != TE_OK) {
        LOG_E("cannot open a window: %s", te_strerror(err));
        goto out;
    }
    a.opacity = 0.0f;   /* forces the first update_opacity() to report a fallback */
    update_opacity(&a);
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
    if (load_ui_font(&a) != TE_OK) {
        LOG_W("could not load the sidebar font; it will zoom with the terminal");
    }

    /* Sessions: the sidebar, this program's path (for the helpers) and a private
     * directory for SSH connection sharing. Each is optional. */
    if (launch_self_exe(a.self_exe, sizeof a.self_exe) != TE_OK) {
        a.self_exe[0] = '\0';
        LOG_W("cannot find this program's path: Telnet and serial sessions are unavailable");
    }
    if (launch_control_dir(a.control_dir, sizeof a.control_dir) != TE_OK) {
        a.control_dir[0] = '\0';
        LOG_D("no private runtime directory: SSH connection sharing disabled");
    }
    {
        char cfgdir[TE_PATH_MAX] = { 0 };
        char root[TE_PATH_MAX] = { 0 };
        bool have_root = paths_config_dir(cfgdir, sizeof cfgdir) == TE_OK &&
                         str_fmt(root, sizeof root, "%s/sessions", cfgdir) == TE_OK;
        te_sidebar_host sh = { .user = &a, .connect = host_sb_connect, .toast = host_sb_toast, .copy = host_sb_copy };
        te_files_host fh = { .user = &a, .dialog = host_files_dialog, .toast = host_sb_toast, .copy = host_sb_copy };
        sidebar_init(&a.sb, have_root ? root : nullptr, &sh, &fh);
        sidebar_configure(&a.sb, a.cfg.sidebar_visible, a.cfg.sidebar_right, a.cfg.sidebar_width, true);
        a.sb_ready = true;
    }
    apply_files_config(&a);
#ifdef _WIN32
    {
        /* No runtime directory here: %LOCALAPPDATA%, which only this user can read. */
        char base[TE_PATH_MAX] = { 0 };
        char app_dir[TE_PATH_MAX] = { 0 };
        if (os_local_data_base(base, sizeof base) != TE_OK ||
            str_fmt(app_dir, sizeof app_dir, "%s/%s", base, APP_NAME) != TE_OK ||
            (mkdir(app_dir, 0700) != 0 && errno != EEXIST) ||
            str_fmt(a.edit_root, sizeof a.edit_root, "%s/edit", app_dir) != TE_OK ||
            (mkdir(a.edit_root, 0700) != 0 && errno != EEXIST)) {
            a.edit_root[0] = '\0';   /* no private folder: edit-locally is unavailable */
        }
    }
#else
    if (a.control_dir[0] != '\0' &&
        str_fmt(a.edit_root, sizeof a.edit_root, "%s/edit", a.control_dir) == TE_OK &&
        mkdir(a.edit_root, 0700) != 0 && errno != EEXIST) {
        a.edit_root[0] = '\0';   /* no private folder: edit-locally is unavailable */
    }
#endif

    const te_font_metrics *m = font_metrics(a.font);
    int pad = padding_px(&a);
    a.width = (int)a.cfg.cols * m->cell_w + 2 * pad;
    a.height = (int)a.cfg.rows * m->cell_h + 2 * pad;
    layout_sidebar(&a);
    a.width += (int)sidebar_width(&a.sb);   /* the terminal keeps its configured size */
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
#ifdef _WIN32
    te_err wrc = watch_init(&a.watch, a.cli.config_path) == TE_OK
                     ? plat_watch_handle(a.plat, watch_handle(&a.watch), &a.watch_tag)
                     : TE_ERR_NOTFOUND;
    if (wrc != TE_OK && wrc != TE_ERR_NOTFOUND) {
#else
    if (watch_init(&a.watch, a.cli.config_path) == TE_OK &&
        plat_watch_fd(a.plat, a.watch.fd, &a.watch_tag) != TE_OK) {
#endif
        a.watch_tag = -1;
        watch_destroy(&a.watch);
    }

    plat_show(a.plat);
    a.running = true;
    a.focused = true;
    a.cursor_on = true;
    a.last_input_ms = plat_ticks_ms();
    apply_size(&a);
    if (a.cli.exec_argv == nullptr) {
        show_banner(&a, first);   /* sized already; not before a -e command's own output */
    }
    run(&a);
    /* `-e cmd` passes the command's exit status on. */
    exit_code = a.exit_status;

out:
    app_destroy(&a);
    return exit_code;
}
