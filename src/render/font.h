/* Font loading (Fontconfig), rasterisation (FreeType), fallback and colour emoji. */
#ifndef TE_FONT_H
#define TE_FONT_H

#include <stdbool.h>
#include "common.h"

enum { TE_FONT_REGULAR = 0, TE_FONT_BOLD = 1, TE_FONT_ITALIC = 2, TE_FONT_BOLD_ITALIC = 3 };

#define TE_GLYPH_MAX_DIM 512

typedef struct te_font_metrics {
    int cell_w, cell_h;      /* pixels */
    int baseline;            /* from the top of the cell */
    int underline_pos;       /* from the top of the cell */
    int underline_thick;
    int strike_pos;
} te_font_metrics;

/* A rasterised glyph. rgba points into the font's scratch buffer (valid until the next call). */
typedef struct te_raster {
    const uint8_t *rgba;     /* premultiplied RGBA, w*h*4 bytes; grey glyphs have r=g=b=a */
    int w, h;
    int left, top;           /* offset of the bitmap from the pen position / baseline */
    bool color;
} te_raster;

typedef struct te_font te_font;

[[nodiscard]] te_err font_create(te_font **out, const char *family, float px_size, float line_height);
void font_destroy(te_font **f);
const te_font_metrics *font_metrics(const te_font *f);

/*
 * Rasterise cp in the given style into a box of max_w x cell_h pixels
 * (max_w is cell_w for normal and 2*cell_w for wide characters).
 * Returns TE_ERR_NOTFOUND if no font has the character.
 */
[[nodiscard]] te_err font_rasterize(te_font *f, uint32_t cp, unsigned style, int max_w, te_raster *out);

#endif
