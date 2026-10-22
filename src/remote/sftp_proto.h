/*
 * SFTP version 3 (draft-ietf-secsh-filexfer-02) packet encoding and
 * decoding. Pure functions over byte buffers, so they can be tested with
 * hostile input. Everything from the server is untrusted: every length and
 * count is checked against the bytes actually present, packets are limited
 * to TE_SFTP_MAX_PACKET, and names to TE_SFTP_MAX_NAME bytes.
 */
#ifndef TE_SFTP_PROTO_H
#define TE_SFTP_PROTO_H

#include <stdbool.h>
#include "common.h"

#define TE_SFTP_MAX_PACKET   (256u * 1024u)
#define TE_SFTP_MAX_NAME     4096u
#define TE_SFTP_MAX_ENTRIES  4096u      /* per NAME reply */
#define TE_SFTP_MAX_HANDLE   256u
#define TE_SFTP_CHUNK        32768u     /* bytes per READ/WRITE request */

enum {
    SSH_FXP_INIT = 1, SSH_FXP_VERSION = 2, SSH_FXP_OPEN = 3, SSH_FXP_CLOSE = 4, SSH_FXP_READ = 5,
    SSH_FXP_WRITE = 6, SSH_FXP_LSTAT = 7, SSH_FXP_FSTAT = 8, SSH_FXP_SETSTAT = 9, SSH_FXP_OPENDIR = 11,
    SSH_FXP_READDIR = 12, SSH_FXP_REMOVE = 13, SSH_FXP_MKDIR = 14, SSH_FXP_RMDIR = 15, SSH_FXP_REALPATH = 16,
    SSH_FXP_STAT = 17, SSH_FXP_RENAME = 18,
    SSH_FXP_STATUS = 101, SSH_FXP_HANDLE = 102, SSH_FXP_DATA = 103, SSH_FXP_NAME = 104, SSH_FXP_ATTRS = 105,
};

enum {
    SSH_FX_OK = 0, SSH_FX_EOF = 1, SSH_FX_NO_SUCH_FILE = 2, SSH_FX_PERMISSION_DENIED = 3, SSH_FX_FAILURE = 4,
    SSH_FX_BAD_MESSAGE = 5, SSH_FX_NO_CONNECTION = 6, SSH_FX_CONNECTION_LOST = 7, SSH_FX_OP_UNSUPPORTED = 8,
};

enum {
    SSH_FXF_READ = 0x01, SSH_FXF_WRITE = 0x02, SSH_FXF_APPEND = 0x04, SSH_FXF_CREAT = 0x08,
    SSH_FXF_TRUNC = 0x10, SSH_FXF_EXCL = 0x20,
};

enum {
    SSH_FILEXFER_ATTR_SIZE = 0x01, SSH_FILEXFER_ATTR_UIDGID = 0x02, SSH_FILEXFER_ATTR_PERMISSIONS = 0x04,
    SSH_FILEXFER_ATTR_ACMODTIME = 0x08,
};
#define SSH_FILEXFER_ATTR_EXTENDED 0x80000000u

typedef struct te_sftp_attrs {
    uint32_t flags;
    uint64_t size;
    uint32_t uid, gid;
    uint32_t perm;        /* includes the file type bits (S_IFDIR...) */
    uint32_t atime, mtime;
} te_sftp_attrs;

typedef struct te_sftp_name {
    char         *name;   /* owned, NUL-terminated, no embedded NUL */
    te_sftp_attrs a;
} te_sftp_name;

typedef struct te_sftp_reply {
    uint8_t        type;
    uint32_t       id;
    uint32_t       version;                       /* VERSION */
    uint32_t       status;                        /* STATUS */
    char           msg[256];                      /* STATUS message, sanitised */
    uint8_t        handle[TE_SFTP_MAX_HANDLE];    /* HANDLE */
    size_t         handle_len;
    const uint8_t *data;                          /* DATA: points into the packet */
    size_t         data_len;
    te_sftp_name  *names;                         /* NAME: owned */
    size_t         nnames;
    te_sftp_attrs  attrs;                         /* ATTRS */
} te_sftp_reply;

/* Growing output buffer with a hard size limit; any failure sets err and stops writing. */
typedef struct te_sbuf {
    uint8_t *data;
    size_t   len, cap, limit;
    bool     err;
} te_sbuf;

void sbuf_init(te_sbuf *b, size_t limit);
void sbuf_free(te_sbuf *b);
/* Drop the first n bytes (after they were written out). */
void sbuf_consume(te_sbuf *b, size_t n);

/* Request encoders: each appends one complete packet. False if it did not fit. */
bool sftp_enc_init(te_sbuf *b);
bool sftp_enc_path(te_sbuf *b, uint8_t type, uint32_t id, const char *path);   /* REALPATH OPENDIR REMOVE RMDIR STAT LSTAT */
bool sftp_enc_handle(te_sbuf *b, uint8_t type, uint32_t id, const uint8_t *h, size_t hl);   /* CLOSE READDIR FSTAT */
bool sftp_enc_open(te_sbuf *b, uint32_t id, const char *path, uint32_t pflags, uint32_t perm);
bool sftp_enc_read(te_sbuf *b, uint32_t id, const uint8_t *h, size_t hl, uint64_t off, uint32_t len);
bool sftp_enc_write(te_sbuf *b, uint32_t id, const uint8_t *h, size_t hl, uint64_t off, const uint8_t *data, size_t len);
bool sftp_enc_mkdir(te_sbuf *b, uint32_t id, const char *path, uint32_t perm);
bool sftp_enc_rename(te_sbuf *b, uint32_t id, const char *from, const char *to);
bool sftp_enc_chmod(te_sbuf *b, uint32_t id, const char *path, uint32_t perm);

/*
 * Framing: if buf starts with a complete packet, set *total to its size
 * (4-byte length included) and return 1; 0 if more bytes are needed; -1 if
 * the declared length is zero or above the limit (a protocol error).
 */
int sftp_frame(const uint8_t *buf, size_t len, size_t *total);
/* Decode one packet (without its 4-byte length). On success free with sftp_reply_free. */
[[nodiscard]] te_err sftp_parse(const uint8_t *pkt, size_t len, te_sftp_reply *out);
void sftp_reply_free(te_sftp_reply *r);

const char *sftp_status_text(uint32_t code);
bool sftp_is_dir(const te_sftp_attrs *a);
bool sftp_is_link(const te_sftp_attrs *a);

#endif
