/*
 * Drawing interface for the sidebar, menus and dialogs. The UI code only
 * talks to this; the renderer supplies the real implementation and the
 * tests a recording one, so none of the UI logic needs OpenGL.
 */
#ifndef TE_PAINT_H
#define TE_PAINT_H

#include <stdbool.h>
#include "common.h"

typedef enum { TE_LAYER_BASE, TE_LAYER_OVERLAY } te_layer;

typedef struct te_painter {
    void  *user;
    float  cell_w, cell_h;        /* monospace cell in pixels */
    float  scale;                 /* content scale, for borders and spacing */
    /* Filled rectangle. */
    void (*rect)(void *user, te_layer layer, float x, float y, float w, float h, uint32_t rgb, float alpha);
    /* One line of UTF-8 text with its top-left at (x, y); at most max_cols cells. */
    void (*text)(void *user, te_layer layer, float x, float y, const char *utf8, size_t max_cols, uint32_t rgb,
                 bool bold);
} te_painter;

/* Colours the UI draws with, derived from the terminal theme. */
typedef struct te_ui_colors {
    uint32_t bg;          /* panel background */
    uint32_t bg_alt;      /* headers, input fields */
    uint32_t fg;
    uint32_t muted;       /* secondary text */
    uint32_t accent;      /* active tab, focus ring */
    uint32_t sel_bg, sel_fg;
    uint32_t hover;
    uint32_t border;
    uint32_t danger;      /* destructive actions */
} te_ui_colors;

/* Width of UTF-8 text in cells (wide characters count 2, invalid bytes 1). */
size_t ui_text_cols(const char *utf8);
/* Byte length of the longest prefix of utf8 that fits in max_cols cells. */
size_t ui_text_fit(const char *utf8, size_t max_cols);
/* Linear blend of two 0xRRGGBB colours (t = 0: a, t = 1: b). */
uint32_t ui_mix(uint32_t a, uint32_t b, float t);

void ui_rect(const te_painter *p, te_layer layer, float x, float y, float w, float h, uint32_t rgb, float alpha);
void ui_text(const te_painter *p, te_layer layer, float x, float y, const char *utf8, size_t max_cols, uint32_t rgb,
             bool bold);
/* A one-pixel (times scale) outline. */
void ui_frame(const te_painter *p, te_layer layer, float x, float y, float w, float h, uint32_t rgb);

#endif
