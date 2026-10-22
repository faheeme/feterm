/* Strict streaming UTF-8 decoder (rejects overlongs, surrogates, > U+10FFFF). */
#ifndef TE_UTF8_H
#define TE_UTF8_H

#include <stdbool.h>
#include "common.h"

#define TE_UTF8_REPLACEMENT 0xFFFDu

typedef struct te_utf8_dec {
    uint32_t cp;      /* code point under construction */
    uint32_t min;     /* smallest legal value for this sequence length */
    uint8_t  need;    /* continuation bytes still expected */
} te_utf8_dec;

typedef enum { TE_UTF8_MORE, TE_UTF8_DONE, TE_UTF8_ERROR, TE_UTF8_ERROR_REPROCESS } te_utf8_res;

/*
 * Feed one byte. DONE: *out holds a code point. ERROR: emit U+FFFD.
 * ERROR_REPROCESS: emit U+FFFD, then feed the same byte again (it began a new sequence).
 */
[[nodiscard]] te_utf8_res utf8_feed(te_utf8_dec *d, uint8_t byte, uint32_t *out);

/* Encode cp into out (capacity outsz). Returns bytes written or 0 if invalid/no room. */
[[nodiscard]] size_t utf8_encode(uint32_t cp, char *out, size_t outsz);

/* Display width of a code point: 0 (combining/zero-width), 1, or 2. Controls return 0. */
[[nodiscard]] int te_wcwidth(uint32_t cp);

#endif
