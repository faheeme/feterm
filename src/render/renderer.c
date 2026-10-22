#include "render/renderer.h"

#include <math.h>
#include <string.h>

#include <GLES3/gl3.h>

#include "render/boxdraw.h"
#include "util/log.h"
#include "util/mem.h"
#include "util/str.h"
#include "util/utf8.h"

#define ATLAS_MAX        2048
#define GLYPH_PAD        1
#define MAX_INSTANCES    (4u * 1024u * 1024u)
#define CACHE_MIN        1024u
#define CACHE_MAX        (1u << 20)
#define IMG_TEX_CACHE  32
#define IMG_TEX_IDLE   300u   /* frames before an unused image texture is freed */

typedef struct rect_inst {
    float x, y, w, h;
    float r, g, b, a;
} rect_inst;

typedef struct glyph_inst {
    float x, y, w, h;
    float u, v, uw, vh;
    float r, g, b, a;
    float is_color, pad0, pad1, pad2;
} glyph_inst;

typedef struct glyph {
    uint16_t x, y, w, h;
    int16_t  left, top;
    bool     color;
    bool     present;   /* false: known missing or empty (e.g. space) */
} glyph;

typedef struct cache_entry {
    uint32_t key;
    bool     used;
    glyph    g;
} cache_entry;

typedef struct inst_vec {
    void   *data;
    size_t  len, cap, elem;
} inst_vec;

struct te_renderer {
    te_font     *font;
    GLuint       prog_rect, prog_glyph;
    GLint        u_rect_vp, u_glyph_vp, u_glyph_atlas, u_glyph_atlas_size;
    GLuint       vao_rect, vao_glyph;
    GLuint       vbo_quad, vbo_rect, vbo_glyph;
    GLuint       atlas;
    int          atlas_size;
    int          pack_x, pack_y, shelf_h;
    bool         atlas_full;

    cache_entry *cache;
    uint32_t     cache_cap, cache_len;

    inst_vec     bg, glyphs, deco;
    inst_vec     tint;                  /* dim and bell: change colour, never alpha */
    inst_vec     ov_rects, ov_glyphs;   /* overlays (toast) drawn above everything */
    uint16_t    *colstyle;              /* per-column highlight style for the current row */
    size_t       colstyle_cap;

    int          vp_w, vp_h;            /* window size for this frame */
    uint32_t     clear_bg;

    /* Cursor animation (one pane at a time: the focused one). */
    uint32_t     anim_key;
    bool         anim_init;
    float        ax, ay, fx, fy, tx, ty;
    uint64_t     anim_t0;
    bool         animating;

    /* Inline images */
    GLint        max_tex;
    uint64_t     frame_no;
    struct { uint32_t serial; GLuint tex; uint64_t used; } texs[IMG_TEX_CACHE];
    struct { GLuint tex; float x, y, w, h, tw, th; } imgs[TE_MAX_IMAGES];
    size_t       nimgs;
};

#define CURSOR_ANIM_MS 90.0f

/* ------------------------------------------------------------------------- */
/* Instance vectors */

static void vec_init(inst_vec *v, size_t elem)
{
    v->data = nullptr;
    v->len = 0;
    v->cap = 0;
    v->elem = elem;
}

static void vec_free(inst_vec *v)
{
    TE_FREE(v->data);
    v->len = 0;
    v->cap = 0;
}

static void *vec_push(inst_vec *v)
{
    if (v->len == v->cap) {
        if (v->cap >= MAX_INSTANCES) {
            return nullptr;
        }
        size_t ncap = v->cap == 0 ? 4096 : v->cap * 2;
        if (ncap > MAX_INSTANCES) {
            ncap = MAX_INSTANCES;
        }
        if (te_realloc_array(&v->data, v->cap, ncap, v->elem) != TE_OK) {
            return nullptr;
        }
        v->cap = ncap;
    }
    return (unsigned char *)v->data + v->len++ * v->elem;
}

/* ------------------------------------------------------------------------- */
/* Shaders */

static const char *const rect_vs =
    "#version 300 es\n"
    "layout(location=0) in vec2 a_corner;\n"
    "layout(location=1) in vec4 a_rect;\n"
    "layout(location=2) in vec4 a_color;\n"
    "uniform vec2 u_vp;\n"
    "out vec4 v_color;\n"
    "void main() {\n"
    "  vec2 p = a_rect.xy + a_corner * a_rect.zw;\n"
    "  gl_Position = vec4(p.x / u_vp.x * 2.0 - 1.0, 1.0 - p.y / u_vp.y * 2.0, 0.0, 1.0);\n"
    "  v_color = vec4(a_color.rgb * a_color.a, a_color.a);\n"
    "}\n";

static const char *const rect_fs =
    "#version 300 es\n"
    "precision mediump float;\n"
    "in vec4 v_color;\n"
    "out vec4 o;\n"
    "void main() { o = v_color; }\n";

static const char *const glyph_vs =
    "#version 300 es\n"
    "layout(location=0) in vec2 a_corner;\n"
    "layout(location=1) in vec4 a_rect;\n"
    "layout(location=2) in vec4 a_uv;\n"
    "layout(location=3) in vec4 a_color;\n"
    "layout(location=4) in vec4 a_flags;\n"
    "uniform vec2 u_vp;\n"
    "uniform vec2 u_atlas_size;\n"
    "out vec2 v_uv;\n"
    "out vec4 v_color;\n"
    "flat out float v_is_color;\n"
    "void main() {\n"
    "  vec2 p = a_rect.xy + a_corner * a_rect.zw;\n"
    "  gl_Position = vec4(p.x / u_vp.x * 2.0 - 1.0, 1.0 - p.y / u_vp.y * 2.0, 0.0, 1.0);\n"
    "  v_uv = (a_uv.xy + a_corner * a_uv.zw) / u_atlas_size;\n"
    "  v_color = a_color;\n"
    "  v_is_color = a_flags.x;\n"
    "}\n";

static const char *const glyph_fs =
    "#version 300 es\n"
    "precision mediump float;\n"
    "uniform sampler2D u_atlas;\n"
    "in vec2 v_uv;\n"
    "in vec4 v_color;\n"
    "flat in float v_is_color;\n"
    "out vec4 o;\n"
    "void main() {\n"
    "  vec4 t = texture(u_atlas, v_uv);\n"
    "  if (v_is_color > 0.5) { o = t * v_color.a; }\n"
    "  else { o = vec4(v_color.rgb, 1.0) * (t.a * v_color.a); }\n"
    "}\n";

static GLuint compile(GLenum type, const char *src)
{
    GLuint s = glCreateShader(type);
    if (s == 0) {
        return 0;
    }
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    GLint ok = GL_FALSE;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (ok != GL_TRUE) {
        char log[1024] = { 0 };
        GLsizei n = 0;
        glGetShaderInfoLog(s, (GLsizei)sizeof log - 1, &n, log);
        log[sizeof log - 1] = '\0';
        LOG_E("shader compile failed: %s", log);
        glDeleteShader(s);
        return 0;
    }
    return s;
}

static GLuint link_program(const char *vs_src, const char *fs_src)
{
    GLuint vs = compile(GL_VERTEX_SHADER, vs_src);
    GLuint fs = compile(GL_FRAGMENT_SHADER, fs_src);
    GLuint p = 0;
    if (vs == 0 || fs == 0) {
        goto out;
    }
    p = glCreateProgram();
    if (p == 0) {
        goto out;
    }
    glAttachShader(p, vs);
    glAttachShader(p, fs);
    glLinkProgram(p);
    GLint ok = GL_FALSE;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (ok != GL_TRUE) {
        char log[1024] = { 0 };
        GLsizei n = 0;
        glGetProgramInfoLog(p, (GLsizei)sizeof log - 1, &n, log);
        log[sizeof log - 1] = '\0';
        LOG_E("shader link failed: %s", log);
        glDeleteProgram(p);
        p = 0;
    }
out:
    if (vs != 0) glDeleteShader(vs);
    if (fs != 0) glDeleteShader(fs);
    return p;
}

static void attrib(GLuint loc, GLint n, GLsizei stride, size_t offset, GLuint divisor)
{
    glEnableVertexAttribArray(loc);
    glVertexAttribPointer(loc, n, GL_FLOAT, GL_FALSE, stride, (const void *)(uintptr_t)offset);
    glVertexAttribDivisor(loc, divisor);
}

/* ------------------------------------------------------------------------- */
/* Glyph cache and atlas */

static void cache_clear(te_renderer *r)
{
    if (r->cache != nullptr) {
        memset(r->cache, 0, (size_t)r->cache_cap * sizeof *r->cache);
    }
    r->cache_len = 0;
    r->pack_x = 0;
    r->pack_y = 0;
    r->shelf_h = 0;
    r->atlas_full = false;
}

static uint32_t hash32(uint32_t x)
{
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

static cache_entry *cache_find(te_renderer *r, uint32_t key)
{
    uint32_t mask = r->cache_cap - 1u;
    for (uint32_t i = hash32(key) & mask, probes = 0; probes < r->cache_cap; i = (i + 1u) & mask, probes++) {
        cache_entry *e = &r->cache[i];
        if (!e->used || e->key == key) {
            return e;
        }
    }
    return nullptr;
}

[[nodiscard]] static bool cache_grow(te_renderer *r)
{
    if (r->cache_cap >= CACHE_MAX) {
        return false;
    }
    uint32_t ncap = r->cache_cap * 2u;
    cache_entry *n = te_calloc(ncap, sizeof *n);
    if (n == nullptr) {
        return false;
    }
    cache_entry *old = r->cache;
    uint32_t oldcap = r->cache_cap;
    r->cache = n;
    r->cache_cap = ncap;
    for (uint32_t i = 0; i < oldcap; i++) {
        if (old[i].used) {
            cache_entry *e = cache_find(r, old[i].key);
            if (e != nullptr) {
                *e = old[i];
            }
        }
    }
    free(old);
    return true;
}

static bool atlas_alloc(te_renderer *r, int w, int h, int *x, int *y)
{
    if (w + GLYPH_PAD > r->atlas_size || h + GLYPH_PAD > r->atlas_size) {
        return false;
    }
    if (r->pack_x + w + GLYPH_PAD > r->atlas_size) {
        r->pack_x = 0;
        r->pack_y += r->shelf_h + GLYPH_PAD;
        r->shelf_h = 0;
    }
    if (r->pack_y + h + GLYPH_PAD > r->atlas_size) {
        r->atlas_full = true;
        return false;
    }
    *x = r->pack_x;
    *y = r->pack_y;
    r->pack_x += w + GLYPH_PAD;
    if (h > r->shelf_h) {
        r->shelf_h = h;
    }
    return true;
}

/* Look up (or rasterise and upload) a glyph. Returns nullptr if it cannot be drawn now. */
static const glyph *get_glyph(te_renderer *r, uint32_t cp, unsigned style, bool wide)
{
    uint32_t key = (cp & 0x1FFFFFu) | ((uint32_t)(style & 3u) << 21) | (wide ? 1u << 23 : 0u);
    if ((r->cache_len + 1u) * 2u > r->cache_cap && !cache_grow(r)) {
        if (r->cache_len + 1u >= r->cache_cap) {
            r->atlas_full = true;   /* forces a full reset next frame */
            return nullptr;
        }
    }
    cache_entry *e = cache_find(r, key);
    if (e == nullptr) {
        return nullptr;
    }
    if (e->used) {
        return &e->g;
    }
    if (r->atlas_full) {
        return nullptr;
    }
    const te_font_metrics *m = font_metrics(r->font);
    te_raster ras = { 0 };
    glyph g = { 0 };
    te_err rc = font_rasterize(r->font, cp, style, wide ? m->cell_w * 2 : m->cell_w, &ras);
    if (rc == TE_OK && ras.w > 0 && ras.h > 0) {
        int ax = 0;
        int ay = 0;
        if (!atlas_alloc(r, ras.w, ras.h, &ax, &ay)) {
            return nullptr;
        }
        glBindTexture(GL_TEXTURE_2D, r->atlas);
        glTexSubImage2D(GL_TEXTURE_2D, 0, ax, ay, ras.w, ras.h, GL_RGBA, GL_UNSIGNED_BYTE, ras.rgba);
        g.x = (uint16_t)ax;
        g.y = (uint16_t)ay;
        g.w = (uint16_t)ras.w;
        g.h = (uint16_t)ras.h;
        g.left = (int16_t)TE_CLAMP(ras.left, INT16_MIN, INT16_MAX);
        g.top = (int16_t)TE_CLAMP(ras.top, INT16_MIN, INT16_MAX);
        g.color = ras.color;
        g.present = true;
    }
    /* Missing and empty glyphs are cached too, so the lookup is never repeated. */
    e->key = key;
    e->used = true;
    e->g = g;
    r->cache_len++;
    return &e->g;
}

/* ------------------------------------------------------------------------- */
/* Lifecycle */

te_err renderer_create(te_renderer **out, te_font *font)
{
    if (out == nullptr || font == nullptr) {
        return TE_ERR_INVAL;
    }
    *out = nullptr;
    te_renderer *r = te_calloc(1, sizeof *r);
    if (r == nullptr) {
        return TE_ERR_NOMEM;
    }
    te_err rc = TE_ERR_GL;
    r->font = font;
    vec_init(&r->bg, sizeof(rect_inst));
    vec_init(&r->glyphs, sizeof(glyph_inst));
    vec_init(&r->deco, sizeof(rect_inst));
    vec_init(&r->tint, sizeof(rect_inst));
    vec_init(&r->ov_rects, sizeof(rect_inst));
    vec_init(&r->ov_glyphs, sizeof(glyph_inst));

    r->cache_cap = CACHE_MIN;
    r->cache = te_calloc(r->cache_cap, sizeof *r->cache);
    if (r->cache == nullptr) {
        rc = TE_ERR_NOMEM;
        goto fail;
    }

    r->prog_rect = link_program(rect_vs, rect_fs);
    r->prog_glyph = link_program(glyph_vs, glyph_fs);
    if (r->prog_rect == 0 || r->prog_glyph == 0) {
        goto fail;
    }
    r->u_rect_vp = glGetUniformLocation(r->prog_rect, "u_vp");
    r->u_glyph_vp = glGetUniformLocation(r->prog_glyph, "u_vp");
    r->u_glyph_atlas = glGetUniformLocation(r->prog_glyph, "u_atlas");
    r->u_glyph_atlas_size = glGetUniformLocation(r->prog_glyph, "u_atlas_size");

    static const float quad[8] = { 0, 0, 1, 0, 0, 1, 1, 1 };
    glGenBuffers(1, &r->vbo_quad);
    glGenBuffers(1, &r->vbo_rect);
    glGenBuffers(1, &r->vbo_glyph);
    glGenVertexArrays(1, &r->vao_rect);
    glGenVertexArrays(1, &r->vao_glyph);
    if (r->vbo_quad == 0 || r->vbo_rect == 0 || r->vbo_glyph == 0 || r->vao_rect == 0 || r->vao_glyph == 0) {
        goto fail;
    }
    glBindBuffer(GL_ARRAY_BUFFER, r->vbo_quad);
    glBufferData(GL_ARRAY_BUFFER, sizeof quad, quad, GL_STATIC_DRAW);

    glBindVertexArray(r->vao_rect);
    glBindBuffer(GL_ARRAY_BUFFER, r->vbo_quad);
    attrib(0, 2, 2 * sizeof(float), 0, 0);
    glBindBuffer(GL_ARRAY_BUFFER, r->vbo_rect);
    attrib(1, 4, sizeof(rect_inst), offsetof(rect_inst, x), 1);
    attrib(2, 4, sizeof(rect_inst), offsetof(rect_inst, r), 1);

    glBindVertexArray(r->vao_glyph);
    glBindBuffer(GL_ARRAY_BUFFER, r->vbo_quad);
    attrib(0, 2, 2 * sizeof(float), 0, 0);
    glBindBuffer(GL_ARRAY_BUFFER, r->vbo_glyph);
    attrib(1, 4, sizeof(glyph_inst), offsetof(glyph_inst, x), 1);
    attrib(2, 4, sizeof(glyph_inst), offsetof(glyph_inst, u), 1);
    attrib(3, 4, sizeof(glyph_inst), offsetof(glyph_inst, r), 1);
    attrib(4, 4, sizeof(glyph_inst), offsetof(glyph_inst, is_color), 1);
    glBindVertexArray(0);

    GLint maxtex = 0;
    glGetIntegerv(GL_MAX_TEXTURE_SIZE, &maxtex);
    r->atlas_size = maxtex > 0 && maxtex < ATLAS_MAX ? maxtex : ATLAS_MAX;
    r->max_tex = maxtex > 0 ? maxtex : 2048;
    glGenTextures(1, &r->atlas);
    if (r->atlas == 0) {
        goto fail;
    }
    glBindTexture(GL_TEXTURE_2D, r->atlas);
    glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, r->atlas_size, r->atlas_size);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    if (glGetError() != GL_NO_ERROR) {
        LOG_E("OpenGL error while creating the renderer");
        goto fail;
    }
    *out = r;
    return TE_OK;

fail:
    renderer_destroy(&r);
    return rc;
}

void renderer_destroy(te_renderer **rp)
{
    if (rp == nullptr || *rp == nullptr) {
        return;
    }
    te_renderer *r = *rp;
    if (r->atlas != 0) glDeleteTextures(1, &r->atlas);
    for (size_t i = 0; i < IMG_TEX_CACHE; i++) {
        if (r->texs[i].tex != 0) glDeleteTextures(1, &r->texs[i].tex);
        r->texs[i].tex = 0;
    }
    if (r->vao_rect != 0) glDeleteVertexArrays(1, &r->vao_rect);
    if (r->vao_glyph != 0) glDeleteVertexArrays(1, &r->vao_glyph);
    if (r->vbo_quad != 0) glDeleteBuffers(1, &r->vbo_quad);
    if (r->vbo_rect != 0) glDeleteBuffers(1, &r->vbo_rect);
    if (r->vbo_glyph != 0) glDeleteBuffers(1, &r->vbo_glyph);
    if (r->prog_rect != 0) glDeleteProgram(r->prog_rect);
    if (r->prog_glyph != 0) glDeleteProgram(r->prog_glyph);
    vec_free(&r->bg);
    vec_free(&r->glyphs);
    vec_free(&r->deco);
    vec_free(&r->tint);
    vec_free(&r->ov_rects);
    vec_free(&r->ov_glyphs);
    TE_FREE(r->colstyle);
    r->colstyle_cap = 0;
    TE_FREE(r->cache);
    TE_FREE(*rp);
}

void renderer_set_font(te_renderer *r, te_font *font)
{
    if (r == nullptr || font == nullptr) {
        return;
    }
    r->font = font;
    cache_clear(r);
}

/* ------------------------------------------------------------------------- */
/* Frame building */

static void push_rect(inst_vec *v, float x, float y, float w, float h, uint32_t rgb, float a)
{
    rect_inst *ri = vec_push(v);
    if (ri == nullptr) {
        return;
    }
    ri->x = x;
    ri->y = y;
    ri->w = w;
    ri->h = h;
    ri->r = (float)((rgb >> 16) & 0xFFu) / 255.0f;
    ri->g = (float)((rgb >> 8) & 0xFFu) / 255.0f;
    ri->b = (float)(rgb & 0xFFu) / 255.0f;
    ri->a = a;
}

static void push_glyph(inst_vec *v, const glyph *g, float x, float y, uint32_t rgb, float a)
{
    glyph_inst *gi = vec_push(v);
    if (gi == nullptr) {
        return;
    }
    gi->x = x;
    gi->y = y;
    gi->w = (float)g->w;
    gi->h = (float)g->h;
    gi->u = (float)g->x;
    gi->v = (float)g->y;
    gi->uw = (float)g->w;
    gi->vh = (float)g->h;
    gi->r = (float)((rgb >> 16) & 0xFFu) / 255.0f;
    gi->g = (float)((rgb >> 8) & 0xFFu) / 255.0f;
    gi->b = (float)(rgb & 0xFFu) / 255.0f;
    gi->a = a;
    gi->is_color = g->color ? 1.0f : 0.0f;
    gi->pad0 = gi->pad1 = gi->pad2 = 0.0f;
}

static uint32_t mix(uint32_t a, uint32_t b, float t)
{
    uint32_t out = 0;
    for (int s = 0; s <= 16; s += 8) {
        float ca = (float)((a >> s) & 0xFFu);
        float cb = (float)((b >> s) & 0xFFu);
        uint32_t c = (uint32_t)lroundf(ca + (cb - ca) * t);
        out |= (c > 255u ? 255u : c) << s;
    }
    return out;
}

typedef struct box_ctx {
    te_renderer *r;
    uint32_t     color;
} box_ctx;

static void box_rect(void *user, float x, float y, float w, float h, float alpha)
{
    box_ctx *b = user;
    push_rect(&b->r->deco, x, y, w, h, b->color, alpha);
}

static void underline(te_renderer *r, te_ul_style st, float x, float y, float w, float thick, uint32_t col)
{
    switch (st) {
    case TE_UL_SINGLE:
        push_rect(&r->deco, x, y, w, thick, col, 1.0f);
        break;
    case TE_UL_DOUBLE:
        push_rect(&r->deco, x, y - thick, w, thick, col, 1.0f);
        push_rect(&r->deco, x, y + thick, w, thick, col, 1.0f);
        break;
    case TE_UL_CURLY: {
        int n = (int)w;
        float amp = thick < 1.5f ? 1.5f : thick;
        for (int i = 0; i < n; i++) {
            float off = sinf((float)i / w * 6.2831853f) * amp;
            push_rect(&r->deco, x + (float)i, y + off, 1.0f, thick, col, 1.0f);
        }
        break;
    }
    case TE_UL_DOTTED: {
        float step = thick * 2.0f;
        for (float px = 0.0f; px < w; px += step) {
            push_rect(&r->deco, x + px, y, thick, thick, col, 1.0f);
        }
        break;
    }
    case TE_UL_DASHED: {
        float seg = w / 3.0f;
        push_rect(&r->deco, x, y, seg * 0.7f, thick, col, 1.0f);
        push_rect(&r->deco, x + seg * 1.5f, y, seg * 0.7f, thick, col, 1.0f);
        break;
    }
    case TE_UL_NONE:
    default:
        break;
    }
}

static te_cursor_shape effective_shape(const te_modes *m, te_cursor_shape def)
{
    switch (m->decscusr) {
    case 1:
    case 2: return TE_CURSOR_BLOCK;
    case 3:
    case 4: return TE_CURSOR_UNDERLINE;
    case 5:
    case 6: return TE_CURSOR_BEAM;
    default: return def;
    }
}

typedef enum { PILL_TOP_RIGHT, PILL_CENTER, PILL_BOTTOM_BAR } pill_pos;

/* A one-line message on a filled background: toast (top right), modal (centre) or search bar. */
static void draw_pill(te_renderer *r, uint32_t pfg, uint32_t pbg, int area_w, int area_h, const char *text,
                      pill_pos pos, float cw, float ch, float pad)
{
    enum { TOAST_MAX = 160 };
    uint32_t cps[TOAST_MAX] = { 0 };
    size_t n = 0;
    te_utf8_dec d = { 0 };
    for (const char *p = text; *p != '\0' && n < TOAST_MAX; p++) {
        uint32_t cp = 0;
        if (utf8_feed(&d, (uint8_t)*p, &cp) == TE_UTF8_DONE && cp >= 0x20) {
            cps[n++] = cp;
        }
    }
    if (n == 0) {
        return;
    }
    const te_font_metrics *m = font_metrics(r->font);
    float w = ((float)n + 2.0f) * cw;
    float h = ch * 1.6f;
    float x = (float)area_w - w - pad;
    float y = pad;
    if (pos == PILL_CENTER) {
        x = ((float)area_w - w) / 2.0f;
        y = ((float)area_h - h) / 2.0f;
    } else if (pos == PILL_BOTTOM_BAR) {
        x = 0.0f;
        y = (float)area_h - h;
        w = (float)area_w;
    }
    if (x < 0.0f) {
        x = 0.0f;
    }
    push_rect(&r->ov_rects, x, y, w, h, pbg, 1.0f);   /* UI stays solid on a translucent window */
    if (pos == PILL_BOTTOM_BAR) {
        push_rect(&r->ov_rects, x, y, w, 2.0f, pfg, 0.35f);
    } else {
        push_rect(&r->ov_rects, x, y + h - 2.0f, w, 2.0f, pfg, 0.35f);
    }
    float tx = x + cw;
    float ty = y + (h - ch) / 2.0f;
    for (size_t i = 0; i < n; i++) {
        const glyph *g = get_glyph(r, cps[i], TE_FONT_REGULAR, false);
        if (g != nullptr && g->present) {
            push_glyph(&r->ov_glyphs, g, tx + (float)i * cw + (float)g->left, ty + (float)m->baseline - (float)g->top,
                       pfg, 1.0f);
        }
    }
}

/* GL texture for an image, uploading it on first use (least recently used slot is recycled). */
static GLuint image_texture(te_renderer *r, const te_image *im)
{
    size_t slot = 0;
    uint64_t oldest = UINT64_MAX;
    for (size_t i = 0; i < IMG_TEX_CACHE; i++) {
        if (r->texs[i].tex != 0 && r->texs[i].serial == im->serial) {
            r->texs[i].used = r->frame_no;
            return r->texs[i].tex;
        }
        uint64_t age = r->texs[i].tex == 0 ? 0 : r->texs[i].used;
        if (age < oldest) {
            oldest = age;
            slot = i;
        }
    }
    if (im->w > (uint32_t)r->max_tex || im->h > (uint32_t)r->max_tex || im->rgba == nullptr) {
        return 0;   /* larger than the GPU allows: not drawn */
    }
    if (r->texs[slot].tex == 0) {
        glGenTextures(1, &r->texs[slot].tex);
        if (r->texs[slot].tex == 0) {
            return 0;
        }
    }
    glBindTexture(GL_TEXTURE_2D, r->texs[slot].tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, (GLsizei)im->w, (GLsizei)im->h, 0, GL_RGBA, GL_UNSIGNED_BYTE, im->rgba);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    r->texs[slot].serial = im->serial;
    r->texs[slot].used = r->frame_no;
    return r->texs[slot].tex;
}

/* Queue the visible images of this pane (drawn between backgrounds and text). */
static void queue_images(te_renderer *r, const te_term *t, float pad, float cw, float ch)
{
    r->nimgs = 0;
    size_t n = term_image_count(t);
    if (n == 0) {
        return;
    }
    bool alt = term_modes(t)->alt_screen;
    int64_t top = (int64_t)term_view_abs(t, 0);
    int64_t rows = term_rows(t);
    for (size_t i = 0; i < n && r->nimgs < TE_ARRAY_LEN(r->imgs); i++) {
        const te_image *im = term_image_at(t, i);
        if (im == nullptr || !im->placed || im->alt != alt) {
            continue;
        }
        int64_t vr = (int64_t)im->row - top;
        if (vr + im->rows <= 0 || vr >= rows) {
            continue;
        }
        GLuint tex = image_texture(r, im);
        if (tex == 0) {
            continue;
        }
        /* Natural size, scaled down (keeping the aspect ratio) to fit its cell box. */
        float bw = (float)im->cols * cw;
        float bh = (float)im->rows * ch;
        float sx = bw / (float)im->w;
        float sy = bh / (float)im->h;
        float s = TE_MIN(sx, sy);
        if (s > 1.0f) s = 1.0f;
        r->imgs[r->nimgs].tex = tex;
        r->imgs[r->nimgs].x = pad + (float)im->col * cw;
        r->imgs[r->nimgs].y = pad + (float)vr * ch;
        r->imgs[r->nimgs].w = (float)im->w * s;
        r->imgs[r->nimgs].h = (float)im->h * s;
        r->imgs[r->nimgs].tw = (float)im->w;
        r->imgs[r->nimgs].th = (float)im->h;
        r->nimgs++;
    }
}

static void build(te_renderer *r, const te_term *t, const te_render_opts *o, const te_frame *f)
{
    const te_font_metrics *m = font_metrics(r->font);
    const te_modes *modes = term_modes(t);
    float cw = (float)m->cell_w;
    float ch = (float)m->cell_h;
    float pad = (float)o->padding;
    float thick = (float)m->underline_thick;
    uint16_t rows = term_rows(t);
    uint16_t cols = term_cols(t);
    uint16_t cx = 0;
    uint16_t cy = 0;
    term_cursor(t, &cx, &cy);
    bool show_cursor = modes->cursor_visible && term_view_offset(t) == 0;
    te_cursor_shape shape = effective_shape(modes, o->cursor_shape);
    bool block_cursor = show_cursor && f->focused && f->cursor_on && shape == TE_CURSOR_BLOCK;
    uint32_t dfg = term_default_fg(t);

    /* Smooth cursor movement: ease from the previous position to the new cell. */
    float ctx = pad + (float)cx * cw;
    float cty = pad + (float)cy * ch;
    float cdx = ctx;
    float cdy = cty;
    bool cursor_moving = false;
    if (f->animate_cursor && show_cursor) {
        if (!r->anim_init || r->anim_key != f->anim_key) {
            r->anim_init = true;
            r->anim_key = f->anim_key;
            r->ax = r->fx = r->tx = ctx;
            r->ay = r->fy = r->ty = cty;
        } else if (ctx != r->tx || cty != r->ty) {
            r->fx = r->ax;
            r->fy = r->ay;
            r->tx = ctx;
            r->ty = cty;
            r->anim_t0 = f->now_ms;
        }
        float p = (float)(f->now_ms - r->anim_t0) / CURSOR_ANIM_MS;
        if (p < 1.0f && (r->fx != r->tx || r->fy != r->ty)) {
            float e = 1.0f - (1.0f - p) * (1.0f - p) * (1.0f - p);
            r->ax = r->fx + (r->tx - r->fx) * e;
            r->ay = r->fy + (r->ty - r->fy) * e;
            cursor_moving = true;
            r->animating = true;
        } else {
            r->ax = r->fx = r->tx;
            r->ay = r->fy = r->ty;
        }
        cdx = r->ax;
        cdy = r->ay;
    }
    bool use_hl = o->hl != nullptr && (!modes->alt_screen || o->hl_alt_screen);
    queue_images(r, t, pad, cw, ch);

    /* Per-column highlight styles; disabled if the scratch buffer cannot be allocated. */
    if (use_hl && r->colstyle_cap < cols) {
        void *p = r->colstyle;
        if (te_realloc_array(&p, r->colstyle_cap, cols, sizeof(uint16_t)) == TE_OK) {
            r->colstyle = p;
            r->colstyle_cap = cols;
        } else {
            use_hl = false;
        }
    }

    for (uint16_t y = 0; y < rows; y++) {
        const te_line *line = term_view_line(t, y);
        if (line == nullptr || line->cells == nullptr) {
            continue;
        }
        uint16_t n = line->cols < cols ? line->cols : cols;
        float py = pad + (float)y * ch;
        uint64_t abs = term_view_abs(t, y);
        bool row_hl = false;
        if (use_hl) {
            const te_hl_span *spans = nullptr;
            size_t ns = hl_line(o->hl, line->cells, n, &spans);
            if (ns > 0) {
                row_hl = true;
                memset(r->colstyle, 0, (size_t)n * sizeof r->colstyle[0]);
                for (size_t i = 0; i < ns; i++) {
                    for (uint16_t x = spans[i].start; x < spans[i].end && x < n; x++) {
                        r->colstyle[x] = spans[i].style;
                    }
                }
            }
        }
        for (uint16_t x = 0; x < n; x++) {
            const te_cell *c = &line->cells[x];
            bool wide = (c->attrs & TE_ATTR_WIDE) != 0 && x + 1 < n;
            bool spacer = (c->attrs & TE_ATTR_SPACER) != 0;
            float px = pad + (float)x * cw;
            float w = wide ? cw * 2.0f : cw;

            /* Highlight overlay: applies only where the program left default colours,
             * unless the rule says override. */
            uint16_t attrs = c->attrs;
            uint32_t fgc = c->fg;
            uint32_t bgc = c->bg;
            const te_hl_style *hs = row_hl && r->colstyle[x] != 0 ? hl_style(o->hl, r->colstyle[x]) : nullptr;
            if (hs != nullptr) {
                bool plain = TE_COLOR_KIND(c->fg) == 0 && (c->attrs & TE_ATTR_INVERSE) == 0;
                if (hs->fg != TE_COLOR_DEFAULT && (plain || hs->override)) {
                    fgc = hs->fg;
                }
                if (hs->bg != TE_COLOR_DEFAULT && (TE_COLOR_KIND(c->bg) == 0 || hs->override)) {
                    bgc = hs->bg;
                }
                if (plain || hs->override) {
                    attrs |= hs->attrs;
                    if ((hs->attrs & TE_ATTR_UL_MASK) != 0) {
                        attrs = (uint16_t)(((unsigned)attrs & ~(unsigned)TE_ATTR_UL_MASK) |
                                           ((unsigned)hs->attrs & (unsigned)TE_ATTR_UL_MASK));
                    }
                }
            }
            if (o->bold_is_bright && (attrs & TE_ATTR_BOLD) != 0 && TE_COLOR_KIND(fgc) == 1 && (fgc & 0xFFu) < 8u) {
                fgc = TE_COLOR_PAL((fgc & 0xFFu) + 8u);
            }
            uint32_t fg = term_resolve_color(t, fgc, true);
            uint32_t bg = term_resolve_color(t, bgc, false);
            bool bg_default = TE_COLOR_KIND(bgc) == 0;
            bool inv = ((attrs & TE_ATTR_INVERSE) != 0) != modes->reverse_video;
            if (inv) {
                uint32_t tmp = fg;
                fg = bg;
                bg = tmp;
                bg_default = false;
            }
            if ((attrs & TE_ATTR_DIM) != 0) {
                fg = mix(fg, bg, 0.45f);
            }
            /* Search matches, then the selection, take over the cell colours. */
            bool ui_bg = false;   /* search, selection and cursor stay solid */
            bool cur_match = false;
            if (o->search != nullptr && search_cell(o->search, abs, x, &cur_match)) {
                bg = cur_match ? o->search_cur_bg : o->search_bg;
                fg = o->search_fg;
                bg_default = false;
                ui_bg = true;
            } else if (o->sel != nullptr && sel_contains(o->sel, abs, x)) {
                bg = o->sel_bg;
                fg = o->sel_fg;
                bg_default = false;
                ui_bg = true;
            }
            if (o->hover != nullptr && url_hit_contains(o->hover, abs, x)) {
                attrs = (uint16_t)(((unsigned)attrs & ~(unsigned)TE_ATTR_UL_MASK) | ((unsigned)TE_UL_SINGLE << TE_ATTR_UL_SHIFT));
            }
            bool at_cursor = y == cy && (x == cx || (wide && x + 1 == cx));
            if (at_cursor && block_cursor && !cursor_moving) {
                bg = term_cursor_color(t);
                fg = o->cursor_text;
                bg_default = false;
                ui_bg = true;
            }
            float bg_alpha = ui_bg || !o->transparent_cells ? 1.0f : TE_CLAMP(o->opacity, 0.0f, 1.0f);
            if (!bg_default && !spacer) {
                push_rect(&r->bg, px, py, w, ch, bg, bg_alpha);
            } else if (!bg_default && spacer && (x == 0 || (line->cells[x - 1].attrs & TE_ATTR_WIDE) == 0)) {
                push_rect(&r->bg, px, py, cw, ch, bg, bg_alpha);
            }
            if (spacer || (attrs & TE_ATTR_HIDDEN) != 0 || c->cp == 0) {
                continue;
            }
            unsigned style = ((attrs & TE_ATTR_BOLD) != 0 ? (unsigned)TE_FONT_BOLD : 0u) |
                             ((attrs & TE_ATTR_ITALIC) != 0 ? (unsigned)TE_FONT_ITALIC : 0u);
            if (boxdraw_handles(c->cp)) {
                box_ctx bc = { .r = r, .color = fg };
                (void)boxdraw(c->cp, px, py, cw, ch, thick, box_rect, &bc);
            } else if (c->cp != ' ') {
                const glyph *g = get_glyph(r, c->cp, style, wide);
                if (g != nullptr && g->present) {
                    push_glyph(&r->glyphs, g, px + (float)g->left, py + (float)m->baseline - (float)g->top, fg, 1.0f);
                }
            }
            for (size_t k = 0; k < TE_CELL_MAX_COMBINING && c->comb[k] != 0; k++) {
                const glyph *g = get_glyph(r, c->comb[k], style, wide);
                if (g != nullptr && g->present) {
                    push_glyph(&r->glyphs, g, px + (float)g->left, py + (float)m->baseline - (float)g->top, fg, 1.0f);
                }
            }
            te_ul_style ul = (te_ul_style)((attrs & TE_ATTR_UL_MASK) >> TE_ATTR_UL_SHIFT);
            if (ul != TE_UL_NONE) {
                uint32_t ulc = TE_COLOR_KIND(c->ul) == 0 ? fg : term_resolve_color(t, c->ul, true);
                underline(r, ul, px, py + (float)m->underline_pos, w, thick, ulc);
            }
            if ((attrs & TE_ATTR_STRIKE) != 0) {
                push_rect(&r->deco, px, py + (float)m->strike_pos, w, thick, fg, 1.0f);
            }
            if ((attrs & TE_ATTR_OVERLINE) != 0) {
                push_rect(&r->deco, px, py, w, thick, fg, 1.0f);
            }
        }
    }

    /* Non-block cursors, and a hollow block when the window is unfocused. */
    if (show_cursor && cy < rows && cx < cols) {
        float px = cdx;
        float py = cdy;
        if (cursor_moving && block_cursor) {
            push_rect(&r->bg, px, py, cw, ch, term_cursor_color(t), 0.85f);   /* in flight */
        }
        uint32_t cc = term_cursor_color(t);
        float bw = f->scale > 1.0f ? floorf(1.5f * f->scale) : 1.5f;
        if (!f->focused) {
            push_rect(&r->deco, px, py, cw, 1.0f, cc, 1.0f);
            push_rect(&r->deco, px, py + ch - 1.0f, cw, 1.0f, cc, 1.0f);
            push_rect(&r->deco, px, py, 1.0f, ch, cc, 1.0f);
            push_rect(&r->deco, px + cw - 1.0f, py, 1.0f, ch, cc, 1.0f);
        } else if (f->cursor_on && shape == TE_CURSOR_BEAM) {
            push_rect(&r->deco, px, py, bw < 1.0f ? 1.0f : bw, ch, cc, 1.0f);
        } else if (f->cursor_on && shape == TE_CURSOR_UNDERLINE) {
            push_rect(&r->deco, px, py + ch - thick * 2.0f, cw, thick * 2.0f, cc, 1.0f);
        }
    }

    /* Scroll position indicator while viewing history. */
    uint32_t off = term_view_offset(t);
    if (off > 0) {
        float total = (float)term_scrollback_count(t) + (float)rows;
        float track = (float)f->height;
        float thumb = track * (float)rows / total;
        if (thumb < ch) thumb = ch;
        float top = (track - thumb) * (float)(term_scrollback_count(t) - off) / (float)term_scrollback_count(t);
        float sw = 4.0f * (f->scale > 0.0f ? f->scale : 1.0f);
        push_rect(&r->deco, (float)f->width - sw - 2.0f, top, sw, thumb, dfg, 0.35f);
    }
    if (f->bell > 0.0f) {
        push_rect(&r->tint, 0.0f, 0.0f, (float)f->width, (float)f->height, dfg, 0.12f * f->bell);
    }
    if (f->dim > 0.0f) {
        push_rect(&r->tint, 0.0f, 0.0f, (float)f->width, (float)f->height, term_default_bg(t),
                  TE_CLAMP(f->dim, 0.0f, 0.9f));
    }
    if (f->bar != nullptr) {
        draw_pill(r, o->toast_fg, o->toast_bg, f->width, f->height, f->bar, PILL_BOTTOM_BAR, cw, ch, pad);
    }
    if (f->modal != nullptr && f->modal[0] != '\0') {
        /* Dim the terminal so the question stands out. */
        push_rect(&r->tint, 0.0f, 0.0f, (float)f->width, (float)f->height, term_default_bg(t), 0.6f);
        draw_pill(r, o->toast_fg, o->toast_bg, f->width, f->height, f->modal, PILL_CENTER, cw, ch, pad);
    } else if (f->toast != nullptr && f->toast[0] != '\0') {
        draw_pill(r, o->toast_fg, o->toast_bg, f->width, f->height, f->toast, PILL_TOP_RIGHT, cw, ch, pad);
    }
}

/* Move every instance built for a pane to the pane's place in the window. */
static void translate(inst_vec *v, bool glyphs, float dx, float dy)
{
    for (size_t i = 0; i < v->len; i++) {
        if (glyphs) {
            glyph_inst *g = (glyph_inst *)v->data + i;
            g->x += dx;
            g->y += dy;
        } else {
            rect_inst *q = (rect_inst *)v->data + i;
            q->x += dx;
            q->y += dy;
        }
    }
}

static void draw_rects(te_renderer *r, const inst_vec *v, const te_frame *f)
{
    if (v->len == 0) {
        return;
    }
    (void)f;
    glUseProgram(r->prog_rect);
    glUniform2f(r->u_rect_vp, (float)r->vp_w, (float)r->vp_h);
    glBindVertexArray(r->vao_rect);
    glBindBuffer(GL_ARRAY_BUFFER, r->vbo_rect);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(v->len * v->elem), v->data, GL_STREAM_DRAW);
    glDrawArraysInstanced(GL_TRIANGLE_STRIP, 0, 4, (GLsizei)v->len);
}

static void draw_glyphs(te_renderer *r, const inst_vec *v, const te_frame *f)
{
    if (v->len == 0) {
        return;
    }
    (void)f;
    glUseProgram(r->prog_glyph);
    glUniform2f(r->u_glyph_vp, (float)r->vp_w, (float)r->vp_h);
    glUniform2f(r->u_glyph_atlas_size, (float)r->atlas_size, (float)r->atlas_size);
    glUniform1i(r->u_glyph_atlas, 0);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, r->atlas);
    glBindVertexArray(r->vao_glyph);
    glBindBuffer(GL_ARRAY_BUFFER, r->vbo_glyph);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(v->len * v->elem), v->data, GL_STREAM_DRAW);
    glDrawArraysInstanced(GL_TRIANGLE_STRIP, 0, 4, (GLsizei)v->len);
}

static void clear_vectors(te_renderer *r)
{
    r->bg.len = 0;
    r->glyphs.len = 0;
    r->deco.len = 0;
    r->tint.len = 0;
    r->ov_rects.len = 0;
    r->ov_glyphs.len = 0;
}

static void draw_images(te_renderer *r)
{
    if (r->nimgs == 0) {
        return;
    }
    glUseProgram(r->prog_glyph);
    glUniform2f(r->u_glyph_vp, (float)r->vp_w, (float)r->vp_h);
    glUniform1i(r->u_glyph_atlas, 0);
    glActiveTexture(GL_TEXTURE0);
    glBindVertexArray(r->vao_glyph);
    glBindBuffer(GL_ARRAY_BUFFER, r->vbo_glyph);
    for (size_t i = 0; i < r->nimgs; i++) {
        glyph_inst gi = {
            .x = r->imgs[i].x, .y = r->imgs[i].y, .w = r->imgs[i].w, .h = r->imgs[i].h,
            .u = 0.0f, .v = 0.0f, .uw = r->imgs[i].tw, .vh = r->imgs[i].th,
            .r = 1.0f, .g = 1.0f, .b = 1.0f, .a = 1.0f, .is_color = 1.0f,
        };
        glUniform2f(r->u_glyph_atlas_size, r->imgs[i].tw, r->imgs[i].th);
        glBindTexture(GL_TEXTURE_2D, r->imgs[i].tex);
        glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)sizeof gi, &gi, GL_STREAM_DRAW);
        glDrawArraysInstanced(GL_TRIANGLE_STRIP, 0, 4, 1);
    }
    r->nimgs = 0;
}

static void flush_vectors(te_renderer *r, const te_frame *f)
{
    draw_rects(r, &r->bg, f);
    draw_images(r);
    draw_glyphs(r, &r->glyphs, f);
    draw_rects(r, &r->deco, f);
    if (r->tint.len > 0) {
        /* Keep the destination alpha, so dimming a translucent pane does not make
         * it more opaque, and scale the tint by that alpha to stay premultiplied. */
        glBlendFuncSeparate(GL_DST_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ZERO, GL_ONE);
        draw_rects(r, &r->tint, f);
        glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    }
    draw_rects(r, &r->ov_rects, f);
    draw_glyphs(r, &r->ov_glyphs, f);
}

void renderer_begin(te_renderer *r, int win_w, int win_h, uint32_t bg, float opacity)
{
    if (r == nullptr) {
        return;
    }
    r->vp_w = win_w > 0 ? win_w : 1;
    r->vp_h = win_h > 0 ? win_h : 1;
    r->clear_bg = bg;
    r->animating = false;
    r->frame_no++;
    /* Free image textures nobody has drawn for a while. */
    for (size_t i = 0; i < IMG_TEX_CACHE; i++) {
        if (r->texs[i].tex != 0 && r->frame_no - r->texs[i].used > IMG_TEX_IDLE) {
            glDeleteTextures(1, &r->texs[i].tex);
            r->texs[i].tex = 0;
        }
    }
    if (r->atlas_full) {
        LOG_D("glyph atlas full; rebuilding");
        cache_clear(r);
    }
    float op = TE_CLAMP(opacity, 0.0f, 1.0f);
    glViewport(0, 0, r->vp_w, r->vp_h);
    glDisable(GL_SCISSOR_TEST);
    glClearColor((float)((bg >> 16) & 0xFFu) / 255.0f * op, (float)((bg >> 8) & 0xFFu) / 255.0f * op,
                 (float)(bg & 0xFFu) / 255.0f * op, op);
    glClear(GL_COLOR_BUFFER_BIT);
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
}

te_err renderer_draw(te_renderer *r, const te_term *t, const te_render_opts *o, const te_frame *f)
{
    if (r == nullptr || t == nullptr || o == nullptr || f == nullptr || f->width <= 0 || f->height <= 0) {
        return TE_ERR_INVAL;
    }
    clear_vectors(r);
    /* A pane whose background differs from the window's (OSC 11, reverse video) paints its own. */
    uint32_t bg = term_modes(t)->reverse_video ? term_default_fg(t) : term_default_bg(t);
    if (bg != r->clear_bg) {
        push_rect(&r->bg, 0.0f, 0.0f, (float)f->width, (float)f->height, bg, TE_CLAMP(o->opacity, 0.0f, 1.0f));
    }
    build(r, t, o, f);
    float dx = (float)f->x;
    float dy = (float)f->y;
    translate(&r->bg, false, dx, dy);
    translate(&r->glyphs, true, dx, dy);
    translate(&r->deco, false, dx, dy);
    translate(&r->tint, false, dx, dy);
    translate(&r->ov_rects, false, dx, dy);
    translate(&r->ov_glyphs, true, dx, dy);
    for (size_t i = 0; i < r->nimgs; i++) {
        r->imgs[i].x += dx;
        r->imgs[i].y += dy;
    }

    glEnable(GL_SCISSOR_TEST);
    glScissor(f->x, r->vp_h - f->y - f->height, f->width, f->height);   /* GL origin is bottom-left */
    flush_vectors(r, f);
    glDisable(GL_SCISSOR_TEST);
    return TE_OK;
}

/* Truncate a UTF-8 title to at most max code points into cps; returns the count. */
static size_t title_cps(const char *s, uint32_t *cps, size_t max)
{
    size_t n = 0;
    te_utf8_dec d = { 0 };
    for (; s != nullptr && *s != '\0' && n < max; s++) {
        uint32_t cp = 0;
        if (utf8_feed(&d, (uint8_t)*s, &cp) == TE_UTF8_DONE && cp >= 0x20) {
            cps[n++] = cp;
        }
    }
    return n;
}

/* Tab geometry shared by drawing and hit-testing. */
static float tab_width(const te_renderer *r, const te_chrome *c)
{
    const te_font_metrics *m = font_metrics(r->font);
    float maxw = (float)m->cell_w * 32.0f;
    float aw = (float)(c->area_w > 0 ? c->area_w : c->win_w);
    float w = c->ntabs > 0 ? aw / (float)c->ntabs : aw;
    return w > maxw ? maxw : w;
}

int renderer_tab_at(const te_renderer *r, const te_chrome *c, int x, int y)
{
    if (r == nullptr || c == nullptr || c->tab_bar_h <= 0 || y < 0 || y >= c->tab_bar_h || x < c->area_x) {
        return -1;
    }
    float tw = tab_width(r, c);
    size_t i = (size_t)((float)(x - c->area_x) / tw);
    return i < c->ntabs ? (int)i : -1;
}

void renderer_chrome(te_renderer *r, const te_chrome *c)
{
    if (r == nullptr || c == nullptr) {
        return;
    }
    clear_vectors(r);
    r->nimgs = 0;
    const te_font_metrics *m = font_metrics(r->font);
    float cw = (float)m->cell_w;
    float ch = (float)m->cell_h;
    float pad = 8.0f * (c->scale > 0.0f ? c->scale : 1.0f);

    if (c->tab_bar_h > 0 && c->ntabs > 0) {
        float bh = (float)c->tab_bar_h;
        float tw = tab_width(r, c);
        float ax = (float)c->area_x;
        float aw = (float)(c->area_w > 0 ? c->area_w : c->win_w);
        push_rect(&r->bg, ax, 0.0f, aw, bh, c->bar_bg, 1.0f);
        for (size_t i = 0; i < c->ntabs; i++) {
            bool active = i == c->active_tab;
            float x = ax + (float)i * tw;
            if (active) {
                push_rect(&r->bg, x, 0.0f, tw, bh, c->active_bg, 1.0f);
                push_rect(&r->deco, x, bh - 2.0f, tw, 2.0f, c->accent, 1.0f);
            } else if (i > 0 && i - 1 != c->active_tab) {
                push_rect(&r->deco, x, bh * 0.25f, 1.0f, bh * 0.5f, c->bar_fg, 0.25f);
            }
            /* A session's colour tag: a stripe along the top of its tab. */
            if (c->tab_colors != nullptr && c->tab_colors[i] != 0) {
                float sh = TE_MAX(3.0f, bh * 0.1f);
                push_rect(&r->deco, x, 0.0f, tw, sh, c->tab_colors[i], 1.0f);
            }
            /* "N title", truncated with an ellipsis to fit. */
            uint32_t cps[160] = { 0 };
            char label[16] = { 0 };
            (void)str_fmt(label, sizeof label, "%zu ", i + 1);
            size_t n = title_cps(label, cps, TE_ARRAY_LEN(cps));
            const char *title = c->tab_titles != nullptr ? c->tab_titles[i] : "";
            n += title_cps(title, cps + n, TE_ARRAY_LEN(cps) - n);
            size_t fit = (size_t)((tw - cw * 2.0f) / cw);
            if (n > fit && fit > 1) {
                n = fit;
                cps[n - 1] = 0x2026;
            }
            float tx = x + cw;
            float ty = (bh - ch) / 2.0f;
            for (size_t k = 0; k < n; k++) {
                const glyph *g = get_glyph(r, cps[k], active ? TE_FONT_BOLD : TE_FONT_REGULAR, false);
                if (g != nullptr && g->present) {
                    push_glyph(&r->glyphs, g, tx + (float)k * cw + (float)g->left, ty + (float)m->baseline - (float)g->top,
                               active ? c->active_fg : c->bar_fg, active ? 1.0f : 0.7f);
                }
            }
        }
    }
    for (size_t i = 0; i < c->ndividers && c->dividers != nullptr; i++) {
        push_rect(&r->deco, (float)c->dividers[i][0], (float)c->dividers[i][1], (float)c->dividers[i][2],
                  (float)c->dividers[i][3], c->divider, 1.0f);
    }
    if (c->modal != nullptr && c->modal[0] != '\0') {
        push_rect(&r->tint, 0.0f, 0.0f, (float)c->win_w, (float)c->win_h, c->dim_bg, 0.6f);
        draw_pill(r, c->toast_fg, c->toast_bg, c->win_w, c->win_h, c->modal, PILL_CENTER, cw, ch, pad);
    } else if (c->toast != nullptr && c->toast[0] != '\0') {
        float off = (float)c->tab_bar_h;
        size_t before = r->ov_rects.len;
        size_t gbefore = r->ov_glyphs.len;
        draw_pill(r, c->toast_fg, c->toast_bg, c->win_w, c->win_h, c->toast, PILL_TOP_RIGHT, cw, ch, pad);
        /* Keep the toast below the tab bar. */
        for (size_t i = before; i < r->ov_rects.len; i++) ((rect_inst *)r->ov_rects.data)[i].y += off;
        for (size_t i = gbefore; i < r->ov_glyphs.len; i++) ((glyph_inst *)r->ov_glyphs.data)[i].y += off;
    }
    te_frame f = { 0 };
    flush_vectors(r, &f);
}

te_err renderer_end(te_renderer *r)
{
    glBindVertexArray(0);
    GLenum err = glGetError();
    if (r == nullptr || err != GL_NO_ERROR) {
        LOG_W("OpenGL error 0x%x during frame", (unsigned)err);
        return TE_ERR_GL;
    }
    return TE_OK;
}

bool renderer_animating(const te_renderer *r)
{
    return r != nullptr && r->animating;
}

/* ------------------------------------------------------------------------- */
/* Free-form UI */

static void ui_rect_cb(void *user, te_layer layer, float x, float y, float w, float h, uint32_t rgb, float alpha)
{
    te_renderer *r = user;
    push_rect(layer == TE_LAYER_OVERLAY ? &r->ov_rects : &r->bg, x, y, w, h, rgb, alpha);
}

static void ui_text_cb(void *user, te_layer layer, float x, float y, const char *utf8, size_t max_cols, uint32_t rgb,
                       bool bold)
{
    te_renderer *r = user;
    const te_font_metrics *m = font_metrics(r->font);
    inst_vec *v = layer == TE_LAYER_OVERLAY ? &r->ov_glyphs : &r->glyphs;
    size_t col = 0;
    te_utf8_dec d = { 0 };
    for (const char *s = utf8; *s != '\0' && col < max_cols; s++) {
        uint32_t cp = 0;
        te_utf8_res res = utf8_feed(&d, (uint8_t)*s, &cp);
        if (res == TE_UTF8_MORE) {
            continue;
        }
        if (res == TE_UTF8_ERROR) {
            cp = 0xFFFD;
        }
        int w = te_wcwidth(cp);
        if (w <= 0) {
            continue;   /* combining marks and controls take no cell here */
        }
        if (col + (size_t)w > max_cols) {
            break;
        }
        if (cp != ' ') {
            const glyph *g = get_glyph(r, cp, bold ? TE_FONT_BOLD : TE_FONT_REGULAR, w == 2);
            if (g != nullptr && g->present) {
                push_glyph(v, g, x + (float)col * (float)m->cell_w + (float)g->left,
                           y + (float)m->baseline - (float)g->top, rgb, 1.0f);
            }
        }
        col += (size_t)w;
    }
}

void renderer_painter(te_renderer *r, float scale, te_painter *out)
{
    const te_font_metrics *m = font_metrics(r->font);
    *out = (te_painter){
        .user = r,
        .cell_w = (float)m->cell_w,
        .cell_h = (float)m->cell_h,
        .scale = scale > 0.0f ? scale : 1.0f,
        .rect = ui_rect_cb,
        .text = ui_text_cb,
    };
}

void renderer_ui_begin(te_renderer *r)
{
    if (r == nullptr) {
        return;
    }
    clear_vectors(r);
    r->nimgs = 0;
}

void renderer_ui_end(te_renderer *r)
{
    if (r == nullptr) {
        return;
    }
    te_frame f = { 0 };
    flush_vectors(r, &f);
}
