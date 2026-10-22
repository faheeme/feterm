#include "vt/images.h"

#include <math.h>
#include <string.h>

#include "util/log.h"
#include "util/mem.h"
#include "util/str.h"

/* ------------------------------------------------------------------------- */
/* Store */

void images_init(te_images *s)
{
    memset(s, 0, sizeof *s);
    s->next_serial = 1;
}

static void image_free(te_image *im)
{
    TE_FREE(im->rgba);
    im->w = 0;
    im->h = 0;
}

static void remove_at(te_images *s, size_t i)
{
    if (i >= s->n) {
        return;
    }
    s->bytes -= (size_t)s->items[i].w * s->items[i].h * 4u;
    image_free(&s->items[i]);
    memmove(&s->items[i], &s->items[i + 1], (s->n - i - 1) * sizeof s->items[0]);
    s->n--;
}

void images_free(te_images *s)
{
    while (s->n > 0) {
        remove_at(s, s->n - 1);
    }
    TE_FREE(s->chunk);
    s->chunk_len = 0;
    s->chunk_cap = 0;
    s->chunking = false;
    s->bytes = 0;
}

void images_clear(te_images *s)
{
    while (s->n > 0) {
        remove_at(s, s->n - 1);
    }
}

te_image *images_find(te_images *s, uint32_t id)
{
    if (id == 0) {
        return nullptr;
    }
    for (size_t i = 0; i < s->n; i++) {
        if (s->items[i].id == id) {
            return &s->items[i];
        }
    }
    return nullptr;
}

void images_delete_id(te_images *s, uint32_t id)
{
    for (size_t i = 0; i < s->n;) {
        if (s->items[i].id == id) {
            remove_at(s, i);
        } else {
            i++;
        }
    }
}

te_image *images_add(te_images *s, uint32_t id, uint32_t w, uint32_t h, uint8_t *rgba)
{
    size_t bytes = 0;
    if (rgba == nullptr || w == 0 || h == 0 || w > TE_IMAGE_MAX_DIM || h > TE_IMAGE_MAX_DIM ||
        ckd_mul(&bytes, (size_t)w * h, (size_t)4) || bytes > TE_IMAGE_BUDGET) {
        free(rgba);
        return nullptr;
    }
    if (id != 0) {
        images_delete_id(s, id);
    }
    /* Evict the oldest images until the new one fits. */
    while (s->n > 0 && (s->n >= TE_MAX_IMAGES || s->bytes + bytes > TE_IMAGE_BUDGET)) {
        remove_at(s, 0);
    }
    te_image *im = &s->items[s->n++];
    memset(im, 0, sizeof *im);
    im->id = id;
    im->serial = s->next_serial++;
    if (s->next_serial == 0) {
        s->next_serial = 1;
    }
    im->w = w;
    im->h = h;
    im->rgba = rgba;
    s->bytes += bytes;
    return im;
}

void images_prune(te_images *s, uint64_t first_abs)
{
    for (size_t i = 0; i < s->n;) {
        const te_image *im = &s->items[i];
        if (im->placed && !im->alt && im->row + im->rows <= first_abs) {
            remove_at(s, i);
        } else {
            i++;
        }
    }
}

void images_drop_screen(te_images *s, bool alt)
{
    for (size_t i = 0; i < s->n;) {
        if (s->items[i].placed && s->items[i].alt == alt) {
            remove_at(s, i);
        } else {
            i++;
        }
    }
}

/* ------------------------------------------------------------------------- */
/* Sixel */

static uint32_t hls_to_rgb(uint32_t hue, uint32_t lum, uint32_t sat)
{
    /* Sixel hue 0 is blue; standard HLS has red at 0. */
    double h = (double)((hue + 240u) % 360u) / 360.0;
    double l = (double)TE_MIN(lum, 100u) / 100.0;
    double s = (double)TE_MIN(sat, 100u) / 100.0;
    double q = l < 0.5 ? l * (1.0 + s) : l + s - l * s;
    double p = 2.0 * l - q;
    double ch[3] = { 0 };
    double off[3] = { 1.0 / 3.0, 0.0, -1.0 / 3.0 };
    for (int i = 0; i < 3; i++) {
        double t = h + off[i];
        if (t < 0.0) t += 1.0;
        if (t > 1.0) t -= 1.0;
        if (t < 1.0 / 6.0) ch[i] = p + (q - p) * 6.0 * t;
        else if (t < 0.5) ch[i] = q;
        else if (t < 2.0 / 3.0) ch[i] = p + (q - p) * (2.0 / 3.0 - t) * 6.0;
        else ch[i] = p;
    }
    uint32_t r = (uint32_t)lround(ch[0] * 255.0);
    uint32_t g = (uint32_t)lround(ch[1] * 255.0);
    uint32_t b = (uint32_t)lround(ch[2] * 255.0);
    return (TE_MIN(r, 255u) << 16) | (TE_MIN(g, 255u) << 8) | TE_MIN(b, 255u);
}

typedef struct sixel_canvas {
    uint32_t *px;          /* 0 = unset, else 0xFF000000 | rgb */
    uint32_t  cap_w, cap_h;
    uint32_t  w, h;        /* extent actually drawn */
} sixel_canvas;

[[nodiscard]] static bool canvas_grow(sixel_canvas *c, uint32_t need_w, uint32_t need_h)
{
    if (need_w > TE_SIXEL_MAX_DIM || need_h > TE_SIXEL_MAX_DIM) {
        return false;
    }
    if (need_w <= c->cap_w && need_h <= c->cap_h) {
        return true;
    }
    uint32_t nw = c->cap_w;
    uint32_t nh = c->cap_h;
    while (nw < need_w) nw = nw == 0 ? 256 : nw * 2;
    while (nh < need_h) nh = nh == 0 ? 96 : nh * 2;
    nw = TE_MIN(nw, TE_SIXEL_MAX_DIM);
    nh = TE_MIN(nh, TE_SIXEL_MAX_DIM);
    uint32_t *np = te_calloc((size_t)nw * nh, sizeof *np);
    if (np == nullptr) {
        return false;
    }
    for (uint32_t y = 0; y < c->cap_h; y++) {
        memcpy(np + (size_t)y * nw, c->px + (size_t)y * c->cap_w, (size_t)c->cap_w * sizeof *np);
    }
    free(c->px);
    c->px = np;
    c->cap_w = nw;
    c->cap_h = nh;
    return true;
}

static uint32_t read_num(const char *d, size_t len, size_t *i)
{
    uint32_t v = 0;
    while (*i < len && d[*i] >= '0' && d[*i] <= '9') {
        v = v * 10u + (uint32_t)(d[*i] - '0');
        if (v > 100000u) {
            v = 100000u;   /* clamp: never overflow */
        }
        (*i)++;
    }
    return v;
}

te_err sixel_decode(const vt_params *params, const char *d, size_t len, uint32_t bg_rgb, uint8_t **rgba, uint32_t *ow,
                    uint32_t *oh)
{
    static const uint32_t vt340[16] = {
        0x000000, 0x3333cc, 0xcc2121, 0x33cc33, 0xcc33cc, 0x33cccc, 0xcccc33, 0x878787,
        0x424242, 0x545499, 0x994242, 0x549954, 0x995499, 0x549999, 0x999954, 0xcccccc,
    };
    *rgba = nullptr;
    *ow = 0;
    *oh = 0;
    uint32_t palette[256] = { 0 };
    memcpy(palette, vt340, sizeof vt340);
    bool transparent = params != nullptr && params->n >= 2 && params->v[1] == 1;
    sixel_canvas c = { 0 };
    uint32_t x = 0;
    uint32_t y = 0;          /* top pixel row of the current band */
    uint32_t color = 0;
    uint32_t raster_w = 0;
    uint32_t raster_h = 0;
    te_err rc = TE_OK;

    for (size_t i = 0; i < len;) {
        char ch = d[i];
        if (ch == '"') {                                  /* raster attributes */
            i++;
            uint32_t v[4] = { 0 };
            for (size_t k = 0; k < 4; k++) {
                v[k] = read_num(d, len, &i);
                if (i < len && d[i] == ';') i++; else break;
            }
            raster_w = TE_MIN(v[2], TE_SIXEL_MAX_DIM);
            raster_h = TE_MIN(v[3], TE_SIXEL_MAX_DIM);
            continue;
        }
        if (ch == '#') {                                  /* colour select / define */
            i++;
            uint32_t v[5] = { 0 };
            size_t nv = 0;
            for (; nv < 5; nv++) {
                v[nv] = read_num(d, len, &i);
                if (i < len && d[i] == ';') i++; else { nv++; break; }
            }
            color = v[0] & 0xFFu;
            if (nv >= 5) {
                if (v[1] == 2) {
                    uint32_t r = TE_MIN(v[2], 100u) * 255u / 100u;
                    uint32_t g = TE_MIN(v[3], 100u) * 255u / 100u;
                    uint32_t b = TE_MIN(v[4], 100u) * 255u / 100u;
                    palette[color] = (r << 16) | (g << 8) | b;
                } else if (v[1] == 1) {
                    palette[color] = hls_to_rgb(v[2], v[3], v[4]);
                }
            }
            continue;
        }
        if (ch == '$') { x = 0; i++; continue; }
        if (ch == '-') { x = 0; y += 6; i++; continue; }
        uint32_t rep = 1;
        if (ch == '!') {
            i++;
            rep = read_num(d, len, &i);
            if (rep == 0) rep = 1;
            if (i >= len) break;
            ch = d[i];
        }
        if (ch >= '?' && ch <= '~') {
            uint32_t bits = (uint32_t)(ch - '?');
            if (x >= TE_SIXEL_MAX_DIM || y >= TE_SIXEL_MAX_DIM) {
                i++;
                continue;   /* outside the maximum canvas: ignored */
            }
            uint32_t end = TE_MIN(x + rep, TE_SIXEL_MAX_DIM);
            if (bits != 0) {
                uint32_t need_h = TE_MIN(y + 6u, TE_SIXEL_MAX_DIM);
                if (!canvas_grow(&c, end, need_h)) {
                    rc = TE_ERR_NOMEM;
                    break;
                }
                uint32_t col = 0xFF000000u | palette[color];
                for (uint32_t b = 0; b < 6; b++) {
                    if ((bits & (1u << b)) == 0 || y + b >= c.cap_h) continue;
                    uint32_t *row = c.px + (size_t)(y + b) * c.cap_w;
                    for (uint32_t xx = x; xx < end; xx++) {
                        row[xx] = col;
                    }
                    if (y + b + 1 > c.h) c.h = y + b + 1;
                }
                if (end > c.w) c.w = end;
            }
            x = end;
        }
        i++;
    }
    if (rc != TE_OK) {
        free(c.px);
        return rc;
    }
    uint32_t w = TE_MAX(c.w, raster_w);
    uint32_t h = TE_MAX(c.h, raster_h);
    if (w == 0 || h == 0) {
        free(c.px);
        return TE_ERR_INVAL;
    }
    uint8_t *out = te_calloc((size_t)w * h, 4);
    if (out == nullptr) {
        free(c.px);
        return TE_ERR_NOMEM;
    }
    for (uint32_t yy = 0; yy < h; yy++) {
        for (uint32_t xx = 0; xx < w; xx++) {
            uint32_t v = (xx < c.cap_w && yy < c.cap_h && c.px != nullptr) ? c.px[(size_t)yy * c.cap_w + xx] : 0u;
            uint8_t *o = out + ((size_t)yy * w + xx) * 4u;
            if (v == 0) {
                if (transparent) continue;   /* stays 0,0,0,0 */
                v = 0xFF000000u | bg_rgb;
            }
            o[0] = (uint8_t)((v >> 16) & 0xFFu);
            o[1] = (uint8_t)((v >> 8) & 0xFFu);
            o[2] = (uint8_t)(v & 0xFFu);
            o[3] = 0xFF;
        }
    }
    free(c.px);
    *rgba = out;
    *ow = w;
    *oh = h;
    return TE_OK;
}

/* ------------------------------------------------------------------------- */
/* Kitty graphics */

static int b64v(char c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

/* Decode base64 into a new buffer; *outlen receives the size. */
static uint8_t *b64_decode_alloc(const char *in, size_t len, size_t *outlen)
{
    *outlen = 0;
    size_t cap = len / 4 * 3 + 3;
    uint8_t *out = te_malloc(cap > 0 ? cap : 1);
    if (out == nullptr) {
        return nullptr;
    }
    uint32_t acc = 0;
    int bits = 0;
    size_t o = 0;
    for (size_t i = 0; i < len; i++) {
        if (in[i] == '=') break;
        int v = b64v(in[i]);
        if (v < 0) {
            free(out);
            return nullptr;
        }
        acc = (acc << 6) | (uint32_t)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (o >= cap) {
                free(out);
                return nullptr;
            }
            out[o++] = (uint8_t)((acc >> bits) & 0xFFu);
        }
    }
    *outlen = o;
    return out;
}

static void parse_keys(const char *keys, size_t klen, te_kitty_cmd *c)
{
    size_t i = 0;
    while (i < klen) {
        char key = keys[i];
        if (i + 1 >= klen || keys[i + 1] != '=') {
            while (i < klen && keys[i] != ',') i++;
            i++;
            continue;
        }
        i += 2;
        size_t vs = i;
        while (i < klen && keys[i] != ',') i++;
        size_t vl = i - vs;
        uint32_t num = 0;
        for (size_t k = 0; k < vl && k < 10; k++) {
            char d = keys[vs + k];
            if (d < '0' || d > '9') break;
            num = num * 10u + (uint32_t)(d - '0');
        }
        char v0 = vl > 0 ? keys[vs] : '\0';
        switch (key) {
        case 'a': c->action = v0; break;
        case 'f': c->format = num; break;
        case 'i': c->id = num; break;
        case 's': c->w = num; break;
        case 'v': c->h = num; break;
        case 'c': c->cols = (uint16_t)TE_MIN(num, 1000u); break;
        case 'r': c->rows = (uint16_t)TE_MIN(num, 1000u); break;
        case 'q': c->quiet = (int)TE_MIN(num, 2u); break;
        case 'C': c->no_move = num == 1; break;
        case 'd': c->del = v0; break;
        case 't': if (v0 != 'd') (void)str_copy(c->err, sizeof c->err, "EINVAL:only direct transmission supported"); break;
        case 'o': (void)str_copy(c->err, sizeof c->err, "EINVAL:compression not supported"); break;
        default: break;
        }
        i++;
    }
}

static bool chunk_append(te_images *s, const char *p, size_t n)
{
    if (n > TE_KITTY_MAX_PAYLOAD - s->chunk_len) {
        return false;
    }
    if (s->chunk_len + n + 1 > s->chunk_cap) {
        size_t ncap = s->chunk_cap == 0 ? 8192 : s->chunk_cap;
        while (ncap < s->chunk_len + n + 1) ncap *= 2;
        void *tmp = s->chunk;
        if (te_realloc_array(&tmp, s->chunk_cap, ncap, 1) != TE_OK) {
            return false;
        }
        s->chunk = tmp;
        s->chunk_cap = ncap;
    }
    memcpy(s->chunk + s->chunk_len, p, n);
    s->chunk_len += n;
    s->chunk[s->chunk_len] = '\0';
    return true;
}

static void chunk_reset(te_images *s)
{
    s->chunk_len = 0;
    s->chunking = false;
    s->chunk_keys[0] = '\0';
}

void kitty_command(te_images *s, const char *data, size_t len, te_kitty_cmd *c)
{
    memset(c, 0, sizeof *c);
    c->action = 't';
    c->format = 32;
    const char *semi = memchr(data, ';', len);
    size_t klen = semi != nullptr ? (size_t)(semi - data) : len;
    const char *payload = semi != nullptr ? semi + 1 : "";
    size_t plen = semi != nullptr ? len - klen - 1 : 0;

    /* m=1 means more chunks follow; the first chunk carries the keys. */
    bool more = false;
    for (size_t i = 0; i + 1 < klen; i++) {
        if ((i == 0 || data[i - 1] == ',') && data[i] == 'm' && data[i + 1] == '=') {
            more = i + 2 < klen && data[i + 2] == '1';
        }
    }
    if (!s->chunking) {
        if (str_copy_n(s->chunk_keys, sizeof s->chunk_keys, data, klen) != TE_OK) {
            (void)str_copy(c->err, sizeof c->err, "EINVAL:keys too long");
            c->ready = true;
            return;
        }
        s->chunk_len = 0;
    }
    if (!chunk_append(s, payload, plen)) {
        chunk_reset(s);
        (void)str_copy(c->err, sizeof c->err, "EFBIG:image too large");
        c->ready = true;
        return;
    }
    if (more) {
        s->chunking = true;
        return;   /* wait for the final chunk */
    }
    parse_keys(s->chunk_keys, strlen(s->chunk_keys), c);
    c->ready = true;
    if (c->err[0] != '\0' || c->action == 'd' || c->action == 'p') {
        chunk_reset(s);
        return;
    }
    if (c->format != 24 && c->format != 32) {
        (void)str_copy(c->err, sizeof c->err, c->format == 100 ? "EINVAL:PNG not supported, send f=24 or f=32"
                                                              : "EINVAL:unknown format");
        chunk_reset(s);
        return;
    }
    if (c->w == 0 || c->h == 0 || c->w > TE_IMAGE_MAX_DIM || c->h > TE_IMAGE_MAX_DIM) {
        (void)str_copy(c->err, sizeof c->err, "EINVAL:bad image size");
        chunk_reset(s);
        return;
    }
    size_t raw_len = 0;
    uint8_t *raw = b64_decode_alloc(s->chunk, s->chunk_len, &raw_len);
    chunk_reset(s);
    size_t bpp = c->format == 24 ? 3u : 4u;
    size_t expect = (size_t)c->w * c->h * bpp;   /* <= 4096*4096*4: no overflow on 64-bit */
    if (raw == nullptr || raw_len != expect) {
        free(raw);
        (void)str_copy(c->err, sizeof c->err, "ENODATA:payload size does not match s*v");
        return;
    }
    if (c->action == 'q') {
        free(raw);
        return;   /* query: valid, nothing stored */
    }
    uint8_t *px = te_malloc((size_t)c->w * c->h * 4u);
    if (px == nullptr) {
        free(raw);
        (void)str_copy(c->err, sizeof c->err, "ENOMEM:out of memory");
        return;
    }
    for (size_t i = 0; i < (size_t)c->w * c->h; i++) {
        uint8_t r = raw[i * bpp], g = raw[i * bpp + 1], b = raw[i * bpp + 2];
        uint8_t a = bpp == 4 ? raw[i * bpp + 3] : 255;
        /* Premultiply so the shared shader blends correctly. */
        px[i * 4 + 0] = (uint8_t)((r * a + 127u) / 255u);
        px[i * 4 + 1] = (uint8_t)((g * a + 127u) / 255u);
        px[i * 4 + 2] = (uint8_t)((b * a + 127u) / 255u);
        px[i * 4 + 3] = a;
    }
    free(raw);
    c->pixels = px;
}
