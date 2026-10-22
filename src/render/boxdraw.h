/*
 * Box-drawing (U+2500..U+257F) and block elements (U+2580..U+259F) drawn as
 * rectangles instead of font glyphs, so lines join seamlessly across cells at
 * any line height.
 */
#ifndef TE_BOXDRAW_H
#define TE_BOXDRAW_H

#include <stdbool.h>
#include "common.h"

typedef void (*box_rect_fn)(void *user, float x, float y, float w, float h, float alpha);

[[nodiscard]] bool boxdraw_handles(uint32_t cp);

/*
 * Emit the rectangles for cp in the cell (x, y, cw, ch). thick is the light
 * line width in pixels. Returns false if cp is not handled (draw the glyph).
 */
bool boxdraw(uint32_t cp, float x, float y, float cw, float ch, float thick, box_rect_fn fn, void *user);

#endif
