#include "ui/url.h"

#include <string.h>
#include <strings.h>

#define PCRE2_CODE_UNIT_WIDTH 8
#include <pcre2.h>

#include "ui/linetext.h"
#include "util/log.h"
#include "util/mem.h"
#include "util/spawn.h"
#include "util/str.h"

#define URL_MAX_ROWS   16u        /* longest soft-wrapped logical line considered */
#define URL_TEXT_CAP   16384u

struct te_urlfinder {
    pcre2_code          *re;
    pcre2_match_data    *md;
    pcre2_match_context *mctx;
    char                 text[URL_TEXT_CAP];
    te_textpos           map[URL_TEXT_CAP];
};

te_err url_create(te_urlfinder **out)
{
    *out = nullptr;
    te_urlfinder *u = te_calloc(1, sizeof *u);
    if (u == nullptr) {
        return TE_ERR_NOMEM;
    }
    static const char pattern[] =
        "(?:\\b(?:https?|ftp|sftp|ssh|file|git)://|\\bmailto:|\\bwww\\.)"
        "[^\\s<>\"'`{}|\\\\^\\x00-\\x1f\\x7f]+";
    int err = 0;
    PCRE2_SIZE off = 0;
    u->re = pcre2_compile((PCRE2_SPTR)pattern, PCRE2_ZERO_TERMINATED, PCRE2_UTF | PCRE2_CASELESS, &err, &off, nullptr);
    u->md = pcre2_match_data_create(4, nullptr);
    u->mctx = pcre2_match_context_create(nullptr);
    if (u->re == nullptr || u->md == nullptr || u->mctx == nullptr) {
        url_destroy(&u);
        return TE_ERR_NOMEM;
    }
    (void)pcre2_jit_compile(u->re, PCRE2_JIT_COMPLETE);
    (void)pcre2_set_match_limit(u->mctx, 100000);
    *out = u;
    return TE_OK;
}

void url_destroy(te_urlfinder **up)
{
    if (up == nullptr || *up == nullptr) {
        return;
    }
    te_urlfinder *u = *up;
    pcre2_code_free(u->re);
    pcre2_match_data_free(u->md);
    pcre2_match_context_free(u->mctx);
    TE_FREE(*up);
}

bool url_hit_contains(const te_url_hit *h, uint64_t row, uint16_t col)
{
    if (row < h->row0 || row > h->row1) {
        return false;
    }
    if (row == h->row0 && col < h->col0) {
        return false;
    }
    if (row == h->row1 && col > h->col1) {
        return false;
    }
    return true;
}

static const te_cell *cell_at(const te_term *t, uint64_t row, uint16_t col)
{
    const te_line *l = term_abs_line(t, row);
    return (l != nullptr && l->cells != nullptr && col < l->cols) ? &l->cells[col] : nullptr;
}

/* The OSC 8 link's extent: neighbouring cells with the same link id, across soft wraps. */
static bool osc8_at(const te_term *t, uint64_t row, uint16_t col, te_url_hit *hit)
{
    const te_cell *c = cell_at(t, row, col);
    if (c == nullptr || c->link == 0) {
        return false;
    }
    const char *uri = term_link_uri(t, c->link);
    if (uri == nullptr || str_copy(hit->uri, sizeof hit->uri, uri) != TE_OK) {
        return false;
    }
    uint16_t id = c->link;
    uint64_t r = row;
    uint16_t x = col;
    for (unsigned guard = 0; guard < URL_MAX_ROWS * 1024u; guard++) {
        if (x > 0) {
            const te_cell *p = cell_at(t, r, (uint16_t)(x - 1));
            if (p == nullptr || p->link != id) break;
            x--;
        } else {
            const te_line *prev = r > term_abs_first(t) ? term_abs_line(t, r - 1) : nullptr;
            if (prev == nullptr || (prev->flags & TE_LINE_WRAPPED) == 0 || prev->cols == 0 ||
                prev->cells[prev->cols - 1].link != id) break;
            r--;
            x = (uint16_t)(prev->cols - 1);
        }
    }
    hit->row0 = r;
    hit->col0 = x;
    r = row;
    x = col;
    for (unsigned guard = 0; guard < URL_MAX_ROWS * 1024u; guard++) {
        const te_line *l = term_abs_line(t, r);
        if (l == nullptr) break;
        if (x + 1u < l->cols) {
            if (l->cells[x + 1].link != id) break;
            x++;
        } else {
            const te_line *next = term_abs_line(t, r + 1);
            if ((l->flags & TE_LINE_WRAPPED) == 0 || next == nullptr || next->cols == 0 || next->cells[0].link != id) break;
            r++;
            x = 0;
        }
    }
    hit->row1 = r;
    hit->col1 = x;
    return true;
}

/* Trim trailing punctuation that is almost never part of a URL, keeping balanced brackets. */
static size_t trim_url(const char *s, size_t len)
{
    while (len > 0) {
        char c = s[len - 1];
        if (c == '.' || c == ',' || c == ';' || c == ':' || c == '!' || c == '?') {
            len--;
            continue;
        }
        if (c == ')' || c == ']') {
            char open = c == ')' ? '(' : '[';
            int depth = 0;
            for (size_t i = 0; i < len; i++) {
                if (s[i] == open) depth++;
                else if (s[i] == c) depth--;
            }
            if (depth < 0) {
                len--;
                continue;
            }
        }
        break;
    }
    return len;
}

bool url_at(te_urlfinder *u, const te_term *t, uint64_t row, uint16_t col, te_url_hit *hit)
{
    if (u == nullptr || t == nullptr || hit == nullptr) {
        return false;
    }
    memset(hit, 0, sizeof *hit);
    if (osc8_at(t, row, col, hit)) {
        return true;
    }
    /* Logical line around row. */
    uint64_t first = term_abs_first(t);
    uint64_t r0 = row;
    while (r0 > first && row - r0 < URL_MAX_ROWS) {
        const te_line *prev = term_abs_line(t, r0 - 1);
        if (prev == nullptr || (prev->flags & TE_LINE_WRAPPED) == 0) break;
        r0--;
    }
    size_t len = 0;
    uint64_t r = r0;
    for (; r - r0 < URL_MAX_ROWS; r++) {
        const te_line *l = term_abs_line(t, r);
        if (l == nullptr || l->cells == nullptr) break;
        bool wrapped = (l->flags & TE_LINE_WRAPPED) != 0;
        uint16_t n = wrapped ? l->cols : linetext_trimmed(l->cells, l->cols);
        len = linetext_append(l->cells, n, (uint32_t)(r - r0), u->text, u->map, len, sizeof u->text);
        if (!wrapped) break;
    }
    PCRE2_SIZE start = 0;
    for (unsigned guard = 0; guard < 256 && start < len; guard++) {
        int rc = pcre2_match(u->re, (PCRE2_SPTR)u->text, len, start, 0, u->md, u->mctx);
        if (rc < 0) {
            return false;
        }
        const PCRE2_SIZE *ov = pcre2_get_ovector_pointer(u->md);
        size_t s = ov[0];
        size_t e = s + trim_url(u->text + s, ov[1] - s);
        if (e > s) {
            te_textpos a = u->map[s];
            te_textpos b = u->map[e - 1];
            uint64_t ra = r0 + a.row;
            uint64_t rb = r0 + b.row;
            te_url_hit h = { .row0 = ra, .row1 = rb, .col0 = a.col, .col1 = b.col };
            if (url_hit_contains(&h, row, col)) {
                if (e - s > TE_MAX_URI) {
                    return false;
                }
                memcpy(hit->uri, u->text + s, e - s);
                hit->uri[e - s] = '\0';
                hit->row0 = ra;
                hit->row1 = rb;
                hit->col0 = a.col;
                hit->col1 = b.col;
                return true;
            }
        }
        start = ov[1] > s ? ov[1] : s + 1;
    }
    return false;
}

bool url_is_openable(const char *uri)
{
    static const char *const schemes[] = { "http://", "https://", "ftp://", "sftp://", "ssh://", "file://",
                                           "mailto:", "www." };
    if (uri == nullptr || uri[0] == '\0' || uri[0] == '-') {
        return false;
    }
    size_t n = strnlen(uri, TE_MAX_URI + 1);
    if (n > TE_MAX_URI) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)uri[i];
        if (c < 0x21 || c > 0x7E) {
            return false;
        }
    }
    for (size_t i = 0; i < TE_ARRAY_LEN(schemes); i++) {
        if (strncasecmp(uri, schemes[i], strlen(schemes[i])) == 0) {
            return true;
        }
    }
    return false;
}

te_err url_open(const char *uri)
{
    if (!url_is_openable(uri)) {
        LOG_W("refusing to open a link with an unsupported scheme or characters");
        return TE_ERR_INVAL;
    }
    char full[TE_MAX_URI + 16] = { 0 };
    te_err rc = strncasecmp(uri, "www.", 4) == 0 ? str_fmt(full, sizeof full, "https://%s", uri)
                                                  : str_copy(full, sizeof full, uri);
    if (rc != TE_OK) {
        return rc;
    }
    const char *argv[] = { "xdg-open", full, nullptr };
    return spawn_detached(argv);
}
