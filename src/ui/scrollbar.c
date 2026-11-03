#include "ui/scrollbar.h"

#include <math.h>
#include <string.h>

#define BAR_W      4.0f    /* drawn width, at scale 1 */
#define BAR_W_HOT  7.0f    /* while hovered or dragged */
#define BAR_GAP    2.0f    /* from the pane's right edge */
#define HIT_W     12.0f    /* the grab strip is wider than the bar */

bool scrollbar_geom(uint32_t count, uint32_t rows, uint32_t off, int pane_w, int pane_h,
                    float min_thumb, float scale, bool active, te_scrollbar *out)
{
    if (out == nullptr) {
        return false;
    }
    memset(out, 0, sizeof *out);
    if (count == 0 || rows == 0 || pane_w <= 0 || pane_h <= 0) {
        return false;
    }
    float s = scale > 0.0f && isfinite(scale) ? scale : 1.0f;
    if (off > count) {
        off = count;
    }
    float track = (float)pane_h;
    float total = (float)count + (float)rows;
    float len = track * (float)rows / total;
    if (len < min_thumb) len = min_thumb;
    if (len > track) len = track;
    out->len = len;
    out->span = track - len;
    out->top = out->span * (float)(count - off) / (float)count;
    out->w = (active ? BAR_W_HOT : BAR_W) * s;
    out->x = (float)pane_w - out->w - BAR_GAP * s;
    float hit = HIT_W * s;
    out->hit_x = (float)pane_w - (hit < (float)pane_w ? hit : (float)pane_w);
    return true;
}

uint32_t scrollbar_offset_at(const te_scrollbar *sb, uint32_t count, float thumb_top)
{
    if (sb == nullptr || count == 0 || !(sb->span > 0.0f) || !isfinite(thumb_top)) {
        return 0;
    }
    float t = thumb_top < 0.0f ? 0.0f : (thumb_top > sb->span ? sb->span : thumb_top);
    /* top = span * (count - off) / count, solved for off. */
    double from_top = round((double)t * (double)count / (double)sb->span);
    if (from_top < 0.0) from_top = 0.0;
    if (from_top > (double)count) from_top = (double)count;
    return count - (uint32_t)from_top;
}
