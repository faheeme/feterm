#include "sessions/store.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include "config/ini.h"
#include "util/log.h"
#include "util/mem.h"
#include "util/os.h"
#include "util/str.h"

#define FOLDER_FILE    "_folder.ini"
#define TMP_PREFIX     ".feterm-tmp-"
#define FILE_TEXT_MAX  16384
#define SCAN_MAX_NAMES 4096           /* entries considered per folder */

typedef struct name_ent {
    char name[TE_SESS_NAME_MAX + 1];
    bool dir;
} name_ent;

/* ------------------------------------------------------------------------- */
/* Paths and file descriptors */

te_err store_join(const char *folder, const char *name, char *out, size_t outsz)
{
    if (folder == nullptr || folder[0] == '\0') {
        return str_copy(out, outsz, name);
    }
    return str_fmt(out, outsz, "%s/%s", folder, name);
}

static int open_root(const te_store *st)
{
    return open(st->root, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
}

/*
 * Open the folder rel ("" = root) one component at a time, never following
 * a symlink. With create, missing components are made (mode 0700).
 * Returns a directory fd or -1.
 */
static int open_rel(const te_store *st, const char *rel, bool create)
{
    if (!session_folder_valid(rel)) {
        errno = EINVAL;
        return -1;
    }
    int fd = open_root(st);
    if (fd < 0 || rel[0] == '\0') {
        return fd;
    }
    char buf[TE_SESS_FOLDER_MAX + 1] = { 0 };
    if (str_copy(buf, sizeof buf, rel) != TE_OK) {
        close(fd);
        errno = ENAMETOOLONG;
        return -1;
    }
    char *save = nullptr;
    for (char *part = strtok_r(buf, "/", &save); part != nullptr; part = strtok_r(nullptr, "/", &save)) {
        if (create && mkdirat(fd, part, 0700) != 0 && errno != EEXIST) {
            close(fd);
            return -1;
        }
        int next = openat(fd, part, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
        close(fd);
        if (next < 0) {
            return -1;
        }
        fd = next;
    }
    return fd;
}

static te_err errno_to_err(int e)
{
    switch (e) {
    case ENOENT:  return TE_ERR_NOTFOUND;
    case EEXIST:
    case ENOTEMPTY: return TE_ERR_EXISTS;
    case EACCES:
    case EPERM:   return TE_ERR_PERM;
    case ENOMEM:  return TE_ERR_NOMEM;
    case EINVAL:
    case ELOOP:
    case ENOTDIR: return TE_ERR_INVAL;
    default:      return TE_ERR_IO;
    }
}

static te_err file_name(const char *name, char *out, size_t outsz)
{
    return str_fmt(out, outsz, "%s.ini", name);
}

/* Write data to dirfd/name atomically with mode 0600. */
static te_err write_atomic(int dirfd, const char *name, const char *data, size_t len)
{
    static unsigned counter = 0;
    char tmp[64] = { 0 };
    if (str_fmt(tmp, sizeof tmp, TMP_PREFIX "%ld-%u", (long)getpid(), counter++) != TE_OK) {
        return TE_ERR_TRUNC;
    }
    int fd = openat(dirfd, tmp, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0) {
        return errno_to_err(errno);
    }
    te_err rc = TE_OK;
    size_t done = 0;
    while (done < len) {
        ssize_t n = write(fd, data + done, len - done);
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n <= 0) {
            rc = TE_ERR_IO;
            break;
        }
        done += (size_t)n;
    }
    if (rc == TE_OK && fsync(fd) != 0) {
        rc = TE_ERR_IO;
    }
    if (close(fd) != 0 && rc == TE_OK) {
        rc = TE_ERR_IO;
    }
    if (rc == TE_OK && renameat(dirfd, tmp, dirfd, name) != 0) {
        rc = errno_to_err(errno);
    }
    if (rc != TE_OK) {
        (void)unlinkat(dirfd, tmp, 0);
    }
    return rc;
}

static bool exists_at(int dirfd, const char *name)
{
    struct stat sb = { 0 };
    return fstatat(dirfd, name, &sb, AT_SYMLINK_NOFOLLOW) == 0;
}

/* ------------------------------------------------------------------------- */
/* Loading */

static void free_nodes(te_store *st)
{
    for (size_t i = 0; i < st->len; i++) {
        TE_FREE(st->nodes[i].sess);
    }
    TE_FREE(st->nodes);
    st->len = 0;
    st->cap = 0;
}

static te_store_node *push_node(te_store *st)
{
    if (st->len >= TE_STORE_MAX_NODES) {
        st->truncated = true;
        return nullptr;
    }
    if (st->len == st->cap) {
        size_t ncap = st->cap == 0 ? 64 : st->cap * 2;
        if (ncap > TE_STORE_MAX_NODES) {
            ncap = TE_STORE_MAX_NODES;
        }
        void *tmp = st->nodes;
        if (te_realloc_array(&tmp, st->cap, ncap, sizeof *st->nodes) != TE_OK) {
            return nullptr;
        }
        st->nodes = tmp;
        st->cap = ncap;
    }
    te_store_node *n = &st->nodes[st->len++];
    memset(n, 0, sizeof *n);
    n->parent = -1;
    return n;
}

static int cmp_names(const void *a, const void *b)
{
    const name_ent *x = a;
    const name_ent *y = b;
    if (x->dir != y->dir) {
        return x->dir ? -1 : 1;   /* folders first */
    }
    int c = strcasecmp(x->name, y->name);
    return c != 0 ? c : strcmp(x->name, y->name);
}

typedef struct parse_ctx {
    te_session         *s;
    te_folder_defaults *d;
} parse_ctx;

static te_err parse_cb(void *user, const char *section, const char *key, const char *value, const char *path,
                       unsigned line)
{
    parse_ctx *c = user;
    te_err rc = c->s != nullptr ? session_parse_kv(c->s, section, key, value)
                                : folder_parse_kv(c->d, section, key, value);
    if (rc != TE_OK) {
        LOG_W("%s:%u: invalid value for '%s'", path, line, key);
    }
    return TE_OK;   /* keep going: one bad line should not hide the session */
}

/* Read the names in dirfd worth showing: folders and *.ini sessions, sorted. */
static te_err list_dir(int dirfd, name_ent **out, size_t *count)
{
    *out = nullptr;
    *count = 0;
    int dfd = openat(dirfd, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (dfd < 0) {
        return errno_to_err(errno);
    }
    DIR *d = fdopendir(dfd);
    if (d == nullptr) {
        close(dfd);
        return TE_ERR_IO;
    }
    name_ent *list = nullptr;
    size_t len = 0;
    size_t cap = 0;
    te_err rc = TE_OK;
    struct dirent *e = nullptr;
    while ((e = readdir(d)) != nullptr && len < SCAN_MAX_NAMES) {
        const char *n = e->d_name;
        if (n[0] == '.' || strcmp(n, FOLDER_FILE) == 0) {
            continue;
        }
        struct stat sb = { 0 };
        if (fstatat(dirfd, n, &sb, AT_SYMLINK_NOFOLLOW) != 0) {
            continue;
        }
        name_ent ent = { 0 };
        if (S_ISDIR(sb.st_mode)) {
            if (!session_name_valid(n) || str_copy(ent.name, sizeof ent.name, n) != TE_OK) {
                continue;
            }
            ent.dir = true;
        } else if (S_ISREG(sb.st_mode)) {
            size_t nl = strlen(n);
            if (nl <= 4 || strcmp(n + nl - 4, ".ini") != 0 ||
                str_copy_n(ent.name, sizeof ent.name, n, nl - 4) != TE_OK || !session_name_valid(ent.name)) {
                continue;
            }
        } else {
            continue;   /* symlinks, sockets, devices: never ours */
        }
        if (len == cap) {
            size_t ncap = cap == 0 ? 32 : cap * 2;
            void *tmp = list;
            if (te_realloc_array(&tmp, cap, ncap, sizeof *list) != TE_OK) {
                rc = TE_ERR_NOMEM;
                break;
            }
            list = tmp;
            cap = ncap;
        }
        list[len++] = ent;
    }
    closedir(d);
    if (rc != TE_OK) {
        TE_FREE(list);
        return rc;
    }
    if (len > 1) {
        qsort(list, len, sizeof *list, cmp_names);
    }
    *out = list;
    *count = len;
    return TE_OK;
}

static void scan(te_store *st, int dirfd, const char *rel, uint8_t depth, int parent)
{
    name_ent *list = nullptr;
    size_t count = 0;
    if (list_dir(dirfd, &list, &count) != TE_OK) {
        LOG_W("sessions: cannot read folder '%s'", rel[0] != '\0' ? rel : "/");
        return;
    }
    for (size_t i = 0; i < count; i++) {
        char child_path[TE_SESS_FOLDER_MAX + 1] = { 0 };
        char display[TE_PATH_MAX] = { 0 };
        if (list[i].dir) {
            if (depth + 1u > TE_SESS_DEPTH_MAX || store_join(rel, list[i].name, child_path, sizeof child_path) != TE_OK) {
                st->truncated = true;
                continue;
            }
            te_store_node *n = push_node(st);
            if (n == nullptr) {
                break;
            }
            int idx = (int)(st->len - 1);
            n->folder = true;
            n->parent = parent;
            n->depth = depth;
            (void)str_copy(n->name, sizeof n->name, list[i].name);
            (void)str_copy(n->path, sizeof n->path, child_path);
            folder_defaults_init(&n->fdef);
            int sub = openat(dirfd, list[i].name, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
            if (sub < 0) {
                continue;
            }
            (void)str_fmt(display, sizeof display, "%s/%s/" FOLDER_FILE, st->root, child_path);
            parse_ctx fc = { .d = &st->nodes[idx].fdef };
            (void)ini_parse_at(sub, FOLDER_FILE, display, parse_cb, &fc);
            scan(st, sub, child_path, (uint8_t)(depth + 1u), idx);
            close(sub);
        } else {
            te_session *s = te_calloc(1, sizeof *s);
            if (s == nullptr) {
                break;
            }
            session_defaults(s, TE_SESS_SSH);
            char fname[TE_SESS_NAME_MAX + 8] = { 0 };
            (void)file_name(list[i].name, fname, sizeof fname);
            (void)str_fmt(display, sizeof display, "%s/%s%s%s", st->root, rel, rel[0] != '\0' ? "/" : "", fname);
            parse_ctx sc = { .s = s };
            if (ini_parse_at(dirfd, fname, display, parse_cb, &sc) != TE_OK) {
                TE_FREE(s);
                continue;
            }
            (void)str_copy(s->name, sizeof s->name, list[i].name);
            (void)str_copy(s->folder, sizeof s->folder, rel);
            te_store_node *n = push_node(st);
            if (n == nullptr) {
                TE_FREE(s);
                break;
            }
            n->parent = parent;
            n->depth = depth;
            (void)str_copy(n->name, sizeof n->name, list[i].name);
            (void)str_copy(n->path, sizeof n->path, rel);
            n->sess = s;
        }
    }
    TE_FREE(list);
}

te_err store_reload(te_store *st)
{
    /* Remember which folders were open. */
    char (*open_paths)[TE_SESS_FOLDER_MAX + 1] = nullptr;
    size_t nopen = 0;
    for (size_t i = 0; i < st->len; i++) {
        nopen += st->nodes[i].folder && st->nodes[i].expanded;
    }
    if (nopen > 0) {
        open_paths = te_calloc(nopen, sizeof *open_paths);
        if (open_paths == nullptr) {
            nopen = 0;
        }
    }
    size_t k = 0;
    for (size_t i = 0; i < st->len && k < nopen; i++) {
        if (st->nodes[i].folder && st->nodes[i].expanded) {
            (void)str_copy(open_paths[k++], sizeof open_paths[0], st->nodes[i].path);
        }
    }
    free_nodes(st);
    st->truncated = false;
    int fd = open_root(st);
    if (fd < 0) {
        TE_FREE(open_paths);
        return errno_to_err(errno);
    }
    scan(st, fd, "", 0, -1);
    close(fd);
    for (size_t i = 0; i < st->len; i++) {
        for (size_t j = 0; j < k; j++) {
            if (st->nodes[i].folder && strcmp(st->nodes[i].path, open_paths[j]) == 0) {
                st->nodes[i].expanded = true;
            }
        }
    }
    TE_FREE(open_paths);
    if (st->truncated) {
        LOG_W("sessions: only the first %d entries (and %d folder levels) are shown", TE_STORE_MAX_NODES,
              TE_SESS_DEPTH_MAX);
    }
    return TE_OK;
}

te_err store_open(te_store *st, const char *root)
{
    if (st == nullptr || !os_path_is_absolute(root)) {
        return TE_ERR_INVAL;
    }
    memset(st, 0, sizeof *st);
    te_err rc = str_copy(st->root, sizeof st->root, root);
    if (rc != TE_OK) {
        return rc;
    }
    char parent[TE_PATH_MAX] = { 0 };
    rc = str_copy(parent, sizeof parent, root);
    if (rc != TE_OK) {
        return rc;
    }
#ifdef _WIN32
    /* %APPDATA% always exists; only the config directory may be missing. */
    char *slash = strrchr(parent, '/');
    if (slash != nullptr && slash != parent) {
        *slash = '\0';
        if (mkdir(parent, 0700) != 0 && errno != EEXIST) {
            return errno_to_err(errno);
        }
    }
#else
    /* The config directory, and even ~/.config on a fresh account, may not
       exist yet: create each missing folder on the way, like mkdir -p. */
    for (char *p = parent + 1; *p != '\0'; p++) {
        if (*p != '/') {
            continue;
        }
        *p = '\0';
        if (mkdir(parent, 0700) != 0 && errno != EEXIST) {
            return errno_to_err(errno);
        }
        *p = '/';
    }
#endif
    if (mkdir(root, 0700) != 0 && errno != EEXIST) {
        return errno_to_err(errno);
    }
    struct stat sb = { 0 };
    if (lstat(root, &sb) != 0 || !S_ISDIR(sb.st_mode)) {
        LOG_E("sessions: %s is not a folder", root);
        return TE_ERR_INVAL;
    }
    if (sb.st_uid != getuid()) {
        LOG_E("sessions: %s belongs to another user", root);
        return TE_ERR_PERM;
    }
    return store_reload(st);
}

void store_close(te_store *st)
{
    if (st != nullptr) {
        free_nodes(st);
    }
}

/* ------------------------------------------------------------------------- */
/* Lookup */

int store_find_folder(const te_store *st, const char *path)
{
    for (size_t i = 0; i < st->len; i++) {
        if (st->nodes[i].folder && strcmp(st->nodes[i].path, path) == 0) {
            return (int)i;
        }
    }
    return -1;
}

int store_find_session(const te_store *st, const char *folder, const char *name)
{
    for (size_t i = 0; i < st->len; i++) {
        const te_store_node *n = &st->nodes[i];
        if (!n->folder && strcmp(n->path, folder) == 0 && strcmp(n->name, name) == 0) {
            return (int)i;
        }
    }
    return -1;
}

bool store_session_exists(const te_store *st, const char *folder, const char *name)
{
    char fname[TE_SESS_NAME_MAX + 8] = { 0 };
    if (file_name(name, fname, sizeof fname) != TE_OK) {
        return false;
    }
    int fd = open_rel(st, folder, false);
    if (fd < 0) {
        return false;
    }
    bool e = exists_at(fd, fname);
    close(fd);
    return e;
}

void store_effective(const te_store *st, const te_session *s, te_session_eff *out)
{
    out->color = s->color;
    out->anti_idle = s->anti_idle;
    (void)str_copy(out->user, sizeof out->user, s->user);
    char path[TE_SESS_FOLDER_MAX + 1] = { 0 };
    (void)str_copy(path, sizeof path, s->folder);
    /* Walk from the session's folder up to the top, filling what is still unset. */
    while (path[0] != '\0') {
        int fi = store_find_folder(st, path);
        if (fi >= 0) {
            const te_folder_defaults *d = &st->nodes[fi].fdef;
            if (out->color == TE_TAG_INHERIT && d->color != TE_TAG_INHERIT) out->color = d->color;
            if (out->anti_idle == TE_ANTI_IDLE_INHERIT) out->anti_idle = d->anti_idle;
            if (out->user[0] == '\0' && d->user[0] != '\0') (void)str_copy(out->user, sizeof out->user, d->user);
        }
        char *slash = strrchr(path, '/');
        if (slash == nullptr) {
            break;
        }
        *slash = '\0';
    }
    if (out->color == TE_TAG_INHERIT) {
        out->color = TE_TAG_NONE;
    }
}

/* ------------------------------------------------------------------------- */
/* Changes */

te_err store_save_session(te_store *st, const te_session *s, const char *old_folder, const char *old_name)
{
    const char *why = nullptr;
    if (!session_validate(s, &why)) {
        LOG_W("session not saved: %s", why);
        return TE_ERR_INVAL;
    }
    bool replacing = old_name != nullptr && old_folder != nullptr;
    bool same_place = replacing && strcmp(old_folder, s->folder) == 0 && strcmp(old_name, s->name) == 0;
    char text[FILE_TEXT_MAX] = { 0 };
    char fname[TE_SESS_NAME_MAX + 8] = { 0 };
    te_err rc = session_serialize(s, text, sizeof text);
    if (rc == TE_OK) rc = file_name(s->name, fname, sizeof fname);
    if (rc != TE_OK) {
        return rc;
    }
    int fd = open_rel(st, s->folder, true);
    if (fd < 0) {
        return errno_to_err(errno);
    }
    if (!same_place && exists_at(fd, fname)) {
        close(fd);
        return TE_ERR_EXISTS;
    }
    rc = write_atomic(fd, fname, text, strlen(text));
    close(fd);
    if (rc == TE_OK && replacing && !same_place) {
        rc = store_delete_session(st, old_folder, old_name);   /* also reloads */
        return rc;
    }
    if (rc == TE_OK) {
        rc = store_reload(st);
    }
    return rc;
}

te_err store_delete_session(te_store *st, const char *folder, const char *name)
{
    char fname[TE_SESS_NAME_MAX + 8] = { 0 };
    if (!session_name_valid(name) || file_name(name, fname, sizeof fname) != TE_OK) {
        return TE_ERR_INVAL;
    }
    int fd = open_rel(st, folder, false);
    if (fd < 0) {
        return errno_to_err(errno);
    }
    te_err rc = unlinkat(fd, fname, 0) == 0 ? TE_OK : errno_to_err(errno);
    close(fd);
    return rc == TE_OK ? store_reload(st) : rc;
}

te_err store_move_session(te_store *st, const char *folder, const char *name, const char *to_folder)
{
    char fname[TE_SESS_NAME_MAX + 8] = { 0 };
    if (!session_name_valid(name) || file_name(name, fname, sizeof fname) != TE_OK) {
        return TE_ERR_INVAL;
    }
    if (strcmp(folder, to_folder) == 0) {
        return TE_OK;
    }
    int src = open_rel(st, folder, false);
    if (src < 0) {
        return errno_to_err(errno);
    }
    int dst = open_rel(st, to_folder, false);
    if (dst < 0) {
        te_err e = errno_to_err(errno);
        close(src);
        return e;
    }
    te_err rc = renameat2(src, fname, dst, fname, RENAME_NOREPLACE) == 0 ? TE_OK : errno_to_err(errno);
    close(src);
    close(dst);
    return rc == TE_OK ? store_reload(st) : rc;
}

te_err store_create_folder(te_store *st, const char *parent, const char *name)
{
    char path[TE_SESS_FOLDER_MAX + 1] = { 0 };
    if (!session_name_valid(name) || store_join(parent, name, path, sizeof path) != TE_OK ||
        !session_folder_valid(path)) {
        return TE_ERR_INVAL;
    }
    int fd = open_rel(st, parent, false);
    if (fd < 0) {
        return errno_to_err(errno);
    }
    te_err rc = mkdirat(fd, name, 0700) == 0 ? TE_OK : errno_to_err(errno);
    close(fd);
    if (rc != TE_OK) {
        return rc;
    }
    rc = store_reload(st);
    int fi = store_find_folder(st, path);
    if (fi >= 0) {
        st->nodes[fi].expanded = true;
    }
    return rc;
}

/* Split "a/b/c" into parent "a/b" and leaf "c". */
static te_err split_path(const char *path, char *parent, size_t psz, char *leaf, size_t lsz)
{
    const char *slash = strrchr(path, '/');
    if (slash == nullptr) {
        parent[0] = '\0';
        return str_copy(leaf, lsz, path);
    }
    te_err rc = str_copy_n(parent, psz, path, (size_t)(slash - path));
    return rc != TE_OK ? rc : str_copy(leaf, lsz, slash + 1);
}

te_err store_rename_folder(te_store *st, const char *path, const char *new_name)
{
    char parent[TE_SESS_FOLDER_MAX + 1] = { 0 };
    char leaf[TE_SESS_NAME_MAX + 1] = { 0 };
    char np[TE_SESS_FOLDER_MAX + 1] = { 0 };
    if (path[0] == '\0' || !session_folder_valid(path) || !session_name_valid(new_name) ||
        split_path(path, parent, sizeof parent, leaf, sizeof leaf) != TE_OK ||
        store_join(parent, new_name, np, sizeof np) != TE_OK || !session_folder_valid(np)) {
        return TE_ERR_INVAL;
    }
    int fd = open_rel(st, parent, false);
    if (fd < 0) {
        return errno_to_err(errno);
    }
    te_err rc = renameat2(fd, leaf, fd, new_name, RENAME_NOREPLACE) == 0 ? TE_OK : errno_to_err(errno);
    close(fd);
    if (rc != TE_OK) {
        return rc;
    }
    /* Keep the renamed folder (and those inside it) open if they were. */
    size_t plen = strlen(path);
    for (size_t i = 0; i < st->len; i++) {
        te_store_node *n = &st->nodes[i];
        if (n->folder && strncmp(n->path, path, plen) == 0 && (n->path[plen] == '\0' || n->path[plen] == '/')) {
            char moved[TE_SESS_FOLDER_MAX + 1] = { 0 };
            if (str_fmt(moved, sizeof moved, "%s%s", np, n->path + plen) == TE_OK) {
                (void)str_copy(n->path, sizeof n->path, moved);
            }
        }
    }
    return store_reload(st);
}

/*
 * Remove name (a folder inside dirfd) and everything in it. With dry_run,
 * only check that every entry is something FeTerm made: sub-folders,
 * session files, _folder.ini, and leftover temporary files.
 */
static te_err remove_tree(int dirfd, const char *name, int depth, bool dry_run)
{
    if (depth > TE_SESS_DEPTH_MAX + 1) {
        return TE_ERR_RANGE;
    }
    int fd = openat(dirfd, name, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) {
        return errno_to_err(errno);
    }
    int dfd = openat(fd, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    DIR *d = dfd >= 0 ? fdopendir(dfd) : nullptr;
    if (d == nullptr) {
        if (dfd >= 0) close(dfd);
        close(fd);
        return TE_ERR_IO;
    }
    te_err rc = TE_OK;
    struct dirent *e = nullptr;
    while (rc == TE_OK && (e = readdir(d)) != nullptr) {
        const char *n = e->d_name;
        if (strcmp(n, ".") == 0 || strcmp(n, "..") == 0) {
            continue;
        }
        struct stat sb = { 0 };
        if (fstatat(fd, n, &sb, AT_SYMLINK_NOFOLLOW) != 0) {
            rc = errno_to_err(errno);
            break;
        }
        size_t nl = strlen(n);
        bool ours_file = S_ISREG(sb.st_mode) &&
                         ((nl > 4 && strcmp(n + nl - 4, ".ini") == 0) || strncmp(n, TMP_PREFIX, strlen(TMP_PREFIX)) == 0);
        if (S_ISDIR(sb.st_mode)) {
            rc = remove_tree(fd, n, depth + 1, dry_run);
        } else if (ours_file) {
            if (!dry_run && unlinkat(fd, n, 0) != 0) {
                rc = errno_to_err(errno);
            }
        } else {
            LOG_W("sessions: '%s' is not a FeTerm file; folder not deleted", n);
            rc = TE_ERR_EXISTS;
        }
    }
    closedir(d);
    if (rc == TE_OK && !dry_run && unlinkat(dirfd, name, AT_REMOVEDIR) != 0) {
        rc = errno_to_err(errno);
    }
    close(fd);
    return rc;
}

te_err store_delete_folder(te_store *st, const char *path)
{
    char parent[TE_SESS_FOLDER_MAX + 1] = { 0 };
    char leaf[TE_SESS_NAME_MAX + 1] = { 0 };
    if (path[0] == '\0' || !session_folder_valid(path) ||
        split_path(path, parent, sizeof parent, leaf, sizeof leaf) != TE_OK) {
        return TE_ERR_INVAL;
    }
    int fd = open_rel(st, parent, false);
    if (fd < 0) {
        return errno_to_err(errno);
    }
    te_err rc = remove_tree(fd, leaf, 0, true);     /* check first: never delete half a folder */
    if (rc == TE_OK) {
        rc = remove_tree(fd, leaf, 0, false);
    }
    close(fd);
    te_err rr = store_reload(st);
    return rc != TE_OK ? rc : rr;
}

te_err store_save_folder_defaults(te_store *st, const char *path, const te_folder_defaults *d)
{
    char text[FILE_TEXT_MAX] = { 0 };
    te_err rc = folder_serialize(d, text, sizeof text);
    if (rc != TE_OK) {
        return rc;
    }
    if (path[0] == '\0') {
        return TE_ERR_INVAL;   /* the top level has no folder settings */
    }
    int fd = open_rel(st, path, false);
    if (fd < 0) {
        return errno_to_err(errno);
    }
    rc = write_atomic(fd, FOLDER_FILE, text, strlen(text));
    close(fd);
    return rc == TE_OK ? store_reload(st) : rc;
}
