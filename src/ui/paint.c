#include "ui/paint.h"

#include "util/utf8.h"

size_t ui_text_cols(const char *utf8)
{
    if (utf8 == nullptr) {
        return 0;
    }
    size_t cols = 0;
    te_utf8_dec d = { 0 };
    for (const char *s = utf8; *s != '\0'; s++) {
        uint32_t cp = 0;
        te_utf8_res r = utf8_feed(&d, (uint8_t)*s, &cp);
        if (r == TE_UTF8_DONE) {
            int w = te_wcwidth(cp);
            cols += w > 0 ? (size_t)w : 0;
        } else if (r == TE_UTF8_ERROR) {
            cols++;
        }
    }
    return cols;
}

size_t ui_text_fit(const char *utf8, size_t max_cols)
{
    if (utf8 == nullptr) {
        return 0;
    }
    size_t cols = 0;
    size_t fit = 0;
    te_utf8_dec d = { 0 };
    for (size_t i = 0; utf8[i] != '\0'; i++) {
        uint32_t cp = 0;
        te_utf8_res r = utf8_feed(&d, (uint8_t)utf8[i], &cp);
        if (r == TE_UTF8_MORE) {
            continue;
        }
        size_t w = 1;
        if (r == TE_UTF8_DONE) {
            int cw = te_wcwidth(cp);
            w = cw > 0 ? (size_t)cw : 0;
        }
        if (cols + w > max_cols) {
            break;
        }
        cols += w;
        fit = i + 1;   /* the whole character up to here fits */
    }
    return fit;
}

uint32_t ui_mix(uint32_t a, uint32_t b, float t)
{
    float k = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
    uint32_t out = 0;
    for (unsigned shift = 0; shift <= 16; shift += 8) {
        float ca = (float)((a >> shift) & 0xFFu);
        float cb = (float)((b >> shift) & 0xFFu);
        float c = ca + (cb - ca) * k + 0.5f;
        uint32_t v = c <= 0.0f ? 0u : (c >= 255.0f ? 255u : (uint32_t)c);
        out |= v << shift;
    }
    return out;
}

void ui_rect(const te_painter *p, te_layer layer, float x, float y, float w, float h, uint32_t rgb, float alpha)
{
    if (p != nullptr && p->rect != nullptr && w > 0.0f && h > 0.0f) {
        p->rect(p->user, layer, x, y, w, h, rgb, alpha);
    }
}

void ui_text(const te_painter *p, te_layer layer, float x, float y, const char *utf8, size_t max_cols, uint32_t rgb,
             bool bold)
{
    if (p != nullptr && p->text != nullptr && utf8 != nullptr && utf8[0] != '\0' && max_cols > 0) {
        p->text(p->user, layer, x, y, utf8, max_cols, rgb, bold);
    }
}

void ui_frame(const te_painter *p, te_layer layer, float x, float y, float w, float h, uint32_t rgb)
{
    float t = p != nullptr && p->scale > 0.0f ? p->scale : 1.0f;
    ui_rect(p, layer, x, y, w, t, rgb, 1.0f);
    ui_rect(p, layer, x, y + h - t, w, t, rgb, 1.0f);
    ui_rect(p, layer, x, y, t, h, rgb, 1.0f);
    ui_rect(p, layer, x + w - t, y, t, h, rgb, 1.0f);
}
