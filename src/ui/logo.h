/*
 * Vendor / OS logos for saved sessions: alpha masks compiled into the binary
 * (src/ui/logo_data.h, from tools/gen-logos.py), tinted when drawn.
 */
#ifndef TE_LOGO_H
#define TE_LOGO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define TE_LOGO_SLUG_MAX 23        /* bytes, without the terminator */
#define TE_LOGO_DEFAULT  0         /* the generic terminal */
#define TE_LOGO_PX_MAX   48        /* the masks' size: logos are never drawn larger */

/* Number of selectable logos; valid indices are 0 .. logo_count() - 1. */
size_t logo_count(void);
/* The status dot: a filled circle, drawable like a logo but not selectable. */
int logo_dot(void);
/* Index of the logo saved as slug, or -1 (unknown or empty). */
int logo_find(const char *slug);
const char *logo_slug(int i);   /* "" when out of range */
const char *logo_name(int i);   /* "" when out of range */
/* Display names in index order, for a choice list. */
const char *const *logo_names(void);
/* A slug as stored in session files: 1..TE_LOGO_SLUG_MAX of [a-z0-9-]. */
[[nodiscard]] bool logo_slug_valid(const char *slug);

/*
 * Scale logo i (or logo_dot()) to px x px coverage values in out (row by row,
 * px * px bytes). px is clamped to 1..TE_LOGO_PX; the size used is returned
 * (0 on a bad index or when out is too small).
 */
int logo_render(int i, int px, uint8_t *out, size_t outsz);
int logo_max_px(void);

#endif
