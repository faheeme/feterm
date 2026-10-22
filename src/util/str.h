/* Bounded string helpers. Every output buffer is always NUL-terminated. */
#ifndef TE_STR_H
#define TE_STR_H

#include <stdarg.h>
#include <stdbool.h>
#include "common.h"

/* Copy src into dst (capacity dstsz). Returns TE_ERR_TRUNC if truncated. */
[[nodiscard]] te_err str_copy(char *dst, size_t dstsz, const char *src);

/* Append src to the NUL-terminated dst. Returns TE_ERR_TRUNC if truncated. */
[[nodiscard]] te_err str_append(char *dst, size_t dstsz, const char *src);

/* Copy at most n bytes of src (which need not be terminated). */
[[nodiscard]] te_err str_copy_n(char *dst, size_t dstsz, const char *src, size_t n);

/* snprintf wrapper: TE_ERR_TRUNC on truncation, TE_ERR_INVAL on encoding error. */
[[nodiscard]] te_err str_fmt(char *dst, size_t dstsz, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

/* Heap duplicate (nullptr on failure). Caller frees. */
[[nodiscard]] char *str_dup(const char *s);

/* Trim leading/trailing ASCII whitespace in place; returns pointer into s. */
char *str_trim(char *s);

[[nodiscard]] bool str_eq_ci(const char *a, const char *b);
[[nodiscard]] bool str_to_long(const char *s, long min, long max, long *out);
[[nodiscard]] bool str_to_float(const char *s, float min, float max, float *out);
[[nodiscard]] bool str_to_bool(const char *s, bool *out);
/*
 * Decode C-style escapes (\0 \r \n \t \e \\ \xHH) into raw bytes. Fails on a
 * malformed escape or if the result does not fit in outsz bytes.
 */
[[nodiscard]] bool str_unescape(const char *s, uint8_t *out, size_t outsz, size_t *len);

#endif
