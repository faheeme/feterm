#include "ui/logo.h"

#include <string.h>
#include "common.h"
#include "ui/logo_data.h"

#define NLOGOS TE_ARRAY_LEN(te_logo_defs)

static_assert(NLOGOS > TE_LOGO_DEFAULT);
static_assert(TE_LOGO_PX == TE_LOGO_PX_MAX);
static_assert(NLOGOS < 255, "the renderer keys logos by an 8-bit index");

static const char *const *names_table(void)
{
    static const char *names[NLOGOS];
    if (names[0] == nullptr) {
        for (size_t i = 0; i < NLOGOS; i++) {
            names[i] = te_logo_defs[i].name;
        }
    }
    return names;
}

size_t logo_count(void)
{
    return NLOGOS;
}

int logo_dot(void)
{
    return (int)NLOGOS;
}

int logo_max_px(void)
{
    return TE_LOGO_PX;
}

int logo_find(const char *slug)
{
    if (slug == nullptr || slug[0] == '\0') {
        return -1;
    }
    for (size_t i = 0; i < NLOGOS; i++) {
        if (strcmp(te_logo_defs[i].slug, slug) == 0) {
            return (int)i;
        }
    }
    return -1;
}

const char *logo_slug(int i)
{
    return i >= 0 && (size_t)i < NLOGOS ? te_logo_defs[i].slug : "";
}

const char *logo_name(int i)
{
    return i >= 0 && (size_t)i < NLOGOS ? te_logo_defs[i].name : "";
}

const char *const *logo_names(void)
{
    return names_table();
}

bool logo_slug_valid(const char *slug)
{
    if (slug == nullptr) {
        return false;
    }
    size_t n = strnlen(slug, TE_LOGO_SLUG_MAX + 1);
    if (n == 0 || n > TE_LOGO_SLUG_MAX) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        char c = slug[i];
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-')) {
            return false;
        }
    }
    return true;
}

/* Box filter: each output pixel is the area-weighted mean of the source pixels it covers. */
int logo_render(int i, int px, uint8_t *out, size_t outsz)
{
    const uint8_t *src = nullptr;
    if (i >= 0 && (size_t)i < NLOGOS) {
        src = te_logo_defs[i].mask;
    } else if (i == logo_dot()) {
        src = logo_dot_mask;
    }
    if (src == nullptr || out == nullptr) {
        return 0;
    }
    px = TE_CLAMP(px, 1, TE_LOGO_PX);
    if (outsz < (size_t)px * (size_t)px) {
        return 0;
    }
    const double step = (double)TE_LOGO_PX / (double)px;
    for (int oy = 0; oy < px; oy++) {
        double y0 = (double)oy * step;
        double y1 = y0 + step;
        for (int ox = 0; ox < px; ox++) {
            double x0 = (double)ox * step;
            double x1 = x0 + step;
            double sum = 0.0;
            for (int sy = (int)y0; sy < TE_LOGO_PX && (double)sy < y1; sy++) {
                double wy = TE_MIN(y1, (double)(sy + 1)) - TE_MAX(y0, (double)sy);
                if (wy <= 0.0) {
                    continue;
                }
                for (int sx = (int)x0; sx < TE_LOGO_PX && (double)sx < x1; sx++) {
                    double wx = TE_MIN(x1, (double)(sx + 1)) - TE_MAX(x0, (double)sx);
                    if (wx > 0.0) {
                        sum += wx * wy * (double)src[(size_t)sy * TE_LOGO_PX + (size_t)sx];
                    }
                }
            }
            double v = sum / (step * step) + 0.5;
            out[(size_t)oy * (size_t)px + (size_t)ox] = (uint8_t)TE_CLAMP(v, 0.0, 255.0);
        }
    }
    return px;
}
