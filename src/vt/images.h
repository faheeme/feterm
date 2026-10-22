/*
 * Inline images: storage plus decoders for sixel (DCS q) and the kitty
 * graphics protocol (APC G). Every size is capped: per-image dimensions, the
 * number of images, and the total pixel memory (oldest images are evicted).
 */
#ifndef TE_IMAGES_H
#define TE_IMAGES_H

#include <stdbool.h>
#include "common.h"
#include "vt/parser.h"

#define TE_MAX_IMAGES        128u
#define TE_IMAGE_MAX_DIM     4096u
#define TE_SIXEL_MAX_DIM     2048u
#define TE_IMAGE_BUDGET      (64u * 1024u * 1024u)    /* bytes of RGBA kept in total */
#define TE_KITTY_MAX_PAYLOAD (48u * 1024u * 1024u)    /* base64 bytes across chunks */

typedef struct te_image {
    uint32_t id;          /* kitty image id (0 for sixel) */
    uint32_t serial;      /* unique per stored image, for texture caches */
    uint32_t w, h;        /* pixels */
    uint8_t *rgba;        /* premultiplied RGBA, w*h*4 */
    bool     placed;      /* shown on screen (else only transmitted) */
    bool     alt;         /* placed on the alternate screen */
    uint64_t row;         /* absolute row of the top-left cell */
    uint16_t col;
    uint16_t cols, rows;  /* size in cells */
} te_image;

typedef struct te_images {
    te_image items[TE_MAX_IMAGES];
    size_t   n;
    size_t   bytes;
    uint32_t next_serial;
    /* kitty chunked transmission in progress */
    char    *chunk;
    size_t   chunk_len, chunk_cap;
    char     chunk_keys[256];
    bool     chunking;
} te_images;

void images_init(te_images *s);
void images_free(te_images *s);

/* Store an image (takes ownership of rgba). Same non-zero id replaces. Evicts to fit the budget. */
[[nodiscard]] te_image *images_add(te_images *s, uint32_t id, uint32_t w, uint32_t h, uint8_t *rgba);
te_image *images_find(te_images *s, uint32_t id);
void images_delete_id(te_images *s, uint32_t id);
void images_clear(te_images *s);
/* Drop placed images that scrolled out of the stored history, or belong to a screen being discarded. */
void images_prune(te_images *s, uint64_t first_abs);
void images_drop_screen(te_images *s, bool alt);

/*
 * Decode a sixel payload (the DCS data after 'q'). params are the DCS
 * parameters (P2 = 1: unset pixels are transparent). On success *rgba is a
 * heap buffer of w*h*4 premultiplied bytes.
 */
[[nodiscard]] te_err sixel_decode(const vt_params *params, const char *data, size_t len, uint32_t bg_rgb,
                                  uint8_t **rgba, uint32_t *w, uint32_t *h);

typedef struct te_kitty_cmd {
    char     action;      /* t transmit, T transmit+display, p place, d delete, q query */
    uint32_t id;
    uint32_t format;      /* 24 RGB, 32 RGBA, 100 PNG (unsupported) */
    uint32_t w, h;
    uint16_t cols, rows;  /* requested display size in cells (0 = from pixels) */
    int      quiet;
    bool     no_move;     /* C=1: leave the cursor where it is */
    char     del;         /* d= for deletes */
    bool     ready;       /* false: waiting for more chunks */
    uint8_t *pixels;      /* premultiplied RGBA w*h*4 (heap, caller takes it) */
    char     err[80];     /* non-empty: reply with this error */
} te_kitty_cmd;

/* Parse one APC G payload (without the leading 'G'), handling chunking. */
void kitty_command(te_images *s, const char *data, size_t len, te_kitty_cmd *out);

#endif
