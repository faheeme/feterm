/*
 * A pane's scrollbar: where the thumb is for a given scrollback position, and
 * which position a dragged thumb stands for. Shared by the renderer (drawing)
 * and the mouse code (grabbing), so both always agree. Pure geometry: no state.
 */
#ifndef TE_SCROLLBAR_H
#define TE_SCROLLBAR_H

#include <stdbool.h>
#include "common.h"

typedef struct te_scrollbar {
    float x, w;       /* the drawn bar, relative to the pane */
    float hit_x;      /* the grab strip runs from here to the pane's right edge */
    float top, len;   /* the thumb */
    float span;       /* how far the thumb can travel: track height - len */
} te_scrollbar;

/*
 * Geometry for a pane of pane_w x pane_h pixels showing rows lines, with count
 * lines of history scrolled back by off. min_thumb keeps the thumb grabbable
 * (one cell high); active (hovered or dragged) draws it wider. Returns false
 * when there is nothing to scroll or the pane has no size.
 */
[[nodiscard]] bool scrollbar_geom(uint32_t count, uint32_t rows, uint32_t off, int pane_w, int pane_h,
                                  float min_thumb, float scale, bool active, te_scrollbar *out);

/* The scroll offset (0 = bottom, count = top) for a thumb whose top edge is at
 * thumb_top, clamped to the track. */
[[nodiscard]] uint32_t scrollbar_offset_at(const te_scrollbar *sb, uint32_t count, float thumb_top);

#endif
