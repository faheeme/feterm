#include "app/banner.h"

#include <string.h>

#include "util/mem.h"
#include "util/str.h"

#define CHUNK 4096u   /* base64 bytes per kitty graphics escape */

size_t base64_encode(const uint8_t *src, size_t n, char *dst, size_t dstsz)
{
    static const char tbl[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t groups = n / 3 + (n % 3 != 0);
    size_t need = 0;
    if (ckd_mul(&need, groups, (size_t)4) || need >= dstsz) {
        if (dstsz > 0) dst[0] = '\0';
        return 0;
    }
    size_t k = 0;
    for (size_t i = 0; i < n; i += 3) {
        uint32_t v = (uint32_t)src[i] << 16;
        if (i + 1 < n) v |= (uint32_t)src[i + 1] << 8;
        if (i + 2 < n) v |= src[i + 2];
        dst[k++] = tbl[(v >> 18) & 63u];
        dst[k++] = tbl[(v >> 12) & 63u];
        dst[k++] = i + 1 < n ? tbl[(v >> 6) & 63u] : '=';
        dst[k++] = i + 2 < n ? tbl[v & 63u] : '=';
    }
    dst[k] = '\0';
    return k;
}

/* A bounded append-only buffer. */
typedef struct out_buf {
    char  *p;
    size_t len, cap;
    bool   err;
} out_buf;

static void put(out_buf *o, const char *s, size_t n)
{
    if (o->err || n > o->cap - o->len) {
        o->err = true;
        return;
    }
    memcpy(o->p + o->len, s, n);
    o->len += n;
}

static void puts_(out_buf *o, const char *s)
{
    put(o, s, strlen(s));
}

te_err banner_build(const uint8_t *rgba, size_t rgba_len, uint32_t px, uint16_t img_cols, uint16_t img_rows, char **out,
                    size_t *len)
{
    if (out == nullptr || len == nullptr) {
        return TE_ERR_INVAL;
    }
    *out = nullptr;
    *len = 0;
    bool logo = rgba != nullptr && px > 0 && px <= 512 && rgba_len == (size_t)px * px * 4u && img_cols > 0 &&
                img_cols <= 16 && img_rows > 0 && img_rows <= 8;
    size_t b64_cap = logo ? (rgba_len / 3 + 1) * 4 + 1 : 1;
    char *b64 = te_malloc(b64_cap);
    if (b64 == nullptr) {
        return TE_ERR_NOMEM;
    }
    size_t b64_len = logo ? base64_encode(rgba, rgba_len, b64, b64_cap) : 0;
    if (logo && b64_len == 0) {
        TE_FREE(b64);
        return TE_ERR_RANGE;
    }
    /* Each chunk adds at most ~64 bytes of framing; the text needs well under 1 KiB. */
    size_t cap = b64_len + (b64_len / CHUNK + 1) * 64 + 1024;
    out_buf o = { .p = te_malloc(cap), .cap = cap };
    if (o.p == nullptr) {
        TE_FREE(b64);
        return TE_ERR_NOMEM;
    }
    char tmp[160] = { 0 };
    if (logo) {
        /* Transmit and place without moving the cursor, quietly, in chunks. */
        for (size_t off = 0; off < b64_len; off += CHUNK) {
            size_t n = b64_len - off < CHUNK ? b64_len - off : CHUNK;
            bool more = off + n < b64_len;
            if (off == 0) {
                (void)str_fmt(tmp, sizeof tmp, "\x1b_Ga=T,f=32,s=%u,v=%u,c=%u,r=%u,C=1,q=2,m=%d;", (unsigned)px,
                              (unsigned)px, (unsigned)img_cols, (unsigned)img_rows, more ? 1 : 0);
            } else {
                (void)str_fmt(tmp, sizeof tmp, "\x1b_Gm=%d;", more ? 1 : 0);
            }
            puts_(&o, tmp);
            put(&o, b64 + off, n);
            puts_(&o, "\x1b\\");
        }
        (void)str_fmt(tmp, sizeof tmp, "\x1b[%uC", (unsigned)img_cols + 1u);
    } else {
        tmp[0] = '\0';
    }
    TE_FREE(b64);
    char indent[24] = { 0 };
    (void)str_copy(indent, sizeof indent, tmp);
    /* Name in the logo's green, then the version. */
    puts_(&o, indent);
    puts_(&o, "\x1b[1;38;2;74;222;128m" APP_DISPLAY_NAME "\x1b[0m v" APP_VERSION "\r\n");
    puts_(&o, indent);
    puts_(&o, "\x1b[2m\xc2\xa9 " APP_AUTHOR " - " APP_EMAIL "\x1b[0m\r\n");
    for (uint16_t r = 2; logo && r < img_rows; r++) {
        puts_(&o, "\r\n");   /* make room below a taller logo */
    }
    puts_(&o, "\r\n");      /* a blank line before the prompt */
    put(&o, "", 1);   /* always NUL-terminated, so it is also a valid C string */
    if (o.err) {
        TE_FREE(o.p);
        return TE_ERR_TRUNC;
    }
    o.len--;          /* the terminator is not part of the length */
    *out = o.p;
    *len = o.len;
    return TE_OK;
}
