#ifndef TE_THEME_H
#define TE_THEME_H

#include <stdbool.h>
#include "common.h"

typedef struct te_theme {
    char     name[128];
    uint32_t ansi[16];      /* 0xRRGGBB */
    uint32_t fg;
    uint32_t bg;
    uint32_t cursor;
    uint32_t cursor_text;
    uint32_t selection_bg;
    uint32_t selection_fg;
    uint32_t search_bg;          /* search matches */
    uint32_t search_fg;
    uint32_t search_current_bg;  /* the selected match */
} te_theme;

/* Built-in default (Catppuccin Mocha). */
void theme_defaults(te_theme *t);

/* Load themes/<name>.ini on top of the defaults. On error *t keeps the defaults. */
[[nodiscard]] te_err theme_load(te_theme *t, const char *name);

/* Parse "#rrggbb" or "rrggbb". */
[[nodiscard]] bool color_parse(const char *s, uint32_t *rgb);

#define TE_THEME_LIST_MAX 64

/* Names of all installed themes (bundled and user), sorted and de-duplicated. */
size_t theme_list(char names[][128], size_t max);

/* Fill a full 256-colour palette: 16 from the theme, 6x6x6 cube, 24 greys. */
void theme_build_palette(const te_theme *t, uint32_t pal[256]);

#endif
