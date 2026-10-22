#include "util/str.h"

#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "util/mem.h"

te_err str_copy_n(char *dst, size_t dstsz, const char *src, size_t n)
{
    if (dst == nullptr || dstsz == 0) {
        return TE_ERR_INVAL;
    }
    if (src == nullptr) {
        dst[0] = '\0';
        return TE_ERR_INVAL;
    }
    size_t len = strnlen(src, n);
    te_err rc = TE_OK;
    if (len >= dstsz) {
        len = dstsz - 1;
        rc = TE_ERR_TRUNC;
    }
    memcpy(dst, src, len);
    dst[len] = '\0';
    return rc;
}

te_err str_copy(char *dst, size_t dstsz, const char *src)
{
    return str_copy_n(dst, dstsz, src, SIZE_MAX);
}

te_err str_append(char *dst, size_t dstsz, const char *src)
{
    if (dst == nullptr || dstsz == 0 || src == nullptr) {
        return TE_ERR_INVAL;
    }
    size_t cur = strnlen(dst, dstsz);
    if (cur >= dstsz) {
        dst[dstsz - 1] = '\0';
        return TE_ERR_TRUNC;
    }
    return str_copy(dst + cur, dstsz - cur, src);
}

te_err str_fmt(char *dst, size_t dstsz, const char *fmt, ...)
{
    if (dst == nullptr || dstsz == 0 || fmt == nullptr) {
        return TE_ERR_INVAL;
    }
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(dst, dstsz, fmt, ap);
    va_end(ap);
    if (n < 0) {
        dst[0] = '\0';
        return TE_ERR_INVAL;
    }
    dst[dstsz - 1] = '\0';
    if ((size_t)n >= dstsz) {
        return TE_ERR_TRUNC;
    }
    return TE_OK;
}

char *str_dup(const char *s)
{
    if (s == nullptr) {
        return nullptr;
    }
    size_t len = strlen(s);
    size_t sz = 0;
    if (ckd_add(&sz, len, (size_t)1)) {
        return nullptr;
    }
    char *d = te_malloc(sz);
    if (d == nullptr) {
        return nullptr;
    }
    memcpy(d, s, len);
    d[len] = '\0';
    return d;
}

static bool is_space(char c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\v' || c == '\f';
}

char *str_trim(char *s)
{
    if (s == nullptr) {
        return nullptr;
    }
    while (*s != '\0' && is_space(*s)) {
        s++;
    }
    size_t len = strlen(s);
    while (len > 0 && is_space(s[len - 1])) {
        s[--len] = '\0';
    }
    return s;
}

bool str_eq_ci(const char *a, const char *b)
{
    if (a == nullptr || b == nullptr) {
        return false;
    }
    return strcasecmp(a, b) == 0;
}

bool str_to_long(const char *s, long min, long max, long *out)
{
    if (s == nullptr || out == nullptr || *s == '\0') {
        return false;
    }
    char *end = nullptr;
    errno = 0;
    long v = strtol(s, &end, 10);
    if (errno != 0 || end == s || *end != '\0' || v < min || v > max) {
        return false;
    }
    *out = v;
    return true;
}

bool str_to_float(const char *s, float min, float max, float *out)
{
    if (s == nullptr || out == nullptr || *s == '\0') {
        return false;
    }
    char *end = nullptr;
    errno = 0;
    float v = strtof(s, &end);
    if (errno != 0 || end == s || *end != '\0' || !isfinite(v) || v < min || v > max) {
        return false;
    }
    *out = v;
    return true;
}

bool str_to_bool(const char *s, bool *out)
{
    if (s == nullptr || out == nullptr) {
        return false;
    }
    if (str_eq_ci(s, "true") || str_eq_ci(s, "yes") || str_eq_ci(s, "on") || strcmp(s, "1") == 0) {
        *out = true;
        return true;
    }
    if (str_eq_ci(s, "false") || str_eq_ci(s, "no") || str_eq_ci(s, "off") || strcmp(s, "0") == 0) {
        *out = false;
        return true;
    }
    return false;
}

static int hex_val(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool str_unescape(const char *s, uint8_t *out, size_t outsz, size_t *len)
{
    size_t n = 0;
    *len = 0;
    for (; *s != '\0'; s++) {
        uint8_t b = (uint8_t)*s;
        if (*s == '\\') {
            s++;
            switch (*s) {
            case '0':  b = 0; break;
            case 'r':  b = '\r'; break;
            case 'n':  b = '\n'; break;
            case 't':  b = '\t'; break;
            case 'e':  b = 0x1B; break;
            case '\\': b = '\\'; break;
            case 'x': {
                int hi = hex_val(s[1]);
                int lo = hi >= 0 ? hex_val(s[2]) : -1;
                if (lo < 0) return false;
                b = (uint8_t)(hi * 16 + lo);
                s += 2;
                break;
            }
            default:
                return false;
            }
        }
        if (n >= outsz) {
            return false;
        }
        out[n++] = b;
    }
    *len = n;
    return true;
}
