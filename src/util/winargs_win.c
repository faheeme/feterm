#include "util/winargs.h"

#include <string.h>

#include "util/mem.h"

#define CMDLINE_MAX 32767u   /* CreateProcessW's limit, in UTF-16 units */

typedef struct cbuf {
    char  *s;
    size_t len, cap;
    bool   failed;
} cbuf;

static void put(cbuf *b, const char *s, size_t n)
{
    if (b->failed) {
        return;
    }
    size_t need = 0;
    if (ckd_add(&need, b->len, n) || ckd_add(&need, need, (size_t)1) || need > 4u * CMDLINE_MAX) {
        b->failed = true;
        return;
    }
    if (need > b->cap) {
        size_t ncap = b->cap == 0 ? 256 : b->cap;
        while (ncap < need) {
            ncap *= 2;
        }
        if (te_realloc_array((void **)&b->s, b->cap, ncap, 1) != TE_OK) {
            b->failed = true;
            return;
        }
        b->cap = ncap;
    }
    memcpy(b->s + b->len, s, n);
    b->len += n;
    b->s[b->len] = '\0';
}

static void repeat(cbuf *b, char c, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        put(b, &c, 1);
    }
}

/* One argument, quoted the way the C runtime's argv parser undoes it. */
static void append_arg(cbuf *b, const char *arg)
{
    if (b->len > 0) {
        put(b, " ", 1);
    }
    if (arg[0] != '\0' && strpbrk(arg, " \t\n\v\"") == nullptr) {
        put(b, arg, strlen(arg));
        return;
    }
    put(b, "\"", 1);
    size_t slashes = 0;
    for (const char *c = arg; *c != '\0'; c++) {
        if (*c == '\\') {
            slashes++;
            continue;
        }
        /* Backslashes before a quote are doubled and the quote escaped; elsewhere they are literal. */
        repeat(b, '\\', *c == '"' ? slashes * 2 + 1 : slashes);
        slashes = 0;
        put(b, c, 1);
    }
    repeat(b, '\\', slashes * 2);   /* before the closing quote */
    put(b, "\"", 1);
}

te_err win_cmdline(const char *const argv[], wchar_t **out)
{
    if (argv == nullptr || argv[0] == nullptr || out == nullptr) {
        return TE_ERR_INVAL;
    }
    *out = nullptr;
    cbuf b = { 0 };
    for (size_t i = 0; argv[i] != nullptr; i++) {
        append_arg(&b, argv[i]);
    }
    if (b.failed || b.s == nullptr) {
        TE_FREE(b.s);
        return TE_ERR_OVERFLOW;
    }
    wchar_t *w = te_w_widen(b.s);
    TE_FREE(b.s);
    if (w == nullptr) {
        return TE_ERR_INVAL;
    }
    if (wcslen(w) > CMDLINE_MAX) {
        TE_FREE(w);
        return TE_ERR_OVERFLOW;
    }
    *out = w;
    return TE_OK;
}
