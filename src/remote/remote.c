#include "remote/remote.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#ifndef _WIN32
#include <sys/inotify.h>
#endif
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "remote/proc.h"
#include "remote/sftp_proto.h"
#include "sessions/launch.h"
#include "util/log.h"
#include "util/mem.h"
#include "util/str.h"

#define MAX_PENDING   64
#define PIPELINE      16
#define OUT_LIMIT     (4u * 1024u * 1024u)
#define IN_LIMIT      (TE_SFTP_MAX_PACKET + 4u)
#define TTY_KEEP      2048
#define EDIT_MAX      64
#define WALK_DEPTH    32
#define EDIT_SETTLE_MS 400u
#define NO_XFER       SIZE_MAX

enum {
    K_NONE, K_HOME, K_LIST_REALPATH, K_LIST_OPENDIR, K_LIST_READDIR, K_LIST_CLOSE, K_SIMPLE,
    K_X_OPEN, K_X_READ, K_X_WRITE, K_X_CLOSE, K_X_STATUS, K_SCAN_OPENDIR, K_SCAN_READDIR, K_SCAN_CLOSE,
};

typedef struct req {
    bool     used;
    uint8_t  kind;
    uint32_t id;
    uint32_t gen;          /* listing generation or transfer serial it belongs to */
    uint64_t off;
    uint32_t len;
    char     what[96]; /* K_SIMPLE: message on success */
} req;

typedef struct edit_watch {
    int      wd;
    char    *local;
    char    *remote;
    char    *dir; /* the private folder holding the copy */
    uint64_t ignore_until;
    uint64_t due;          /* upload scheduled (0: none) */
} edit_watch;

struct te_remote {
    te_remote_host  host;
    te_session      s;
    te_session_eff  eff;
    char            control_dir[TE_PATH_MAX];
    char            edit_dir[TE_PATH_MAX];
    int             keepalive;
    char            title[TE_SESS_USER_MAX + TE_SESS_HOST_MAX + 4];

    te_remote_state state;
    char            error[256];
    te_proc         proc;
    int             tag_out, tag_tty;
    bool            direct;          /* retried without the shared login */
    bool            used_mux;

    uint8_t        *in;
    size_t          in_len;
    te_sbuf         out;
    uint32_t        next_id;
    req             reqs[MAX_PENDING];

    char            tty[TTY_KEEP];
    size_t          tty_len;
    bool            prompting;
    te_proc        *prompt_proc;     /* whose terminal gets the answer */

    /* Listing */
    char            cwd[TE_REMOTE_PATH_MAX];
    char            pending_path[TE_REMOTE_PATH_MAX];
    char            list_target[TE_REMOTE_PATH_MAX];
    bool            loading;
    uint32_t        list_gen;
    uint32_t        generation;
    te_rentry      *entries;
    size_t          nentries;
    te_rentry      *pend;
    size_t          npend, cappend;
    uint8_t         lh[TE_SFTP_MAX_HANDLE];
    size_t          lhl;
    bool            want_refresh;

    /* Transfer queue */
    te_xfer        *q;
    size_t          qlen, qcap;
    size_t          active;
    uint32_t        xser;
    int             xfd;             /* local file of the active transfer */
    int             xdirfd;          /* its folder (downloads) */
    char            xtmp[96]; /* temporary download name inside xdirfd */
    char            xfinal[TE_REMOTE_PATH_MAX];
    uint8_t         xh[TE_SFTP_MAX_HANDLE];
    size_t          xhl;
    uint64_t        next_off;
    uint32_t        inflight;
    bool            eof, closing, have_size;
    te_rentry      *scan;
    size_t          nscan, capscan;

    /* SCP fallback */
    te_proc         scp;
    int             tag_scp;

    /* Edit locally */
    int             ino_fd;
    int             tag_ino;
    edit_watch      edits[EDIT_MAX];
    size_t          nedits;
    uint32_t        edit_counter;
};

static uint64_t now_ms(void)
{
    struct timespec ts = { 0 };
    (void)clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

static void changed(te_remote *r)
{
    if (r->host.changed != nullptr) {
        r->host.changed(r->host.user);
    }
}

static void toast(te_remote *r, const char *msg)
{
    if (r->host.toast != nullptr) {
        r->host.toast(r->host.user, msg);
    }
}

/* ------------------------------------------------------------------------- */
/* Pure helpers */

te_err remote_join(const char *dir, const char *name, char *out, size_t outsz)
{
    if (dir == nullptr || dir[0] == '\0') {
        return str_copy(out, outsz, name);
    }
    size_t dl = strlen(dir);
    return str_fmt(out, outsz, "%s%s%s", dir, dir[dl - 1] == '/' ? "" : "/", name);
}

bool remote_local_name(const char *remote_name, char *out, size_t outsz)
{
    if (remote_name == nullptr || outsz == 0) {
        return false;
    }
    size_t k = 0;
    for (const char *s = remote_name; *s != '\0' && k + 1 < outsz; s++) {
        unsigned char c = (unsigned char)*s;
        if (c < 0x20 || c == 0x7F) {
            continue;   /* control characters never reach a local file name */
        }
        out[k++] = (c == '/' || c == '\\') ? '_' : (char)c;
    }
    out[k] = '\0';
    if (k == 0 || strcmp(out, ".") == 0 || strcmp(out, "..") == 0) {
        return false;
    }
    if (out[0] == '-') {
        out[0] = '_';   /* a file named "-rf" is an accident waiting to happen */
    }
    return true;
}

te_err remote_unique_name(int dirfd, const char *name, char *out, size_t outsz)
{
    struct stat sb = { 0 };
    if (fstatat(dirfd, name, &sb, AT_SYMLINK_NOFOLLOW) != 0 && errno == ENOENT) {
        return str_copy(out, outsz, name);
    }
    const char *dot = strrchr(name, '.');
    size_t stem = dot != nullptr && dot != name ? (size_t)(dot - name) : strlen(name);
    const char *ext = dot != nullptr && dot != name ? dot : "";
    for (int i = 1; i < 1000; i++) {
        char cand[TE_REMOTE_PATH_MAX] = { 0 };
        if (str_fmt(cand, sizeof cand, "%.*s (%d)%s", (int)stem, name, i, ext) != TE_OK) {
            return TE_ERR_TRUNC;
        }
        if (fstatat(dirfd, cand, &sb, AT_SYMLINK_NOFOLLOW) != 0 && errno == ENOENT) {
            return str_copy(out, outsz, cand);
        }
    }
    return TE_ERR_EXISTS;
}

bool remote_scp_quote(const char *path, char *out, size_t outsz)
{
    if (path == nullptr || path[0] == '\0' || path[0] == '-' || strchr(path, '\n') != nullptr ||
        strchr(path, '\r') != nullptr) {
        return false;
    }
    /* Plain paths go as they are (Cisco IOS takes "flash:/x" literally); others are single-quoted. */
    bool plain = true;
    for (const char *s = path; *s != '\0'; s++) {
        unsigned char c = (unsigned char)*s;
        bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                  strchr("/._-:@+,=%~", (int)c) != nullptr;
        if (c < 0x20 || c == 0x7F) {
            return false;
        }
        plain = plain && ok;
    }
    if (plain) {
        return str_copy(out, outsz, path) == TE_OK;
    }
    size_t k = 0;
    if (outsz < 3) return false;
    out[k++] = '\'';
    for (const char *s = path; *s != '\0'; s++) {
        if (*s == '\'') {
            if (k + 5 >= outsz) return false;
            memcpy(out + k, "'\\''", 4);   /* close, escaped quote, reopen */
            k += 4;
        } else {
            if (k + 2 >= outsz) return false;
            out[k++] = *s;
        }
    }
    out[k++] = '\'';
    out[k] = '\0';
    return true;
}

/* ------------------------------------------------------------------------- */
/* Entries */

static void free_entries(te_rentry **list, size_t *n, size_t *cap)
{
    for (size_t i = 0; i < *n; i++) {
        TE_FREE((*list)[i].name);
    }
    TE_FREE(*list);
    *n = 0;
    if (cap != nullptr) {
        *cap = 0;
    }
}

static bool push_entry(te_rentry **list, size_t *n, size_t *cap, const te_sftp_name *src)
{
    if (*n >= TE_REMOTE_MAX_ENTRIES) {
        return false;
    }
    if (*n == *cap) {
        size_t ncap = *cap == 0 ? 128 : *cap * 2;
        void *tmp = *list;
        if (te_realloc_array(&tmp, *cap, ncap, sizeof **list) != TE_OK) {
            return false;
        }
        *list = tmp;
        *cap = ncap;
    }
    char *name = str_dup(src->name);
    if (name == nullptr) {
        return false;
    }
    te_rentry *e = &(*list)[(*n)++];
    memset(e, 0, sizeof *e);
    e->name = name;
    e->size = src->a.size;
    e->perm = src->a.perm;
    e->mtime = src->a.mtime;
    e->uid = src->a.uid;
    e->gid = src->a.gid;
    e->has_size = (src->a.flags & SSH_FILEXFER_ATTR_SIZE) != 0;
    e->has_time = (src->a.flags & SSH_FILEXFER_ATTR_ACMODTIME) != 0;
    e->has_perm = (src->a.flags & SSH_FILEXFER_ATTR_PERMISSIONS) != 0;
    e->dir = sftp_is_dir(&src->a);
    e->link = sftp_is_link(&src->a);
    return true;
}

static int cmp_entry(const void *a, const void *b)
{
    const te_rentry *x = a;
    const te_rentry *y = b;
    if (x->dir != y->dir) {
        return x->dir ? -1 : 1;
    }
    int c = strcasecmp(x->name, y->name);
    return c != 0 ? c : strcmp(x->name, y->name);
}

static const te_rentry *find_entry(const te_remote *r, const char *name)
{
    for (size_t i = 0; i < r->nentries; i++) {
        if (strcmp(r->entries[i].name, name) == 0) {
            return &r->entries[i];
        }
    }
    return nullptr;
}

/* ------------------------------------------------------------------------- */
/* Requests */

static req *new_req(te_remote *r, uint8_t kind, uint32_t gen)
{
    for (size_t i = 0; i < MAX_PENDING; i++) {
        if (!r->reqs[i].used) {
            req *q = &r->reqs[i];
            memset(q, 0, sizeof *q);
            q->used = true;
            q->kind = kind;
            q->gen = gen;
            q->id = ++r->next_id;
            return q;
        }
    }
    return nullptr;
}

static req *find_req(te_remote *r, uint32_t id)
{
    for (size_t i = 0; i < MAX_PENDING; i++) {
        if (r->reqs[i].used && r->reqs[i].id == id) {
            return &r->reqs[i];
        }
    }
    return nullptr;
}

static void fail_conn(te_remote *r, const char *why);

/* The encoders return false when the output buffer is full: a broken connection. */
static void sent(te_remote *r, req *q, bool ok)
{
    if (!ok) {
        if (q != nullptr) q->used = false;
        fail_conn(r, "request queue overflow");
    }
}

static void send_path(te_remote *r, uint8_t type, uint8_t kind, uint32_t gen, const char *path)
{
    req *q = new_req(r, kind, gen);
    if (q == nullptr) {
        fail_conn(r, "too many outstanding requests");
        return;
    }
    sent(r, q, sftp_enc_path(&r->out, type, q->id, path));
}

static void send_close(te_remote *r, uint8_t kind, uint32_t gen, const uint8_t *h, size_t hl)
{
    req *q = new_req(r, kind, gen);
    if (q == nullptr) {
        fail_conn(r, "too many outstanding requests");
        return;
    }
    sent(r, q, sftp_enc_handle(&r->out, SSH_FXP_CLOSE, q->id, h, hl));
}

/* ------------------------------------------------------------------------- */
/* Connection */

static void unwatch(te_remote *r, int *tag)
{
    if (*tag >= 0 && r->host.unwatch != nullptr) {
        r->host.unwatch(r->host.user, *tag);
    }
    *tag = -1;
}

static void stop_proc(te_remote *r)
{
    unwatch(r, &r->tag_out);
    unwatch(r, &r->tag_tty);
    proc_stop(&r->proc);
    r->in_len = 0;
    r->out.len = 0;
    memset(r->reqs, 0, sizeof r->reqs);
    r->prompting = false;
    r->loading = false;
}

static void finish_active(te_remote *r, te_xfer_state st, const char *err);

static void fail_conn(te_remote *r, const char *why)
{
    if (r->state == TE_RS_FAILED) {
        return;
    }
    (void)str_copy(r->error, sizeof r->error, why);
    LOG_W("files %s: %s", r->title, why);
    if (r->active != NO_XFER) {
        finish_active(r, TE_XS_FAILED, why);
    }
    stop_proc(r);
    r->state = TE_RS_FAILED;
    changed(r);
}

static size_t add_common_ssh_opts(te_remote *r, const char **argv, size_t n, char *storage, size_t storsz, bool scp)
{
    /* storage holds the formatted strings; argv points into it. */
    size_t used = 0;
#define ARG(s) do { if (n < 36) argv[n++] = (s); } while (0)
#define FMT(...) do { \
        int w_ = snprintf(storage + used, storsz - used, __VA_ARGS__); \
        if (w_ > 0 && (size_t)w_ < storsz - used) { ARG(storage + used); used += (size_t)w_ + 1; } \
    } while (0)
    ARG("-o");
    FMT("ServerAliveInterval=%d", r->keepalive > 0 ? r->keepalive : 30);
    ARG("-o");
    ARG("ServerAliveCountMax=10");
    if (!r->direct && r->s.share_conn && r->control_dir[0] == '/') {
        ARG("-o");
        FMT("ControlPath=%s/cm-%%C", r->control_dir);
        ARG("-o");
        ARG("ControlMaster=no");
        r->used_mux = true;
    } else {
        ARG("-o");
        ARG("ControlPath=none");
        r->used_mux = false;
    }
    if (r->s.port != 0) {
        ARG(scp ? "-P" : "-p");
        FMT("%u", (unsigned)r->s.port);
    }
    if (r->eff.user[0] != '\0') {
        ARG("-o");
        FMT("User=%s", r->eff.user);
    }
    if (r->s.identity[0] != '\0') {
        char key[TE_SESS_PATH_MAX] = { 0 };
        if (launch_expand_home(r->s.identity, key, sizeof key) == TE_OK) {
            ARG("-i");
            FMT("%s", key);
        }
    }
    if (r->s.jump[0] != '\0') {
        ARG("-J");
        FMT("%s", r->s.jump);
    }
#undef FMT
#undef ARG
    return n;
}

static bool watch_fd(te_remote *r, int fd, int *tag)
{
    *tag = -1;
    return fd >= 0 && r->host.watch != nullptr && r->host.watch(r->host.user, fd, tag);
}

void remote_connect(te_remote *r)
{
    if (r == nullptr || r->state == TE_RS_CONNECTING || r->state == TE_RS_READY) {
        return;
    }
    stop_proc(r);
    const char *argv[48] = { 0 };   /* options stop at 36, then 4 more and the terminator */
    char storage[4096] = { 0 };
    size_t n = 0;
    argv[n++] = "ssh";
    n = add_common_ssh_opts(r, argv, n, storage, sizeof storage, false);
    argv[n++] = "-s";
    argv[n++] = "--";
    argv[n++] = r->s.host;
    argv[n++] = "sftp";
    argv[n] = nullptr;
    r->tty_len = 0;
    r->error[0] = '\0';
    if (proc_spawn(&r->proc, argv) != TE_OK) {
        r->state = TE_RS_IDLE;
        fail_conn(r, "could not start ssh");
        return;
    }
    if (!watch_fd(r, r->proc.out_fd, &r->tag_out) || !watch_fd(r, r->proc.tty_fd, &r->tag_tty)) {
        r->state = TE_RS_IDLE;
        fail_conn(r, "could not watch the connection");
        return;
    }
    r->state = TE_RS_CONNECTING;
    if (!sftp_enc_init(&r->out)) {
        fail_conn(r, "could not start the SFTP protocol");
        return;
    }
    changed(r);
}

/* The last non-empty line of the terminal text. */
static void last_line(const te_remote *r, char *out, size_t outsz)
{
    size_t end = r->tty_len;
    while (end > 0 && (r->tty[end - 1] == '\n' || r->tty[end - 1] == '\r' || r->tty[end - 1] == ' ')) {
        end--;
    }
    size_t start = end;
    while (start > 0 && r->tty[start - 1] != '\n' && r->tty[start - 1] != '\r') {
        start--;
    }
    size_t k = 0;
    for (size_t i = start; i < end && k + 1 < outsz; i++) {
        unsigned char c = (unsigned char)r->tty[i];
        out[k++] = c < 0x20 || c == 0x7F ? ' ' : (char)c;
    }
    out[k] = '\0';
}

static bool tty_has(const te_remote *r, const char *needle)
{
    size_t n = strlen(needle);
    if (n == 0 || n > r->tty_len) {
        return false;
    }
    for (size_t i = 0; i + n <= r->tty_len; i++) {
        if (strncasecmp(r->tty + i, needle, n) == 0) {
            return true;
        }
    }
    return false;
}

static void child_exited(te_remote *r)
{
    int st = 0;
    for (int i = 0; i < 20 && !proc_reap(&r->proc, &st); i++) {
        struct timespec ts = { .tv_sec = 0, .tv_nsec = 10 * 1000 * 1000 };
        nanosleep(&ts, nullptr);
    }
    char line[200] = { 0 };
    last_line(r, line, sizeof line);
    if (r->state == TE_RS_CONNECTING) {
        if (tty_has(r, "subsystem request failed")) {
            stop_proc(r);
            r->state = TE_RS_NO_SFTP;
            (void)str_copy(r->error, sizeof r->error, "This server has no SFTP. You can still copy single files with SCP.");
            changed(r);
            return;
        }
        if (r->used_mux && !r->direct) {
            /* Some devices refuse a second channel on a shared login: log in separately. */
            r->direct = true;
            stop_proc(r);
            r->state = TE_RS_IDLE;
            remote_connect(r);
            return;
        }
    }
    char msg[256] = { 0 };
    (void)str_fmt(msg, sizeof msg, "%s", line[0] != '\0' ? line : "the connection closed");
    stop_proc(r);
    r->state = TE_RS_IDLE;   /* let fail_conn record it */
    fail_conn(r, msg);
}

static void detect_prompt(te_remote *r, te_proc *who)
{
    if (r->prompting) {
        return;
    }
    char line[200] = { 0 };
    last_line(r, line, sizeof line);
    size_t n = strlen(line);
    if (n == 0) {
        return;
    }
    char lc = line[n - 1];
    bool confirm = strstr(line, "(yes/no") != nullptr;
    if (lc != ':' && lc != '?' && !confirm) {
        return;
    }
    te_prompt_kind kind = TE_PROMPT_TEXT;
    if (confirm) {
        kind = TE_PROMPT_CONFIRM;
    } else if (strcasestr(line, "password") != nullptr || strcasestr(line, "passphrase") != nullptr ||
               strcasestr(line, "pin") != nullptr || strcasestr(line, "code") != nullptr) {
        kind = TE_PROMPT_SECRET;
    }
    /* For the host-key question, show the lines with the fingerprint too. */
    char text[TTY_KEEP] = { 0 };
    if (confirm) {
        size_t start = r->tty_len > 600 ? r->tty_len - 600 : 0;
        size_t k = 0;
        for (size_t i = start; i < r->tty_len && k + 1 < sizeof text; i++) {
            unsigned char c = (unsigned char)r->tty[i];
            if (c == '\r') continue;
            text[k++] = (c < 0x20 && c != '\n') || c == 0x7F ? ' ' : (char)c;
        }
        text[k] = '\0';
    } else {
        (void)str_copy(text, sizeof text, line);
    }
    r->prompting = true;
    r->prompt_proc = who;
    r->tty_len = 0;   /* the next question starts fresh */
    if (r->host.prompt != nullptr) {
        r->host.prompt(r->host.user, r, kind, text);
    }
}

void remote_answer(te_remote *r, const char *answer)
{
    if (r == nullptr || !r->prompting) {
        return;
    }
    r->prompting = false;
    te_proc *p = r->prompt_proc;
    r->prompt_proc = nullptr;
    if (answer == nullptr || p == nullptr || p->tty_fd < 0) {
        if (p == &r->scp) {
            proc_stop(&r->scp);
            unwatch(r, &r->tag_scp);
            finish_active(r, TE_XS_CANCELLED, "login cancelled");
        } else {
            fail_conn(r, "login cancelled");
        }
        return;
    }
    char buf[1100] = { 0 };
    size_t n = strnlen(answer, 1024);
    memcpy(buf, answer, n);
    buf[n++] = '\n';
    size_t done = 0;
    for (int tries = 0; done < n && tries < 100; tries++) {
        ssize_t w = write(p->tty_fd, buf + done, n - done);
        if (w > 0) {
            done += (size_t)w;
        } else if (w < 0 && errno != EAGAIN && errno != EINTR) {
            break;
        }
    }
    explicit_bzero(buf, sizeof buf);   /* the password never stays in memory */
}

bool remote_prompting(const te_remote *r)
{
    return r != nullptr && r->prompting;
}

static void read_tty(te_remote *r, te_proc *p, int *tag)
{
    char buf[1024] = { 0 };
    for (;;) {
        ssize_t n = read(p->tty_fd, buf, sizeof buf);
        if (n > 0) {
            static_assert(sizeof buf < TTY_KEEP, "one read must fit in the kept text");
            size_t len = (size_t)n;
            if (r->tty_len + len >= TTY_KEEP) {
                size_t drop = r->tty_len + len - (TTY_KEEP - 1);   /* keep the newest text */
                memmove(r->tty, r->tty + drop, r->tty_len - drop);
                r->tty_len -= drop;
            }
            memcpy(r->tty + r->tty_len, buf, len);
            r->tty_len += len;
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EINTR)) {
            break;
        }
        /* EOF or EIO: every holder of the terminal (the child) has gone. */
        unwatch(r, tag);
        if (p->tty_fd >= 0) {
            close(p->tty_fd);
            p->tty_fd = -1;
        }
        return;
    }
    explicit_bzero(buf, sizeof buf);
    if (p == &r->proc && r->state == TE_RS_CONNECTING) {
        detect_prompt(r, p);
    } else if (p == &r->scp) {
        detect_prompt(r, p);
    }
    if (*tag >= 0 && r->host.rearm != nullptr) {
        r->host.rearm(r->host.user, *tag);
    }
}

/* ------------------------------------------------------------------------- */
/* Listing */

static void start_list(te_remote *r, const char *target)
{
    free_entries(&r->pend, &r->npend, &r->cappend);
    r->loading = true;
    r->list_gen++;
    send_path(r, SSH_FXP_REALPATH, K_LIST_REALPATH, r->list_gen, target);
    changed(r);
}

void remote_list(te_remote *r, const char *path)
{
    if (r == nullptr || path == nullptr || path[0] == '\0') {
        return;
    }
    char target[TE_REMOTE_PATH_MAX] = { 0 };
    if (path[0] == '/') {
        (void)str_copy(target, sizeof target, path);
    } else if (remote_join(r->cwd[0] != '\0' ? r->cwd : ".", path, target, sizeof target) != TE_OK) {
        return;
    }
    if (r->state != TE_RS_READY) {
        (void)str_copy(r->pending_path, sizeof r->pending_path, target);
        return;
    }
    start_list(r, target);
}

void remote_refresh(te_remote *r)
{
    if (r != nullptr && r->cwd[0] != '\0') {
        remote_list(r, r->cwd);
    }
}

void remote_home(te_remote *r)
{
    if (r != nullptr) {
        remote_list(r, ".");
    }
}

static void list_done(te_remote *r)
{
    free_entries(&r->entries, &r->nentries, nullptr);
    r->entries = r->pend;
    r->nentries = r->npend;
    r->pend = nullptr;
    r->npend = 0;
    r->cappend = 0;
    if (r->nentries > 1) {
        qsort(r->entries, r->nentries, sizeof *r->entries, cmp_entry);
    }
    (void)str_copy(r->cwd, sizeof r->cwd, r->list_target);
    r->loading = false;
    r->generation++;
    changed(r);
}

static void list_failed(te_remote *r, const char *what, uint32_t code, const char *msg)
{
    r->loading = false;
    free_entries(&r->pend, &r->npend, &r->cappend);
    char t[300] = { 0 };
    (void)str_fmt(t, sizeof t, "Cannot open %s: %s", what, msg[0] != '\0' ? msg : sftp_status_text(code));
    toast(r, t);
    changed(r);
}

/* ------------------------------------------------------------------------- */
/* Transfer queue */

static void free_xfer(te_xfer *x)
{
    TE_FREE(x->local);
    TE_FREE(x->remote);
}

/* Insert at position pos (qlen = append). Takes copies of the strings. */
static bool queue_insert(te_remote *r, size_t pos, te_xfer_kind kind, const char *local, const char *remote,
                         uint64_t size, te_conflict c, bool edit, uint8_t depth)
{
    if (r->qlen >= TE_REMOTE_MAX_QUEUE) {
        return false;
    }
    if (r->qlen == r->qcap) {
        size_t ncap = r->qcap == 0 ? 64 : r->qcap * 2;
        void *tmp = r->q;
        if (te_realloc_array(&tmp, r->qcap, ncap, sizeof *r->q) != TE_OK) {
            return false;
        }
        r->q = tmp;
        r->qcap = ncap;
    }
    te_xfer x = { .kind = kind, .state = TE_XS_QUEUED, .size = size, .conflict = c, .edit = edit, .depth = depth };
    x.local = local != nullptr ? str_dup(local) : nullptr;
    x.remote = remote != nullptr ? str_dup(remote) : nullptr;
    if ((local != nullptr && x.local == nullptr) || (remote != nullptr && x.remote == nullptr)) {
        free_xfer(&x);
        return false;
    }
    if (pos > r->qlen) {
        pos = r->qlen;
    }
    memmove(&r->q[pos + 1], &r->q[pos], (r->qlen - pos) * sizeof *r->q);
    r->q[pos] = x;
    r->qlen++;
    if (r->active != NO_XFER && pos <= r->active) {
        r->active++;
    }
    return true;
}

static void close_local(te_remote *r)
{
    if (r->xfd >= 0) {
        close(r->xfd);
        r->xfd = -1;
    }
    if (r->xdirfd >= 0) {
        if (r->xtmp[0] != '\0') {
            (void)unlinkat(r->xdirfd, r->xtmp, 0);   /* an unfinished download never lingers */
        }
        close(r->xdirfd);
        r->xdirfd = -1;
    }
    r->xtmp[0] = '\0';
}

static void add_edit_watch(te_remote *r, const char *local, const char *remote);

static void finish_active(te_remote *r, te_xfer_state st, const char *err)
{
    if (r->active == NO_XFER || r->active >= r->qlen) {
        r->active = NO_XFER;
        return;
    }
    te_xfer *x = &r->q[r->active];
    if (r->xhl > 0 && r->state == TE_RS_READY && st != TE_XS_DONE) {
        send_close(r, K_NONE, 0, r->xh, r->xhl);   /* best effort; the reply is ignored */
    }
    r->xhl = 0;
    close_local(r);
    free_entries(&r->scan, &r->nscan, &r->capscan);
    x->state = st;
    x->finished_ms = now_ms();
    if (err != nullptr) {
        (void)str_copy(x->error, sizeof x->error, err);
    }
    if (st == TE_XS_FAILED) {
        char msg[300] = { 0 };
        const char *name = x->remote != nullptr ? strrchr(x->remote, '/') : nullptr;
        (void)str_fmt(msg, sizeof msg, "%s failed: %s", name != nullptr ? name + 1 : "Transfer", x->error);
        toast(r, msg);
    }
    r->active = NO_XFER;
    r->xser++;
    changed(r);
}

static void fail_active(te_remote *r, const char *why)
{
    finish_active(r, TE_XS_FAILED, why);
}

static void fill_download(te_remote *r);
static void fill_upload(te_remote *r);
static void check_done(te_remote *r);

static bool start_download(te_remote *r, te_xfer *x)
{
    char dir[TE_REMOTE_PATH_MAX] = { 0 };
    char base[TE_REMOTE_PATH_MAX] = { 0 };
    const char *slash = strrchr(x->local, '/');
    if (slash == nullptr || str_copy_n(dir, sizeof dir, x->local, (size_t)(slash - x->local)) != TE_OK ||
        str_copy(base, sizeof base, slash + 1) != TE_OK || base[0] == '\0') {
        fail_active(r, "bad local path");
        return false;
    }
    if (dir[0] == '\0') {
        (void)str_copy(dir, sizeof dir, "/");
    }
    r->xdirfd = open(dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (r->xdirfd < 0) {
        fail_active(r, strerror(errno));
        return false;
    }
    struct stat sb = { 0 };
    bool exists = fstatat(r->xdirfd, base, &sb, AT_SYMLINK_NOFOLLOW) == 0;
    if (exists && x->conflict == TE_CONFLICT_SKIP) {
        finish_active(r, TE_XS_SKIPPED, "already exists");
        return false;
    }
    if (exists && x->conflict == TE_CONFLICT_KEEP_BOTH) {
        char alt[TE_REMOTE_PATH_MAX] = { 0 };
        if (remote_unique_name(r->xdirfd, base, alt, sizeof alt) != TE_OK) {
            fail_active(r, "no free file name");
            return false;
        }
        (void)str_copy(base, sizeof base, alt);
    }
    (void)str_copy(r->xfinal, sizeof r->xfinal, base);
    static unsigned counter = 0;
    (void)str_fmt(r->xtmp, sizeof r->xtmp, ".feterm-%ld-%u.part", (long)getpid(), counter++);
    r->xfd = openat(r->xdirfd, r->xtmp, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0644);
    if (r->xfd < 0) {
        r->xtmp[0] = '\0';
        fail_active(r, strerror(errno));
        return false;
    }
    req *q = new_req(r, K_X_OPEN, r->xser);
    if (q == nullptr) {
        fail_active(r, "busy");
        return false;
    }
    sent(r, q, sftp_enc_open(&r->out, q->id, x->remote, SSH_FXF_READ, 0));
    return true;
}

static bool start_upload(te_remote *r, te_xfer *x)
{
    r->xfd = open(x->local, O_RDONLY | O_CLOEXEC | O_NOCTTY);
    struct stat sb = { 0 };
    if (r->xfd < 0 || fstat(r->xfd, &sb) != 0 || !S_ISREG(sb.st_mode)) {
        fail_active(r, r->xfd < 0 ? strerror(errno) : "not a regular file");
        return false;
    }
    x->size = (uint64_t)sb.st_size;
    uint32_t flags = SSH_FXF_WRITE | SSH_FXF_CREAT | (x->conflict == TE_CONFLICT_REPLACE ? SSH_FXF_TRUNC : SSH_FXF_EXCL);
    req *q = new_req(r, K_X_OPEN, r->xser);
    if (q == nullptr) {
        fail_active(r, "busy");
        return false;
    }
    sent(r, q, sftp_enc_open(&r->out, q->id, x->remote, flags, (uint32_t)(sb.st_mode & 0777)));
    return true;
}

static void start_scp(te_remote *r, te_xfer *x);

static void start_next(te_remote *r)
{
    if (r->active != NO_XFER) {
        return;
    }
    for (size_t i = 0; i < r->qlen; i++) {
        te_xfer *x = &r->q[i];
        if (x->state != TE_XS_QUEUED) {
            continue;
        }
        bool scp = x->kind == TE_XK_SCP_DOWN || x->kind == TE_XK_SCP_UP;
        if ((scp && r->state != TE_RS_NO_SFTP) || (!scp && r->state != TE_RS_READY)) {
            continue;
        }
        r->active = i;
        r->xser++;
        x->state = TE_XS_ACTIVE;
        x->started_ms = now_ms();
        x->done = 0;
        x->error[0] = '\0';
        r->next_off = 0;
        r->inflight = 0;
        r->eof = false;
        r->closing = false;
        r->xhl = 0;
        r->have_size = x->size > 0 || x->kind == TE_XK_UPLOAD;
        switch (x->kind) {
        case TE_XK_DOWNLOAD:    (void)start_download(r, x); break;
        case TE_XK_UPLOAD:      (void)start_upload(r, x); break;
        case TE_XK_MKDIR: {
            req *q = new_req(r, K_X_STATUS, r->xser);
            if (q != nullptr) sent(r, q, sftp_enc_mkdir(&r->out, q->id, x->remote, 0755));
            break;
        }
        case TE_XK_REMOVE:      send_path(r, SSH_FXP_REMOVE, K_X_STATUS, r->xser, x->remote); break;
        case TE_XK_RMDIR:       send_path(r, SSH_FXP_RMDIR, K_X_STATUS, r->xser, x->remote); break;
        case TE_XK_SCAN_DOWN:
            if (x->local != nullptr && mkdir(x->local, 0755) != 0 && errno != EEXIST) {
                fail_active(r, strerror(errno));
                break;
            }
            send_path(r, SSH_FXP_OPENDIR, K_SCAN_OPENDIR, r->xser, x->remote);
            break;
        case TE_XK_SCAN_DELETE: send_path(r, SSH_FXP_OPENDIR, K_SCAN_OPENDIR, r->xser, x->remote); break;
        case TE_XK_SCP_DOWN:
        case TE_XK_SCP_UP:      start_scp(r, x); break;
        default:                fail_active(r, "unknown step"); break;
        }
        changed(r);
        return;
    }
    /* Queue finished: refresh the listing once if anything changed it. */
    if (r->want_refresh && r->state == TE_RS_READY && !r->loading) {
        r->want_refresh = false;
        remote_refresh(r);
    }
}

static void fill_download(te_remote *r)
{
    te_xfer *x = &r->q[r->active];
    while (r->inflight < PIPELINE && !r->eof && !r->closing && (!r->have_size || r->next_off < x->size)) {
        req *q = new_req(r, K_X_READ, r->xser);
        if (q == nullptr) {
            break;
        }
        q->off = r->next_off;
        q->len = TE_SFTP_CHUNK;
        if (!sftp_enc_read(&r->out, q->id, r->xh, r->xhl, q->off, q->len)) {
            q->used = false;
            break;
        }
        r->next_off += TE_SFTP_CHUNK;
        r->inflight++;
    }
    check_done(r);
}

static void fill_upload(te_remote *r)
{
    static uint8_t chunk[TE_SFTP_CHUNK];
    te_xfer *x = &r->q[r->active];
    while (r->inflight < PIPELINE && !r->closing && r->next_off < x->size) {
        uint64_t left = x->size - r->next_off;
        size_t want = left < TE_SFTP_CHUNK ? (size_t)left : TE_SFTP_CHUNK;
        ssize_t got = pread(r->xfd, chunk, want, (off_t)r->next_off);
        if (got < 0) {
            fail_active(r, strerror(errno));
            return;
        }
        if (got == 0) {
            x->size = r->next_off;   /* the file shrank while uploading */
            break;
        }
        req *q = new_req(r, K_X_WRITE, r->xser);
        if (q == nullptr) {
            break;
        }
        q->off = r->next_off;
        q->len = (uint32_t)got;
        if (!sftp_enc_write(&r->out, q->id, r->xh, r->xhl, q->off, chunk, (size_t)got)) {
            q->used = false;
            break;
        }
        r->next_off += (uint64_t)got;
        r->inflight++;
    }
    check_done(r);
}

static void check_done(te_remote *r)
{
    if (r->active == NO_XFER || r->closing || r->inflight > 0 || r->xhl == 0) {
        return;
    }
    te_xfer *x = &r->q[r->active];
    bool finished = x->kind == TE_XK_UPLOAD ? r->next_off >= x->size
                                            : (r->eof || (r->have_size && r->next_off >= x->size));
    if (!finished) {
        return;
    }
    r->closing = true;
    send_close(r, K_X_CLOSE, r->xser, r->xh, r->xhl);
    r->xhl = 0;
}

static bool same_dir(const char *path, const char *dir)
{
    const char *slash = strrchr(path, '/');
    if (slash == nullptr) {
        return false;
    }
    size_t n = (size_t)(slash - path);
    if (n == 0) {
        return strcmp(dir, "/") == 0;
    }
    return strlen(dir) == n && strncmp(path, dir, n) == 0;
}

static void transfer_closed(te_remote *r, bool ok, const char *msg)
{
    te_xfer *x = &r->q[r->active];
    if (!ok) {
        fail_active(r, msg);
        return;
    }
    if (x->kind == TE_XK_DOWNLOAD) {
        if (fsync(r->xfd) != 0) {
            fail_active(r, strerror(errno));
            return;
        }
        close(r->xfd);
        r->xfd = -1;
        int rc = x->conflict == TE_CONFLICT_REPLACE
                     ? renameat(r->xdirfd, r->xtmp, r->xdirfd, r->xfinal)
                     : renameat2(r->xdirfd, r->xtmp, r->xdirfd, r->xfinal, RENAME_NOREPLACE);
        if (rc != 0) {
            fail_active(r, errno == EEXIST ? "a file with that name appeared meanwhile" : strerror(errno));
            return;
        }
        r->xtmp[0] = '\0';   /* renamed: nothing to clean up */
        char done_path[TE_REMOTE_PATH_MAX] = { 0 };
        const char *slash = strrchr(x->local, '/');
        if (slash != nullptr) {
            (void)str_fmt(done_path, sizeof done_path, "%.*s/%s", (int)(slash - x->local), x->local, r->xfinal);
        }
        bool edit = x->edit;
        char remote_copy[TE_REMOTE_PATH_MAX] = { 0 };
        (void)str_copy(remote_copy, sizeof remote_copy, x->remote);
        finish_active(r, TE_XS_DONE, nullptr);
        if (edit && done_path[0] != '\0') {
            add_edit_watch(r, done_path, remote_copy);
        }
        return;
    }
    if (x->remote != nullptr && same_dir(x->remote, r->cwd)) {
        r->want_refresh = true;
    }
    if (x->edit) {
        const char *name = strrchr(x->remote, '/');
        char t[300] = { 0 };
        (void)str_fmt(t, sizeof t, "Saved %s to %s", name != nullptr ? name + 1 : x->remote, r->title);
        toast(r, t);
    }
    finish_active(r, TE_XS_DONE, nullptr);
}

/* After a folder was read: queue what is inside it, right after the walk step. */
static void scan_done(te_remote *r)
{
    te_xfer *x = &r->q[r->active];
    size_t pos = r->active + 1;
    char rpath[TE_REMOTE_PATH_MAX] = { 0 };
    char lpath[TE_REMOTE_PATH_MAX] = { 0 };
    char lname[TE_REMOTE_PATH_MAX] = { 0 };
    bool overflow = false;
    te_xfer_kind kind = x->kind;
    char *xremote = str_dup(x->remote);
    char *xlocal = x->local != nullptr ? str_dup(x->local) : nullptr;
    uint8_t depth = x->depth;
    te_conflict conflict = x->conflict;
    if (xremote == nullptr || (x->local != nullptr && xlocal == nullptr)) {
        TE_FREE(xremote);
        TE_FREE(xlocal);
        fail_active(r, "out of memory");
        return;
    }
    for (size_t i = 0; i < r->nscan && !overflow; i++) {
        const te_rentry *e = &r->scan[i];
        if (e->link) {
            continue;   /* never follow links in a recursive copy or delete */
        }
        if (remote_join(xremote, e->name, rpath, sizeof rpath) != TE_OK) {
            continue;
        }
        if (kind == TE_XK_SCAN_DELETE) {
            if (e->dir && depth + 1 > WALK_DEPTH) {
                overflow = true;
                break;
            }
            overflow = !queue_insert(r, pos++, e->dir ? TE_XK_SCAN_DELETE : TE_XK_REMOVE, nullptr, rpath, 0,
                                     TE_CONFLICT_REPLACE, false, (uint8_t)(depth + 1));
        } else {
            if (!remote_local_name(e->name, lname, sizeof lname) ||
                remote_join(xlocal, lname, lpath, sizeof lpath) != TE_OK) {
                continue;
            }
            if (e->dir && depth + 1 > WALK_DEPTH) {
                overflow = true;
                break;
            }
            overflow = !queue_insert(r, pos++, e->dir ? TE_XK_SCAN_DOWN : TE_XK_DOWNLOAD, lpath, rpath,
                                     e->has_size ? e->size : 0, conflict, false, (uint8_t)(depth + 1));
        }
    }
    if (!overflow && kind == TE_XK_SCAN_DELETE) {
        overflow = !queue_insert(r, pos, TE_XK_RMDIR, nullptr, xremote, 0, TE_CONFLICT_REPLACE, false, depth);
    }
    TE_FREE(xremote);
    TE_FREE(xlocal);
    if (overflow) {
        fail_active(r, "folder too large or too deep (limit 20000 items, 32 levels)");
        return;
    }
    finish_active(r, TE_XS_DONE, nullptr);
}

/* ------------------------------------------------------------------------- */
/* Replies */

static void handle_reply(te_remote *r, const te_sftp_reply *rep)
{
    if (rep->type == SSH_FXP_VERSION) {
        if (r->state != TE_RS_CONNECTING) {
            fail_conn(r, "unexpected VERSION");
            return;
        }
        if (rep->version < 3) {
            fail_conn(r, "the server's SFTP is too old (version < 3)");
            return;
        }
        r->state = TE_RS_READY;
        r->tty_len = 0;
        send_path(r, SSH_FXP_REALPATH, K_HOME, 0, ".");
        changed(r);
        return;
    }
    req *q = find_req(r, rep->id);
    if (q == nullptr) {
        fail_conn(r, "reply to an unknown request");
        return;
    }
    req rq = *q;
    q->used = false;
    bool is_status = rep->type == SSH_FXP_STATUS;
    uint32_t code = is_status ? rep->status : SSH_FX_OK;
    bool stale_x = rq.gen != r->xser || r->active == NO_XFER;

    switch (rq.kind) {
    case K_NONE:
        return;
    case K_HOME:
        if (r->loading) {
            start_next(r);   /* something else (following the terminal) is already being listed */
            return;
        }
        if (rep->type == SSH_FXP_NAME && rep->nnames >= 1) {
            if (r->pending_path[0] != '\0') {
                char p[TE_REMOTE_PATH_MAX] = { 0 };
                (void)str_copy(p, sizeof p, r->pending_path);
                r->pending_path[0] = '\0';
                start_list(r, p);
            } else {
                start_list(r, rep->names[0].name);
            }
        } else {
            start_list(r, "/");
        }
        start_next(r);
        return;
    case K_LIST_REALPATH:
        if (rq.gen != r->list_gen) return;
        if (rep->type == SSH_FXP_NAME && rep->nnames >= 1) {
            (void)str_copy(r->list_target, sizeof r->list_target, rep->names[0].name);
            send_path(r, SSH_FXP_OPENDIR, K_LIST_OPENDIR, rq.gen, r->list_target);
        } else {
            list_failed(r, "folder", code, rep->msg);
        }
        return;
    case K_LIST_OPENDIR:
        if (rep->type == SSH_FXP_HANDLE) {
            if (rq.gen != r->list_gen) {
                send_close(r, K_NONE, 0, rep->handle, rep->handle_len);
                return;
            }
            memcpy(r->lh, rep->handle, rep->handle_len);
            r->lhl = rep->handle_len;
            req *n = new_req(r, K_LIST_READDIR, rq.gen);
            if (n != nullptr) sent(r, n, sftp_enc_handle(&r->out, SSH_FXP_READDIR, n->id, r->lh, r->lhl));
        } else if (rq.gen == r->list_gen) {
            list_failed(r, r->list_target, code, rep->msg);
        }
        return;
    case K_LIST_READDIR:
        if (rep->type == SSH_FXP_NAME && rq.gen == r->list_gen) {
            for (size_t i = 0; i < rep->nnames; i++) {
                const char *nm = rep->names[i].name;
                if (strcmp(nm, ".") == 0 || strcmp(nm, "..") == 0) continue;
                if (!push_entry(&r->pend, &r->npend, &r->cappend, &rep->names[i])) break;
            }
            req *n = new_req(r, K_LIST_READDIR, rq.gen);
            if (n != nullptr) sent(r, n, sftp_enc_handle(&r->out, SSH_FXP_READDIR, n->id, r->lh, r->lhl));
            return;
        }
        send_close(r, K_LIST_CLOSE, rq.gen, r->lh, r->lhl);
        if (rq.gen != r->list_gen) return;
        if (code == SSH_FX_EOF) {
            list_done(r);
        } else {
            list_failed(r, r->list_target, code, rep->msg);
        }
        return;
    case K_LIST_CLOSE:
        return;
    case K_SIMPLE:
        if (code == SSH_FX_OK) {
            toast(r, rq.what);
            remote_refresh(r);
        } else {
            char t[300] = { 0 };
            (void)str_fmt(t, sizeof t, "Failed: %s", rep->msg[0] != '\0' ? rep->msg : sftp_status_text(code));
            toast(r, t);
        }
        return;
    default:
        break;
    }

    /* Everything below belongs to the active transfer step. */
    if (stale_x) {
        if (rep->type == SSH_FXP_HANDLE) {
            send_close(r, K_NONE, 0, rep->handle, rep->handle_len);   /* a cancelled transfer's file */
        }
        return;
    }
    te_xfer *x = &r->q[r->active];
    char err[160] = { 0 };
    (void)str_copy(err, sizeof err, rep->msg[0] != '\0' ? rep->msg : sftp_status_text(code));
    switch (rq.kind) {
    case K_X_OPEN:
        if (rep->type != SSH_FXP_HANDLE) {
            fail_active(r, code == SSH_FX_FAILURE && x->kind == TE_XK_UPLOAD && x->conflict != TE_CONFLICT_REPLACE
                               ? "already exists on the server" : err);
            return;
        }
        memcpy(r->xh, rep->handle, rep->handle_len);
        r->xhl = rep->handle_len;
        if (x->kind == TE_XK_DOWNLOAD) fill_download(r);
        else fill_upload(r);
        return;
    case K_X_READ:
        r->inflight--;
        if (rep->type == SSH_FXP_DATA) {
            if (rep->data_len == 0 || rep->data_len > rq.len) {
                fail_active(r, "the server sent more data than asked for");
                return;
            }
            size_t off = 0;
            while (off < rep->data_len) {
                ssize_t w = pwrite(r->xfd, rep->data + off, rep->data_len - off, (off_t)(rq.off + off));
                if (w < 0 && errno == EINTR) continue;
                if (w <= 0) {
                    fail_active(r, strerror(errno != 0 ? errno : EIO));
                    return;
                }
                off += (size_t)w;
            }
            x->done += rep->data_len;
            if (rep->data_len < rq.len && !r->eof) {
                /* A short read: ask for the rest of that range. */
                req *n = new_req(r, K_X_READ, r->xser);
                if (n != nullptr) {
                    n->off = rq.off + rep->data_len;
                    n->len = rq.len - (uint32_t)rep->data_len;
                    sent(r, n, sftp_enc_read(&r->out, n->id, r->xh, r->xhl, n->off, n->len));
                    r->inflight++;
                }
            }
            fill_download(r);
            changed(r);
        } else if (code == SSH_FX_EOF) {
            r->eof = true;
            check_done(r);
        } else {
            fail_active(r, err);
        }
        return;
    case K_X_WRITE:
        r->inflight--;
        if (code != SSH_FX_OK) {
            fail_active(r, err);
            return;
        }
        x->done += rq.len;
        fill_upload(r);
        changed(r);
        return;
    case K_X_CLOSE:
        transfer_closed(r, code == SSH_FX_OK, err);
        return;
    case K_X_STATUS:
        if (code == SSH_FX_OK || (x->kind == TE_XK_MKDIR && code == SSH_FX_FAILURE)) {
            if (x->remote != nullptr && same_dir(x->remote, r->cwd)) r->want_refresh = true;
            finish_active(r, TE_XS_DONE, nullptr);
        } else {
            fail_active(r, err);
        }
        return;
    case K_SCAN_OPENDIR:
        if (rep->type != SSH_FXP_HANDLE) {
            fail_active(r, err);
            return;
        }
        memcpy(r->xh, rep->handle, rep->handle_len);
        r->xhl = rep->handle_len;
        {
            req *n = new_req(r, K_SCAN_READDIR, r->xser);
            if (n != nullptr) sent(r, n, sftp_enc_handle(&r->out, SSH_FXP_READDIR, n->id, r->xh, r->xhl));
        }
        return;
    case K_SCAN_READDIR:
        if (rep->type == SSH_FXP_NAME) {
            for (size_t i = 0; i < rep->nnames; i++) {
                const char *nm = rep->names[i].name;
                if (strcmp(nm, ".") == 0 || strcmp(nm, "..") == 0) continue;
                if (!push_entry(&r->scan, &r->nscan, &r->capscan, &rep->names[i])) {
                    fail_active(r, "folder too large");
                    return;
                }
            }
            req *n = new_req(r, K_SCAN_READDIR, r->xser);
            if (n != nullptr) sent(r, n, sftp_enc_handle(&r->out, SSH_FXP_READDIR, n->id, r->xh, r->xhl));
            return;
        }
        send_close(r, K_SCAN_CLOSE, r->xser, r->xh, r->xhl);
        r->xhl = 0;
        if (code != SSH_FX_EOF) {
            fail_active(r, err);
        }
        return;
    case K_SCAN_CLOSE:
        scan_done(r);
        return;
    default:
        return;
    }
}

static void read_out(te_remote *r)
{
    for (;;) {
        if (r->in_len >= IN_LIMIT) {
            fail_conn(r, "oversized SFTP packet");
            return;
        }
        ssize_t n = read(r->proc.out_fd, r->in + r->in_len, IN_LIMIT - r->in_len);
        if (n > 0) {
            r->in_len += (size_t)n;
        } else if (n == 0) {
            child_exited(r);
            return;
        } else if (errno == EAGAIN || errno == EINTR) {
            break;
        } else {
            child_exited(r);
            return;
        }
        size_t total = 0;
        int f = 0;
        while ((f = sftp_frame(r->in, r->in_len, &total)) == 1) {
            te_sftp_reply rep = { 0 };
            te_err rc = sftp_parse(r->in + 4, total - 4, &rep);
            if (rc != TE_OK) {
                fail_conn(r, "malformed reply from the server");
                return;
            }
            handle_reply(r, &rep);
            sftp_reply_free(&rep);
            if (r->state == TE_RS_FAILED || r->proc.out_fd < 0) {
                return;
            }
            memmove(r->in, r->in + total, r->in_len - total);
            r->in_len -= total;
        }
        if (f < 0) {
            fail_conn(r, "malformed packet length from the server");
            return;
        }
    }
    if (r->tag_out >= 0 && r->host.rearm != nullptr) {
        r->host.rearm(r->host.user, r->tag_out);
    }
}

/* ------------------------------------------------------------------------- */
/* SCP fallback */

static void start_scp(te_remote *r, te_xfer *x)
{
    const char *argv[48] = { 0 };
    char storage[4096] = { 0 };
    char target[TE_REMOTE_PATH_MAX + TE_SESS_HOST_MAX + 8] = { 0 };
    char quoted[TE_REMOTE_PATH_MAX] = { 0 };
    if (!remote_scp_quote(x->remote, quoted, sizeof quoted)) {
        fail_active(r, "this remote path cannot be used with SCP");
        return;
    }
    bool v6 = strchr(r->s.host, ':') != nullptr;
    if (str_fmt(target, sizeof target, v6 ? "[%s]:%s" : "%s:%s", r->s.host, quoted) != TE_OK) {
        fail_active(r, "path too long");
        return;
    }
    size_t n = 0;
    argv[n++] = "scp";
    argv[n++] = "-O";   /* the classic protocol: what Cisco IOS speaks */
    argv[n++] = "-q";
    n = add_common_ssh_opts(r, argv, n, storage, sizeof storage, true);
    argv[n++] = "--";
    if (x->kind == TE_XK_SCP_UP) {
        argv[n++] = x->local;
        argv[n++] = target;
    } else {
        argv[n++] = target;
        argv[n++] = x->local;
    }
    argv[n] = nullptr;
    r->tty_len = 0;
    if (proc_spawn(&r->scp, argv) != TE_OK || !watch_fd(r, r->scp.tty_fd, &r->tag_scp)) {
        proc_stop(&r->scp);
        fail_active(r, "could not start scp");
        return;
    }
    close(r->scp.in_fd);
    r->scp.in_fd = -1;
}

static void scp_check(te_remote *r)
{
    if (r->active == NO_XFER || r->scp.pid <= 0) {
        return;
    }
    int st = 0;
    if (r->scp.tty_fd >= 0 || !proc_reap(&r->scp, &st)) {
        return;   /* still running (its terminal is open) */
    }
    unwatch(r, &r->tag_scp);
    char line[200] = { 0 };
    last_line(r, line, sizeof line);
    proc_stop(&r->scp);
    if (st == 0) {
        te_xfer *x = &r->q[r->active];
        char t[300] = { 0 };
        (void)str_fmt(t, sizeof t, "SCP %s finished", x->kind == TE_XK_SCP_UP ? "upload" : "download");
        toast(r, t);
        finish_active(r, TE_XS_DONE, nullptr);
    } else {
        fail_active(r, line[0] != '\0' ? line : "scp failed");
    }
}

void remote_scp_upload(te_remote *r, const char *local, const char *remote_path)
{
    if (r != nullptr && local != nullptr && remote_path != nullptr) {
        (void)queue_insert(r, r->qlen, TE_XK_SCP_UP, local, remote_path, 0, TE_CONFLICT_REPLACE, false, 0);
        start_next(r);
    }
}

void remote_scp_download(te_remote *r, const char *remote_path, const char *local_dir)
{
    if (r == nullptr || remote_path == nullptr || local_dir == nullptr) {
        return;
    }
    /* scp would silently overwrite a local file of the same name: pick a free one first. */
    const char *base = remote_path;
    for (const char *s = remote_path; *s != '\0'; s++) {
        if (*s == '/' || *s == ':') base = s + 1;
    }
    char name[TE_REMOTE_PATH_MAX] = { 0 };
    char unique[TE_REMOTE_PATH_MAX] = { 0 };
    char local[TE_REMOTE_PATH_MAX] = { 0 };
    int dfd = open(local_dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (dfd < 0 || !remote_local_name(base, name, sizeof name) || remote_unique_name(dfd, name, unique, sizeof unique) != TE_OK ||
        remote_join(local_dir, unique, local, sizeof local) != TE_OK) {
        if (dfd >= 0) close(dfd);
        toast(r, "Cannot choose a local file name for that download");
        return;
    }
    close(dfd);
    (void)queue_insert(r, r->qlen, TE_XK_SCP_DOWN, local, remote_path, 0, TE_CONFLICT_REPLACE, false, 0);
    start_next(r);
}

/* ------------------------------------------------------------------------- */
/* Edit locally */

static void add_edit_watch(te_remote *r, const char *local, const char *remote)
{
    if (r->nedits >= EDIT_MAX) {
        toast(r, "Too many files open for editing");
        return;
    }
    if (r->ino_fd < 0) {
        r->ino_fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
        if (r->ino_fd < 0 || !watch_fd(r, r->ino_fd, &r->tag_ino)) {
            toast(r, "Cannot watch the edited file; changes will not be uploaded");
            return;
        }
    }
    char dir[TE_REMOTE_PATH_MAX] = { 0 };
    const char *slash = strrchr(local, '/');
    if (slash == nullptr || str_copy_n(dir, sizeof dir, local, (size_t)(slash - local)) != TE_OK) {
        return;
    }
    int wd = inotify_add_watch(r->ino_fd, dir, IN_CLOSE_WRITE | IN_MOVED_TO);
    if (wd < 0) {
        toast(r, "Cannot watch the edited file; changes will not be uploaded");
        return;
    }
    edit_watch *e = &r->edits[r->nedits];
    memset(e, 0, sizeof *e);
    e->wd = wd;
    e->local = str_dup(local);
    e->remote = str_dup(remote);
    e->dir = str_dup(dir);
    if (e->local == nullptr || e->remote == nullptr || e->dir == nullptr) {
        TE_FREE(e->local);
        TE_FREE(e->remote);
        TE_FREE(e->dir);
        return;
    }
    e->ignore_until = now_ms() + 1000u;
    r->nedits++;
    if (r->host.edit_open != nullptr) {
        r->host.edit_open(r->host.user, local);
    }
}

static void read_inotify(te_remote *r)
{
    _Alignas(struct inotify_event) char buf[4096];
    for (;;) {
        ssize_t n = read(r->ino_fd, buf, sizeof buf);
        if (n <= 0) {
            break;
        }
        size_t off = 0;
        while (off + sizeof(struct inotify_event) <= (size_t)n) {
            const struct inotify_event *ev = (const struct inotify_event *)(const void *)(buf + off);
            size_t step = sizeof *ev + ev->len;
            if (step > (size_t)n - off) {
                break;
            }
            if (ev->len > 0) {
                for (size_t i = 0; i < r->nedits; i++) {
                    edit_watch *e = &r->edits[i];
                    const char *base = strrchr(e->local, '/');
                    size_t nl = strnlen(ev->name, ev->len);   /* the name is NUL-padded */
                    if (e->wd == ev->wd && base != nullptr && nl == strlen(base + 1) && memcmp(ev->name, base + 1, nl) == 0) {
                        if (now_ms() >= e->ignore_until) {
                            e->due = now_ms() + EDIT_SETTLE_MS;   /* editors save in bursts */
                        }
                    }
                }
            }
            off += step;
        }
    }
    if (r->tag_ino >= 0 && r->host.rearm != nullptr) {
        r->host.rearm(r->host.user, r->tag_ino);
    }
}

static void service_edits(te_remote *r)
{
    uint64_t now = now_ms();
    for (size_t i = 0; i < r->nedits; i++) {
        edit_watch *e = &r->edits[i];
        if (e->due != 0 && now >= e->due) {
            e->due = 0;
            (void)queue_insert(r, r->qlen, TE_XK_UPLOAD, e->local, e->remote, 0, TE_CONFLICT_REPLACE, true, 0);
        }
    }
}

void remote_edit(te_remote *r, const char *name)
{
    const te_rentry *e = r != nullptr ? find_entry(r, name) : nullptr;
    if (e == nullptr || e->dir) {
        return;
    }
    if (r->edit_dir[0] != '/') {
        toast(r, "No private folder for editing");
        return;
    }
    char sub[TE_REMOTE_PATH_MAX] = { 0 };
    char local[TE_REMOTE_PATH_MAX] = { 0 };
    char lname[TE_REMOTE_PATH_MAX] = { 0 };
    char rpath[TE_REMOTE_PATH_MAX] = { 0 };
    if (mkdir(r->edit_dir, 0700) != 0 && errno != EEXIST) {
        toast(r, "Cannot create the editing folder");
        return;
    }
    struct stat sb = { 0 };
    if (lstat(r->edit_dir, &sb) != 0 || !S_ISDIR(sb.st_mode) || sb.st_uid != getuid() || (sb.st_mode & 0077) != 0) {
        toast(r, "The editing folder is not private; not using it");
        return;
    }
    if (str_fmt(sub, sizeof sub, "%s/%u", r->edit_dir, r->edit_counter++) != TE_OK || mkdir(sub, 0700) != 0 ||
        !remote_local_name(name, lname, sizeof lname) || remote_join(sub, lname, local, sizeof local) != TE_OK ||
        remote_join(r->cwd, name, rpath, sizeof rpath) != TE_OK) {
        toast(r, "Cannot prepare the file for editing");
        return;
    }
    (void)queue_insert(r, r->qlen, TE_XK_DOWNLOAD, local, rpath, e->has_size ? e->size : 0, TE_CONFLICT_REPLACE, true, 0);
    start_next(r);
}

/* ------------------------------------------------------------------------- */
/* Public operations */

static void simple(te_remote *r, uint8_t type, const char *path, const char *path2, uint32_t perm, const char *ok_msg)
{
    if (r->state != TE_RS_READY) {
        toast(r, "Not connected");
        return;
    }
    req *q = new_req(r, K_SIMPLE, 0);
    if (q == nullptr) {
        toast(r, "Busy; try again");
        return;
    }
    (void)str_copy(q->what, sizeof q->what, ok_msg);
    bool ok = false;
    switch (type) {
    case SSH_FXP_MKDIR:   ok = sftp_enc_mkdir(&r->out, q->id, path, perm); break;
    case SSH_FXP_RENAME:  ok = sftp_enc_rename(&r->out, q->id, path, path2); break;
    case SSH_FXP_SETSTAT: ok = sftp_enc_chmod(&r->out, q->id, path, perm); break;
    default:              ok = sftp_enc_path(&r->out, type, q->id, path); break;
    }
    sent(r, q, ok);
}

void remote_mkdir(te_remote *r, const char *name)
{
    char p[TE_REMOTE_PATH_MAX] = { 0 };
    char msg[96] = { 0 };
    if (r != nullptr && remote_join(r->cwd, name, p, sizeof p) == TE_OK) {
        (void)str_fmt(msg, sizeof msg, "Created folder %.80s", name);
        simple(r, SSH_FXP_MKDIR, p, nullptr, 0755, msg);
    }
}

void remote_rename(te_remote *r, const char *from, const char *to)
{
    char a[TE_REMOTE_PATH_MAX] = { 0 };
    char b[TE_REMOTE_PATH_MAX] = { 0 };
    char msg[96] = { 0 };
    if (r != nullptr && remote_join(r->cwd, from, a, sizeof a) == TE_OK && remote_join(r->cwd, to, b, sizeof b) == TE_OK) {
        (void)str_fmt(msg, sizeof msg, "Renamed to %.80s", to);
        simple(r, SSH_FXP_RENAME, a, b, 0, msg);
    }
}

void remote_chmod(te_remote *r, const char *name, uint32_t mode)
{
    char p[TE_REMOTE_PATH_MAX] = { 0 };
    char msg[96] = { 0 };
    if (r != nullptr && remote_join(r->cwd, name, p, sizeof p) == TE_OK) {
        (void)str_fmt(msg, sizeof msg, "Permissions of %.60s set to %03o", name, (unsigned)(mode & 07777u));
        simple(r, SSH_FXP_SETSTAT, p, nullptr, mode, msg);
    }
}

void remote_delete(te_remote *r, const char *const *names, size_t n)
{
    if (r == nullptr) {
        return;
    }
    for (size_t i = 0; i < n; i++) {
        const te_rentry *e = find_entry(r, names[i]);
        char p[TE_REMOTE_PATH_MAX] = { 0 };
        if (e == nullptr || remote_join(r->cwd, names[i], p, sizeof p) != TE_OK) {
            continue;
        }
        (void)queue_insert(r, r->qlen, e->dir && !e->link ? TE_XK_SCAN_DELETE : TE_XK_REMOVE, nullptr, p, 0,
                           TE_CONFLICT_REPLACE, false, 0);
    }
    start_next(r);
}

void remote_download(te_remote *r, const char *const *names, size_t n, const char *local_dir, te_conflict c)
{
    if (r == nullptr || local_dir == nullptr) {
        return;
    }
    for (size_t i = 0; i < n; i++) {
        const te_rentry *e = find_entry(r, names[i]);
        char rp[TE_REMOTE_PATH_MAX] = { 0 };
        char lp[TE_REMOTE_PATH_MAX] = { 0 };
        char ln[TE_REMOTE_PATH_MAX] = { 0 };
        if (e == nullptr || remote_join(r->cwd, names[i], rp, sizeof rp) != TE_OK ||
            !remote_local_name(names[i], ln, sizeof ln) || remote_join(local_dir, ln, lp, sizeof lp) != TE_OK) {
            continue;
        }
        (void)queue_insert(r, r->qlen, e->dir && !e->link ? TE_XK_SCAN_DOWN : TE_XK_DOWNLOAD, lp, rp,
                           e->has_size ? e->size : 0, c, false, 0);
    }
    start_next(r);
}

/* Queue a local folder for upload: the remote folder first, then its contents. */
static void upload_dir(te_remote *r, const char *local, const char *remote, int depth, te_conflict c)
{
    if (depth > WALK_DEPTH || !queue_insert(r, r->qlen, TE_XK_MKDIR, nullptr, remote, 0, TE_CONFLICT_REPLACE, false, 0)) {
        return;
    }
    DIR *d = opendir(local);
    if (d == nullptr) {
        return;
    }
    struct dirent *de = nullptr;
    while ((de = readdir(d)) != nullptr) {
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) {
            continue;
        }
        char lp[TE_REMOTE_PATH_MAX] = { 0 };
        char rp[TE_REMOTE_PATH_MAX] = { 0 };
        struct stat sb = { 0 };
        if (remote_join(local, de->d_name, lp, sizeof lp) != TE_OK || remote_join(remote, de->d_name, rp, sizeof rp) != TE_OK ||
            lstat(lp, &sb) != 0) {
            continue;
        }
        if (S_ISDIR(sb.st_mode)) {
            upload_dir(r, lp, rp, depth + 1, c);
        } else if (S_ISREG(sb.st_mode)) {
            /* Inside a folder: replace only if the user chose Replace; otherwise never overwrite. */
            (void)queue_insert(r, r->qlen, TE_XK_UPLOAD, lp, rp, (uint64_t)sb.st_size,
                               c == TE_CONFLICT_REPLACE ? TE_CONFLICT_REPLACE : TE_CONFLICT_SKIP, false, 0);
        }
        /* symlinks, devices and sockets are skipped */
    }
    closedir(d);
}

static const char *base_name(const char *path)
{
    size_t n = strlen(path);
    while (n > 1 && path[n - 1] == '/') n--;
    const char *s = path + n;
    while (s > path && s[-1] != '/') s--;
    return s;
}

size_t remote_upload_conflicts(const te_remote *r, const char *const *paths, size_t n)
{
    size_t c = 0;
    for (size_t i = 0; r != nullptr && i < n; i++) {
        char nm[TE_REMOTE_PATH_MAX] = { 0 };
        if (str_copy(nm, sizeof nm, base_name(paths[i])) == TE_OK) {
            size_t l = strlen(nm);
            while (l > 1 && nm[l - 1] == '/') nm[--l] = '\0';
            c += find_entry(r, nm) != nullptr;
        }
    }
    return c;
}

void remote_upload(te_remote *r, const char *const *paths, size_t n, te_conflict c)
{
    if (r == nullptr) {
        return;
    }
    size_t skipped = 0;
    for (size_t i = 0; i < n; i++) {
        struct stat sb = { 0 };
        char name[TE_REMOTE_PATH_MAX] = { 0 };
        char rp[TE_REMOTE_PATH_MAX] = { 0 };
        if (lstat(paths[i], &sb) != 0 || str_copy(name, sizeof name, base_name(paths[i])) != TE_OK) {
            char t[TE_REMOTE_PATH_MAX + 64] = { 0 };
            (void)str_fmt(t, sizeof t, "Cannot read %s: %s", paths[i], strerror(errno));
            toast(r, t);
            continue;
        }
        size_t l = strlen(name);
        while (l > 1 && name[l - 1] == '/') name[--l] = '\0';
        if (name[0] == '\0' || strcmp(name, "/") == 0) {
            continue;
        }
        bool exists = find_entry(r, name) != nullptr;
        if (exists && c == TE_CONFLICT_SKIP) {
            skipped++;
            continue;
        }
        if (exists && c == TE_CONFLICT_KEEP_BOTH) {
            const char *dot = strrchr(name, '.');
            size_t stem = dot != nullptr && dot != name ? (size_t)(dot - name) : strlen(name);
            const char *ext = dot != nullptr && dot != name ? dot : "";
            char alt[TE_REMOTE_PATH_MAX] = { 0 };
            for (int k = 1; k < 1000; k++) {
                if (str_fmt(alt, sizeof alt, "%.*s (%d)%s", (int)stem, name, k, ext) != TE_OK) break;
                if (find_entry(r, alt) == nullptr) break;
            }
            (void)str_copy(name, sizeof name, alt);
        }
        if (remote_join(r->cwd, name, rp, sizeof rp) != TE_OK) {
            continue;
        }
        if (S_ISDIR(sb.st_mode)) {
            upload_dir(r, paths[i], rp, 0, c);
        } else if (S_ISREG(sb.st_mode)) {
            (void)queue_insert(r, r->qlen, TE_XK_UPLOAD, paths[i], rp, (uint64_t)sb.st_size,
                               exists ? TE_CONFLICT_REPLACE : TE_CONFLICT_SKIP, false, 0);
        }
    }
    if (skipped > 0) {
        char t[96] = { 0 };
        (void)str_fmt(t, sizeof t, "Skipped %zu existing item%s", skipped, skipped == 1 ? "" : "s");
        toast(r, t);
    }
    start_next(r);
}

/* ------------------------------------------------------------------------- */
/* Lifecycle and service */

te_err remote_create(te_remote **out, const te_session *s, const te_session_eff *eff, const te_remote_opts *o,
                     const te_remote_host *h)
{
    if (out == nullptr || s == nullptr || eff == nullptr || o == nullptr || h == nullptr || s->type != TE_SESS_SSH) {
        return TE_ERR_INVAL;
    }
    *out = nullptr;
    te_remote *r = te_calloc(1, sizeof *r);
    if (r == nullptr) {
        return TE_ERR_NOMEM;
    }
    r->in = te_malloc(IN_LIMIT);
    if (r->in == nullptr) {
        TE_FREE(r);
        return TE_ERR_NOMEM;
    }
    r->host = *h;
    r->s = *s;
    r->eff = *eff;
    r->keepalive = o->keepalive;
    (void)str_copy(r->control_dir, sizeof r->control_dir, o->control_dir != nullptr ? o->control_dir : "");
    (void)str_copy(r->edit_dir, sizeof r->edit_dir, o->edit_dir != nullptr ? o->edit_dir : "");
    if (eff->user[0] != '\0') {
        (void)str_fmt(r->title, sizeof r->title, "%s@%s", eff->user, s->host);
    } else {
        (void)str_copy(r->title, sizeof r->title, s->host);
    }
    proc_init(&r->proc);
    proc_init(&r->scp);
    sbuf_init(&r->out, OUT_LIMIT);
    r->tag_out = r->tag_tty = r->tag_scp = r->tag_ino = -1;
    r->ino_fd = -1;
    r->xfd = -1;
    r->xdirfd = -1;
    r->active = NO_XFER;
    r->state = TE_RS_IDLE;
    *out = r;
    return TE_OK;
}

void remote_destroy(te_remote **rp)
{
    if (rp == nullptr || *rp == nullptr) {
        return;
    }
    te_remote *r = *rp;
    close_local(r);
    stop_proc(r);
    unwatch(r, &r->tag_scp);
    proc_stop(&r->scp);
    unwatch(r, &r->tag_ino);
    if (r->ino_fd >= 0) {
        close(r->ino_fd);
        r->ino_fd = -1;
    }
    for (size_t i = 0; i < r->nedits; i++) {
        /* Remove only the copies FeTerm made, and their private folders. */
        (void)unlink(r->edits[i].local);
        (void)rmdir(r->edits[i].dir);
        TE_FREE(r->edits[i].local);
        TE_FREE(r->edits[i].remote);
        TE_FREE(r->edits[i].dir);
    }
    r->nedits = 0;
    for (size_t i = 0; i < r->qlen; i++) {
        free_xfer(&r->q[i]);
    }
    TE_FREE(r->q);
    free_entries(&r->entries, &r->nentries, nullptr);
    free_entries(&r->pend, &r->npend, &r->cappend);
    free_entries(&r->scan, &r->nscan, &r->capscan);
    sbuf_free(&r->out);
    explicit_bzero(r->tty, sizeof r->tty);
    TE_FREE(r->in);
    TE_FREE(*rp);
}

bool remote_fd_ready(te_remote *r, int tag)
{
    if (r == nullptr || tag < 0) {
        return false;
    }
    if (tag == r->tag_out) {
        read_out(r);
    } else if (tag == r->tag_tty) {
        read_tty(r, &r->proc, &r->tag_tty);
    } else if (tag == r->tag_scp) {
        read_tty(r, &r->scp, &r->tag_scp);
        scp_check(r);
    } else if (tag == r->tag_ino) {
        read_inotify(r);
    } else {
        return false;
    }
    remote_service(r);
    return true;
}

void remote_service(te_remote *r)
{
    if (r == nullptr) {
        return;
    }
    while (r->out.len > 0 && r->proc.in_fd >= 0) {
        ssize_t n = write(r->proc.in_fd, r->out.data, r->out.len);
        if (n > 0) {
            sbuf_consume(&r->out, (size_t)n);
        } else if (n < 0 && (errno == EAGAIN || errno == EINTR)) {
            break;
        } else {
            child_exited(r);
            return;
        }
    }
    if (r->proc.pid > 0 && r->proc.out_fd < 0 && r->proc.tty_fd < 0) {
        child_exited(r);
    }
    scp_check(r);
    service_edits(r);
    start_next(r);
}

bool remote_busy(const te_remote *r)
{
    if (r == nullptr) {
        return false;
    }
    if (r->out.len > 0 || r->scp.pid > 0) {
        return true;
    }
    for (size_t i = 0; i < r->nedits; i++) {
        if (r->edits[i].due != 0) {
            return true;   /* an upload of an edited file is about to be queued */
        }
    }
    return false;
}

te_remote_state remote_state(const te_remote *r)
{
    return r != nullptr ? r->state : TE_RS_IDLE;
}

const char *remote_error(const te_remote *r)
{
    return r != nullptr ? r->error : "";
}

const char *remote_title(const te_remote *r)
{
    return r != nullptr ? r->title : "";
}

const char *remote_cwd(const te_remote *r)
{
    return r != nullptr ? r->cwd : "";
}

bool remote_loading(const te_remote *r)
{
    return r != nullptr && (r->loading || r->state == TE_RS_CONNECTING);
}

size_t remote_entries(const te_remote *r, const te_rentry **out)
{
    *out = r != nullptr ? r->entries : nullptr;
    return r != nullptr ? r->nentries : 0;
}

uint32_t remote_generation(const te_remote *r)
{
    return r != nullptr ? r->generation : 0;
}

size_t remote_transfers(const te_remote *r, const te_xfer **out)
{
    *out = r != nullptr ? r->q : nullptr;
    return r != nullptr ? r->qlen : 0;
}

void remote_cancel_all(te_remote *r)
{
    if (r == nullptr) {
        return;
    }
    for (size_t i = 0; i < r->qlen; i++) {
        if (r->q[i].state == TE_XS_QUEUED) {
            r->q[i].state = TE_XS_CANCELLED;
        }
    }
    if (r->active != NO_XFER) {
        if (r->scp.pid > 0) {
            unwatch(r, &r->tag_scp);
            proc_stop(&r->scp);
        }
        finish_active(r, TE_XS_CANCELLED, "cancelled");
    }
    changed(r);
}

void remote_retry_failed(te_remote *r)
{
    if (r == nullptr) {
        return;
    }
    for (size_t i = 0; i < r->qlen; i++) {
        if (r->q[i].state == TE_XS_FAILED || r->q[i].state == TE_XS_CANCELLED) {
            r->q[i].state = TE_XS_QUEUED;
            r->q[i].done = 0;
            r->q[i].error[0] = '\0';
        }
    }
    if (r->state == TE_RS_FAILED) {
        r->state = TE_RS_IDLE;
        r->direct = false;
        remote_connect(r);
    }
    start_next(r);
    changed(r);
}

void remote_clear_finished(te_remote *r)
{
    if (r == nullptr) {
        return;
    }
    size_t k = 0;
    for (size_t i = 0; i < r->qlen; i++) {
        te_xfer_state st = r->q[i].state;
        if (st == TE_XS_QUEUED || st == TE_XS_ACTIVE) {
            if (i == r->active) r->active = k;
            r->q[k++] = r->q[i];
        } else {
            free_xfer(&r->q[i]);
        }
    }
    r->qlen = k;
    changed(r);
}
