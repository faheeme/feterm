#include "ui/sidebar.h"

#include <string.h>
#include <strings.h>

#include "util/log.h"
#include "util/mem.h"
#include "util/str.h"

#define MIN_WIDTH       180.0f
#define MAX_WIDTH       640.0f
#define EDGE_SLOP       5.0f
#define DRAG_START_PX   6.0f
#define MAX_FOLDERS     512

/* Menu items */
enum {
    M_CONNECT = 1, M_CONNECT_SPLIT, M_EDIT, M_DUPLICATE, M_RENAME, M_DELETE, M_COPY_HOST,
    M_NEW_SESSION, M_NEW_FOLDER, M_FOLDER_SETTINGS, M_RENAME_FOLDER, M_DELETE_FOLDER,
    M_QUICK, M_EXPAND_ALL, M_COLLAPSE_ALL, M_RELOAD,
};

/* Dialog purposes */
enum {
    F_SESSION_NEW = 1, F_SESSION_EDIT, F_SESSION_RENAME, F_FOLDER_NEW, F_FOLDER_RENAME, F_FOLDER_SETTINGS,
    F_DELETE_SESSION, F_DELETE_FOLDER, F_QUICK,
};

/* Dialog fields */
enum {
    FD_NAME = 1, FD_FOLDER, FD_TYPE, FD_HOST, FD_PORT, FD_USER, FD_KEY, FD_JUMP, FD_SHARE, FD_DEVICE, FD_BAUD,
    FD_BAUD_CUSTOM, FD_DATA, FD_PARITY, FD_STOP, FD_FLOW, FD_COMMAND, FD_DIR, FD_COLOR, FD_ANTI_IDLE,
    FD_HIGHLIGHT, FD_TARGET, FD_SAVE,
};

static const char *const type_choices[] = { "SSH", "Telnet", "Serial console", "Local shell" };
static const char *const quick_types[] = { "SSH", "Telnet", "Serial console" };
static const char *const data_choices[] = { "8", "7", "6", "5" };
static const char *const parity_choices[] = { "None", "Even", "Odd" };
static const char *const stop_choices[] = { "1", "2" };
static const char *const flow_choices[] = { "None", "RTS/CTS (hardware)", "XON/XOFF (software)" };
/* Index 0 is "inherit"; the rest follow te_tag_color from TE_TAG_NONE. */
static const char *const color_choices[] = {
    "Inherit from folder", "None", "Red", "Orange", "Yellow", "Green", "Cyan", "Blue", "Purple", "Pink", "Grey",
};
static const char *const hl_choices[] = { "Auto-detect", "Cisco", "Arista", "Juniper", "Network (common)", "Off" };
static const char *const hl_values[] = { "auto", "cisco", "arista", "juniper", "network-common", "off" };

static char baud_text[24][16];
static const char *baud_choices[25];
static size_t nbaud_choices;

static te_tag_color color_from_choice(size_t i)
{
    return i == 0 || i >= TE_ARRAY_LEN(color_choices) ? TE_TAG_INHERIT : (te_tag_color)(TE_TAG_NONE + (int)i - 1);
}

static size_t choice_from_color(te_tag_color c)
{
    return c == TE_TAG_INHERIT || (unsigned)c >= TE_TAG_INHERIT ? 0 : (size_t)c + 1;
}

static void init_baud_choices(void)
{
    if (nbaud_choices > 0) {
        return;
    }
    const uint32_t *b = nullptr;
    size_t n = serial_standard_bauds(&b);
    for (size_t i = 0; i < n && i < TE_ARRAY_LEN(baud_text) - 1; i++) {
        (void)str_fmt(baud_text[i], sizeof baud_text[i], "%u", (unsigned)b[i]);
        baud_choices[nbaud_choices++] = baud_text[i];
    }
    baud_choices[nbaud_choices++] = "Custom...";
}

/* ------------------------------------------------------------------------- */
/* Geometry */

static float header_h(const te_sidebar *sb) { return (float)(int)(sb->cell_h * 1.6f + 0.5f); }
static float filter_h(const te_sidebar *sb) { return sb->cell_h * 1.8f; }
static float toolbar_h(const te_sidebar *sb) { return sb->cell_h * 1.8f; }
static float row_h(const te_sidebar *sb) { return sb->cell_h * 1.4f; }
static float strip_w(const te_sidebar *sb) { return sb->cell_w * 3.0f; }
static float tree_top(const te_sidebar *sb) { return header_h(sb) + filter_h(sb); }
static float tree_bottom(const te_sidebar *sb) { return sb->win_h - toolbar_h(sb); }

static size_t rows_fit(const te_sidebar *sb)
{
    float h = tree_bottom(sb) - tree_top(sb);
    return h > 0.0f && row_h(sb) > 0.0f ? (size_t)(h / row_h(sb)) : 0;
}

float sidebar_width(const te_sidebar *sb)
{
    return sb->w;
}

float sidebar_term_x(const te_sidebar *sb)
{
    return sb->right ? 0.0f : sb->w;
}

static float edge_x(const te_sidebar *sb)
{
    return sb->right ? sb->x : sb->x + sb->w;
}

void sidebar_layout(te_sidebar *sb, const te_painter *p, float win_w, float win_h)
{
    sb->cell_w = p->cell_w > 0.0f ? p->cell_w : 8.0f;
    sb->cell_h = p->cell_h > 0.0f ? p->cell_h : 16.0f;
    sb->scale = p->scale > 0.0f ? p->scale : 1.0f;
    sb->win_w = win_w;
    sb->win_h = win_h;
    float w = sb->visible ? sb->width_logical * sb->scale : strip_w(sb);
    float maxw = win_w * 0.6f;
    if (w > maxw) w = maxw;
    if (w < 0.0f) w = 0.0f;
    sb->w = (float)(int)w;
    sb->x = sb->right ? win_w - sb->w : 0.0f;
    if (sb->form.open) {
        form_layout(&sb->form, p, win_w, win_h);
    }
}

/* Dialog geometry is needed for clicks as soon as a dialog opens. */
static void relayout_form(te_sidebar *sb)
{
    if (sb->form.open) {
        te_painter p = { .cell_w = sb->cell_w, .cell_h = sb->cell_h, .scale = sb->scale };
        form_layout(&sb->form, &p, sb->win_w, sb->win_h);
    }
}

bool sidebar_take_layout_change(te_sidebar *sb)
{
    bool c = sb->layout_changed;
    sb->layout_changed = false;
    return c;
}

bool sidebar_contains(const te_sidebar *sb, float x, float y)
{
    return y >= 0.0f && y < sb->win_h && x >= sb->x && x < sb->x + sb->w;
}

bool sidebar_on_edge(const te_sidebar *sb, float x, float y)
{
    float e = edge_x(sb);
    return sb->visible && y >= 0.0f && y < sb->win_h && x >= e - EDGE_SLOP * sb->scale && x <= e + EDGE_SLOP * sb->scale;
}

bool sidebar_captures(const te_sidebar *sb)
{
    return sb->menu.open || sb->form.open;
}

bool sidebar_focused(const te_sidebar *sb)
{
    return sb->focused || sidebar_captures(sb);
}

/* ------------------------------------------------------------------------- */
/* Tree */

static const te_store_node *node(const te_sidebar *sb, int i)
{
    return i >= 0 && (size_t)i < sb->store.len ? &sb->store.nodes[i] : nullptr;
}

static bool contains_ci(const char *hay, const char *needle)
{
    size_t n = strlen(needle);
    if (n == 0) {
        return true;
    }
    for (const char *h = hay; *h != '\0'; h++) {
        if (strncasecmp(h, needle, n) == 0) {
            return true;
        }
    }
    return false;
}

static void key_of(const te_sidebar *sb, int i, char *out, size_t outsz)
{
    const te_store_node *n = node(sb, i);
    out[0] = '\0';
    if (n == nullptr) {
        return;
    }
    if (n->folder) {
        (void)str_fmt(out, outsz, "D:%s", n->path);
    } else {
        (void)str_fmt(out, outsz, "S:%s/%s", n->path, n->name);
    }
}

static int find_key(const te_sidebar *sb, const char *key)
{
    char k[sizeof sb->sel_key];
    for (size_t i = 0; i < sb->store.len; i++) {
        key_of(sb, (int)i, k, sizeof k);
        if (strcmp(k, key) == 0) {
            return (int)i;
        }
    }
    return -1;
}

static bool push_vis(te_sidebar *sb, int idx)
{
    if (sb->nvis == sb->viscap) {
        size_t ncap = sb->viscap == 0 ? 64 : sb->viscap * 2;
        void *tmp = sb->vis;
        if (te_realloc_array(&tmp, sb->viscap, ncap, sizeof *sb->vis) != TE_OK) {
            return false;
        }
        sb->vis = tmp;
        sb->viscap = ncap;
    }
    sb->vis[sb->nvis++] = idx;
    return true;
}

void sidebar_rebuild(te_sidebar *sb)
{
    sb->nvis = 0;
    size_t len = sb->store.len;
    if (sb->filter[0] == '\0') {
        int hide_below = -1;   /* depth of a collapsed folder whose contents are hidden */
        for (size_t i = 0; i < len; i++) {
            const te_store_node *n = &sb->store.nodes[i];
            if (hide_below >= 0 && (int)n->depth > hide_below) {
                continue;
            }
            hide_below = -1;
            if (!push_vis(sb, (int)i)) {
                break;
            }
            if (n->folder && !n->expanded) {
                hide_below = n->depth;
            }
        }
    } else if (len > 0) {
        /* Matching sessions and the folders leading to them. */
        bool *keep = te_calloc(len, sizeof *keep);
        if (keep != nullptr) {
            for (size_t i = 0; i < len; i++) {
                const te_store_node *n = &sb->store.nodes[i];
                if (n->folder || n->sess == nullptr) {
                    continue;
                }
                if (contains_ci(n->name, sb->filter) || contains_ci(n->sess->host, sb->filter) ||
                    contains_ci(n->path, sb->filter)) {
                    keep[i] = true;
                    for (int p = n->parent; p >= 0 && (size_t)p < len; p = sb->store.nodes[p].parent) {
                        keep[p] = true;
                    }
                }
            }
            for (size_t i = 0; i < len; i++) {
                if (keep[i] && !push_vis(sb, (int)i)) {
                    break;
                }
            }
            TE_FREE(keep);
        }
    }
    if (sb->sel_key[0] != '\0') {
        sb->sel = find_key(sb, sb->sel_key);
    }
    size_t fit = rows_fit(sb);
    if (sb->nvis <= fit) {
        sb->scroll = 0;
    } else if (sb->scroll > sb->nvis - fit) {
        sb->scroll = sb->nvis - fit;
    }
}

static int vis_pos(const te_sidebar *sb, int idx)
{
    for (size_t i = 0; i < sb->nvis; i++) {
        if (sb->vis[i] == idx) {
            return (int)i;
        }
    }
    return -1;
}

static void select_node(te_sidebar *sb, int idx)
{
    sb->sel = idx;
    key_of(sb, idx, sb->sel_key, sizeof sb->sel_key);
    int pos = vis_pos(sb, idx);
    size_t fit = rows_fit(sb);
    if (pos >= 0 && fit > 0) {
        size_t p = (size_t)pos;
        if (p < sb->scroll) {
            sb->scroll = p;
        } else if (p >= sb->scroll + fit) {
            sb->scroll = p + 1 - fit;
        }
    }
}

/* Node under a point in the tree, or -1 (empty space). */
static int node_at(const te_sidebar *sb, float x, float y)
{
    (void)x;
    if (y < tree_top(sb) || y >= tree_bottom(sb)) {
        return -1;
    }
    size_t row = (size_t)((y - tree_top(sb)) / row_h(sb)) + sb->scroll;
    return row < sb->nvis ? sb->vis[row] : -1;
}

static void rebuild_folder_choices(te_sidebar *sb)
{
    TE_FREE(sb->folder_paths);
    TE_FREE(sb->folder_choices);
    sb->nfolders = 0;
    size_t count = 1;
    for (size_t i = 0; i < sb->store.len && count < MAX_FOLDERS; i++) {
        count += sb->store.nodes[i].folder;
    }
    sb->folder_paths = te_calloc(count, sizeof *sb->folder_paths);
    sb->folder_choices = te_calloc(count, sizeof *sb->folder_choices);
    if (sb->folder_paths == nullptr || sb->folder_choices == nullptr) {
        TE_FREE(sb->folder_paths);
        TE_FREE(sb->folder_choices);
        return;
    }
    (void)str_copy(sb->folder_paths[0], sizeof sb->folder_paths[0], "");
    sb->folder_choices[0] = "(top level)";
    sb->nfolders = 1;
    for (size_t i = 0; i < sb->store.len && sb->nfolders < count; i++) {
        if (sb->store.nodes[i].folder) {
            (void)str_copy(sb->folder_paths[sb->nfolders], sizeof sb->folder_paths[0], sb->store.nodes[i].path);
            sb->folder_choices[sb->nfolders] = sb->folder_paths[sb->nfolders];
            sb->nfolders++;
        }
    }
}

static size_t folder_choice_of(const te_sidebar *sb, const char *path)
{
    for (size_t i = 0; i < sb->nfolders; i++) {
        if (strcmp(sb->folder_paths[i], path) == 0) {
            return i;
        }
    }
    return 0;
}

static void after_store_change(te_sidebar *sb)
{
    rebuild_folder_choices(sb);
    sidebar_rebuild(sb);
}

/* ------------------------------------------------------------------------- */
/* Lifecycle */

void sidebar_init(te_sidebar *sb, const char *sessions_root, const te_sidebar_host *host)
{
    memset(sb, 0, sizeof *sb);
    sb->host = *host;
    sb->visible = true;
    sb->width_logical = 280.0f;
    sb->sel = -1;
    sb->hover = -1;
    sb->press_node = -1;
    sb->drop_folder = -2;
    sb->menu_node = -1;
    sb->form_node = -1;
    sb->cell_w = 8.0f;
    sb->cell_h = 16.0f;
    sb->scale = 1.0f;
    init_baud_choices();
    sb->store_ok = sessions_root != nullptr && store_open(&sb->store, sessions_root) == TE_OK;
    if (!sb->store_ok) {
        LOG_W("sessions folder unavailable; the session manager is read-only and empty");
        memset(&sb->store, 0, sizeof sb->store);
    }
    after_store_change(sb);
}

void sidebar_destroy(te_sidebar *sb)
{
    store_close(&sb->store);
    TE_FREE(sb->vis);
    sb->nvis = 0;
    sb->viscap = 0;
    TE_FREE(sb->folder_paths);
    TE_FREE(sb->folder_choices);
    sb->nfolders = 0;
}

void sidebar_configure(te_sidebar *sb, bool visible, bool right, float width_logical, bool first_time)
{
    if (first_time) {
        sb->visible = visible;
    }
    float w = TE_CLAMP(width_logical, MIN_WIDTH, MAX_WIDTH);
    if (right != sb->right || w != sb->width_logical || first_time) {
        sb->layout_changed = true;
    }
    sb->right = right;
    sb->width_logical = w;
}

void sidebar_reload(te_sidebar *sb)
{
    if (sb->store_ok && store_reload(&sb->store) == TE_OK) {
        after_store_change(sb);
    }
}

void sidebar_toggle(te_sidebar *sb)
{
    sb->visible = !sb->visible;
    if (!sb->visible) {
        sb->focused = false;
        menu_close(&sb->menu);
    }
    sb->layout_changed = true;
}

void sidebar_show_tab(te_sidebar *sb, te_sb_tab tab)
{
    if (!sb->visible) {
        sb->visible = true;
        sb->layout_changed = true;
    }
    sb->tab = tab;
    sb->focused = true;
    if (sb->sel < 0 && sb->nvis > 0) {
        select_node(sb, sb->vis[0]);
    }
}

void sidebar_blur(te_sidebar *sb)
{
    sb->focused = false;
}

/* ------------------------------------------------------------------------- */
/* Dialogs */

static void toast(const te_sidebar *sb, const char *msg)
{
    if (sb->host.toast != nullptr) {
        sb->host.toast(sb->host.user, msg);
    }
}

static void update_session_form(te_sidebar *sb)
{
    te_form *f = &sb->form;
    size_t t = form_choice(f, FD_TYPE);
    if (sb->form.purpose == F_QUICK) {
        return;
    }
    bool net = t == TE_SESS_SSH || t == TE_SESS_TELNET;
    bool ssh = t == TE_SESS_SSH;
    bool ser = t == TE_SESS_SERIAL;
    bool loc = t == TE_SESS_LOCAL;
    form_show(f, FD_HOST, net);
    form_show(f, FD_PORT, net);
    form_show(f, FD_USER, ssh);
    form_show(f, FD_KEY, ssh);
    form_show(f, FD_JUMP, ssh);
    form_show(f, FD_SHARE, ssh);
    form_show(f, FD_DEVICE, ser);
    form_show(f, FD_BAUD, ser);
    form_show(f, FD_BAUD_CUSTOM, ser && form_choice(f, FD_BAUD) + 1 == nbaud_choices);
    form_show(f, FD_DATA, ser);
    form_show(f, FD_PARITY, ser);
    form_show(f, FD_STOP, ser);
    form_show(f, FD_FLOW, ser);
    form_show(f, FD_COMMAND, loc);
    form_show(f, FD_DIR, loc);
    te_form_field *port = form_get(f, FD_PORT);
    if (port != nullptr) {
        (void)str_copy(port->placeholder, sizeof port->placeholder, ssh ? "22" : "23");
    }
}

static size_t baud_choice_of(uint32_t baud, bool *custom)
{
    const uint32_t *b = nullptr;
    size_t n = serial_standard_bauds(&b);
    for (size_t i = 0; i < n && i + 1 < nbaud_choices; i++) {
        if (b[i] == baud) {
            *custom = false;
            return i;
        }
    }
    *custom = true;
    return nbaud_choices - 1;
}

static size_t index_of(const char *const *list, size_t n, const char *v)
{
    for (size_t i = 0; i < n; i++) {
        if (strcmp(list[i], v) == 0) {
            return i;
        }
    }
    return 0;
}

static void open_session_form(te_sidebar *sb, int purpose, const te_session *s, const char *title)
{
    te_form *f = &sb->form;
    form_init(f, purpose, title, purpose == F_SESSION_EDIT ? "Save" : "Create");
    sb->form_session = *s;
    char num[32] = { 0 };
    te_form_field *fl = form_add(f, FD_NAME, TE_FIELD_TEXT, "Name", s->name);
    if (fl != nullptr) {
        fl->maxlen = TE_SESS_NAME_MAX;
        (void)str_copy(fl->placeholder, sizeof fl->placeholder, "e.g. core-sw1");
    }
    form_set_choices(form_add(f, FD_FOLDER, TE_FIELD_CHOICE, "Folder", nullptr), sb->folder_choices, sb->nfolders,
                     folder_choice_of(sb, s->folder));
    form_set_choices(form_add(f, FD_TYPE, TE_FIELD_CHOICE, "Type", nullptr), type_choices, TE_ARRAY_LEN(type_choices),
                     (size_t)s->type);
    fl = form_add(f, FD_HOST, TE_FIELD_TEXT, "Host", s->host);
    if (fl != nullptr) {
        fl->maxlen = TE_SESS_HOST_MAX;
        (void)str_copy(fl->placeholder, sizeof fl->placeholder, "name or IP address");
    }
    if (s->port != 0) (void)str_fmt(num, sizeof num, "%u", (unsigned)s->port);
    fl = form_add(f, FD_PORT, TE_FIELD_NUMBER, "Port", num);
    if (fl != nullptr) fl->maxlen = 5;
    fl = form_add(f, FD_USER, TE_FIELD_TEXT, "User", s->user);
    if (fl != nullptr) {
        fl->maxlen = TE_SESS_USER_MAX;
        (void)str_copy(fl->placeholder, sizeof fl->placeholder, "from folder or ~/.ssh/config");
    }
    fl = form_add(f, FD_KEY, TE_FIELD_TEXT, "Key file", s->identity);
    if (fl != nullptr) (void)str_copy(fl->placeholder, sizeof fl->placeholder, "optional, e.g. ~/.ssh/id_ed25519");
    fl = form_add(f, FD_JUMP, TE_FIELD_TEXT, "Jump host", s->jump);
    if (fl != nullptr) {
        fl->maxlen = TE_SESS_HOST_MAX;
        (void)str_copy(fl->placeholder, sizeof fl->placeholder, "optional, user@bastion");
    }
    fl = form_add(f, FD_SHARE, TE_FIELD_CHECK, "Share login", nullptr);
    if (fl != nullptr) fl->checked = s->share_conn;
    fl = form_add(f, FD_DEVICE, TE_FIELD_TEXT, "Device", s->serial.device[0] != '\0' ? s->serial.device : "/dev/ttyUSB0");
    bool custom = false;
    size_t bc = baud_choice_of(s->serial.baud, &custom);
    form_set_choices(form_add(f, FD_BAUD, TE_FIELD_CHOICE, "Baud rate", nullptr), baud_choices, nbaud_choices, bc);
    num[0] = '\0';
    if (custom) (void)str_fmt(num, sizeof num, "%u", (unsigned)s->serial.baud);
    fl = form_add(f, FD_BAUD_CUSTOM, TE_FIELD_NUMBER, "Custom baud", num);
    if (fl != nullptr) {
        fl->maxlen = 8;
        (void)str_copy(fl->placeholder, sizeof fl->placeholder, "any rate, e.g. 250000");
    }
    form_set_choices(form_add(f, FD_DATA, TE_FIELD_CHOICE, "Data bits", nullptr), data_choices,
                     TE_ARRAY_LEN(data_choices), (size_t)(8 - TE_CLAMP(s->serial.data_bits, 5, 8)));
    form_set_choices(form_add(f, FD_PARITY, TE_FIELD_CHOICE, "Parity", nullptr), parity_choices,
                     TE_ARRAY_LEN(parity_choices), (size_t)s->serial.parity);
    form_set_choices(form_add(f, FD_STOP, TE_FIELD_CHOICE, "Stop bits", nullptr), stop_choices,
                     TE_ARRAY_LEN(stop_choices), s->serial.stop_bits == 2 ? 1 : 0);
    form_set_choices(form_add(f, FD_FLOW, TE_FIELD_CHOICE, "Flow control", nullptr), flow_choices,
                     TE_ARRAY_LEN(flow_choices), (size_t)s->serial.flow);
    fl = form_add(f, FD_COMMAND, TE_FIELD_TEXT, "Command", s->command);
    if (fl != nullptr) (void)str_copy(fl->placeholder, sizeof fl->placeholder, "empty: your login shell");
    fl = form_add(f, FD_DIR, TE_FIELD_TEXT, "Directory", s->directory);
    if (fl != nullptr) (void)str_copy(fl->placeholder, sizeof fl->placeholder, "optional, e.g. ~/projects");
    form_set_choices(form_add(f, FD_COLOR, TE_FIELD_CHOICE, "Tab colour", nullptr), color_choices,
                     TE_ARRAY_LEN(color_choices), choice_from_color(s->color));
    num[0] = '\0';
    if (s->anti_idle != TE_ANTI_IDLE_INHERIT) (void)str_fmt(num, sizeof num, "%d", (int)s->anti_idle);
    fl = form_add(f, FD_ANTI_IDLE, TE_FIELD_NUMBER, "Anti-idle (s)", num);
    if (fl != nullptr) {
        fl->maxlen = 5;
        (void)str_copy(fl->placeholder, sizeof fl->placeholder, "default; 0 = off");
    }
    form_set_choices(form_add(f, FD_HIGHLIGHT, TE_FIELD_CHOICE, "Highlighting", nullptr), hl_choices,
                     TE_ARRAY_LEN(hl_choices),
                     s->highlight[0] == '\0' ? 0 : index_of(hl_values, TE_ARRAY_LEN(hl_values), s->highlight));
    update_session_form(sb);
    form_focus_first(f);
}

/* Read the session dialog back into a session; false (with the dialog's error set) if invalid. */
static bool session_from_form(te_sidebar *sb, te_session *s)
{
    te_form *f = &sb->form;
    *s = sb->form_session;
    long v = 0;
    te_sess_type type = (te_sess_type)form_choice(f, FD_TYPE);
    if ((unsigned)type >= TE_SESS_TYPE_COUNT) {
        type = TE_SESS_SSH;
    }
    s->type = type;
    if (str_copy(s->name, sizeof s->name, form_value(f, FD_NAME)) != TE_OK) {
        form_set_error(f, "Name is too long");
        return false;
    }
    size_t fi = form_choice(f, FD_FOLDER);
    (void)str_copy(s->folder, sizeof s->folder, fi < sb->nfolders ? sb->folder_paths[fi] : "");
    (void)str_copy(s->host, sizeof s->host, form_value(f, FD_HOST));
    const char *port = form_value(f, FD_PORT);
    s->port = 0;
    if (port[0] != '\0') {
        if (!str_to_long(port, 1, 65535, &v)) {
            form_set_error(f, "Port must be 1 to 65535");
            return false;
        }
        s->port = (uint16_t)v;
    }
    (void)str_copy(s->user, sizeof s->user, form_value(f, FD_USER));
    (void)str_copy(s->identity, sizeof s->identity, form_value(f, FD_KEY));
    (void)str_copy(s->jump, sizeof s->jump, form_value(f, FD_JUMP));
    s->share_conn = form_checked(f, FD_SHARE);
    (void)str_copy(s->serial.device, sizeof s->serial.device, form_value(f, FD_DEVICE));
    size_t bc = form_choice(f, FD_BAUD);
    if (bc + 1 == nbaud_choices) {
        if (!str_to_long(form_value(f, FD_BAUD_CUSTOM), 50, 16000000, &v)) {
            form_set_error(f, "Custom baud rate must be 50 to 16000000");
            return false;
        }
        s->serial.baud = (uint32_t)v;
    } else {
        const uint32_t *b = nullptr;
        size_t n = serial_standard_bauds(&b);
        s->serial.baud = bc < n ? b[bc] : 9600;
    }
    s->serial.data_bits = (uint8_t)(8 - TE_MIN(form_choice(f, FD_DATA), (size_t)3));
    s->serial.parity = (te_parity)TE_MIN(form_choice(f, FD_PARITY), (size_t)TE_PARITY_ODD);
    s->serial.stop_bits = form_choice(f, FD_STOP) == 1 ? 2 : 1;
    s->serial.flow = (te_flow)TE_MIN(form_choice(f, FD_FLOW), (size_t)TE_FLOW_XONXOFF);
    (void)str_copy(s->command, sizeof s->command, form_value(f, FD_COMMAND));
    (void)str_copy(s->directory, sizeof s->directory, form_value(f, FD_DIR));
    s->color = color_from_choice(form_choice(f, FD_COLOR));
    const char *ai = form_value(f, FD_ANTI_IDLE);
    s->anti_idle = TE_ANTI_IDLE_INHERIT;
    if (ai[0] != '\0') {
        if (!str_to_long(ai, 0, TE_ANTI_IDLE_MAX, &v)) {
            form_set_error(f, "Anti-idle must be 0 (off) to 86400 seconds, or empty for the default");
            return false;
        }
        s->anti_idle = (int32_t)v;
    }
    size_t hc = form_choice(f, FD_HIGHLIGHT);
    (void)str_copy(s->highlight, sizeof s->highlight, hc == 0 ? "" : hl_values[TE_MIN(hc, TE_ARRAY_LEN(hl_values) - 1)]);
    const char *why = nullptr;
    if (!session_validate(s, &why)) {
        form_set_error(f, why);
        return false;
    }
    return true;
}

static void save_error(te_sidebar *sb, te_err rc, const char *what)
{
    char msg[200] = { 0 };
    if (rc == TE_ERR_EXISTS) {
        (void)str_fmt(msg, sizeof msg, "%s already exists here. Choose another name.", what);
    } else {
        (void)str_fmt(msg, sizeof msg, "Could not save: %s", te_strerror(rc));
    }
    form_set_error(&sb->form, msg);
}

static void open_name_form(te_sidebar *sb, int purpose, const char *title, const char *ok, const char *value)
{
    form_init(&sb->form, purpose, title, ok);
    te_form_field *fl = form_add(&sb->form, FD_NAME, TE_FIELD_TEXT, "Name", value);
    if (fl != nullptr) {
        fl->maxlen = TE_SESS_NAME_MAX;
    }
    form_focus_first(&sb->form);
}

static size_t count_sessions_in(const te_sidebar *sb, const char *path)
{
    size_t n = 0;
    size_t plen = strlen(path);
    for (size_t i = 0; i < sb->store.len; i++) {
        const te_store_node *nd = &sb->store.nodes[i];
        if (!nd->folder && strncmp(nd->path, path, plen) == 0 && (nd->path[plen] == '\0' || nd->path[plen] == '/')) {
            n++;
        }
    }
    return n;
}

static void connect_node(te_sidebar *sb, int idx, bool split)
{
    const te_store_node *n = node(sb, idx);
    if (n == nullptr || n->folder || n->sess == nullptr || sb->host.connect == nullptr) {
        return;
    }
    const char *why = nullptr;
    if (!session_validate(n->sess, &why)) {
        char msg[200] = { 0 };
        (void)str_fmt(msg, sizeof msg, "Cannot connect: %s", why);
        toast(sb, msg);
        return;
    }
    te_session_eff eff;
    store_effective(&sb->store, n->sess, &eff);
    sb->host.connect(sb->host.user, n->sess, &eff, split);
}

static void new_session_in(te_sidebar *sb, const char *folder)
{
    te_session s;
    session_defaults(&s, TE_SESS_SSH);
    (void)str_copy(s.folder, sizeof s.folder, folder);
    open_session_form(sb, F_SESSION_NEW, &s, "New session");
}

static void run_menu(te_sidebar *sb, int id)
{
    const te_store_node *n = node(sb, sb->menu_node);
    const char *here = n != nullptr ? n->path : "";
    switch (id) {
    case M_CONNECT:       connect_node(sb, sb->menu_node, false); break;
    case M_CONNECT_SPLIT: connect_node(sb, sb->menu_node, true); break;
    case M_EDIT:
        if (n != nullptr && n->sess != nullptr) {
            open_session_form(sb, F_SESSION_EDIT, n->sess, "Edit session");
            (void)str_copy(sb->form_folder, sizeof sb->form_folder, n->path);
            (void)str_copy(sb->form_name, sizeof sb->form_name, n->name);
        }
        break;
    case M_DUPLICATE:
        if (n != nullptr && n->sess != nullptr) {
            te_session copy = *n->sess;
            char nm[TE_SESS_NAME_MAX + 1] = { 0 };
            size_t keep = ui_text_fit(n->name, TE_SESS_NAME_MAX - 7);
            (void)str_copy_n(nm, sizeof nm, n->name, keep);
            (void)str_append(nm, sizeof nm, " (copy)");
            (void)str_copy(copy.name, sizeof copy.name, nm);
            open_session_form(sb, F_SESSION_NEW, &copy, "Duplicate session");
        }
        break;
    case M_RENAME:
        if (n != nullptr && n->sess != nullptr) {
            open_name_form(sb, F_SESSION_RENAME, "Rename session", "Rename", n->name);
            sb->form_session = *n->sess;
            (void)str_copy(sb->form_folder, sizeof sb->form_folder, n->path);
            (void)str_copy(sb->form_name, sizeof sb->form_name, n->name);
        }
        break;
    case M_DELETE:
        if (n != nullptr && !n->folder) {
            form_init(&sb->form, F_DELETE_SESSION, "Delete session", "Delete");
            (void)str_fmt(sb->form.message, sizeof sb->form.message,
                          "Delete the session \"%s\"? This cannot be undone.", n->name);
            sb->form.danger = true;
            sb->form.focus = 1;   /* Cancel is the safe default */
            (void)str_copy(sb->form_folder, sizeof sb->form_folder, n->path);
            (void)str_copy(sb->form_name, sizeof sb->form_name, n->name);
        }
        break;
    case M_COPY_HOST:
        if (n != nullptr && n->sess != nullptr && sb->host.copy != nullptr) {
            const char *v = n->sess->type == TE_SESS_SERIAL ? n->sess->serial.device : n->sess->host;
            sb->host.copy(sb->host.user, v);
        }
        break;
    case M_NEW_SESSION:
        new_session_in(sb, here);
        break;
    case M_NEW_FOLDER:
        open_name_form(sb, F_FOLDER_NEW, "New folder", "Create", "");
        (void)str_copy(sb->form_folder, sizeof sb->form_folder, here);
        break;
    case M_RENAME_FOLDER:
        if (n != nullptr && n->folder) {
            open_name_form(sb, F_FOLDER_RENAME, "Rename folder", "Rename", n->name);
            (void)str_copy(sb->form_folder, sizeof sb->form_folder, n->path);
        }
        break;
    case M_FOLDER_SETTINGS:
        if (n != nullptr && n->folder) {
            char title[160] = { 0 };
            char num[32] = { 0 };
            (void)str_fmt(title, sizeof title, "Folder settings: %s", n->name);
            form_init(&sb->form, F_FOLDER_SETTINGS, title, "Save");
            (void)str_copy(sb->form.message, sizeof sb->form.message,
                           "Sessions in this folder use these unless they set their own.");
            form_set_choices(form_add(&sb->form, FD_COLOR, TE_FIELD_CHOICE, "Tab colour", nullptr), color_choices,
                             TE_ARRAY_LEN(color_choices), choice_from_color(n->fdef.color));
            if (n->fdef.anti_idle != TE_ANTI_IDLE_INHERIT) (void)str_fmt(num, sizeof num, "%d", (int)n->fdef.anti_idle);
            te_form_field *fl = form_add(&sb->form, FD_ANTI_IDLE, TE_FIELD_NUMBER, "Anti-idle (s)", num);
            if (fl != nullptr) {
                fl->maxlen = 5;
                (void)str_copy(fl->placeholder, sizeof fl->placeholder, "inherit; 0 = off");
            }
            fl = form_add(&sb->form, FD_USER, TE_FIELD_TEXT, "Default user", n->fdef.user);
            if (fl != nullptr) fl->maxlen = TE_SESS_USER_MAX;
            form_focus_first(&sb->form);
            (void)str_copy(sb->form_folder, sizeof sb->form_folder, n->path);
        }
        break;
    case M_DELETE_FOLDER:
        if (n != nullptr && n->folder) {
            size_t count = count_sessions_in(sb, n->path);
            form_init(&sb->form, F_DELETE_FOLDER, "Delete folder", "Delete");
            if (count > 0) {
                (void)str_fmt(sb->form.message, sizeof sb->form.message,
                              "Delete the folder \"%s\" and the %zu session%s in it? This cannot be undone.", n->name,
                              count, count == 1 ? "" : "s");
            } else {
                (void)str_fmt(sb->form.message, sizeof sb->form.message, "Delete the empty folder \"%s\"?", n->name);
            }
            sb->form.danger = true;
            sb->form.focus = 1;
            (void)str_copy(sb->form_folder, sizeof sb->form_folder, n->path);
        }
        break;
    case M_QUICK:        sidebar_quick_connect(sb); break;
    case M_EXPAND_ALL:
    case M_COLLAPSE_ALL:
        for (size_t i = 0; i < sb->store.len; i++) {
            sb->store.nodes[i].expanded = id == M_EXPAND_ALL;
        }
        sidebar_rebuild(sb);
        break;
    case M_RELOAD:
        sidebar_reload(sb);
        toast(sb, "Sessions reloaded");
        break;
    default:
        break;
    }
    relayout_form(sb);
}

void sidebar_quick_connect(te_sidebar *sb)
{
    form_init(&sb->form, F_QUICK, "Quick connect", "Connect");
    form_set_choices(form_add(&sb->form, FD_TYPE, TE_FIELD_CHOICE, "Type", nullptr), quick_types,
                     TE_ARRAY_LEN(quick_types), 0);
    te_form_field *fl = form_add(&sb->form, FD_TARGET, TE_FIELD_TEXT, "Connect to", "");
    if (fl != nullptr) {
        fl->maxlen = 300;
        (void)str_copy(fl->placeholder, sizeof fl->placeholder, "user@host:port  or  /dev/ttyUSB0,115200");
    }
    fl = form_add(&sb->form, FD_SAVE, TE_FIELD_CHECK, "Save session", nullptr);
    if (fl != nullptr) fl->checked = false;
    sb->form.focus = 1;
    relayout_form(sb);
}

bool quick_parse(te_sess_type type, const char *target, te_session *out)
{
    char buf[320] = { 0 };
    if (str_copy(buf, sizeof buf, target) != TE_OK) {
        return false;
    }
    char *t = str_trim(buf);
    session_defaults(out, type);
    if (t[0] == '\0' || strnlen(t, TE_SESS_NAME_MAX + 1) > TE_SESS_NAME_MAX) {
        return false;
    }
    (void)str_copy(out->name, sizeof out->name, t);
    if (type == TE_SESS_SERIAL) {
        char *comma = strchr(t, ',');
        if (comma != nullptr) {
            *comma = '\0';
            if (!serial_parse(comma + 1, &out->serial)) {
                return false;
            }
        } else {
            out->serial.baud = 9600;
        }
        if (str_copy(out->serial.device, sizeof out->serial.device, t) != TE_OK ||
            !session_device_valid(out->serial.device)) {
            return false;
        }
        /* Names cannot hold '/': call it after the device, e.g. "ttyUSB0 115200". */
        const char *base = strrchr(out->serial.device, '/');
        base = base != nullptr ? base + 1 : out->serial.device;
        if (str_fmt(out->name, sizeof out->name, "%s %u", base, (unsigned)out->serial.baud) != TE_OK ||
            !session_name_valid(out->name)) {
            (void)str_copy(out->name, sizeof out->name, "Serial console");
        }
        return true;
    }
    /* [user@]host[:port], where an IPv6 host needs brackets to carry a port. */
    char *host = t;
    char *at = strrchr(t, '@');
    if (at != nullptr && type == TE_SESS_SSH) {
        *at = '\0';
        if (str_copy(out->user, sizeof out->user, t) != TE_OK || !session_user_valid(out->user)) {
            return false;
        }
        host = at + 1;
    }
    char *port = nullptr;
    if (host[0] == '[') {
        char *close = strchr(host, ']');
        if (close == nullptr) {
            return false;
        }
        *close = '\0';
        host++;
        if (close[1] == ':') port = close + 2;
        else if (close[1] != '\0') return false;
    } else {
        char *colon = strchr(host, ':');
        if (colon != nullptr && strchr(colon + 1, ':') == nullptr) {   /* exactly one ':' is a port */
            *colon = '\0';
            port = colon + 1;
        }
    }
    char *space = strchr(host, ' ');
    if (space != nullptr && port == nullptr) {   /* "host 2323", the telnet way */
        *space = '\0';
        port = str_trim(space + 1);
    }
    if (port != nullptr) {
        long v = 0;
        if (!str_to_long(port, 1, 65535, &v)) {
            return false;
        }
        out->port = (uint16_t)v;
    }
    if (str_copy(out->host, sizeof out->host, host) != TE_OK || !session_host_valid(out->host)) {
        return false;
    }
    if (!session_name_valid(out->name)) {
        (void)str_copy(out->name, sizeof out->name, out->host);
    }
    return session_validate(out, nullptr);
}

static void submit_form(te_sidebar *sb)
{
    te_form *f = &sb->form;
    te_err rc = TE_OK;
    char msg[200] = { 0 };
    switch (f->purpose) {
    case F_SESSION_NEW:
    case F_SESSION_EDIT: {
        te_session s;
        if (!session_from_form(sb, &s)) {
            return;
        }
        bool edit = f->purpose == F_SESSION_EDIT;
        rc = store_save_session(&sb->store, &s, edit ? sb->form_folder : nullptr, edit ? sb->form_name : nullptr);
        if (rc != TE_OK) {
            save_error(sb, rc, "A session with this name");
            return;
        }
        after_store_change(sb);
        int fi = store_find_folder(&sb->store, s.folder);
        if (fi >= 0) {
            sb->store.nodes[fi].expanded = true;
            sidebar_rebuild(sb);
        }
        int si = store_find_session(&sb->store, s.folder, s.name);
        if (si >= 0) select_node(sb, si);
        (void)str_fmt(msg, sizeof msg, edit ? "Saved %s" : "Created %s", s.name);
        toast(sb, msg);
        break;
    }
    case F_SESSION_RENAME: {
        te_session s = sb->form_session;
        if (str_copy(s.name, sizeof s.name, form_value(f, FD_NAME)) != TE_OK || !session_name_valid(s.name)) {
            form_set_error(f, "Name: 1-64 characters, no '/', '\\' or control characters, not starting with '.' or '-'");
            return;
        }
        if (strcmp(s.name, sb->form_name) != 0) {
            rc = store_save_session(&sb->store, &s, sb->form_folder, sb->form_name);
            if (rc != TE_OK) {
                save_error(sb, rc, "A session with this name");
                return;
            }
            after_store_change(sb);
            int si = store_find_session(&sb->store, s.folder, s.name);
            if (si >= 0) select_node(sb, si);
        }
        break;
    }
    case F_FOLDER_NEW: {
        const char *name = form_value(f, FD_NAME);
        if (!session_name_valid(name)) {
            form_set_error(f, "Name: 1-64 characters, no '/', '\\' or control characters, not starting with '.' or '-'");
            return;
        }
        rc = store_create_folder(&sb->store, sb->form_folder, name);
        if (rc != TE_OK) {
            save_error(sb, rc, "A folder with this name");
            return;
        }
        int pi = store_find_folder(&sb->store, sb->form_folder);
        if (pi >= 0) sb->store.nodes[pi].expanded = true;
        after_store_change(sb);
        break;
    }
    case F_FOLDER_RENAME: {
        const char *name = form_value(f, FD_NAME);
        if (!session_name_valid(name)) {
            form_set_error(f, "Name: 1-64 characters, no '/', '\\' or control characters, not starting with '.' or '-'");
            return;
        }
        rc = store_rename_folder(&sb->store, sb->form_folder, name);
        if (rc != TE_OK) {
            save_error(sb, rc, "A folder with this name");
            return;
        }
        after_store_change(sb);
        break;
    }
    case F_FOLDER_SETTINGS: {
        te_folder_defaults d;
        folder_defaults_init(&d);
        d.color = color_from_choice(form_choice(f, FD_COLOR));
        const char *ai = form_value(f, FD_ANTI_IDLE);
        long v = 0;
        if (ai[0] != '\0') {
            if (!str_to_long(ai, 0, TE_ANTI_IDLE_MAX, &v)) {
                form_set_error(f, "Anti-idle must be 0 (off) to 86400 seconds, or empty to inherit");
                return;
            }
            d.anti_idle = (int32_t)v;
        }
        const char *user = form_value(f, FD_USER);
        if (!session_user_valid(user) || str_copy(d.user, sizeof d.user, user) != TE_OK) {
            form_set_error(f, "User name may not contain spaces, '@', ':' or start with '-'");
            return;
        }
        rc = store_save_folder_defaults(&sb->store, sb->form_folder, &d);
        if (rc != TE_OK) {
            save_error(sb, rc, "Folder settings");
            return;
        }
        after_store_change(sb);
        toast(sb, "Folder settings saved");
        break;
    }
    case F_DELETE_SESSION: {
        int pos = vis_pos(sb, store_find_session(&sb->store, sb->form_folder, sb->form_name));
        rc = store_delete_session(&sb->store, sb->form_folder, sb->form_name);
        if (rc != TE_OK) {
            (void)str_fmt(msg, sizeof msg, "Could not delete: %s", te_strerror(rc));
            form_set_error(f, msg);
            return;
        }
        after_store_change(sb);
        if (pos >= 0 && sb->nvis > 0) {
            /* Select the row that took its place, as file managers do. */
            select_node(sb, sb->vis[TE_MIN((size_t)pos, sb->nvis - 1)]);
        }
        (void)str_fmt(msg, sizeof msg, "Deleted %s", sb->form_name);
        toast(sb, msg);
        break;
    }
    case F_DELETE_FOLDER:
        rc = store_delete_folder(&sb->store, sb->form_folder);
        after_store_change(sb);
        if (rc != TE_OK) {
            form_set_error(f, rc == TE_ERR_EXISTS ? "The folder holds files FeTerm did not create; it was left alone."
                                                  : "Could not delete the folder.");
            return;
        }
        toast(sb, "Folder deleted");
        break;
    case F_QUICK: {
        static const te_sess_type map[] = { TE_SESS_SSH, TE_SESS_TELNET, TE_SESS_SERIAL };
        size_t t = TE_MIN(form_choice(f, FD_TYPE), TE_ARRAY_LEN(map) - 1);
        te_session s;
        if (!quick_parse(map[t], form_value(f, FD_TARGET), &s)) {
            form_set_error(f, map[t] == TE_SESS_SERIAL ? "Use a device like /dev/ttyUSB0, optionally ,115200,8N1,none"
                                                       : "Use host, host:port or user@host:port");
            return;
        }
        if (form_checked(f, FD_SAVE)) {
            rc = store_save_session(&sb->store, &s, nullptr, nullptr);
            if (rc != TE_OK) {
                save_error(sb, rc, "A session with this name");
                return;
            }
            after_store_change(sb);
        }
        if (sb->host.connect != nullptr) {
            te_session_eff eff;
            store_effective(&sb->store, &s, &eff);
            sb->host.connect(sb->host.user, &s, &eff, false);
        }
        break;
    }
    default:
        break;
    }
    f->open = false;
}

static void handle_form_result(te_sidebar *sb, te_form_result r)
{
    if (r == TE_FORM_SUBMIT) {
        submit_form(sb);
    } else if (r == TE_FORM_CHANGED) {
        update_session_form(sb);
    }
    relayout_form(sb);   /* fields may have appeared or an error line been added */
}

/* ------------------------------------------------------------------------- */
/* Menus */

static void open_menu(te_sidebar *sb, int idx, float x, float y, const te_painter *p)
{
    te_menu_item items[TE_MENU_MAX_ITEMS];
    size_t n = 0;
    const te_store_node *nd = node(sb, idx);
    bool rw = sb->store_ok;
    if (nd != nullptr && !nd->folder) {
        items[n++] = menu_item(M_CONNECT, "Connect", "Enter", true);
        items[n++] = menu_item(M_CONNECT_SPLIT, "Connect in split", "", true);
        items[n] = menu_item(M_EDIT, "Edit...", "", rw);
        items[n++].separator_before = true;
        items[n++] = menu_item(M_DUPLICATE, "Duplicate...", "", rw);
        items[n++] = menu_item(M_RENAME, "Rename...", "F2", rw);
        items[n++] = menu_item(M_COPY_HOST, nd->sess != nullptr && nd->sess->type == TE_SESS_SERIAL ? "Copy device" : "Copy host",
                               "", nd->sess != nullptr && nd->sess->type != TE_SESS_LOCAL);
        items[n] = menu_item(M_DELETE, "Delete...", "Del", rw);
        items[n].danger = true;
        items[n++].separator_before = true;
    } else if (nd != nullptr) {
        items[n++] = menu_item(M_NEW_SESSION, "New session here...", "", rw);
        items[n++] = menu_item(M_NEW_FOLDER, "New folder here...", "", rw);
        items[n] = menu_item(M_FOLDER_SETTINGS, "Folder settings...", "", rw);
        items[n++].separator_before = true;
        items[n++] = menu_item(M_RENAME_FOLDER, "Rename...", "F2", rw);
        items[n] = menu_item(M_DELETE_FOLDER, "Delete folder...", "Del", rw);
        items[n].danger = true;
        items[n++].separator_before = true;
    } else {
        items[n++] = menu_item(M_NEW_SESSION, "New session...", "", rw);
        items[n++] = menu_item(M_NEW_FOLDER, "New folder...", "", rw);
        items[n++] = menu_item(M_QUICK, "Quick connect...", "Ctrl+Shift+N", true);
        items[n] = menu_item(M_EXPAND_ALL, "Expand all", "", sb->store.len > 0);
        items[n++].separator_before = true;
        items[n++] = menu_item(M_COLLAPSE_ALL, "Collapse all", "", sb->store.len > 0);
        items[n++] = menu_item(M_RELOAD, "Reload", "", rw);
    }
    sb->menu_node = idx;
    menu_open(&sb->menu, items, n, x, y, sb->win_w, sb->win_h, p);
}

/* ------------------------------------------------------------------------- */
/* Input */

static te_painter metrics(const te_sidebar *sb)
{
    return (te_painter){ .cell_w = sb->cell_w, .cell_h = sb->cell_h, .scale = sb->scale };
}

/* Tabs in the header: returns 0 Sessions, 1 Files, 2 the collapse button, -1 none. */
static int header_hit(const te_sidebar *sb, float x)
{
    float lx = x - sb->x;
    float tab_w = (float)(int)((sb->w - sb->cell_w * 3.0f) / 2.0f);
    if (lx >= sb->w - sb->cell_w * 3.0f) {
        return 2;
    }
    if (lx >= 0.0f && lx < tab_w) {
        return 0;
    }
    if (lx >= tab_w && lx < 2.0f * tab_w) {
        return 1;
    }
    return -1;
}

/* Toolbar buttons at the bottom: 0 new session, 1 new folder, 2 quick connect, -1 none. */
static int toolbar_hit(const te_sidebar *sb, float x)
{
    float third = sb->w / 3.0f;
    float lx = x - sb->x;
    if (lx < 0.0f || lx >= sb->w) return -1;
    return (int)(lx / third);
}

static void toggle_folder(te_sidebar *sb, int idx)
{
    if (idx >= 0 && (size_t)idx < sb->store.len && sb->store.nodes[idx].folder) {
        sb->store.nodes[idx].expanded = !sb->store.nodes[idx].expanded;
        sidebar_rebuild(sb);
    }
}

static int drop_target_at(const te_sidebar *sb, float x, float y)
{
    if (!sidebar_contains(sb, x, y) || y < tree_top(sb) || y >= tree_bottom(sb)) {
        return -2;
    }
    int idx = node_at(sb, x, y);
    const te_store_node *n = node(sb, idx);
    if (n == nullptr) {
        return -1;   /* empty space: the top level */
    }
    if (n->folder) {
        return idx;
    }
    return n->parent;
}

bool sidebar_button(te_sidebar *sb, float x, float y, int button, bool down, uint8_t clicks)
{
    te_painter p = metrics(sb);
    sb->mouse_x = x;
    sb->mouse_y = y;
    if (sb->form.open) {
        if (down && button == 0) {
            handle_form_result(sb, form_click(&sb->form, x, y, &p));
        }
        return true;
    }
    if (sb->menu.open) {
        if (down) {
            int id = menu_click(&sb->menu, x, y, &p);
            if (id > 0) {
                run_menu(sb, id);
            }
            /* A click outside closes the menu and does nothing else. */
        }
        return true;
    }
    /* Release ends a drag or a resize, wherever the pointer is. */
    if (!down && button == 0) {
        bool was = sb->pressed || sb->resizing;
        if (sb->resizing) {
            sb->resizing = false;
        } else if (sb->dragging) {
            const te_store_node *src = node(sb, sb->press_node);
            const te_store_node *dst = node(sb, sb->drop_folder);
            const char *to = sb->drop_folder == -1 ? "" : (dst != nullptr ? dst->path : nullptr);
            if (src != nullptr && !src->folder && to != nullptr && strcmp(src->path, to) != 0 && sb->store_ok) {
                char name[TE_SESS_NAME_MAX + 1] = { 0 };
                char from[TE_SESS_FOLDER_MAX + 1] = { 0 };
                char dest[TE_SESS_FOLDER_MAX + 1] = { 0 };
                (void)str_copy(name, sizeof name, src->name);
                (void)str_copy(from, sizeof from, src->path);
                (void)str_copy(dest, sizeof dest, to);
                te_err rc = store_move_session(&sb->store, from, name, dest);
                char msg[200] = { 0 };
                if (rc == TE_OK) {
                    (void)str_fmt(msg, sizeof msg, "Moved %s to %s", name, dest[0] != '\0' ? dest : "the top level");
                    after_store_change(sb);
                    int si = store_find_session(&sb->store, dest, name);
                    if (si >= 0) select_node(sb, si);
                } else {
                    (void)str_fmt(msg, sizeof msg, rc == TE_ERR_EXISTS ? "%s already exists there" : "Could not move %s",
                                  name);
                }
                toast(sb, msg);
            }
        }
        sb->pressed = false;
        sb->dragging = false;
        sb->drop_folder = -2;
        return was || sidebar_contains(sb, x, y);
    }
    if (!down) {
        return sidebar_contains(sb, x, y);
    }
    if (button == 0 && sidebar_on_edge(sb, x, y)) {
        sb->resizing = true;
        return true;
    }
    if (!sidebar_contains(sb, x, y)) {
        sb->focused = false;
        return false;
    }
    sb->focused = true;
    if (!sb->visible) {
        /* The collapsed strip: any click opens the sidebar. */
        sb->visible = true;
        sb->layout_changed = true;
        return true;
    }
    if (y < header_h(sb)) {
        int h = header_hit(sb, x);
        if (h == 2) {
            sidebar_toggle(sb);
        } else if (h >= 0) {
            sb->tab = (te_sb_tab)h;
        }
        return true;
    }
    if (sb->tab != TE_SB_SESSIONS) {
        return true;
    }
    if (y < tree_top(sb)) {
        return true;   /* the filter box: typing goes there once the sidebar has focus */
    }
    if (y >= tree_bottom(sb)) {
        if (button == 0) {
            int t = toolbar_hit(sb, x);
            const te_store_node *sel = node(sb, sb->sel);
            const char *folder = sel == nullptr ? "" : sel->path;
            if (t == 0 && sb->store_ok) new_session_in(sb, folder);
            else if (t == 1 && sb->store_ok) {
                open_name_form(sb, F_FOLDER_NEW, "New folder", "Create", "");
                (void)str_copy(sb->form_folder, sizeof sb->form_folder, folder);
            } else if (t == 2) sidebar_quick_connect(sb);
            relayout_form(sb);
        }
        return true;
    }
    int idx = node_at(sb, x, y);
    if (button == 2) {
        if (idx >= 0) select_node(sb, idx);
        open_menu(sb, idx, x, y, &p);   /* right-click only ever opens a menu */
        return true;
    }
    if (button != 0) {
        return true;
    }
    if (idx < 0) {
        return true;
    }
    select_node(sb, idx);
    const te_store_node *n = node(sb, idx);
    if (n != nullptr && n->folder) {
        toggle_folder(sb, idx);
    } else if (clicks >= 2) {
        connect_node(sb, idx, false);
    } else {
        sb->pressed = true;
        sb->press_x = x;
        sb->press_y = y;
        sb->press_node = idx;
    }
    return true;
}

bool sidebar_motion(te_sidebar *sb, float x, float y, bool left_down)
{
    te_painter p = metrics(sb);
    sb->mouse_x = x;
    sb->mouse_y = y;
    if (sb->form.open) {
        return true;
    }
    if (sb->menu.open) {
        menu_motion(&sb->menu, x, y, &p);
        return true;
    }
    if (sb->resizing) {
        float px = sb->right ? sb->win_w - x : x;
        float w = TE_CLAMP(px / sb->scale, MIN_WIDTH, MAX_WIDTH);
        if (w != sb->width_logical) {
            sb->width_logical = w;
            sb->layout_changed = true;
        }
        return true;
    }
    if (sb->pressed && left_down) {
        float dx = x - sb->press_x;
        float dy = y - sb->press_y;
        if (!sb->dragging && dx * dx + dy * dy > DRAG_START_PX * DRAG_START_PX * sb->scale * sb->scale) {
            sb->dragging = true;
        }
        if (sb->dragging) {
            sb->drop_folder = drop_target_at(sb, x, y);
        }
        return true;
    }
    int h = sidebar_contains(sb, x, y) && sb->visible && sb->tab == TE_SB_SESSIONS ? node_at(sb, x, y) : -1;
    sb->hover = h;
    return sidebar_contains(sb, x, y);
}

bool sidebar_wheel(te_sidebar *sb, float x, float y, float dy)
{
    if (sidebar_captures(sb)) {
        return true;
    }
    if (!sidebar_contains(sb, x, y)) {
        return false;
    }
    size_t fit = rows_fit(sb);
    size_t max = sb->nvis > fit ? sb->nvis - fit : 0;
    int lines = (int)(dy * 3.0f);
    if (lines > 0) {
        sb->scroll = (size_t)lines > sb->scroll ? 0 : sb->scroll - (size_t)lines;
    } else if (lines < 0) {
        sb->scroll += (size_t)(-lines);
        if (sb->scroll > max) sb->scroll = max;
    }
    return true;
}

static void move_sel(te_sidebar *sb, int dir)
{
    if (sb->nvis == 0) {
        return;
    }
    int pos = vis_pos(sb, sb->sel);
    int np = pos < 0 ? 0 : pos + dir;
    np = TE_CLAMP(np, 0, (int)sb->nvis - 1);
    select_node(sb, sb->vis[np]);
}

bool sidebar_key(te_sidebar *sb, te_key key, uint32_t cp, uint32_t mods)
{
    (void)cp;
    te_painter p = metrics(sb);
    if (sb->form.open) {
        handle_form_result(sb, form_key(&sb->form, key, mods));
        return true;
    }
    if (sb->menu.open) {
        int id = menu_key(&sb->menu, key);
        if (id > 0) {
            run_menu(sb, id);
        }
        return true;
    }
    if (!sb->focused || !sb->visible) {
        return false;
    }
    if ((mods & (TE_MOD_CTRL | TE_MOD_ALT | TE_MOD_SUPER)) != 0) {
        return false;   /* shortcuts stay with the application */
    }
    const te_store_node *n = node(sb, sb->sel);
    switch (key) {
    case TE_KEY_ESCAPE:
        if (sb->filter[0] != '\0') {
            sb->filter[0] = '\0';
            sidebar_rebuild(sb);
        } else {
            sb->focused = false;
        }
        return true;
    case TE_KEY_UP:        move_sel(sb, -1); return true;
    case TE_KEY_DOWN:      move_sel(sb, 1); return true;
    case TE_KEY_PAGE_UP:   move_sel(sb, -(int)TE_MAX(rows_fit(sb), (size_t)1)); return true;
    case TE_KEY_PAGE_DOWN: move_sel(sb, (int)TE_MAX(rows_fit(sb), (size_t)1)); return true;
    case TE_KEY_HOME:      if (sb->nvis > 0) select_node(sb, sb->vis[0]); return true;
    case TE_KEY_END:       if (sb->nvis > 0) select_node(sb, sb->vis[sb->nvis - 1]); return true;
    case TE_KEY_LEFT:
        if (n != nullptr && n->folder && n->expanded) {
            toggle_folder(sb, sb->sel);
        } else if (n != nullptr && n->parent >= 0) {
            select_node(sb, n->parent);
        }
        return true;
    case TE_KEY_RIGHT:
        if (n != nullptr && n->folder && !n->expanded) {
            toggle_folder(sb, sb->sel);
        }
        return true;
    case TE_KEY_ENTER:
        if (n != nullptr && n->folder) {
            toggle_folder(sb, sb->sel);
        } else if (n != nullptr) {
            connect_node(sb, sb->sel, (mods & TE_MOD_SHIFT) != 0);
        }
        return true;
    case TE_KEY_DELETE:
        if (n != nullptr) {
            sb->menu_node = sb->sel;
            run_menu(sb, n->folder ? M_DELETE_FOLDER : M_DELETE);   /* opens the confirmation */
        }
        return true;
    case TE_KEY_F2:
        if (n != nullptr) {
            sb->menu_node = sb->sel;
            run_menu(sb, n->folder ? M_RENAME_FOLDER : M_RENAME);
        }
        return true;
    case TE_KEY_F10:
        if ((mods & TE_MOD_SHIFT) != 0) {   /* Shift+F10: the context menu from the keyboard */
            int pos = vis_pos(sb, sb->sel);
            float my = pos >= 0 ? tree_top(sb) + (float)((size_t)pos - TE_MIN((size_t)pos, sb->scroll) + 1) * row_h(sb)
                                : tree_top(sb);
            open_menu(sb, sb->sel, sb->x + sb->cell_w * 4.0f, my, &p);
        }
        return true;
    case TE_KEY_BACKSPACE: {
        size_t len = strlen(sb->filter);
        if (len > 0) {
            len--;
            while (len > 0 && ((unsigned char)sb->filter[len] & 0xC0u) == 0x80u) len--;
            sb->filter[len] = '\0';
            sidebar_rebuild(sb);
        }
        return true;
    }
    case TE_KEY_CHAR:
        return true;   /* the text event that follows goes into the filter */
    default:
        return true;
    }
}

bool sidebar_text(te_sidebar *sb, const char *utf8)
{
    if (sb->form.open) {
        handle_form_result(sb, form_text(&sb->form, utf8));
        return true;
    }
    if (sb->menu.open) {
        return true;
    }
    if (!sb->focused || !sb->visible || sb->tab != TE_SB_SESSIONS) {
        return false;
    }
    size_t len = strlen(sb->filter);
    size_t add = strnlen(utf8, 64);
    for (size_t i = 0; i < add; i++) {
        if ((unsigned char)utf8[i] < 0x20 || utf8[i] == 0x7F) {
            return true;   /* no control characters in the filter */
        }
    }
    if (len + add < sizeof sb->filter) {
        memcpy(sb->filter + len, utf8, add);
        sb->filter[len + add] = '\0';
        sidebar_rebuild(sb);
        if (sb->nvis > 0 && (sb->sel < 0 || vis_pos(sb, sb->sel) < 0)) {
            /* Select the first matching session so Enter connects to it. */
            for (size_t i = 0; i < sb->nvis; i++) {
                const te_store_node *n = node(sb, sb->vis[i]);
                if (n != nullptr && !n->folder) {
                    select_node(sb, sb->vis[i]);
                    break;
                }
            }
        }
    }
    return true;
}

/* ------------------------------------------------------------------------- */
/* Drawing */

static const char *type_tag(te_sess_type t)
{
    switch (t) {
    case TE_SESS_SSH:    return "ssh";
    case TE_SESS_TELNET: return "tel";
    case TE_SESS_SERIAL: return "ser";
    case TE_SESS_LOCAL:  return "sh";
    case TE_SESS_TYPE_COUNT:
    default:             return "";
    }
}

static void draw_collapsed(const te_sidebar *sb, const te_painter *p, const te_ui_colors *c)
{
    ui_rect(p, TE_LAYER_BASE, sb->x, 0.0f, sb->w, sb->win_h, c->bg_alt, 1.0f);
    float ex = sb->right ? sb->x : sb->x + sb->w - p->scale;
    ui_rect(p, TE_LAYER_BASE, ex, 0.0f, p->scale, sb->win_h, c->border, 1.0f);
    float cx = sb->x + (sb->w - p->cell_w) / 2.0f;
    ui_text(p, TE_LAYER_BASE, cx, p->cell_h * 0.3f, sb->right ? "\xc2\xab" : "\xc2\xbb", 1, c->accent, true);   /* « » */
    /* The tab names, one letter per line. */
    const char *word = "SESSIONS";
    float y = p->cell_h * 2.0f;
    for (const char *s = word; *s != '\0' && y < sb->win_h - p->cell_h; s++) {
        char ch[2] = { *s, '\0' };
        ui_text(p, TE_LAYER_BASE, cx, y, ch, 1, sb->tab == TE_SB_SESSIONS ? c->fg : c->muted, false);
        y += p->cell_h;
    }
}

static void draw_header(const te_sidebar *sb, const te_painter *p, const te_ui_colors *c)
{
    float hh = header_h(sb);
    float tab_w = (float)(int)((sb->w - p->cell_w * 3.0f) / 2.0f);
    ui_rect(p, TE_LAYER_BASE, sb->x, 0.0f, sb->w, hh, c->bg_alt, 1.0f);
    static const char *const names[] = { "Sessions", "Files" };
    for (int i = 0; i < 2; i++) {
        float tx = sb->x + (float)i * tab_w;
        bool active = (int)sb->tab == i;
        if (active) {
            ui_rect(p, TE_LAYER_BASE, tx, 0.0f, tab_w, hh, c->bg, 1.0f);
            ui_rect(p, TE_LAYER_BASE, tx, hh - 2.0f * p->scale, tab_w, 2.0f * p->scale, c->accent, 1.0f);
        }
        size_t cols = ui_text_cols(names[i]);
        float lx = tx + (tab_w - (float)cols * p->cell_w) / 2.0f;
        ui_text(p, TE_LAYER_BASE, lx, (hh - p->cell_h) / 2.0f, names[i], (size_t)(tab_w / p->cell_w), active ? c->fg : c->muted,
                active);
    }
    ui_text(p, TE_LAYER_BASE, sb->x + sb->w - p->cell_w * 2.0f, (hh - p->cell_h) / 2.0f,
            sb->right ? "\xc2\xbb" : "\xc2\xab", 1, c->muted, false);
}

static void draw_sessions(const te_sidebar *sb, const te_painter *p, const te_ui_colors *c)
{
    float s = p->scale;
    float cols_f = sb->w / p->cell_w;
    size_t cols = cols_f > 4.0f ? (size_t)cols_f : 4;   /* never below 4, so cols - 2 stays positive */
    /* Filter box */
    float fy = header_h(sb) + p->cell_h * 0.3f;
    float fh = filter_h(sb) - p->cell_h * 0.6f;
    float fx = sb->x + p->cell_w;
    float fw = sb->w - 2.0f * p->cell_w;
    ui_rect(p, TE_LAYER_BASE, fx, fy, fw, fh, c->bg_alt, 1.0f);
    if (sb->focused) {
        ui_frame(p, TE_LAYER_BASE, fx, fy, fw, fh, c->accent);
    }
    float ty = fy + (fh - p->cell_h) / 2.0f;
    ui_text(p, TE_LAYER_BASE, fx + p->cell_w * 0.5f, ty, "\xe2\x8c\x95", 1, c->muted, false);   /* ⌕ */
    size_t fcols = cols > 5 ? cols - 5 : 1;
    if (sb->filter[0] != '\0') {
        ui_text(p, TE_LAYER_BASE, fx + p->cell_w * 2.0f, ty, sb->filter, fcols, c->fg, false);
    } else {
        ui_text(p, TE_LAYER_BASE, fx + p->cell_w * 2.0f, ty, sb->focused ? "Type to filter" : "Filter sessions", fcols,
                c->muted, false);
    }

    /* Tree */
    float top = tree_top(sb);
    float rh = row_h(sb);
    size_t fit = rows_fit(sb);
    if (sb->nvis == 0) {
        const char *msg = !sb->store_ok ? "Session folder unavailable"
                          : sb->filter[0] != '\0' ? "No matching sessions"
                                                  : "No saved sessions yet";
        ui_text(p, TE_LAYER_BASE, sb->x + p->cell_w * 1.5f, top + rh * 0.5f, msg, cols - 2, c->muted, false);
        if (sb->filter[0] == '\0' && sb->store_ok) {
            ui_text(p, TE_LAYER_BASE, sb->x + p->cell_w * 1.5f, top + rh * 1.7f, "Right-click to add one",
                    cols - 2, c->muted, false);
        }
    }
    for (size_t row = 0; row < fit && row + sb->scroll < sb->nvis; row++) {
        int idx = sb->vis[row + sb->scroll];
        const te_store_node *n = node(sb, idx);
        if (n == nullptr) {
            continue;
        }
        float ry = top + (float)row * rh;
        bool selected = idx == sb->sel;
        bool drop = sb->dragging && n->folder && idx == sb->drop_folder;
        if (selected) {
            ui_rect(p, TE_LAYER_BASE, sb->x, ry, sb->w, rh, sb->focused ? c->sel_bg : c->hover, 1.0f);
        } else if (idx == sb->hover) {
            ui_rect(p, TE_LAYER_BASE, sb->x, ry, sb->w, rh, c->hover, 0.6f);
        }
        if (drop) {
            ui_frame(p, TE_LAYER_BASE, sb->x + s, ry, sb->w - 2.0f * s, rh, c->accent);
        }
        uint32_t fg = selected && sb->focused ? c->sel_fg : c->fg;
        float tyy = ry + (rh - p->cell_h) / 2.0f;
        size_t indent = 1 + (size_t)n->depth * 2;
        float x = sb->x + (float)indent * p->cell_w;
        size_t room = cols > indent + 1 ? cols - indent - 1 : 1;
        if (n->folder) {
            ui_text(p, TE_LAYER_BASE, x, tyy, n->expanded ? "\xe2\x96\xbe" : "\xe2\x96\xb8", 1, c->muted, false);  /* ▾ ▸ */
            uint32_t dot = tag_color_rgb(n->fdef.color);
            if (dot != 0) {
                ui_rect(p, TE_LAYER_BASE, x + p->cell_w * 1.25f, ry + rh * 0.3f, 3.0f * s, rh * 0.4f, dot, 1.0f);
            }
            ui_text(p, TE_LAYER_BASE, x + p->cell_w * 2.0f, tyy, n->name, room > 2 ? room - 2 : 1, fg, true);
        } else {
            te_session_eff eff;
            if (n->sess != nullptr) {
                store_effective(&sb->store, n->sess, &eff);
            } else {
                eff.color = TE_TAG_NONE;
            }
            uint32_t dot = tag_color_rgb(eff.color);
            ui_text(p, TE_LAYER_BASE, x, tyy, dot != 0 ? "\xe2\x97\x8f" : "\xe2\x97\x8b", 1, dot != 0 ? dot : c->muted,
                    false);   /* ● ○ */
            const char *tag = n->sess != nullptr ? type_tag(n->sess->type) : "";
            size_t tagc = ui_text_cols(tag);
            size_t name_room = room > tagc + 4 ? room - tagc - 4 : 1;
            ui_text(p, TE_LAYER_BASE, x + p->cell_w * 2.0f, tyy, n->name, name_room, fg, false);
            ui_text(p, TE_LAYER_BASE, sb->x + sb->w - (float)(tagc + 1) * p->cell_w, tyy, tag, tagc,
                    selected && sb->focused ? c->sel_fg : c->muted, false);
        }
    }
    /* Scroll indicator */
    if (sb->nvis > fit && fit > 0) {
        float track = tree_bottom(sb) - top;
        float th = track * (float)fit / (float)sb->nvis;
        if (th < p->cell_h) th = p->cell_h;
        float tyt = top + (track - th) * (float)sb->scroll / (float)(sb->nvis - fit);
        ui_rect(p, TE_LAYER_BASE, sb->x + sb->w - 4.0f * s, tyt, 3.0f * s, th, c->muted, 0.5f);
    }

    /* Toolbar */
    float by = tree_bottom(sb);
    float bh = toolbar_h(sb);
    ui_rect(p, TE_LAYER_BASE, sb->x, by, sb->w, s, c->border, 1.0f);
    static const char *const buttons[] = { "+ Session", "+ Folder", "\xe2\x9a\xa1 Quick" };   /* ⚡ */
    float third = sb->w / 3.0f;
    for (int i = 0; i < 3; i++) {
        size_t bc = ui_text_cols(buttons[i]);
        size_t room = (size_t)(third / p->cell_w);
        float bx = sb->x + (float)i * third + (third - (float)TE_MIN(bc, room) * p->cell_w) / 2.0f;
        bool hot = sb->mouse_y >= by && sidebar_contains(sb, sb->mouse_x, sb->mouse_y) && toolbar_hit(sb, sb->mouse_x) == i;
        ui_text(p, TE_LAYER_BASE, bx, by + (bh - p->cell_h) / 2.0f, buttons[i], room, hot ? c->accent : c->fg, false);
    }
}

static void draw_files(const te_sidebar *sb, const te_painter *p, const te_ui_colors *c)
{
    size_t cols = sb->w / p->cell_w > 3.0f ? (size_t)(sb->w / p->cell_w) - 2 : 1;
    float y = header_h(sb) + p->cell_h;
    ui_text(p, TE_LAYER_BASE, sb->x + p->cell_w, y, "Remote files", cols, c->fg, true);
    y += p->cell_h * 1.6f;
    static const char *const lines[] = {
        "Browse, download and upload files",
        "of the active SSH session here.",
        "",
        "The file explorer arrives in the",
        "next FeTerm update (0.4).",
    };
    for (size_t i = 0; i < TE_ARRAY_LEN(lines); i++) {
        ui_text(p, TE_LAYER_BASE, sb->x + p->cell_w, y, lines[i], cols, c->muted, false);
        y += p->cell_h * 1.2f;
    }
}

void sidebar_draw(const te_sidebar *sb, const te_painter *p, const te_ui_colors *c)
{
    if (sb->w <= 0.0f) {
        return;
    }
    if (!sb->visible) {
        draw_collapsed(sb, p, c);
    } else {
        ui_rect(p, TE_LAYER_BASE, sb->x, 0.0f, sb->w, sb->win_h, c->bg, 1.0f);
        draw_header(sb, p, c);
        if (sb->tab == TE_SB_SESSIONS) {
            draw_sessions(sb, p, c);
        } else {
            draw_files(sb, p, c);
        }
        float ex = sb->right ? sb->x : sb->x + sb->w - p->scale;
        ui_rect(p, TE_LAYER_BASE, ex, 0.0f, p->scale, sb->win_h, sb->resizing ? c->accent : c->border, 1.0f);
        if (sb->dragging) {
            const te_store_node *n = node(sb, sb->press_node);
            if (n != nullptr) {
                ui_rect(p, TE_LAYER_OVERLAY, sb->mouse_x + p->cell_w, sb->mouse_y, (float)(ui_text_cols(n->name) + 2) * p->cell_w,
                        p->cell_h * 1.3f, c->sel_bg, 0.9f);
                ui_text(p, TE_LAYER_OVERLAY, sb->mouse_x + p->cell_w * 2.0f, sb->mouse_y + p->cell_h * 0.15f, n->name,
                        TE_SESS_NAME_MAX, c->sel_fg, false);
            }
        }
    }
    menu_draw(&sb->menu, p, c);
    if (sb->form.open) {
        /* Dim everything behind a dialog. */
        ui_rect(p, TE_LAYER_OVERLAY, 0.0f, 0.0f, sb->win_w, sb->win_h, 0x000000, 0.35f);
        form_draw(&sb->form, p, c);
    }
}
