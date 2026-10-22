#include "render/font.h"

#include <limits.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_SYNTHESIS_H
#include <fontconfig/fontconfig.h>

#include "util/log.h"
#include "util/mem.h"
#include "util/str.h"

#define TE_MAX_FALLBACK 32

typedef struct face_slot {
    FT_Face face;
    char    path[512];
    int     index;
} face_slot;

struct te_font {
    FT_Library      lib;
    FcConfig       *fc;
    face_slot       styles[4];
    face_slot       fallback[TE_MAX_FALLBACK];
    size_t          nfallback;
    char            family[256];
    float           px;
    te_font_metrics m;
    uint8_t        *buf;       /* converted source bitmap */
    size_t          buf_cap;
    uint8_t        *buf2;      /* scaled output */
    size_t          buf2_cap;
};

static void set_face_size(FT_Face face, float px)
{
    if (FT_IS_SCALABLE(face)) {
        /* At 72 dpi one point equals one pixel, so fractional pixel sizes work. */
        (void)FT_Set_Char_Size(face, 0, (FT_F26Dot6)lroundf(px * 64.0f), 72, 72);
        return;
    }
    /* Bitmap-only face (e.g. CBDT colour emoji): choose the closest strike. */
    if (face->num_fixed_sizes <= 0 || face->available_sizes == nullptr) {
        return;
    }
    int best = 0;
    long best_diff = LONG_MAX;
    for (int i = 0; i < face->num_fixed_sizes; i++) {
        long ppem = face->available_sizes[i].y_ppem / 64;
        long diff = labs(ppem - (long)px);
        if (diff < best_diff) {
            best_diff = diff;
            best = i;
        }
    }
    (void)FT_Select_Size(face, best);
}

[[nodiscard]] static te_err load_face(te_font *f, const char *path, int index, face_slot *slot)
{
    FT_Face face = nullptr;
    if (FT_New_Face(f->lib, path, index, &face) != 0 || face == nullptr) {
        return TE_ERR_FONT;
    }
    set_face_size(face, f->px);
    slot->face = face;
    slot->index = index;
    if (str_copy(slot->path, sizeof slot->path, path) != TE_OK) {
        slot->path[0] = '\0';
    }
    return TE_OK;
}

static void free_slot(face_slot *s)
{
    if (s->face != nullptr) {
        (void)FT_Done_Face(s->face);
        s->face = nullptr;
    }
    s->path[0] = '\0';
}

/* Find the face file for family+weight+slant via Fontconfig and load it. */
[[nodiscard]] static te_err match_style(te_font *f, int weight, int slant, face_slot *slot)
{
    te_err rc = TE_ERR_FONT;
    FcPattern *pat = FcNameParse((const FcChar8 *)f->family);
    FcPattern *match = nullptr;
    if (pat == nullptr) {
        return TE_ERR_NOMEM;
    }
    if (!FcPatternAddInteger(pat, FC_WEIGHT, weight) || !FcPatternAddInteger(pat, FC_SLANT, slant) ||
        !FcPatternAddDouble(pat, FC_PIXEL_SIZE, (double)f->px)) {
        rc = TE_ERR_NOMEM;
        goto out;
    }
    if (!FcConfigSubstitute(f->fc, pat, FcMatchPattern)) {
        goto out;
    }
    FcDefaultSubstitute(pat);
    FcResult res = FcResultNoMatch;
    match = FcFontMatch(f->fc, pat, &res);
    if (match == nullptr) {
        goto out;
    }
    FcChar8 *file = nullptr;
    int index = 0;
    if (FcPatternGetString(match, FC_FILE, 0, &file) != FcResultMatch || file == nullptr) {
        goto out;
    }
    if (FcPatternGetInteger(match, FC_INDEX, 0, &index) != FcResultMatch) {
        index = 0;
    }
    rc = load_face(f, (const char *)file, index, slot);

out:
    if (match != nullptr) {
        FcPatternDestroy(match);
    }
    FcPatternDestroy(pat);
    return rc;
}

static void compute_metrics(te_font *f, float line_height)
{
    FT_Face face = f->styles[TE_FONT_REGULAR].face;
    te_font_metrics *m = &f->m;
    int asc = (int)ceil((double)face->size->metrics.ascender / 64.0);
    int desc = (int)ceil((double)-face->size->metrics.descender / 64.0);
    if (asc <= 0) asc = (int)ceilf(f->px * 0.8f);
    if (desc < 0) desc = 0;
    int natural = asc + desc;
    if (natural <= 0) natural = (int)ceilf(f->px);

    int cw = 0;
    if (FT_Load_Char(face, 'M', FT_LOAD_DEFAULT) == 0) {
        cw = (int)lround((double)face->glyph->advance.x / 64.0);
    }
    if (cw <= 0) {
        cw = (int)lround((double)face->size->metrics.max_advance / 64.0);
    }
    m->cell_w = cw > 0 ? cw : (int)ceilf(f->px * 0.6f);
    m->cell_h = (int)lroundf((float)natural * line_height);
    if (m->cell_h < natural / 2 || m->cell_h < 1) {
        m->cell_h = natural;
    }
    m->baseline = asc + (m->cell_h - natural) / 2;

    int thick = 1;
    int upos = m->baseline + (desc > 2 ? desc / 2 : 1);
    if (FT_IS_SCALABLE(face) && face->underline_thickness > 0) {
        thick = (int)lround((double)FT_MulFix(face->underline_thickness, face->size->metrics.y_scale) / 64.0);
        long pos = FT_MulFix(face->underline_position, face->size->metrics.y_scale);
        upos = m->baseline - (int)lround((double)pos / 64.0);
    }
    if (thick < 1) thick = 1;
    if (upos + thick > m->cell_h) upos = m->cell_h - thick;
    if (upos < 0) upos = 0;
    m->underline_thick = thick;
    m->underline_pos = upos;
    m->strike_pos = m->baseline - (int)lround((double)asc * 0.3);
    if (m->strike_pos < 0) m->strike_pos = 0;
}

te_err font_create(te_font **out, const char *family, float px_size, float line_height)
{
    if (out == nullptr || family == nullptr || !(px_size >= 4.0f && px_size <= 512.0f)) {
        return TE_ERR_INVAL;
    }
    *out = nullptr;
    te_font *f = te_calloc(1, sizeof *f);
    if (f == nullptr) {
        return TE_ERR_NOMEM;
    }
    te_err rc = TE_OK;
    f->px = px_size;
    if (str_copy(f->family, sizeof f->family, family) != TE_OK) {
        rc = TE_ERR_TRUNC;
        goto fail;
    }
    if (FT_Init_FreeType(&f->lib) != 0) {
        f->lib = nullptr;
        rc = TE_ERR_FONT;
        goto fail;
    }
    f->fc = FcInitLoadConfigAndFonts();
    if (f->fc == nullptr) {
        rc = TE_ERR_FONT;
        goto fail;
    }
    static const int weights[4] = { FC_WEIGHT_REGULAR, FC_WEIGHT_BOLD, FC_WEIGHT_REGULAR, FC_WEIGHT_BOLD };
    static const int slants[4] = { FC_SLANT_ROMAN, FC_SLANT_ROMAN, FC_SLANT_ITALIC, FC_SLANT_ITALIC };
    for (size_t i = 0; i < 4; i++) {
        rc = match_style(f, weights[i], slants[i], &f->styles[i]);
        if (rc != TE_OK) {
            if (i == TE_FONT_REGULAR) {
                LOG_E("no usable font for '%s'", family);
                goto fail;
            }
            /* Fall back to the regular face (with synthetic bold/italic). */
            rc = load_face(f, f->styles[0].path, f->styles[0].index, &f->styles[i]);
            if (rc != TE_OK) {
                goto fail;
            }
        }
    }
    LOG_I("font: %s (%.1fpx)", f->styles[0].path, (double)px_size);
    compute_metrics(f, line_height);
    *out = f;
    return TE_OK;

fail:
    font_destroy(&f);
    return rc;
}

void font_destroy(te_font **fp)
{
    if (fp == nullptr || *fp == nullptr) {
        return;
    }
    te_font *f = *fp;
    for (size_t i = 0; i < 4; i++) {
        free_slot(&f->styles[i]);
    }
    for (size_t i = 0; i < f->nfallback; i++) {
        free_slot(&f->fallback[i]);
    }
    f->nfallback = 0;
    if (f->lib != nullptr) {
        (void)FT_Done_FreeType(f->lib);
        f->lib = nullptr;
    }
    if (f->fc != nullptr) {
        FcConfigDestroy(f->fc);
        f->fc = nullptr;
    }
    TE_FREE(f->buf);
    TE_FREE(f->buf2);
    TE_FREE(*fp);
}

const te_font_metrics *font_metrics(const te_font *f)
{
    return &f->m;
}

/* Ask Fontconfig for a font containing cp and add it to the fallback list. */
static FT_Face find_fallback(te_font *f, uint32_t cp)
{
    for (size_t i = 0; i < f->nfallback; i++) {
        if (FT_Get_Char_Index(f->fallback[i].face, cp) != 0) {
            return f->fallback[i].face;
        }
    }
    if (f->nfallback >= TE_MAX_FALLBACK) {
        return nullptr;
    }
    FT_Face found = nullptr;
    FcPattern *pat = FcNameParse((const FcChar8 *)f->family);
    FcCharSet *cs = FcCharSetCreate();
    FcFontSet *set = nullptr;
    if (pat == nullptr || cs == nullptr || !FcCharSetAddChar(cs, cp) || !FcPatternAddCharSet(pat, FC_CHARSET, cs)) {
        goto out;
    }
    if (!FcConfigSubstitute(f->fc, pat, FcMatchPattern)) {
        goto out;
    }
    FcDefaultSubstitute(pat);
    FcResult res = FcResultNoMatch;
    set = FcFontSort(f->fc, pat, FcFalse, nullptr, &res);
    if (set == nullptr) {
        goto out;
    }
    for (int i = 0; i < set->nfont && found == nullptr; i++) {
        FcCharSet *fcs = nullptr;
        FcChar8 *file = nullptr;
        int index = 0;
        if (FcPatternGetCharSet(set->fonts[i], FC_CHARSET, 0, &fcs) != FcResultMatch || !FcCharSetHasChar(fcs, cp)) {
            continue;
        }
        if (FcPatternGetString(set->fonts[i], FC_FILE, 0, &file) != FcResultMatch || file == nullptr) {
            continue;
        }
        if (FcPatternGetInteger(set->fonts[i], FC_INDEX, 0, &index) != FcResultMatch) {
            index = 0;
        }
        bool dup = false;
        for (size_t k = 0; k < f->nfallback; k++) {
            if (f->fallback[k].index == index && strcmp(f->fallback[k].path, (const char *)file) == 0) {
                dup = true;
                break;
            }
        }
        if (dup) {
            continue;
        }
        face_slot *slot = &f->fallback[f->nfallback];
        if (load_face(f, (const char *)file, index, slot) != TE_OK) {
            continue;
        }
        if (FT_Get_Char_Index(slot->face, cp) == 0) {
            free_slot(slot);
            continue;
        }
        LOG_D("fallback font for U+%04X: %s", cp, slot->path);
        f->nfallback++;
        found = slot->face;
    }

out:
    if (set != nullptr) FcFontSetDestroy(set);
    if (cs != nullptr) FcCharSetDestroy(cs);
    if (pat != nullptr) FcPatternDestroy(pat);
    return found;
}

[[nodiscard]] static bool ensure_buf(uint8_t **buf, size_t *cap, size_t need)
{
    if (need <= *cap) {
        return true;
    }
    void *p = *buf;
    if (te_realloc_array(&p, *cap, need, 1) != TE_OK) {
        return false;
    }
    *buf = p;
    *cap = need;
    return true;
}

/* Area-average downscale of premultiplied RGBA (sw x sh) into (dw x dh). */
static void downscale(const uint8_t *src, int sw, int sh, uint8_t *dst, int dw, int dh)
{
    for (int dy = 0; dy < dh; dy++) {
        int sy0 = dy * sh / dh;
        int sy1 = (dy + 1) * sh / dh;
        if (sy1 <= sy0) sy1 = sy0 + 1;
        for (int dx = 0; dx < dw; dx++) {
            int sx0 = dx * sw / dw;
            int sx1 = (dx + 1) * sw / dw;
            if (sx1 <= sx0) sx1 = sx0 + 1;
            uint32_t acc[4] = { 0, 0, 0, 0 };
            uint32_t n = 0;
            for (int sy = sy0; sy < sy1 && sy < sh; sy++) {
                for (int sx = sx0; sx < sx1 && sx < sw; sx++) {
                    const uint8_t *px = src + ((size_t)sy * (size_t)sw + (size_t)sx) * 4u;
                    acc[0] += px[0]; acc[1] += px[1]; acc[2] += px[2]; acc[3] += px[3];
                    n++;
                }
            }
            uint8_t *o = dst + ((size_t)dy * (size_t)dw + (size_t)dx) * 4u;
            for (size_t c = 0; c < 4; c++) {
                o[c] = (uint8_t)(n > 0 ? acc[c] / n : 0);
            }
        }
    }
}

te_err font_rasterize(te_font *f, uint32_t cp, unsigned style, int max_w, te_raster *out)
{
    if (f == nullptr || out == nullptr || style > 3 || max_w <= 0) {
        return TE_ERR_INVAL;
    }
    memset(out, 0, sizeof *out);
    bool want_bold = (style & TE_FONT_BOLD) != 0;
    bool want_italic = (style & TE_FONT_ITALIC) != 0;

    FT_Face face = f->styles[style].face;
    FT_UInt gi = FT_Get_Char_Index(face, cp);
    if (gi == 0 && style != TE_FONT_REGULAR) {
        face = f->styles[TE_FONT_REGULAR].face;
        gi = FT_Get_Char_Index(face, cp);
    }
    if (gi == 0) {
        face = find_fallback(f, cp);
        if (face == nullptr) {
            return TE_ERR_NOTFOUND;
        }
        gi = FT_Get_Char_Index(face, cp);
        if (gi == 0) {
            return TE_ERR_NOTFOUND;
        }
    }

    FT_Int32 flags = FT_LOAD_DEFAULT | FT_LOAD_TARGET_LIGHT;
    if (FT_HAS_COLOR(face)) {
        flags |= FT_LOAD_COLOR;
    }
    if (FT_Load_Glyph(face, gi, flags) != 0) {
        return TE_ERR_FONT;
    }
    FT_GlyphSlot slot = face->glyph;
    if (slot->format == FT_GLYPH_FORMAT_OUTLINE) {
        if (want_bold && (face->style_flags & FT_STYLE_FLAG_BOLD) == 0) {
            FT_GlyphSlot_Embolden(slot);
        }
        if (want_italic && (face->style_flags & FT_STYLE_FLAG_ITALIC) == 0) {
            FT_GlyphSlot_Oblique(slot);
        }
    }
    if (slot->format != FT_GLYPH_FORMAT_BITMAP && FT_Render_Glyph(slot, FT_RENDER_MODE_NORMAL) != 0) {
        return TE_ERR_FONT;
    }

    const FT_Bitmap *bm = &slot->bitmap;
    int w = (int)bm->width;
    int h = (int)bm->rows;
    if (w <= 0 || h <= 0 || bm->buffer == nullptr) {
        out->w = 0;   /* e.g. space: valid, nothing to draw */
        out->h = 0;
        return TE_OK;
    }
    if (bm->pitch == INT_MIN) {
        return TE_ERR_FONT;
    }
    if (w > TE_GLYPH_MAX_DIM) w = TE_GLYPH_MAX_DIM;
    if (h > TE_GLYPH_MAX_DIM) h = TE_GLYPH_MAX_DIM;
    size_t pitch = (size_t)(bm->pitch < 0 ? -bm->pitch : bm->pitch);
    size_t need = (size_t)w * (size_t)h * 4u;   /* bounded by TE_GLYPH_MAX_DIM^2 * 4 */
    if (!ensure_buf(&f->buf, &f->buf_cap, need)) {
        return TE_ERR_NOMEM;
    }

    bool color = false;
    for (int y = 0; y < h; y++) {
        size_t src_row = bm->pitch >= 0 ? (size_t)y : (size_t)((int)bm->rows - 1 - y);
        const uint8_t *row = bm->buffer + src_row * pitch;
        uint8_t *o = f->buf + (size_t)y * (size_t)w * 4u;
        for (int x = 0; x < w; x++) {
            uint8_t r = 0, g = 0, b = 0, a = 0;
            switch (bm->pixel_mode) {
            case FT_PIXEL_MODE_GRAY:
                a = row[x];
                r = g = b = a;
                break;
            case FT_PIXEL_MODE_MONO:
                a = (row[x >> 3] & (0x80 >> (x & 7))) != 0 ? 255 : 0;
                r = g = b = a;
                break;
            case FT_PIXEL_MODE_BGRA:
                b = row[x * 4 + 0];
                g = row[x * 4 + 1];
                r = row[x * 4 + 2];
                a = row[x * 4 + 3];
                color = true;
                break;
            default:
                return TE_ERR_FONT;
            }
            o[x * 4 + 0] = r;
            o[x * 4 + 1] = g;
            o[x * 4 + 2] = b;
            o[x * 4 + 3] = a;
        }
    }

    const te_font_metrics *m = &f->m;
    out->color = color;
    out->rgba = f->buf;
    out->w = w;
    out->h = h;
    out->left = slot->bitmap_left;
    out->top = slot->bitmap_top;

    /* Colour bitmaps (emoji strikes) are usually far larger than a cell: fit and centre them. */
    if (color && (w > max_w || h > m->cell_h)) {
        double sx = (double)max_w / (double)w;
        double sy = (double)m->cell_h / (double)h;
        double s = sx < sy ? sx : sy;
        int dw = (int)((double)w * s);
        int dh = (int)((double)h * s);
        if (dw < 1) dw = 1;
        if (dh < 1) dh = 1;
        size_t need2 = (size_t)dw * (size_t)dh * 4u;
        if (!ensure_buf(&f->buf2, &f->buf2_cap, need2)) {
            return TE_ERR_NOMEM;
        }
        downscale(f->buf, w, h, f->buf2, dw, dh);
        out->rgba = f->buf2;
        out->w = dw;
        out->h = dh;
        out->left = (max_w - dw) / 2;
        out->top = m->baseline - (m->cell_h - dh) / 2;
    }
    return TE_OK;
}
