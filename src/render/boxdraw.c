#include "render/boxdraw.h"

#include <math.h>

/* Arm weights: 0 none, 1 light, 2 heavy, 3 double. Packed L R U D, 2 bits each. */
#define B(l, r, u, d) (uint8_t)(((l) << 6) | ((r) << 4) | ((u) << 2) | (d))

static const uint8_t lines[0x80] = {
    /* 2500 */ B(1,1,0,0), B(2,2,0,0), B(0,0,1,1), B(0,0,2,2), B(1,1,0,0), B(2,2,0,0), B(0,0,1,1), B(0,0,2,2),
    /* 2508 */ B(1,1,0,0), B(2,2,0,0), B(0,0,1,1), B(0,0,2,2), B(0,1,0,1), B(0,2,0,1), B(0,1,0,2), B(0,2,0,2),
    /* 2510 */ B(1,0,0,1), B(2,0,0,1), B(1,0,0,2), B(2,0,0,2), B(0,1,1,0), B(0,2,1,0), B(0,1,2,0), B(0,2,2,0),
    /* 2518 */ B(1,0,1,0), B(2,0,1,0), B(1,0,2,0), B(2,0,2,0), B(0,1,1,1), B(0,2,1,1), B(0,1,2,1), B(0,1,1,2),
    /* 2520 */ B(0,1,2,2), B(0,2,2,1), B(0,2,1,2), B(0,2,2,2), B(1,0,1,1), B(2,0,1,1), B(1,0,2,1), B(1,0,1,2),
    /* 2528 */ B(1,0,2,2), B(2,0,2,1), B(2,0,1,2), B(2,0,2,2), B(1,1,0,1), B(2,1,0,1), B(1,2,0,1), B(2,2,0,1),
    /* 2530 */ B(1,1,0,2), B(2,1,0,2), B(1,2,0,2), B(2,2,0,2), B(1,1,1,0), B(2,1,1,0), B(1,2,1,0), B(2,2,1,0),
    /* 2538 */ B(1,1,2,0), B(2,1,2,0), B(1,2,2,0), B(2,2,2,0), B(1,1,1,1), B(2,1,1,1), B(1,2,1,1), B(2,2,1,1),
    /* 2540 */ B(1,1,2,1), B(1,1,1,2), B(1,1,2,2), B(2,1,2,1), B(1,2,2,1), B(2,1,1,2), B(1,2,1,2), B(2,2,2,1),
    /* 2548 */ B(2,2,1,2), B(2,1,2,2), B(1,2,2,2), B(2,2,2,2), B(1,1,0,0), B(2,2,0,0), B(0,0,1,1), B(0,0,2,2),
    /* 2550 */ B(3,3,0,0), B(0,0,3,3), B(0,3,0,1), B(0,1,0,3), B(0,3,0,3), B(3,0,0,1), B(1,0,0,3), B(3,0,0,3),
    /* 2558 */ B(0,3,1,0), B(0,1,3,0), B(0,3,3,0), B(3,0,1,0), B(1,0,3,0), B(3,0,3,0), B(0,3,1,1), B(0,1,3,3),
    /* 2560 */ B(0,3,3,3), B(3,0,1,1), B(1,0,3,3), B(3,0,3,3), B(3,3,0,1), B(1,1,0,3), B(3,3,0,3), B(3,3,1,0),
    /* 2568 */ B(1,1,3,0), B(3,3,3,0), B(3,3,1,1), B(1,1,3,3), B(3,3,3,3), B(0,1,0,1), B(1,0,0,1), B(1,0,1,0),
    /* 2570 */ B(0,1,1,0), 0, 0, 0, B(1,0,0,0), B(0,0,1,0), B(0,1,0,0), B(0,0,0,1),
    /* 2578 */ B(2,0,0,0), B(0,0,2,0), B(0,2,0,0), B(0,0,0,2), B(1,2,0,0), B(0,0,1,2), B(2,1,0,0), B(0,0,2,1),
};

/* Number of dashes for the dashed variants (0 = solid). */
static int dashes(uint32_t cp)
{
    switch (cp) {
    case 0x2504: case 0x2505: case 0x2506: case 0x2507: return 3;
    case 0x2508: case 0x2509: case 0x250A: case 0x250B: return 4;
    case 0x254C: case 0x254D: case 0x254E: case 0x254F: return 2;
    default: return 0;
    }
}

bool boxdraw_handles(uint32_t cp)
{
    if (cp >= 0x2500 && cp <= 0x257F) {
        return lines[cp - 0x2500] != 0;
    }
    return cp >= 0x2580 && cp <= 0x259F;
}

static void hseg(float x0, float x1, float y, float t, int nd, box_rect_fn fn, void *u)
{
    if (nd <= 0) {
        fn(u, x0, y, x1 - x0, t, 1.0f);
        return;
    }
    float seg = (x1 - x0) / (float)nd;
    for (int i = 0; i < nd; i++) {
        fn(u, x0 + seg * (float)i, y, seg * 0.6f, t, 1.0f);
    }
}

static void vseg(float y0, float y1, float x, float t, int nd, box_rect_fn fn, void *u)
{
    if (nd <= 0) {
        fn(u, x, y0, t, y1 - y0, 1.0f);
        return;
    }
    float seg = (y1 - y0) / (float)nd;
    for (int i = 0; i < nd; i++) {
        fn(u, x, y0 + seg * (float)i, t, seg * 0.6f, 1.0f);
    }
}

static bool blocks(uint32_t cp, float x, float y, float cw, float ch, box_rect_fn fn, void *u)
{
    float hw = floorf(cw / 2.0f);
    float hh = floorf(ch / 2.0f);
    if (cp == 0x2580) { fn(u, x, y, cw, hh, 1.0f); return true; }                     /* upper half */
    if (cp >= 0x2581 && cp <= 0x2588) {                                                 /* lower n/8 */
        float h = ch * (float)(cp - 0x2580) / 8.0f;
        fn(u, x, y + ch - h, cw, h, 1.0f);
        return true;
    }
    if (cp >= 0x2589 && cp <= 0x258F) {                                                 /* left n/8 */
        float w = cw * (float)(0x2590 - cp) / 8.0f;
        fn(u, x, y, w, ch, 1.0f);
        return true;
    }
    switch (cp) {
    case 0x2590: fn(u, x + hw, y, cw - hw, ch, 1.0f); return true;                     /* right half */
    case 0x2591: fn(u, x, y, cw, ch, 0.25f); return true;                              /* shades */
    case 0x2592: fn(u, x, y, cw, ch, 0.5f); return true;
    case 0x2593: fn(u, x, y, cw, ch, 0.75f); return true;
    case 0x2594: fn(u, x, y, cw, ch / 8.0f, 1.0f); return true;                        /* upper 1/8 */
    case 0x2595: fn(u, x + cw * 7.0f / 8.0f, y, cw / 8.0f, ch, 1.0f); return true;     /* right 1/8 */
    default: break;
    }
    /* Quadrants U+2596..U+259F: bits TL TR BL BR. */
    static const uint8_t quads[10] = { 0x2, 0x1, 0x8, 0xB, 0x9, 0xE, 0xD, 0x4, 0x6, 0x7 };
    if (cp >= 0x2596 && cp <= 0x259F) {
        uint8_t q = quads[cp - 0x2596];
        if ((q & 0x8) != 0) fn(u, x, y, hw, hh, 1.0f);
        if ((q & 0x4) != 0) fn(u, x + hw, y, cw - hw, hh, 1.0f);
        if ((q & 0x2) != 0) fn(u, x, y + hh, hw, ch - hh, 1.0f);
        if ((q & 0x1) != 0) fn(u, x + hw, y + hh, cw - hw, ch - hh, 1.0f);
        return true;
    }
    return false;
}

bool boxdraw(uint32_t cp, float x, float y, float cw, float ch, float thick, box_rect_fn fn, void *u)
{
    if (fn == nullptr) {
        return false;
    }
    if (cp >= 0x2580 && cp <= 0x259F) {
        return blocks(cp, x, y, cw, ch, fn, u);
    }
    if (cp < 0x2500 || cp > 0x257F) {
        return false;
    }
    uint8_t a = lines[cp - 0x2500];
    if (a == 0) {
        return false;   /* diagonals: use the font */
    }
    int wl = (a >> 6) & 3, wr = (a >> 4) & 3, wu = (a >> 2) & 3, wd = a & 3;
    int nd = dashes(cp);
    float t = thick < 1.0f ? 1.0f : roundf(thick);
    float heavy = t * 2.0f;
    float cx = x + floorf(cw / 2.0f);
    float cy = y + floorf(ch / 2.0f);
    /* How far horizontal arms reach past the centre so they meet the vertical stem. */
    int wv = wu > wd ? wu : wd;
    int wh = wl > wr ? wl : wr;
    float vreach = wv == 3 ? t * 1.5f : (wv == 2 ? heavy / 2.0f : t / 2.0f);
    float hreach = wh == 3 ? t * 1.5f : (wh == 2 ? heavy / 2.0f : t / 2.0f);

    const int wh_arms[2] = { wl, wr };
    for (int side = 0; side < 2; side++) {
        int w = wh_arms[side];
        if (w == 0) continue;
        float x0 = side == 0 ? x : cx - vreach;
        float x1 = side == 0 ? cx + vreach : x + cw;
        if (w == 3) {
            hseg(x0, x1, cy - t * 1.5f, t, nd, fn, u);
            hseg(x0, x1, cy + t * 0.5f, t, nd, fn, u);
        } else {
            float th = w == 2 ? heavy : t;
            hseg(x0, x1, cy - th / 2.0f, th, nd, fn, u);
        }
    }
    const int wv_arms[2] = { wu, wd };
    for (int side = 0; side < 2; side++) {
        int w = wv_arms[side];
        if (w == 0) continue;
        float y0 = side == 0 ? y : cy - hreach;
        float y1 = side == 0 ? cy + hreach : y + ch;
        if (w == 3) {
            vseg(y0, y1, cx - t * 1.5f, t, nd, fn, u);
            vseg(y0, y1, cx + t * 0.5f, t, nd, fn, u);
        } else {
            float tw = w == 2 ? heavy : t;
            vseg(y0, y1, cx - tw / 2.0f, tw, nd, fn, u);
        }
    }
    return true;
}
