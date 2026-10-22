#include "ui/files.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "util/mem.h"
#include "util/str.h"

/* Menu items */
enum {
    F_OPEN = TE_FILES_MENU_BASE, F_DOWNLOAD, F_DOWNLOAD_TO, F_EDIT, F_RENAME, F_DELETE, F_COPY_PATH, F_PERMS,
    F_PROPS, F_UPLOAD_FILES, F_UPLOAD_FOLDER, F_NEW_FOLDER, F_REFRESH, F_TOGGLE_HIDDEN, F_GO_TERMINAL, F_GOTO,
    F_XFER_CANCEL, F_XFER_RETRY, F_XFER_CLEAR, F_SCP_UP, F_SCP_DOWN, F_RECONNECT, F_HOME,
};

/* Dialog purposes */
enum {
    FP_RENAME = TE_FILES_MENU_BASE, FP_NEW_FOLDER, FP_DELETE, FP_PERMS, FP_PROPS, FP_GOTO, FP_CONFLICT_DOWN,
    FP_CONFLICT_UP, FP_PROMPT, FP_SCP_UP, FP_SCP_DOWN,
};

/* Dialog fields */
enum { FF_NAME = 1, FF_MODE, FF_PATH, FF_CHOICE, FF_SECRET, FF_LOCAL };

/* Pending file dialogs */
enum { DLG_NONE, DLG_UPLOAD_FILES, DLG_UPLOAD_FOLDERS, DLG_DOWNLOAD_TO, DLG_SCP_UP };

static const char *const conflict_choices[] = { "Keep both (add a number)", "Replace", "Skip" };
static const te_conflict conflict_map[] = { TE_CONFLICT_KEEP_BOTH, TE_CONFLICT_REPLACE, TE_CONFLICT_SKIP };

/* ------------------------------------------------------------------------- */
/* Helpers */

void files_human_size(uint64_t bytes, char *out, size_t outsz)
{
    static const char units[] = "BKMGTP";
    double v = (double)bytes;
    size_t u = 0;
    while (v >= 1024.0 && u + 1 < sizeof units - 1) {
        v /= 1024.0;
        u++;
    }
    if (u == 0) {
        (void)str_fmt(out, outsz, "%u", (unsigned)bytes);
    } else if (v < 10.0) {
        (void)str_fmt(out, outsz, "%.1f%c", v, units[u]);
    } else {
        (void)str_fmt(out, outsz, "%.0f%c", v, units[u]);
    }
}

static void toast(const te_files *fv, const char *msg)
{
    if (fv->host.toast != nullptr) {
        fv->host.toast(fv->host.user, msg);
    }
}

static void free_list(char ***list, size_t *n)
{
    for (size_t i = 0; i < *n; i++) {
        TE_FREE((*list)[i]);
    }
    TE_FREE(*list);
    *n = 0;
}

static bool set_list(char ***list, size_t *n, const char *const *items, size_t count)
{
    free_list(list, n);
    if (count == 0) {
        return true;
    }
    *list = te_calloc(count, sizeof **list);
    if (*list == nullptr) {
        return false;
    }
    for (size_t i = 0; i < count; i++) {
        (*list)[i] = str_dup(items[i]);
        if ((*list)[i] == nullptr) {
            free_list(list, n);
            return false;
        }
        (*n)++;
    }
    return true;
}

static const te_rentry *entry_at(const te_files *fv, int row)
{
    const te_rentry *e = nullptr;
    size_t n = remote_entries(fv->remote, &e);
    if (row < 0 || (size_t)row >= fv->nrows || e == nullptr || fv->rows[row] >= n) {
        return nullptr;
    }
    return &e[fv->rows[row]];
}

static void rebuild(te_files *fv)
{
    fv->nrows = 0;
    const te_rentry *e = nullptr;
    size_t n = remote_entries(fv->remote, &e);
    if (n > fv->caprows) {
        void *a = fv->rows;
        void *b = fv->marked;
        if (te_realloc_array(&a, fv->caprows, n, sizeof *fv->rows) != TE_OK) {
            return;
        }
        fv->rows = a;
        if (te_realloc_array(&b, fv->caprows, n, sizeof *fv->marked) != TE_OK) {
            return;
        }
        fv->marked = b;
        fv->caprows = n;
    }
    for (size_t i = 0; i < n; i++) {
        if (!fv->show_hidden && e[i].name[0] == '.') {
            continue;
        }
        fv->rows[fv->nrows] = i;
        fv->marked[fv->nrows] = false;
        fv->nrows++;
    }
    fv->sel = fv->nrows > 0 ? 0 : -1;
    fv->anchor = fv->sel;
    fv->scroll = 0;
    fv->gen_seen = remote_generation(fv->remote);
    fv->remote_seen = fv->remote;
}

/* The names a menu action applies to: the marked rows, or the selected one. */
static size_t collect_names(const te_files *fv, const char **out, size_t max)
{
    size_t n = 0;
    for (size_t i = 0; i < fv->nrows && n < max; i++) {
        const te_rentry *e = entry_at(fv, (int)i);
        if (fv->marked[i] && e != nullptr) {
            out[n++] = e->name;
        }
    }
    if (n == 0) {
        const te_rentry *e = entry_at(fv, fv->sel);
        if (e != nullptr && max > 0) {
            out[n++] = e->name;
        }
    }
    return n;
}

static bool ready(const te_files *fv)
{
    return fv->remote != nullptr && remote_state(fv->remote) == TE_RS_READY;
}

/* ------------------------------------------------------------------------- */
/* Lifecycle */

void files_init(te_files *fv, te_menu *menu, te_form *form, const te_files_host *host)
{
    memset(fv, 0, sizeof *fv);
    fv->menu = menu;
    fv->form = form;
    fv->host = *host;
    fv->sel = -1;
    fv->anchor = -1;
    fv->hover = -1;
    fv->follow = true;
    fv->cell_w = 8.0f;
    fv->cell_h = 16.0f;
    fv->scale = 1.0f;
}

void files_destroy(te_files *fv)
{
    TE_FREE(fv->rows);
    TE_FREE(fv->marked);
    fv->nrows = 0;
    fv->caprows = 0;
    free_list(&fv->target, &fv->ntarget);
    free_list(&fv->pending_paths, &fv->npending);
}

void files_set_remote(te_files *fv, te_remote *r, bool ssh_pane)
{
    fv->ssh_pane = ssh_pane;
    if (r != fv->remote) {
        fv->remote = r;
        rebuild(fv);
    }
}

void files_set_download_dir(te_files *fv, const char *dir)
{
    (void)str_copy(fv->download_dir, sizeof fv->download_dir, dir != nullptr ? dir : "");
}

/* ------------------------------------------------------------------------- */
/* Geometry */

static float toolbar_h(const te_files *fv) { return fv->cell_h * 1.8f; }
static float header_h(const te_files *fv) { return fv->cell_h * 1.3f; }
static float row_h(const te_files *fv) { return fv->cell_h * 1.35f; }

static size_t xfer_visible(const te_files *fv)
{
    const te_xfer *x = nullptr;
    size_t n = remote_transfers(fv->remote, &x);
    size_t v = 0;
    for (size_t i = 0; i < n; i++) {
        bool shown = x[i].kind == TE_XK_DOWNLOAD || x[i].kind == TE_XK_UPLOAD || x[i].kind == TE_XK_SCP_DOWN ||
                     x[i].kind == TE_XK_SCP_UP || x[i].state == TE_XS_FAILED;
        v += shown && !x[i].edit;
    }
    return v;
}

static float strip_h(const te_files *fv)
{
    return xfer_visible(fv) > 0 ? fv->cell_h * 3.2f : 0.0f;
}

static float list_top(const te_files *fv) { return fv->y + toolbar_h(fv) + header_h(fv); }
static float list_bottom(const te_files *fv) { return fv->y + fv->h - strip_h(fv); }

static size_t rows_fit(const te_files *fv)
{
    float h = list_bottom(fv) - list_top(fv);
    return h > 0.0f ? (size_t)(h / row_h(fv)) : 0;
}

void files_layout(te_files *fv, const te_painter *p, float x, float y, float w, float h, float win_w, float win_h)
{
    fv->cell_w = p->cell_w;
    fv->cell_h = p->cell_h;
    fv->scale = p->scale > 0.0f ? p->scale : 1.0f;
    fv->x = x;
    fv->y = y;
    fv->w = w;
    fv->h = h;
    fv->win_w = win_w;
    fv->win_h = win_h;
    if (fv->remote != fv->remote_seen || remote_generation(fv->remote) != fv->gen_seen) {
        rebuild(fv);
    }
    size_t fit = rows_fit(fv);
    if (fv->nrows <= fit) {
        fv->scroll = 0;
    } else if (fv->scroll > fv->nrows - fit) {
        fv->scroll = fv->nrows - fit;
    }
}

static int row_at(const te_files *fv, float y)
{
    if (y < list_top(fv) || y >= list_bottom(fv)) {
        return -1;
    }
    size_t r = (size_t)((y - list_top(fv)) / row_h(fv)) + fv->scroll;
    return r < fv->nrows ? (int)r : -1;
}

/* Toolbar buttons: up, home, refresh, hidden, follow; then the path (6). */
static int toolbar_hit(const te_files *fv, float x)
{
    float lx = x - fv->x;
    float bw = fv->cell_w * 3.0f;
    int i = (int)(lx / bw);
    return lx < 0.0f ? -1 : (i < 5 ? i : 5);
}

static void select_row(te_files *fv, int row)
{
    if (row < 0 || (size_t)row >= fv->nrows) {
        return;
    }
    fv->sel = row;
    size_t fit = rows_fit(fv);
    size_t r = (size_t)row;
    if (r < fv->scroll) {
        fv->scroll = r;
    } else if (fit > 0 && r >= fv->scroll + fit) {
        fv->scroll = r + 1 - fit;
    }
}

/* ------------------------------------------------------------------------- */
/* Dialogs */

static void open_menu(te_files *fv, int row, float x, float y)
{
    te_menu_item it[TE_MENU_MAX_ITEMS] = { 0 };
    size_t n = 0;
    te_painter p = { .cell_w = fv->cell_w, .cell_h = fv->cell_h, .scale = fv->scale };
    bool ok = ready(fv);
    const te_rentry *e = entry_at(fv, row);
    if (remote_state(fv->remote) == TE_RS_NO_SFTP) {
        it[n++] = menu_item(F_SCP_UP, "Upload a file (SCP)...", "", true);
        it[n++] = menu_item(F_SCP_DOWN, "Download a file (SCP)...", "", true);
    } else if (e != nullptr) {
        size_t marked = 0;
        for (size_t i = 0; i < fv->nrows; i++) marked += fv->marked[i];
        bool many = marked > 1;
        if (e->dir && !many) {
            it[n++] = menu_item(F_OPEN, "Open", "Enter", ok);
            it[n++] = menu_item(F_DOWNLOAD, "Download folder", "", ok);
            it[n++] = menu_item(F_DOWNLOAD_TO, "Download folder to...", "", ok);
        } else {
            it[n++] = menu_item(F_DOWNLOAD, many ? "Download selected" : "Download", many ? "" : "Enter", ok);
            it[n++] = menu_item(F_DOWNLOAD_TO, many ? "Download selected to..." : "Download to...", "", ok);
            if (!many) it[n++] = menu_item(F_EDIT, "Edit locally", "", ok && !e->dir);
        }
        it[n] = menu_item(F_RENAME, "Rename...", "F2", ok && !many);
        it[n++].separator_before = true;
        it[n++] = menu_item(F_COPY_PATH, "Copy path", "", true);
        it[n++] = menu_item(F_PERMS, "Permissions...", "", ok && !many);
        it[n++] = menu_item(F_PROPS, "Properties", "", !many);
        it[n] = menu_item(F_DELETE, many ? "Delete selected..." : "Delete...", "Del", ok);
        it[n].danger = true;
        it[n++].separator_before = true;
    } else {
        it[n++] = menu_item(F_UPLOAD_FILES, "Upload files...", "", ok);
        it[n++] = menu_item(F_UPLOAD_FOLDER, "Upload folder...", "", ok);
        it[n++] = menu_item(F_NEW_FOLDER, "New folder...", "", ok);
        it[n] = menu_item(F_REFRESH, "Refresh", "F5", ok);
        it[n++].separator_before = true;
        it[n++] = menu_item(F_TOGGLE_HIDDEN, fv->show_hidden ? "Hide hidden files" : "Show hidden files", "", true);
        it[n++] = menu_item(F_GOTO, "Go to folder...", "", ok);
        it[n++] = menu_item(F_HOME, "Home folder", "", ok);
        const te_xfer *xs = nullptr;
        size_t nx = remote_transfers(fv->remote, &xs);
        if (nx > 0) {
            it[n] = menu_item(F_XFER_CANCEL, "Cancel transfers", "", true);
            it[n++].separator_before = true;
            it[n++] = menu_item(F_XFER_RETRY, "Retry failed transfers", "", true);
            it[n++] = menu_item(F_XFER_CLEAR, "Clear finished transfers", "", true);
        }
    }
    menu_open(fv->menu, it, n, x, y, fv->win_w, fv->win_h, &p);
}

static void name_form(te_files *fv, int purpose, const char *title, const char *ok, const char *value)
{
    form_init(fv->form, purpose, title, ok);
    te_form_field *f = form_add(fv->form, FF_NAME, TE_FIELD_TEXT, "Name", value);
    if (f != nullptr) f->maxlen = 255;
    form_focus_first(fv->form);
}

static void conflict_form(te_files *fv, int purpose, size_t count, const char *where)
{
    form_init(fv->form, purpose, "Files already exist", "Continue");
    (void)str_fmt(fv->form->message, sizeof fv->form->message, "%zu item%s already exist%s in %s.", count,
                  count == 1 ? "" : "s", count == 1 ? "s" : "", where);
    form_set_choices(form_add(fv->form, FF_CHOICE, TE_FIELD_CHOICE, "For those", nullptr), conflict_choices,
                     TE_ARRAY_LEN(conflict_choices), 0);
    form_focus_first(fv->form);
}

static size_t local_conflicts(const char *dir, const char *const *names, size_t n)
{
    size_t c = 0;
    for (size_t i = 0; i < n; i++) {
        char ln[TE_REMOTE_PATH_MAX] = { 0 };
        char lp[TE_REMOTE_PATH_MAX] = { 0 };
        struct stat sb = { 0 };
        if (remote_local_name(names[i], ln, sizeof ln) && remote_join(dir, ln, lp, sizeof lp) == TE_OK &&
            lstat(lp, &sb) == 0) {
            c++;
        }
    }
    return c;
}

static void start_download(te_files *fv, const char *dir)
{
    const char *names[256] = { 0 };
    size_t n = fv->ntarget < 256 ? fv->ntarget : 256;
    for (size_t i = 0; i < n; i++) names[i] = fv->target[i];
    size_t c = local_conflicts(dir, names, n);
    if (c > 0) {
        conflict_form(fv, FP_CONFLICT_DOWN, c, dir);
        /* Remember the destination in the dialog itself. */
        te_form_field *f = form_add(fv->form, FF_LOCAL, TE_FIELD_TEXT, "Destination", dir);
        if (f != nullptr) f->hidden = true;
        return;
    }
    remote_download(fv->remote, names, n, dir, TE_CONFLICT_KEEP_BOTH);
    char t[160] = { 0 };
    (void)str_fmt(t, sizeof t, "Downloading %zu item%s to %s", n, n == 1 ? "" : "s", dir);
    toast(fv, t);
}

static void start_upload(te_files *fv, const char *const *paths, size_t n)
{
    if (!ready(fv) || n == 0) {
        return;
    }
    size_t c = remote_upload_conflicts(fv->remote, paths, n);
    if (c > 0) {
        if (!set_list(&fv->pending_paths, &fv->npending, paths, n)) {
            return;
        }
        conflict_form(fv, FP_CONFLICT_UP, c, remote_cwd(fv->remote));
        return;
    }
    remote_upload(fv->remote, paths, n, TE_CONFLICT_SKIP);
}

void files_menu(te_files *fv, int id)
{
    const te_rentry *e = entry_at(fv, fv->sel);
    char path[TE_REMOTE_PATH_MAX] = { 0 };
    char t[TE_REMOTE_PATH_MAX + 128] = { 0 };
    switch (id) {
    case F_OPEN:
        if (e != nullptr && e->dir) remote_list(fv->remote, e->name);
        break;
    case F_DOWNLOAD:
        if (fv->ntarget > 0) start_download(fv, fv->download_dir);
        break;
    case F_DOWNLOAD_TO:
        fv->dialog_kind = DLG_DOWNLOAD_TO;
        if (fv->host.dialog != nullptr) fv->host.dialog(fv->host.user, TE_DIALOG_CHOOSE_FOLDER, DLG_DOWNLOAD_TO);
        break;
    case F_EDIT:
        if (e != nullptr && !e->dir) {
            remote_edit(fv->remote, e->name);
            (void)str_fmt(t, sizeof t, "Opening %s for editing; saving uploads it", e->name);
            toast(fv, t);
        }
        break;
    case F_RENAME:
        if (e != nullptr) {
            name_form(fv, FP_RENAME, "Rename", "Rename", e->name);
        }
        break;
    case F_DELETE: {
        if (fv->ntarget == 0) break;
        form_init(fv->form, FP_DELETE, "Delete on the server", "Delete");
        if (fv->ntarget == 1) {
            const te_rentry *de = nullptr;
            for (size_t i = 0; i < fv->nrows; i++) {
                const te_rentry *ce = entry_at(fv, (int)i);
                if (ce != nullptr && strcmp(ce->name, fv->target[0]) == 0) de = ce;
            }
            (void)str_fmt(fv->form->message, sizeof fv->form->message,
                          de != nullptr && de->dir ? "Delete the folder \"%s\" and everything in it? This cannot be undone."
                                                   : "Delete \"%s\"? This cannot be undone.",
                          fv->target[0]);
        } else {
            (void)str_fmt(fv->form->message, sizeof fv->form->message,
                          "Delete %zu items (folders with everything in them)? This cannot be undone.", fv->ntarget);
        }
        fv->form->danger = true;
        fv->form->focus = 1;   /* Cancel is the safe default */
        break;
    }
    case F_COPY_PATH:
        if (e != nullptr && remote_join(remote_cwd(fv->remote), e->name, path, sizeof path) == TE_OK &&
            fv->host.copy != nullptr) {
            fv->host.copy(fv->host.user, path);
        }
        break;
    case F_PERMS:
        if (e != nullptr) {
            char mode[16] = { 0 };
            (void)str_fmt(mode, sizeof mode, "%03o", (unsigned)(e->perm & 07777u));
            form_init(fv->form, FP_PERMS, "Permissions", "Apply");
            (void)str_fmt(fv->form->message, sizeof fv->form->message,
                          "Octal mode for \"%s\", e.g. 644 (rw-r--r--) or 755 (rwxr-xr-x).", e->name);
            te_form_field *f = form_add(fv->form, FF_MODE, TE_FIELD_NUMBER, "Mode", mode);
            if (f != nullptr) f->maxlen = 4;
            form_focus_first(fv->form);
        }
        break;
    case F_PROPS:
        if (e != nullptr) {
            char size[32] = { 0 };
            char when[64] = "unknown";
            files_human_size(e->size, size, sizeof size);
            if (e->has_time) {
                time_t tt = (time_t)e->mtime;
                struct tm tmv;
                if (localtime_r(&tt, &tmv) != nullptr) (void)strftime(when, sizeof when, "%Y-%m-%d %H:%M:%S", &tmv);
            }
            form_init(fv->form, FP_PROPS, e->name, "Close");
            (void)str_fmt(fv->form->message, sizeof fv->form->message,
                          "Folder: %s\nType: %s\nSize: %s (%llu bytes)\nModified: %s\nPermissions: %03o\nOwner: uid %u, gid %u",
                          remote_cwd(fv->remote), e->dir ? "folder" : (e->link ? "symbolic link" : "file"), size,
                          (unsigned long long)e->size, when, (unsigned)(e->perm & 07777u), (unsigned)e->uid,
                          (unsigned)e->gid);
            fv->form->focus = 0;
        }
        break;
    case F_UPLOAD_FILES:
        fv->dialog_kind = DLG_UPLOAD_FILES;
        if (fv->host.dialog != nullptr) fv->host.dialog(fv->host.user, TE_DIALOG_OPEN_FILES, DLG_UPLOAD_FILES);
        break;
    case F_UPLOAD_FOLDER:
        fv->dialog_kind = DLG_UPLOAD_FOLDERS;
        if (fv->host.dialog != nullptr) fv->host.dialog(fv->host.user, TE_DIALOG_OPEN_FOLDERS, DLG_UPLOAD_FOLDERS);
        break;
    case F_NEW_FOLDER:
        name_form(fv, FP_NEW_FOLDER, "New folder", "Create", "");
        break;
    case F_REFRESH:
        remote_refresh(fv->remote);
        break;
    case F_TOGGLE_HIDDEN:
        fv->show_hidden = !fv->show_hidden;
        rebuild(fv);
        break;
    case F_GOTO: {
        form_init(fv->form, FP_GOTO, "Go to folder", "Go");
        te_form_field *f = form_add(fv->form, FF_PATH, TE_FIELD_TEXT, "Folder", remote_cwd(fv->remote));
        if (f != nullptr) f->maxlen = TE_REMOTE_PATH_MAX - 1;
        form_focus_first(fv->form);
        break;
    }
    case F_HOME:          remote_home(fv->remote); break;
    case F_XFER_CANCEL:   remote_cancel_all(fv->remote); break;
    case F_XFER_RETRY:    remote_retry_failed(fv->remote); break;
    case F_XFER_CLEAR:    remote_clear_finished(fv->remote); break;
    case F_RECONNECT:     remote_retry_failed(fv->remote); break;
    case F_SCP_UP:
        fv->dialog_kind = DLG_SCP_UP;
        if (fv->host.dialog != nullptr) fv->host.dialog(fv->host.user, TE_DIALOG_OPEN_FILES, DLG_SCP_UP);
        break;
    case F_SCP_DOWN: {
        form_init(fv->form, FP_SCP_DOWN, "Download a file (SCP)", "Download");
        (void)str_fmt(fv->form->message, sizeof fv->form->message,
                      "Path on %s, e.g. flash:/running-config or /var/log/messages. It is saved in %s.",
                      remote_title(fv->remote), fv->download_dir);
        te_form_field *f = form_add(fv->form, FF_PATH, TE_FIELD_TEXT, "Remote path", "");
        if (f != nullptr) f->maxlen = 1024;
        form_focus_first(fv->form);
        break;
    }
    default:
        break;
    }
}

void files_submit(te_files *fv)
{
    te_form *f = fv->form;
    const te_rentry *e = entry_at(fv, fv->sel);
    switch (f->purpose) {
    case FP_RENAME: {
        const char *to = form_value(f, FF_NAME);
        if (to[0] == '\0' || strchr(to, '/') != nullptr || strcmp(to, ".") == 0 || strcmp(to, "..") == 0) {
            form_set_error(f, "A name, without '/'");
            return;
        }
        if (e != nullptr && strcmp(e->name, to) != 0) remote_rename(fv->remote, e->name, to);
        break;
    }
    case FP_NEW_FOLDER: {
        const char *nm = form_value(f, FF_NAME);
        if (nm[0] == '\0' || strchr(nm, '/') != nullptr || strcmp(nm, ".") == 0 || strcmp(nm, "..") == 0) {
            form_set_error(f, "A name, without '/'");
            return;
        }
        remote_mkdir(fv->remote, nm);
        break;
    }
    case FP_DELETE: {
        const char *names[256] = { 0 };
        size_t n = fv->ntarget < 256 ? fv->ntarget : 256;
        for (size_t i = 0; i < n; i++) names[i] = fv->target[i];
        remote_delete(fv->remote, names, n);
        break;
    }
    case FP_PERMS: {
        const char *m = form_value(f, FF_MODE);
        unsigned v = 0;
        bool ok = m[0] != '\0' && strlen(m) <= 4;
        for (const char *s = m; ok && *s != '\0'; s++) {
            ok = *s >= '0' && *s <= '7';
            v = v * 8u + (unsigned)(*s - '0');
        }
        if (!ok) {
            form_set_error(f, "Use octal digits 0-7, e.g. 644");
            return;
        }
        if (e != nullptr) remote_chmod(fv->remote, e->name, v);
        break;
    }
    case FP_GOTO: {
        const char *p = form_value(f, FF_PATH);
        if (p[0] == '\0') {
            form_set_error(f, "Enter a folder");
            return;
        }
        remote_list(fv->remote, p);
        break;
    }
    case FP_CONFLICT_DOWN: {
        size_t c = TE_MIN(form_choice(f, FF_CHOICE), TE_ARRAY_LEN(conflict_map) - 1);
        const char *names[256] = { 0 };
        size_t n = fv->ntarget < 256 ? fv->ntarget : 256;
        for (size_t i = 0; i < n; i++) names[i] = fv->target[i];
        char dir[1024] = { 0 };
        (void)str_copy(dir, sizeof dir, form_value(f, FF_LOCAL));
        remote_download(fv->remote, names, n, dir[0] != '\0' ? dir : fv->download_dir, conflict_map[c]);
        break;
    }
    case FP_CONFLICT_UP: {
        size_t c = TE_MIN(form_choice(f, FF_CHOICE), TE_ARRAY_LEN(conflict_map) - 1);
        remote_upload(fv->remote, (const char *const *)fv->pending_paths, fv->npending, conflict_map[c]);
        free_list(&fv->pending_paths, &fv->npending);
        break;
    }
    case FP_PROMPT: {
        te_remote *r = fv->prompt_remote;
        fv->prompt_remote = nullptr;
        const te_form_field *sf = form_cget(f, FF_SECRET);
        if (r != nullptr) {
            remote_answer(r, sf != nullptr ? sf->value : "yes");
        }
        form_wipe(f);   /* the password is gone from memory as soon as it is sent */
        break;
    }
    case FP_SCP_DOWN: {
        const char *p = form_value(f, FF_PATH);
        char quoted[TE_REMOTE_PATH_MAX] = { 0 };
        if (!remote_scp_quote(p, quoted, sizeof quoted)) {
            form_set_error(f, "That path cannot be used (no newlines, not starting with '-')");
            return;
        }
        remote_scp_download(fv->remote, p, fv->download_dir);
        break;
    }
    case FP_SCP_UP: {
        const char *p = form_value(f, FF_PATH);
        char quoted[TE_REMOTE_PATH_MAX] = { 0 };
        if (!remote_scp_quote(p, quoted, sizeof quoted)) {
            form_set_error(f, "That path cannot be used (no newlines, not starting with '-')");
            return;
        }
        if (fv->npending > 0) remote_scp_upload(fv->remote, fv->pending_paths[0], p);
        free_list(&fv->pending_paths, &fv->npending);
        break;
    }
    case FP_PROPS:
    default:
        break;
    }
    f->open = false;
}

void files_cancel(te_files *fv)
{
    if (fv->form->purpose == FP_PROMPT) {
        te_remote *r = fv->prompt_remote;
        fv->prompt_remote = nullptr;
        form_wipe(fv->form);
        if (r != nullptr) {
            remote_answer(r, nullptr);
        }
    } else if (fv->form->purpose == FP_CONFLICT_UP || fv->form->purpose == FP_SCP_UP) {
        free_list(&fv->pending_paths, &fv->npending);
    }
}

void files_prompt(te_files *fv, te_remote *r, te_prompt_kind kind, const char *text)
{
    char title[160] = { 0 };
    (void)str_fmt(title, sizeof title, "%s: %s", remote_title(r),
                  kind == TE_PROMPT_CONFIRM ? "unknown host key" : "file browser login");
    form_init(fv->form, FP_PROMPT, title, kind == TE_PROMPT_CONFIRM ? "Yes, connect" : "OK");
    fv->prompt_remote = r;
    if (kind == TE_PROMPT_CONFIRM) {
        (void)str_copy(fv->form->message, sizeof fv->form->message, text);
        fv->form->focus = 1;   /* trusting a new host key must be a deliberate choice */
        return;
    }
    (void)str_copy(fv->form->message, sizeof fv->form->message,
                   "The file browser needs its own login to this server (it could not reuse the terminal's).");
    te_form_field *fl = form_add(fv->form, FF_SECRET, kind == TE_PROMPT_SECRET ? TE_FIELD_SECRET : TE_FIELD_TEXT, "", "");
    if (fl != nullptr) {
        size_t n = strlen(text);
        (void)str_copy(fl->label, sizeof fl->label, n < sizeof fl->label ? text : "Answer");
        fl->maxlen = 1000;
    }
    form_focus_first(fv->form);
}

void files_dialog_result(te_files *fv, int request, char **paths, size_t n, bool failed, const char *error)
{
    if (request != fv->dialog_kind) {
        return;
    }
    fv->dialog_kind = DLG_NONE;
    if (failed) {
        char t[256] = { 0 };
        (void)str_fmt(t, sizeof t, "No file chooser available (%s). Drag files onto the Files tab instead.",
                      error != nullptr && error[0] != '\0' ? error : "install xdg-desktop-portal or zenity");
        toast(fv, t);
        return;
    }
    if (n == 0) {
        return;   /* cancelled */
    }
    switch (request) {
    case DLG_UPLOAD_FILES:
    case DLG_UPLOAD_FOLDERS:
        start_upload(fv, (const char *const *)paths, n);
        break;
    case DLG_DOWNLOAD_TO:
        if (fv->ntarget > 0) start_download(fv, paths[0]);
        break;
    case DLG_SCP_UP: {
        if (!set_list(&fv->pending_paths, &fv->npending, (const char *const *)paths, 1)) break;
        const char *base = strrchr(paths[0], '/');
        form_init(fv->form, FP_SCP_UP, "Upload a file (SCP)", "Upload");
        (void)str_fmt(fv->form->message, sizeof fv->form->message,
                      "Destination on %s, e.g. flash:/%s", remote_title(fv->remote), base != nullptr ? base + 1 : "file");
        char def[TE_REMOTE_PATH_MAX] = { 0 };
        (void)str_fmt(def, sizeof def, "%s", base != nullptr ? base + 1 : "");
        te_form_field *f = form_add(fv->form, FF_PATH, TE_FIELD_TEXT, "Remote path", def);
        if (f != nullptr) f->maxlen = 1024;
        form_focus_first(fv->form);
        break;
    }
    default:
        break;
    }
}

void files_drop(te_files *fv, const char *path)
{
    if (path == nullptr) {
        return;
    }
    if (!ready(fv)) {
        toast(fv, remote_state(fv->remote) == TE_RS_NO_SFTP ? "This server has no SFTP: use Upload a file (SCP)"
                                                            : "Open the Files tab of a connected SSH session to upload");
        return;
    }
    if (fv->form->open) {
        /* A conflict question is already open: add this file to the same batch. */
        if (fv->form->purpose == FP_CONFLICT_UP) {
            void *tmp = fv->pending_paths;
            if (te_realloc_array(&tmp, fv->npending, fv->npending + 1, sizeof *fv->pending_paths) == TE_OK) {
                fv->pending_paths = tmp;
                fv->pending_paths[fv->npending] = str_dup(path);
                if (fv->pending_paths[fv->npending] != nullptr) fv->npending++;
            }
        }
        return;
    }
    const char *one[1] = { path };
    start_upload(fv, one, 1);
}

void files_follow(te_files *fv, const char *path)
{
    if (fv->follow && ready(fv) && path != nullptr && path[0] == '/' && strcmp(path, remote_cwd(fv->remote)) != 0) {
        remote_list(fv->remote, path);
    }
}

/* ------------------------------------------------------------------------- */
/* Input */

static void capture_targets(te_files *fv)
{
    const char *names[256] = { 0 };
    size_t n = collect_names(fv, names, 256);
    (void)set_list(&fv->target, &fv->ntarget, names, n);
}

static void activate(te_files *fv, int row)
{
    const te_rentry *e = entry_at(fv, row);
    if (e == nullptr || !ready(fv)) {
        return;
    }
    if (e->dir || e->link) {
        remote_list(fv->remote, e->name);   /* a link to a folder opens; to a file it fails cleanly */
    } else {
        capture_targets(fv);
        start_download(fv, fv->download_dir);
    }
}

bool files_button(te_files *fv, float x, float y, int button, bool down, uint8_t clicks, uint32_t mods)
{
    fv->mouse_x = x;
    fv->mouse_y = y;
    if (!down) {
        return true;
    }
    te_remote_state st = remote_state(fv->remote);
    if (y < fv->y + toolbar_h(fv)) {
        if (button != 0 || !ready(fv)) return true;
        switch (toolbar_hit(fv, x)) {
        case 0:
            if (strcmp(remote_cwd(fv->remote), "/") == 0) toast(fv, "Already at the top folder");
            else remote_list(fv->remote, "..");
            break;
        case 1: remote_home(fv->remote); toast(fv, "Home folder"); break;
        case 2: remote_refresh(fv->remote); toast(fv, "Refreshed"); break;
        case 3:
            fv->show_hidden = !fv->show_hidden;
            rebuild(fv);
            toast(fv, fv->show_hidden ? "Showing hidden files" : "Hiding hidden files");
            break;
        case 4:
            fv->follow = !fv->follow;
            toast(fv, fv->follow ? "Following the terminal's folder" : "Not following the terminal's folder");
            break;
        case 5: files_menu(fv, F_GOTO); break;
        default: break;
        }
        return true;
    }
    if (y >= list_bottom(fv)) {
        /* Transfer strip: right-click (or the x) offers cancel / retry / clear. */
        if (button == 2 || (button == 0 && x > fv->x + fv->w - fv->cell_w * 3.0f)) {
            te_menu_item it[3] = {
                menu_item(F_XFER_CANCEL, "Cancel transfers", "", true),
                menu_item(F_XFER_RETRY, "Retry failed transfers", "", true),
                menu_item(F_XFER_CLEAR, "Clear finished transfers", "", true),
            };
            te_painter p = { .cell_w = fv->cell_w, .cell_h = fv->cell_h, .scale = fv->scale };
            menu_open(fv->menu, it, 3, x, y, fv->win_w, fv->win_h, &p);
        }
        return true;
    }
    if (st == TE_RS_FAILED && button == 0) {
        remote_retry_failed(fv->remote);   /* the "Retry" message covers the list */
        return true;
    }
    if (st == TE_RS_NO_SFTP || st == TE_RS_IDLE || st == TE_RS_CONNECTING) {
        if (st == TE_RS_NO_SFTP && (button == 0 || button == 2)) {
            open_menu(fv, -1, x, y);
        }
        return true;
    }
    int row = row_at(fv, y);
    if (button == 2) {
        if (row >= 0 && !fv->marked[row]) {
            for (size_t i = 0; i < fv->nrows; i++) fv->marked[i] = false;
            select_row(fv, row);
        }
        capture_targets(fv);
        open_menu(fv, row, x, y);   /* right-click only ever opens a menu */
        return true;
    }
    if (button != 0 || row < 0) {
        if (button == 0 && row < 0) {
            for (size_t i = 0; i < fv->nrows; i++) fv->marked[i] = false;
        }
        return true;
    }
    if ((mods & TE_MOD_CTRL) != 0) {
        fv->marked[row] = !fv->marked[row];
        if (fv->sel >= 0 && !fv->marked[fv->sel] && fv->sel != row) fv->marked[fv->sel] = true;
        select_row(fv, row);
        fv->anchor = row;
        return true;
    }
    if ((mods & TE_MOD_SHIFT) != 0 && fv->anchor >= 0) {
        int a = TE_MIN(fv->anchor, row);
        int b = TE_MAX(fv->anchor, row);
        for (size_t i = 0; i < fv->nrows; i++) fv->marked[i] = (int)i >= a && (int)i <= b;
        select_row(fv, row);
        return true;
    }
    for (size_t i = 0; i < fv->nrows; i++) fv->marked[i] = false;
    select_row(fv, row);
    fv->anchor = row;
    if (clicks >= 2) {
        activate(fv, row);
    }
    return true;
}

void files_motion(te_files *fv, float x, float y)
{
    fv->mouse_x = x;
    fv->mouse_y = y;
    fv->hover = row_at(fv, y);
}

bool files_wheel(te_files *fv, float dy)
{
    size_t fit = rows_fit(fv);
    size_t max = fv->nrows > fit ? fv->nrows - fit : 0;
    int lines = (int)(dy * 3.0f);
    if (lines > 0) {
        fv->scroll = (size_t)lines > fv->scroll ? 0 : fv->scroll - (size_t)lines;
    } else if (lines < 0) {
        fv->scroll += (size_t)(-lines);
        if (fv->scroll > max) fv->scroll = max;
    }
    return true;
}

bool files_key(te_files *fv, te_key key, uint32_t mods)
{
    int page = (int)TE_MAX(rows_fit(fv), (size_t)1);
    switch (key) {
    case TE_KEY_UP:        select_row(fv, fv->sel > 0 ? fv->sel - 1 : 0); return true;
    case TE_KEY_DOWN:      select_row(fv, fv->sel + 1 < (int)fv->nrows ? fv->sel + 1 : fv->sel); return true;
    case TE_KEY_PAGE_UP:   select_row(fv, TE_MAX(fv->sel - page, 0)); return true;
    case TE_KEY_PAGE_DOWN: select_row(fv, TE_MIN(fv->sel + page, (int)fv->nrows - 1)); return true;
    case TE_KEY_HOME:      select_row(fv, 0); return true;
    case TE_KEY_END:       select_row(fv, (int)fv->nrows - 1); return true;
    case TE_KEY_ENTER:     activate(fv, fv->sel); return true;
    case TE_KEY_BACKSPACE: if (ready(fv)) remote_list(fv->remote, ".."); return true;
    case TE_KEY_F5:        if (ready(fv)) remote_refresh(fv->remote); return true;
    case TE_KEY_F2:        files_menu(fv, F_RENAME); return true;
    case TE_KEY_DELETE:
        if (ready(fv)) {
            capture_targets(fv);
            files_menu(fv, F_DELETE);   /* asks first */
        }
        return true;
    case TE_KEY_F10:
        if ((mods & TE_MOD_SHIFT) != 0) {
            capture_targets(fv);
            size_t r = fv->sel >= 0 ? (size_t)fv->sel : 0;
            float my = list_top(fv) + (float)(r - TE_MIN(r, fv->scroll) + 1) * row_h(fv);
            open_menu(fv, fv->sel, fv->x + fv->cell_w * 4.0f, my);
        }
        return true;
    default:
        return false;
    }
}

/* ------------------------------------------------------------------------- */
/* Drawing */

static void draw_message(const te_files *fv, const te_painter *p, const te_ui_colors *c, const char *const *lines,
                         size_t n, uint32_t first_color)
{
    size_t cols = fv->w / p->cell_w > 3.0f ? (size_t)(fv->w / p->cell_w) - 2 : 1;
    float y = list_top(fv) + p->cell_h * 0.5f;
    for (size_t i = 0; i < n; i++) {
        /* Wrap long lines at the panel width. */
        const char *s = lines[i];
        do {
            size_t fit = ui_text_fit(s, cols);
            if (fit == 0) fit = strlen(s);
            if (s[fit] != '\0' && s[fit] != ' ') {
                size_t sp = fit;
                while (sp > 0 && s[sp] != ' ') sp--;
                if (sp > 0) fit = sp;   /* break at a space, not inside a word */
            }
            char buf[512] = { 0 };
            (void)str_copy_n(buf, sizeof buf, s, fit);
            ui_text(p, TE_LAYER_BASE, fv->x + p->cell_w, y, buf, cols, i == 0 ? first_color : c->muted, i == 0);
            s += fit;
            while (*s == ' ') s++;
            y += p->cell_h * 1.25f;
        } while (*s != '\0');
        y += p->cell_h * 0.3f;
    }
}

void files_draw(const te_files *fv, const te_painter *p, const te_ui_colors *c)
{
    float s = p->scale;
    size_t cols = fv->w / p->cell_w > 4.0f ? (size_t)(fv->w / p->cell_w) : 4;
    te_remote_state st = remote_state(fv->remote);

    /* Toolbar */
    float th = toolbar_h(fv);
    ui_rect(p, TE_LAYER_BASE, fv->x, fv->y, fv->w, th, c->bg_alt, 1.0f);
    /* ↑ ⌂ ↻ .* ⇵ : all from blocks that common monospace fonts cover */
    static const char *const icons[] = { "\xe2\x86\x91", "\xe2\x8c\x82", "\xe2\x86\xbb", ".*", "\xe2\x87\xb5" };
    float bw = p->cell_w * 3.0f;
    float ty = fv->y + (th - p->cell_h) / 2.0f;
    for (int i = 0; i < 5; i++) {
        bool on = (i == 3 && fv->show_hidden) || (i == 4 && fv->follow);
        bool hot = fv->mouse_y >= fv->y && fv->mouse_y < fv->y + th && toolbar_hit(fv, fv->mouse_x) == i &&
                   fv->mouse_x >= fv->x && fv->mouse_x < fv->x + fv->w;
        if (hot) ui_rect(p, TE_LAYER_BASE, fv->x + (float)i * bw, fv->y, bw, th, c->hover, 1.0f);
        size_t ic = ui_text_cols(icons[i]);
        ui_text(p, TE_LAYER_BASE, fv->x + (float)i * bw + (bw - (float)ic * p->cell_w) / 2.0f, ty, icons[i], 2,
                on ? c->accent : c->fg, false);
    }
    /* Path, showing its end when it does not fit. */
    const char *cwd = st == TE_RS_READY ? remote_cwd(fv->remote) : (fv->remote != nullptr ? remote_title(fv->remote) : "");
    size_t pcols = cols > 17 ? cols - 16 : 1;
    size_t plen = ui_text_cols(cwd);
    const char *shown = cwd;
    while (plen > pcols && *shown != '\0') {
        size_t one = ui_text_fit(shown, 1);
        shown += one > 0 ? one : 1;
        plen = ui_text_cols(shown);
    }
    ui_text(p, TE_LAYER_BASE, fv->x + 5.0f * bw + p->cell_w * 0.5f, ty, shown, pcols, c->fg, false);

    /* Tooltip for the toolbar button under the pointer. */
    if (fv->mouse_y >= fv->y && fv->mouse_y < fv->y + th && fv->mouse_x >= fv->x && fv->mouse_x < fv->x + fv->w) {
        int hit = toolbar_hit(fv, fv->mouse_x);
        const char *tips[] = {
            "Up one folder (Backspace)", "Home folder", "Refresh (F5)",
            fv->show_hidden ? "Hide hidden files" : "Show hidden files",
            fv->follow ? "Following the terminal's folder (click to stop)" : "Follow the terminal's folder",
            "Go to folder...",
        };
        if (hit >= 0 && (size_t)hit < TE_ARRAY_LEN(tips)) {
            size_t tc = ui_text_cols(tips[hit]);
            float tw = (float)(tc + 2) * p->cell_w;
            float tx = fv->x + (float)TE_MIN(hit, 5) * bw;
            if (tx + tw > fv->win_w) tx = TE_MAX(0.0f, fv->win_w - tw);
            float tyy = fv->y + th + 2.0f * s;
            ui_rect(p, TE_LAYER_OVERLAY, tx, tyy, tw, p->cell_h * 1.4f, c->bg_alt, 1.0f);
            ui_frame(p, TE_LAYER_OVERLAY, tx, tyy, tw, p->cell_h * 1.4f, c->border);
            ui_text(p, TE_LAYER_OVERLAY, tx + p->cell_w, tyy + p->cell_h * 0.2f, tips[hit], tc, c->fg, false);
        }
    }

    /* Column header */
    float hy = fv->y + th;
    ui_text(p, TE_LAYER_BASE, fv->x + p->cell_w * 3.0f, hy + (header_h(fv) - p->cell_h) / 2.0f, "Name", cols, c->muted, false);
    ui_text(p, TE_LAYER_BASE, fv->x + fv->w - p->cell_w * 7.0f, hy + (header_h(fv) - p->cell_h) / 2.0f, "Size", 4, c->muted, false);
    ui_rect(p, TE_LAYER_BASE, fv->x, hy + header_h(fv) - s, fv->w, s, c->border, 1.0f);

    /* States without a listing */
    if (!fv->ssh_pane || fv->remote == nullptr) {
        static const char *const msg[] = {
            "No SSH session selected",
            "Click into an SSH session to browse its files here. Telnet, serial and local sessions have no file transfer.",
        };
        draw_message(fv, p, c, msg, TE_ARRAY_LEN(msg), c->fg);
        return;
    }
    if (st == TE_RS_CONNECTING || st == TE_RS_IDLE) {
        char line[200] = { 0 };
        (void)str_fmt(line, sizeof line, "Connecting to %s...", remote_title(fv->remote));
        const char *msg[] = { line, remote_prompting(fv->remote) ? "Waiting for the login dialog." : "Using the terminal's login when possible." };
        draw_message(fv, p, c, msg, 2, c->fg);
        return;
    }
    if (st == TE_RS_FAILED) {
        const char *msg[] = { "Could not open the file browser", remote_error(fv->remote), "Click here to try again." };
        draw_message(fv, p, c, msg, 3, c->danger);
        return;
    }
    if (st == TE_RS_NO_SFTP) {
        const char *msg[] = { "No SFTP on this server", remote_error(fv->remote),
                              "Click or right-click here to upload or download a file with SCP." };
        draw_message(fv, p, c, msg, 3, c->fg);
    } else {
        /* The listing */
        const te_rentry *ents = nullptr;
        size_t ne = remote_entries(fv->remote, &ents);
        size_t fit = rows_fit(fv);
        float top = list_top(fv);
        float rh = row_h(fv);
        if (fv->nrows == 0) {
            const char *msg[] = { remote_loading(fv->remote) ? "Loading..." : "This folder is empty",
                                  "Right-click for Upload, New folder and more. You can also drop files here." };
            draw_message(fv, p, c, msg, 2, c->fg);
        }
        for (size_t r = 0; r < fit && r + fv->scroll < fv->nrows; r++) {
            size_t row = r + fv->scroll;
            if (fv->rows[row] >= ne) continue;
            const te_rentry *e = &ents[fv->rows[row]];
            float ry = top + (float)r * rh;
            bool sel = (int)row == fv->sel;
            bool mk = fv->marked[row];
            if (sel || mk) {
                ui_rect(p, TE_LAYER_BASE, fv->x, ry, fv->w, rh, c->sel_bg, sel ? 1.0f : 0.6f);
            } else if ((int)row == fv->hover) {
                ui_rect(p, TE_LAYER_BASE, fv->x, ry, fv->w, rh, c->hover, 0.6f);
            }
            uint32_t fg = sel || mk ? c->sel_fg : c->fg;
            float tyy = ry + (rh - p->cell_h) / 2.0f;
            const char *icon = e->dir ? "\xe2\x96\xb8" : (e->link ? "\xe2\x86\xaa" : "\xc2\xb7");   /* ▸ ↪ · */
            ui_text(p, TE_LAYER_BASE, fv->x + p->cell_w, tyy, icon, 1, e->dir ? c->accent : c->muted, false);
            char size[16] = { 0 };
            if (!e->dir && e->has_size) files_human_size(e->size, size, sizeof size);
            size_t sc = ui_text_cols(size);
            size_t room = cols > sc + 6 ? cols - sc - 6 : 1;
            ui_text(p, TE_LAYER_BASE, fv->x + p->cell_w * 3.0f, tyy, e->name, room, fg, e->dir);
            if (sc > 0) {
                ui_text(p, TE_LAYER_BASE, fv->x + fv->w - (float)(sc + 1) * p->cell_w, tyy, size, sc,
                        sel || mk ? c->sel_fg : c->muted, false);
            }
        }
        if (fv->nrows > fit && fit > 0) {
            float track = list_bottom(fv) - top;
            float h = TE_MAX(track * (float)fit / (float)fv->nrows, p->cell_h);
            float y = top + (track - h) * (float)fv->scroll / (float)(fv->nrows - fit);
            ui_rect(p, TE_LAYER_BASE, fv->x + fv->w - 4.0f * s, y, 3.0f * s, h, c->muted, 0.5f);
        }
    }

    /* Transfer strip */
    float sh = strip_h(fv);
    if (sh <= 0.0f) {
        return;
    }
    float sy = list_bottom(fv);
    ui_rect(p, TE_LAYER_BASE, fv->x, sy, fv->w, sh, c->bg_alt, 1.0f);
    ui_rect(p, TE_LAYER_BASE, fv->x, sy, fv->w, s, c->border, 1.0f);
    const te_xfer *x = nullptr;
    size_t nx = remote_transfers(fv->remote, &x);
    size_t queued = 0, failed = 0, done = 0;
    const te_xfer *act = nullptr;
    for (size_t i = 0; i < nx; i++) {
        if (x[i].edit) continue;
        if (x[i].state == TE_XS_ACTIVE) act = &x[i];
        queued += x[i].state == TE_XS_QUEUED && (x[i].kind == TE_XK_DOWNLOAD || x[i].kind == TE_XK_UPLOAD);
        failed += x[i].state == TE_XS_FAILED;
        done += x[i].state == TE_XS_DONE && (x[i].kind == TE_XK_DOWNLOAD || x[i].kind == TE_XK_UPLOAD);
    }
    float l1 = sy + p->cell_h * 0.3f;
    float l2 = sy + p->cell_h * 1.75f;
    size_t tcols = cols > 5 ? cols - 5 : 1;
    char line[300] = { 0 };
    if (act != nullptr) {
        const char *nm = act->remote != nullptr ? strrchr(act->remote, '/') : nullptr;
        nm = nm != nullptr ? nm + 1 : (act->remote != nullptr ? act->remote : "");
        bool down = act->kind == TE_XK_DOWNLOAD || act->kind == TE_XK_SCAN_DOWN || act->kind == TE_XK_SCP_DOWN;
        const char *verb = act->kind == TE_XK_SCAN_DELETE || act->kind == TE_XK_REMOVE || act->kind == TE_XK_RMDIR ? "Deleting"
                         : act->kind == TE_XK_SCAN_DOWN || act->kind == TE_XK_MKDIR ? "Preparing" : down ? "\xe2\x86\x93" : "\xe2\x86\x91";
        (void)str_fmt(line, sizeof line, "%s %s", verb, nm);
        ui_text(p, TE_LAYER_BASE, fv->x + p->cell_w, l1, line, tcols, c->fg, false);
        if (act->size > 0 && (act->kind == TE_XK_DOWNLOAD || act->kind == TE_XK_UPLOAD)) {
            float frac = (float)((double)act->done / (double)act->size);
            frac = TE_CLAMP(frac, 0.0f, 1.0f);
            float bwid = fv->w - p->cell_w * 2.0f;
            ui_rect(p, TE_LAYER_BASE, fv->x + p->cell_w, l1 + p->cell_h * 1.1f, bwid, 3.0f * s, c->border, 1.0f);
            ui_rect(p, TE_LAYER_BASE, fv->x + p->cell_w, l1 + p->cell_h * 1.1f, bwid * frac, 3.0f * s, c->accent, 1.0f);
        }
    } else {
        (void)str_fmt(line, sizeof line, "%zu transfer%s finished", done, done == 1 ? "" : "s");
        ui_text(p, TE_LAYER_BASE, fv->x + p->cell_w, l1, line, tcols, c->fg, false);
    }
    if (failed > 0) {
        (void)str_fmt(line, sizeof line, "%zu queued \xc2\xb7 %zu failed (right-click: retry)", queued, failed);
    } else {
        (void)str_fmt(line, sizeof line, "%zu queued", queued);
    }
    ui_text(p, TE_LAYER_BASE, fv->x + p->cell_w, l2, line, tcols, failed > 0 ? c->danger : c->muted, false);
    ui_text(p, TE_LAYER_BASE, fv->x + fv->w - p->cell_w * 2.0f, l1, "\xc3\x97", 1, c->muted, false);   /* × */
}
