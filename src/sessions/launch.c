#include "sessions/launch.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "util/str.h"

/* Copy s into the launch's storage and append it to argv. */
static te_err arg(te_launch *l, const char *s)
{
    if (l->argc >= TE_LAUNCH_ARGV_MAX) {
        return TE_ERR_RANGE;
    }
    size_t n = strlen(s) + 1;
    if (n > sizeof l->storage - l->used) {
        return TE_ERR_TRUNC;
    }
    char *dst = l->storage + l->used;
    memcpy(dst, s, n);
    l->used += n;
    l->argv[l->argc++] = dst;
    l->argv[l->argc] = nullptr;
    return TE_OK;
}

/* prefix followed by a number, as one argument ("ServerAliveInterval=30"). */
static te_err arg_num(te_launch *l, const char *prefix, int v)
{
    char buf[96] = { 0 };
    te_err rc = str_fmt(buf, sizeof buf, "%s%d", prefix, v);
    return rc != TE_OK ? rc : arg(l, buf);
}

te_err launch_expand_home(const char *in, char *out, size_t outsz)
{
    if (strncmp(in, "~/", 2) == 0 || strcmp(in, "~") == 0) {
        const char *home = getenv("HOME");
        if (home == nullptr || home[0] != '/') {
            return TE_ERR_NOTFOUND;
        }
        return str_fmt(out, outsz, "%s%s", home, in + 1);
    }
    return str_copy(out, outsz, in);
}

static te_err build_ssh(const te_session *s, const te_session_eff *eff, const te_launch_opts *o, te_launch *l)
{
    int ka = o->keepalive > 0 ? o->keepalive : 30;
    te_err rc = arg(l, "ssh");
    if (rc == TE_OK) rc = arg(l, "-o");
    if (rc == TE_OK) rc = arg_num(l, "ServerAliveInterval=", ka);
    if (rc == TE_OK) rc = arg(l, "-o");
    if (rc == TE_OK) rc = arg(l, "ServerAliveCountMax=10");
    if (rc == TE_OK) rc = arg(l, "-o");
    if (rc == TE_OK) rc = arg(l, "TCPKeepAlive=yes");
    if (rc == TE_OK && s->share_conn && o->control_dir != nullptr && o->control_dir[0] == '/') {
        /* Connection sharing: a later SFTP channel reuses this login. ControlPersist
         * keeps the shared connection alive for a minute after the last tab closes,
         * so closing the first tab never cuts off the others. */
        rc = arg(l, "-o");
        if (rc == TE_OK) rc = arg(l, "ControlMaster=auto");
        if (rc == TE_OK) rc = arg(l, "-o");
        if (rc == TE_OK) {
            char cp[TE_SESS_PATH_MAX] = { 0 };
            rc = str_fmt(cp, sizeof cp, "ControlPath=%s/cm-%%C", o->control_dir);
            if (rc == TE_OK) rc = arg(l, cp);
        }
        if (rc == TE_OK) rc = arg(l, "-o");
        if (rc == TE_OK) rc = arg(l, "ControlPersist=60");
    }
    if (rc == TE_OK && s->port != 0) {
        rc = arg(l, "-p");
        if (rc == TE_OK) rc = arg_num(l, "", (int)s->port);
    }
    if (rc == TE_OK && eff->user[0] != '\0') {
        rc = arg(l, "-l");
        if (rc == TE_OK) rc = arg(l, eff->user);
    }
    if (rc == TE_OK && s->identity[0] != '\0') {
        char key[TE_SESS_PATH_MAX] = { 0 };
        rc = launch_expand_home(s->identity, key, sizeof key);
        if (rc == TE_OK) rc = arg(l, "-i");
        if (rc == TE_OK) rc = arg(l, key);
    }
    if (rc == TE_OK && s->jump[0] != '\0') {
        rc = arg(l, "-J");
        if (rc == TE_OK) rc = arg(l, s->jump);
    }
    if (rc == TE_OK) rc = arg(l, "--");   /* the host can never be taken for an option */
    if (rc == TE_OK) rc = arg(l, s->host);
    return rc;
}

te_err launch_build(const te_session *s, const te_session_eff *eff, const te_launch_opts *o, te_launch *out)
{
    if (s == nullptr || eff == nullptr || o == nullptr || out == nullptr) {
        return TE_ERR_INVAL;
    }
    memset(out, 0, sizeof *out);
    const char *why = nullptr;
    if (!session_validate(s, &why) || !session_user_valid(eff->user)) {
        return TE_ERR_INVAL;
    }
    char num[16] = { 0 };
    te_err rc = TE_OK;
    switch (s->type) {
    case TE_SESS_SSH:
        return build_ssh(s, eff, o, out);
    case TE_SESS_TELNET:
        if (o->self_exe == nullptr || o->self_exe[0] != '/') {
            return TE_ERR_NOTFOUND;
        }
        (void)str_fmt(num, sizeof num, "%u", (unsigned)(s->port != 0 ? s->port : 23));
        rc = arg(out, o->self_exe);
        if (rc == TE_OK) rc = arg(out, "--telnet-helper");
        if (rc == TE_OK) rc = arg(out, s->host);
        if (rc == TE_OK) rc = arg(out, num);
        if (rc == TE_OK) rc = arg_num(out, "", o->keepalive > 0 ? o->keepalive : 30);
        return rc;
    case TE_SESS_SERIAL: {
        if (o->self_exe == nullptr || o->self_exe[0] != '/') {
            return TE_ERR_NOTFOUND;
        }
        char line[64] = { 0 };
        rc = serial_format(&s->serial, line, sizeof line);
        if (rc == TE_OK) rc = arg(out, o->self_exe);
        if (rc == TE_OK) rc = arg(out, "--serial-helper");
        if (rc == TE_OK) rc = arg(out, s->serial.device);
        if (rc == TE_OK) rc = arg(out, line);
        return rc;
    }
    case TE_SESS_LOCAL:
        if (s->directory[0] != '\0') {
            rc = launch_expand_home(s->directory, out->cwd, sizeof out->cwd);
        }
        if (rc == TE_OK && s->command[0] != '\0') {
            rc = arg(out, "/bin/sh");
            if (rc == TE_OK) rc = arg(out, "-c");
            if (rc == TE_OK) rc = arg(out, s->command);
        }
        return rc;
    case TE_SESS_TYPE_COUNT:
    default:
        return TE_ERR_INVAL;
    }
}

te_err launch_control_dir(char *out, size_t outsz)
{
    const char *rt = getenv("XDG_RUNTIME_DIR");
    te_err rc = rt != nullptr && rt[0] == '/' ? str_fmt(out, outsz, "%s/%s", rt, APP_NAME)
                                              : str_fmt(out, outsz, "/tmp/%s-%ld", APP_NAME, (long)getuid());
    if (rc != TE_OK) {
        return rc;
    }
    /* Unix socket paths are limited to 108 bytes; "/cm-" + a 40-character hash must fit. */
    if (strlen(out) > 60) {
        return TE_ERR_TRUNC;
    }
    if (mkdir(out, 0700) != 0 && errno != EEXIST) {
        return TE_ERR_IO;
    }
    struct stat sb = { 0 };
    if (lstat(out, &sb) != 0 || !S_ISDIR(sb.st_mode) || sb.st_uid != getuid() || (sb.st_mode & 0077) != 0) {
        return TE_ERR_PERM;   /* someone else's directory, or a symlink: never use it */
    }
    return TE_OK;
}

te_err launch_self_exe(char *out, size_t outsz)
{
    if (outsz < 2) {
        return TE_ERR_INVAL;
    }
    ssize_t n = readlink("/proc/self/exe", out, outsz - 1);
    if (n <= 0 || (size_t)n >= outsz - 1) {
        return TE_ERR_NOTFOUND;
    }
    out[n] = '\0';
    /* A rebuilt or upgraded binary shows up as "... (deleted)"; it can still be run via /proc. */
    if (out[0] != '/' || strstr(out, " (deleted)") != nullptr) {
        return str_copy(out, outsz, "/proc/self/exe");
    }
    return TE_OK;
}
