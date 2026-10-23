/*
 * The welcome banner a new local shell starts with:
 *
 *   [logo]  FeTerm v0.4.2
 *   [logo]  © Author - email
 *
 *   prompt
 *
 * The logo is the real app icon, drawn inline two lines tall with the kitty
 * graphics protocol that the terminal itself understands. Name, version,
 * author and email come from the build (Makefile), never hard-coded here.
 */
#ifndef TE_BANNER_H
#define TE_BANNER_H

#include "common.h"

/*
 * Build the banner's bytes into *out (heap, NUL-terminated, caller frees; *len its size
 * without the terminator).
 * rgba is the logo (px x px, straight alpha), shown img_cols x img_rows cells;
 * pass rgba == nullptr for a text-only banner.
 */
[[nodiscard]] te_err banner_build(const uint8_t *rgba, size_t rgba_len, uint32_t px, uint16_t img_cols,
                                  uint16_t img_rows, char **out, size_t *len);

/* Standard base64 of n bytes into dst (capacity dstsz, NUL-terminated). Returns the length or 0 if it does not fit. */
size_t base64_encode(const uint8_t *src, size_t n, char *dst, size_t dstsz);

#endif
