#include "remote/sftp_proto.h"

#include <string.h>
#include <sys/stat.h>

#include "util/mem.h"
#include "util/str.h"

/* ------------------------------------------------------------------------- */
/* Output buffer */

void sbuf_init(te_sbuf *b, size_t limit)
{
    memset(b, 0, sizeof *b);
    b->limit = limit;
}

void sbuf_free(te_sbuf *b)
{
    TE_FREE(b->data);
    b->len = 0;
    b->cap = 0;
    b->err = false;
}

void sbuf_consume(te_sbuf *b, size_t n)
{
    if (n >= b->len) {
        b->len = 0;
        return;
    }
    memmove(b->data, b->data + n, b->len - n);
    b->len -= n;
}

static bool reserve(te_sbuf *b, size_t extra)
{
    if (b->err) {
        return false;
    }
    size_t need = 0;
    if (ckd_add(&need, b->len, extra) || need > b->limit) {
        b->err = true;
        return false;
    }
    if (need <= b->cap) {
        return true;
    }
    size_t ncap = b->cap == 0 ? 4096 : b->cap;
    while (ncap < need) {
        if (ckd_mul(&ncap, ncap, (size_t)2)) {
            ncap = need;
            break;
        }
    }
    if (ncap > b->limit) {
        ncap = b->limit;
    }
    void *tmp = b->data;
    if (te_realloc_array(&tmp, b->cap, ncap, 1) != TE_OK) {
        b->err = true;
        return false;
    }
    b->data = tmp;
    b->cap = ncap;
    return true;
}

static void put_u8(te_sbuf *b, uint8_t v)
{
    if (reserve(b, 1)) {
        b->data[b->len++] = v;
    }
}

static void put_u32(te_sbuf *b, uint32_t v)
{
    if (reserve(b, 4)) {
        b->data[b->len++] = (uint8_t)(v >> 24);
        b->data[b->len++] = (uint8_t)(v >> 16);
        b->data[b->len++] = (uint8_t)(v >> 8);
        b->data[b->len++] = (uint8_t)v;
    }
}

static void put_u64(te_sbuf *b, uint64_t v)
{
    put_u32(b, (uint32_t)(v >> 32));
    put_u32(b, (uint32_t)(v & 0xFFFFFFFFu));
}

static void put_bytes(te_sbuf *b, const uint8_t *p, size_t n)
{
    if (n > UINT32_MAX) {
        b->err = true;
        return;
    }
    put_u32(b, (uint32_t)n);
    if (n > 0 && reserve(b, n)) {
        memcpy(b->data + b->len, p, n);
        b->len += n;
    }
}

static void put_str(te_sbuf *b, const char *s)
{
    put_bytes(b, (const uint8_t *)s, strlen(s));
}

/* A packet: 4-byte length placeholder, filled in by finish(). */
static size_t begin(te_sbuf *b, uint8_t type)
{
    size_t start = b->len;
    put_u32(b, 0);
    put_u8(b, type);
    return start;
}

static bool finish(te_sbuf *b, size_t start, size_t mark_on_entry)
{
    if (b->err) {
        b->len = mark_on_entry;   /* never leave half a packet behind */
        b->err = false;
        return false;
    }
    size_t body = b->len - start - 4;
    if (body > TE_SFTP_MAX_PACKET) {
        b->len = mark_on_entry;
        return false;
    }
    b->data[start] = (uint8_t)(body >> 24);
    b->data[start + 1] = (uint8_t)(body >> 16);
    b->data[start + 2] = (uint8_t)(body >> 8);
    b->data[start + 3] = (uint8_t)body;
    return true;
}

bool sftp_enc_init(te_sbuf *b)
{
    size_t m = b->len;
    size_t s = begin(b, SSH_FXP_INIT);
    put_u32(b, 3);
    return finish(b, s, m);
}

bool sftp_enc_path(te_sbuf *b, uint8_t type, uint32_t id, const char *path)
{
    size_t m = b->len;
    size_t s = begin(b, type);
    put_u32(b, id);
    put_str(b, path);
    return finish(b, s, m);
}

bool sftp_enc_handle(te_sbuf *b, uint8_t type, uint32_t id, const uint8_t *h, size_t hl)
{
    size_t m = b->len;
    size_t s = begin(b, type);
    put_u32(b, id);
    put_bytes(b, h, hl);
    return finish(b, s, m);
}

bool sftp_enc_open(te_sbuf *b, uint32_t id, const char *path, uint32_t pflags, uint32_t perm)
{
    size_t m = b->len;
    size_t s = begin(b, SSH_FXP_OPEN);
    put_u32(b, id);
    put_str(b, path);
    put_u32(b, pflags);
    if ((pflags & SSH_FXF_CREAT) != 0) {
        put_u32(b, SSH_FILEXFER_ATTR_PERMISSIONS);
        put_u32(b, perm & 07777u);
    } else {
        put_u32(b, 0);
    }
    return finish(b, s, m);
}

bool sftp_enc_read(te_sbuf *b, uint32_t id, const uint8_t *h, size_t hl, uint64_t off, uint32_t len)
{
    size_t m = b->len;
    size_t s = begin(b, SSH_FXP_READ);
    put_u32(b, id);
    put_bytes(b, h, hl);
    put_u64(b, off);
    put_u32(b, len);
    return finish(b, s, m);
}

bool sftp_enc_write(te_sbuf *b, uint32_t id, const uint8_t *h, size_t hl, uint64_t off, const uint8_t *data, size_t len)
{
    size_t m = b->len;
    size_t s = begin(b, SSH_FXP_WRITE);
    put_u32(b, id);
    put_bytes(b, h, hl);
    put_u64(b, off);
    put_bytes(b, data, len);
    return finish(b, s, m);
}

bool sftp_enc_mkdir(te_sbuf *b, uint32_t id, const char *path, uint32_t perm)
{
    size_t m = b->len;
    size_t s = begin(b, SSH_FXP_MKDIR);
    put_u32(b, id);
    put_str(b, path);
    put_u32(b, SSH_FILEXFER_ATTR_PERMISSIONS);
    put_u32(b, perm & 07777u);
    return finish(b, s, m);
}

bool sftp_enc_rename(te_sbuf *b, uint32_t id, const char *from, const char *to)
{
    size_t m = b->len;
    size_t s = begin(b, SSH_FXP_RENAME);
    put_u32(b, id);
    put_str(b, from);
    put_str(b, to);
    return finish(b, s, m);
}

bool sftp_enc_chmod(te_sbuf *b, uint32_t id, const char *path, uint32_t perm)
{
    size_t m = b->len;
    size_t s = begin(b, SSH_FXP_SETSTAT);
    put_u32(b, id);
    put_str(b, path);
    put_u32(b, SSH_FILEXFER_ATTR_PERMISSIONS);
    put_u32(b, perm & 07777u);
    return finish(b, s, m);
}

/* ------------------------------------------------------------------------- */
/* Decoding */

typedef struct rd {
    const uint8_t *p;
    size_t         left;
    bool           bad;
} rd;

static uint8_t get_u8(rd *r)
{
    if (r->bad || r->left < 1) {
        r->bad = true;
        return 0;
    }
    uint8_t v = r->p[0];
    r->p++;
    r->left--;
    return v;
}

static uint32_t get_u32(rd *r)
{
    if (r->bad || r->left < 4) {
        r->bad = true;
        return 0;
    }
    uint32_t v = ((uint32_t)r->p[0] << 24) | ((uint32_t)r->p[1] << 16) | ((uint32_t)r->p[2] << 8) | r->p[3];
    r->p += 4;
    r->left -= 4;
    return v;
}

static uint64_t get_u64(rd *r)
{
    uint64_t hi = get_u32(r);
    uint64_t lo = get_u32(r);
    return (hi << 32) | lo;
}

/* A string's bytes, in place; nullptr (and r->bad) if it overruns. */
static const uint8_t *get_bytes(rd *r, size_t *n)
{
    uint32_t len = get_u32(r);
    if (r->bad || len > r->left) {
        r->bad = true;
        *n = 0;
        return nullptr;
    }
    const uint8_t *p = r->p;
    r->p += len;
    r->left -= len;
    *n = len;
    return p;
}

static void get_attrs(rd *r, te_sftp_attrs *a)
{
    memset(a, 0, sizeof *a);
    a->flags = get_u32(r);
    if ((a->flags & SSH_FILEXFER_ATTR_SIZE) != 0) a->size = get_u64(r);
    if ((a->flags & SSH_FILEXFER_ATTR_UIDGID) != 0) {
        a->uid = get_u32(r);
        a->gid = get_u32(r);
    }
    if ((a->flags & SSH_FILEXFER_ATTR_PERMISSIONS) != 0) a->perm = get_u32(r);
    if ((a->flags & SSH_FILEXFER_ATTR_ACMODTIME) != 0) {
        a->atime = get_u32(r);
        a->mtime = get_u32(r);
    }
    if ((a->flags & SSH_FILEXFER_ATTR_EXTENDED) != 0) {
        uint32_t count = get_u32(r);
        if (count > 1024) {
            r->bad = true;
            return;
        }
        for (uint32_t i = 0; i < count && !r->bad; i++) {
            size_t n = 0;
            (void)get_bytes(r, &n);   /* type */
            (void)get_bytes(r, &n);   /* data */
        }
    }
}

/* Copy a server string into a fixed buffer, replacing control characters. */
static void copy_text(char *dst, size_t dstsz, const uint8_t *src, size_t n)
{
    size_t k = 0;
    for (size_t i = 0; i < n && k + 1 < dstsz; i++) {
        uint8_t c = src[i];
        dst[k++] = (c < 0x20 || c == 0x7F) ? '?' : (char)c;
    }
    dst[k] = '\0';
}

void sftp_reply_free(te_sftp_reply *r)
{
    if (r == nullptr) {
        return;
    }
    for (size_t i = 0; i < r->nnames; i++) {
        TE_FREE(r->names[i].name);
    }
    TE_FREE(r->names);
    r->nnames = 0;
}

te_err sftp_parse(const uint8_t *pkt, size_t len, te_sftp_reply *out)
{
    memset(out, 0, sizeof *out);
    if (pkt == nullptr || len == 0 || len > TE_SFTP_MAX_PACKET) {
        return TE_ERR_PARSE;
    }
    rd r = { .p = pkt, .left = len };
    out->type = get_u8(&r);
    if (out->type == SSH_FXP_VERSION) {
        out->version = get_u32(&r);
        return r.bad ? TE_ERR_PARSE : TE_OK;   /* extensions after it are ignored */
    }
    out->id = get_u32(&r);
    size_t n = 0;
    const uint8_t *s = nullptr;
    switch (out->type) {
    case SSH_FXP_STATUS:
        out->status = get_u32(&r);
        if (r.bad) {
            return TE_ERR_PARSE;
        }
        if (r.left > 0) {   /* message and language are optional in old servers */
            s = get_bytes(&r, &n);
            if (s != nullptr) copy_text(out->msg, sizeof out->msg, s, n);
        }
        r.bad = false;      /* a missing or truncated message/language tag is harmless */
        break;
    case SSH_FXP_HANDLE:
        s = get_bytes(&r, &n);
        if (s == nullptr || n == 0 || n > TE_SFTP_MAX_HANDLE) {
            return TE_ERR_PARSE;
        }
        memcpy(out->handle, s, n);
        out->handle_len = n;
        break;
    case SSH_FXP_DATA:
        s = get_bytes(&r, &n);
        if (s == nullptr) {
            return TE_ERR_PARSE;
        }
        out->data = s;
        out->data_len = n;
        break;
    case SSH_FXP_ATTRS:
        get_attrs(&r, &out->attrs);
        break;
    case SSH_FXP_NAME: {
        uint32_t count = get_u32(&r);
        /* Each entry needs at least 12 bytes (two empty strings and flags). */
        if (r.bad || count > TE_SFTP_MAX_ENTRIES || (size_t)count > r.left / 12) {
            return TE_ERR_PARSE;
        }
        if (count == 0) {
            break;
        }
        out->names = te_calloc(count, sizeof *out->names);
        if (out->names == nullptr) {
            return TE_ERR_NOMEM;
        }
        for (uint32_t i = 0; i < count; i++) {
            s = get_bytes(&r, &n);
            if (s == nullptr || n == 0 || n > TE_SFTP_MAX_NAME || memchr(s, '\0', n) != nullptr) {
                sftp_reply_free(out);
                return TE_ERR_PARSE;
            }
            char *name = te_malloc(n + 1);
            if (name == nullptr) {
                sftp_reply_free(out);
                return TE_ERR_NOMEM;
            }
            memcpy(name, s, n);
            name[n] = '\0';
            out->names[out->nnames].name = name;
            out->nnames++;
            size_t ln = 0;
            (void)get_bytes(&r, &ln);   /* longname: display only, ignored */
            get_attrs(&r, &out->names[out->nnames - 1].a);
            if (r.bad) {
                sftp_reply_free(out);
                return TE_ERR_PARSE;
            }
        }
        break;
    }
    default:
        return TE_ERR_PARSE;
    }
    return r.bad ? TE_ERR_PARSE : TE_OK;
}

int sftp_frame(const uint8_t *buf, size_t len, size_t *total)
{
    if (len < 4) {
        return 0;
    }
    uint32_t n = ((uint32_t)buf[0] << 24) | ((uint32_t)buf[1] << 16) | ((uint32_t)buf[2] << 8) | buf[3];
    if (n == 0 || n > TE_SFTP_MAX_PACKET) {
        return -1;
    }
    if (len - 4 < n) {
        return 0;
    }
    *total = (size_t)n + 4;
    return 1;
}

const char *sftp_status_text(uint32_t code)
{
    switch (code) {
    case SSH_FX_OK:                return "OK";
    case SSH_FX_EOF:               return "end of file";
    case SSH_FX_NO_SUCH_FILE:      return "no such file or folder";
    case SSH_FX_PERMISSION_DENIED: return "permission denied";
    case SSH_FX_FAILURE:           return "failed";
    case SSH_FX_BAD_MESSAGE:       return "bad message";
    case SSH_FX_NO_CONNECTION:     return "no connection";
    case SSH_FX_CONNECTION_LOST:   return "connection lost";
    case SSH_FX_OP_UNSUPPORTED:    return "not supported by the server";
    default:                       return "error";
    }
}

bool sftp_is_dir(const te_sftp_attrs *a)
{
    return (a->flags & SSH_FILEXFER_ATTR_PERMISSIONS) != 0 && (a->perm & S_IFMT) == S_IFDIR;
}

bool sftp_is_link(const te_sftp_attrs *a)
{
    return (a->flags & SSH_FILEXFER_ATTR_PERMISSIONS) != 0 && (a->perm & S_IFMT) == S_IFLNK;
}
