/* Find the URL (OSC 8 hyperlink or plain-text URL) under a cell, and open it safely. */
#ifndef TE_URL_H
#define TE_URL_H

#include <stdbool.h>
#include "common.h"
#include "vt/term.h"

typedef struct te_url_hit {
    uint64_t row0, row1;        /* absolute rows */
    uint16_t col0, col1;        /* first cell on row0, last cell on row1 (inclusive) */
    char     uri[TE_MAX_URI + 1];
} te_url_hit;

typedef struct te_urlfinder te_urlfinder;

[[nodiscard]] te_err url_create(te_urlfinder **out);
void url_destroy(te_urlfinder **u);

/* OSC 8 link first, then a URL pattern across the soft-wrapped logical line. */
[[nodiscard]] bool url_at(te_urlfinder *u, const te_term *t, uint64_t row, uint16_t col, te_url_hit *hit);

[[nodiscard]] bool url_hit_contains(const te_url_hit *h, uint64_t row, uint16_t col);

/*
 * True if uri is safe to hand to the opener: printable ASCII, no leading '-',
 * and a scheme from the allowlist (http, https, ftp, sftp, ssh, file, mailto).
 */
[[nodiscard]] bool url_is_openable(const char *uri);

/* Open with xdg-open via posix_spawnp (no shell). "www." gets "https://" prepended. */
[[nodiscard]] te_err url_open(const char *uri);

#endif
