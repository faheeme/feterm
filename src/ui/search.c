#include "ui/search.h"

#include <string.h>

#define PCRE2_CODE_UNIT_WIDTH 8
#include <pcre2.h>

#include "ui/linetext.h"
#include "util/log.h"
#include "util/mem.h"
#include "util/str.h"

#define SEARCH_LINE_CAP 8192u

struct te_search {
    bool                 open;
    char                 query[TE_SEARCH_MAX_QUERY + 1];
    pcre2_code          *re;
    bool                 literal;          /* query was not a valid regex */
    pcre2_match_data    *md;
    pcre2_match_context *mctx;
    te_search_match     *matches;          /* sorted by row, then column */
    size_t               nmatches;
    bool                 truncated;
    size_t               cur;              /* index into matches */
    uint64_t             gen;              /* term content generation searched */
    char                 status[64];
    char                 text[SEARCH_LINE_CAP];
    te_textpos           map[SEARCH_LINE_CAP];
};

te_err search_create(te_search **out)
{
    *out = nullptr;
    te_search *s = te_calloc(1, sizeof *s);
    if (s == nullptr) {
        return TE_ERR_NOMEM;
    }
    s->md = pcre2_match_data_create(2, nullptr);
    s->mctx = pcre2_match_context_create(nullptr);
    s->matches = te_calloc(TE_SEARCH_MAX_MATCHES, sizeof *s->matches);
    if (s->md == nullptr || s->mctx == nullptr || s->matches == nullptr) {
        search_destroy(&s);
        return TE_ERR_NOMEM;
    }
    (void)pcre2_set_match_limit(s->mctx, 100000);
    (void)pcre2_set_depth_limit(s->mctx, 1000);
    (void)pcre2_set_heap_limit(s->mctx, 4096);
    *out = s;
    return TE_OK;
}

void search_destroy(te_search **sp)
{
    if (sp == nullptr || *sp == nullptr) {
        return;
    }
    te_search *s = *sp;
    pcre2_code_free(s->re);
    pcre2_match_data_free(s->md);
    pcre2_match_context_free(s->mctx);
    TE_FREE(s->matches);
    TE_FREE(*sp);
}

void search_open(te_search *s)
{
    s->open = true;
}

void search_close(te_search *s)
{
    s->open = false;
    s->nmatches = 0;
}

bool search_is_open(const te_search *s)
{
    return s != nullptr && s->open;
}

const char *search_query(const te_search *s)
{
    return s->query;
}

static bool has_upper(const char *q)
{
    for (; *q != '\0'; q++) {
        if (*q >= 'A' && *q <= 'Z') {
            return true;
        }
    }
    return false;
}

static void compile_query(te_search *s)
{
    pcre2_code_free(s->re);
    s->re = nullptr;
    s->literal = false;
    if (s->query[0] == '\0') {
        return;
    }
    uint32_t opts = PCRE2_UTF | PCRE2_UCP | PCRE2_NEVER_BACKSLASH_C | (has_upper(s->query) ? 0u : PCRE2_CASELESS);
    int err = 0;
    PCRE2_SIZE off = 0;
    s->re = pcre2_compile((PCRE2_SPTR)s->query, PCRE2_ZERO_TERMINATED, opts, &err, &off, nullptr);
    if (s->re == nullptr) {
        /* Half-typed regexes are common ("foo(") : search the text literally meanwhile. */
        /* PCRE2_LITERAL only combines with a few options (not UCP or NEVER_BACKSLASH_C). */
        uint32_t lit = PCRE2_UTF | PCRE2_LITERAL | (opts & PCRE2_CASELESS);
        s->re = pcre2_compile((PCRE2_SPTR)s->query, PCRE2_ZERO_TERMINATED, lit, &err, &off, nullptr);
        s->literal = s->re != nullptr;
    }
    if (s->re != nullptr) {
        (void)pcre2_jit_compile(s->re, PCRE2_JIT_COMPLETE);
    }
}

static void set_status(te_search *s)
{
    if (s->query[0] == '\0') {
        s->status[0] = '\0';
    } else if (s->re == nullptr) {
        (void)str_copy(s->status, sizeof s->status, "invalid");
    } else if (s->nmatches == 0) {
        (void)str_copy(s->status, sizeof s->status, "no matches");
    } else {
        (void)str_fmt(s->status, sizeof s->status, "%zu/%zu%s%s", s->cur + 1, s->nmatches,
                      s->truncated ? "+" : "", s->literal ? " (text)" : "");
    }
}

void search_run(te_search *s, const te_term *t)
{
    s->nmatches = 0;
    s->truncated = false;
    s->cur = 0;
    s->gen = term_content_gen(t);
    if (s->re == nullptr) {
        set_status(s);
        return;
    }
    uint64_t first = term_abs_first(t);
    uint64_t end = term_abs_end(t);
    for (uint64_t r = first; r < end && !s->truncated; r++) {
        const te_line *l = term_abs_line(t, r);
        if (l == nullptr || l->cells == nullptr) {
            continue;
        }
        size_t len = linetext_append(l->cells, linetext_trimmed(l->cells, l->cols), 0, s->text, s->map, 0,
                                     sizeof s->text);
        PCRE2_SIZE start = 0;
        for (unsigned guard = 0; guard < 512 && start <= len; guard++) {
            int rc = pcre2_match(s->re, (PCRE2_SPTR)s->text, len, start, 0, s->md, s->mctx);
            if (rc < 0) {
                break;   /* no match, or a limit was hit on this line */
            }
            const PCRE2_SIZE *ov = pcre2_get_ovector_pointer(s->md);
            if (ov[1] > ov[0] && ov[1] <= len) {
                if (s->nmatches >= TE_SEARCH_MAX_MATCHES) {
                    s->truncated = true;
                    break;
                }
                s->matches[s->nmatches++] = (te_search_match){
                    .row = r, .col0 = s->map[ov[0]].col, .col1 = s->map[ov[1] - 1].col,
                };
                start = ov[1];
            } else {
                start = ov[0] + 1;   /* empty match: step forward */
                while (start < len && ((unsigned char)s->text[start] & 0xC0u) == 0x80u) {
                    start++;
                }
            }
        }
    }
    s->cur = s->nmatches > 0 ? s->nmatches - 1 : 0;   /* newest first */
    set_status(s);
}

void search_append(te_search *s, const te_term *t, const char *utf8)
{
    if (str_append(s->query, sizeof s->query, utf8) != TE_OK) {
        return;
    }
    compile_query(s);
    search_run(s, t);
}

void search_backspace(te_search *s, const te_term *t)
{
    size_t n = strlen(s->query);
    if (n == 0) {
        return;
    }
    /* Remove one whole UTF-8 character. */
    do {
        n--;
    } while (n > 0 && ((unsigned char)s->query[n] & 0xC0u) == 0x80u);
    s->query[n] = '\0';
    compile_query(s);
    search_run(s, t);
}

bool search_step(te_search *s, const te_term *t, int dir)
{
    if (s->gen != term_content_gen(t)) {
        /* New output arrived: refresh the results, keeping the position near the old match. */
        te_search_match old = { 0 };
        bool had = search_current(s, &old);
        search_run(s, t);
        if (had) {
            for (size_t i = 0; i < s->nmatches; i++) {
                if (s->matches[i].row >= old.row) {
                    s->cur = i;
                    break;
                }
            }
        }
    }
    if (s->nmatches == 0) {
        return false;
    }
    if (dir < 0) {
        s->cur = s->cur == 0 ? s->nmatches - 1 : s->cur - 1;
    } else if (dir > 0) {
        s->cur = s->cur + 1 >= s->nmatches ? 0 : s->cur + 1;
    }
    set_status(s);
    return true;
}

bool search_current(const te_search *s, te_search_match *m)
{
    if (s == nullptr || s->nmatches == 0 || s->cur >= s->nmatches) {
        return false;
    }
    *m = s->matches[s->cur];
    return true;
}

bool search_cell(const te_search *s, uint64_t row, uint16_t col, bool *current)
{
    if (s == nullptr || !s->open || s->nmatches == 0) {
        return false;
    }
    /* First match on or after row (binary search). */
    size_t lo = 0;
    size_t hi = s->nmatches;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (s->matches[mid].row < row) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    for (size_t i = lo; i < s->nmatches && s->matches[i].row == row; i++) {
        if (col >= s->matches[i].col0 && col <= s->matches[i].col1) {
            *current = i == s->cur;
            return true;
        }
    }
    return false;
}

const char *search_status(const te_search *s)
{
    return s->status;
}
