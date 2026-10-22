#include "input/mouse.h"

#include <string.h>

#include "platform/platform.h"
#include "util/str.h"
#include "util/utf8.h"

bool mouse_wants(const te_modes *m, te_mouse_action act, bool button_held)
{
    switch (m->mouse) {
    case TE_MOUSE_X10:    return act == TE_MOUSE_PRESS;
    case TE_MOUSE_NORMAL: return act != TE_MOUSE_MOVE;
    case TE_MOUSE_BUTTON: return act != TE_MOUSE_MOVE || button_held;
    case TE_MOUSE_ANY:    return true;
    case TE_MOUSE_NONE:
    default:              return false;
    }
}

static unsigned mod_bits(uint32_t mods)
{
    unsigned b = 0;
    if ((mods & TE_MOD_SHIFT) != 0) b |= 4u;
    if ((mods & TE_MOD_ALT) != 0)   b |= 8u;
    if ((mods & TE_MOD_CTRL) != 0)  b |= 16u;
    return b;
}

size_t mouse_encode(const te_modes *m, te_mouse_action act, int button, uint32_t mods, uint16_t col,
                    uint16_t row, uint32_t px, uint32_t py, char *out, size_t outsz)
{
    if (m == nullptr || out == nullptr || outsz < 32 || button < 0 || button > 127) {
        return 0;
    }
    out[0] = '\0';
    bool wheel = button >= TE_MB_WHEEL_UP;
    if (wheel && act == TE_MOUSE_RELEASE) {
        return 0;   /* wheels have no release */
    }
    if (m->mouse == TE_MOUSE_X10 && (wheel || act != TE_MOUSE_PRESS)) {
        return 0;
    }
    unsigned cb = (unsigned)button;
    bool sgr = m->mouse_enc == TE_MOUSE_ENC_SGR || m->mouse_enc == TE_MOUSE_ENC_SGR_PIXELS;
    if (act == TE_MOUSE_RELEASE && !sgr) {
        cb = TE_MB_NONE;   /* legacy encodings cannot say which button was released */
    }
    if (m->mouse != TE_MOUSE_X10) {
        cb |= mod_bits(mods);
    }
    if (act == TE_MOUSE_MOVE) {
        cb |= 32u;
    }
    uint32_t x = (uint32_t)col + 1u;
    uint32_t y = (uint32_t)row + 1u;
    te_err rc = TE_OK;

    switch (m->mouse_enc) {
    case TE_MOUSE_ENC_SGR:
        rc = str_fmt(out, outsz, "\x1b[<%u;%u;%u%c", cb, x, y, act == TE_MOUSE_RELEASE ? 'm' : 'M');
        break;
    case TE_MOUSE_ENC_SGR_PIXELS:
        rc = str_fmt(out, outsz, "\x1b[<%u;%u;%u%c", cb, px + 1u, py + 1u, act == TE_MOUSE_RELEASE ? 'm' : 'M');
        break;
    case TE_MOUSE_ENC_URXVT:
        rc = str_fmt(out, outsz, "\x1b[%u;%u;%uM", cb + 32u, x, y);
        break;
    case TE_MOUSE_ENC_UTF8: {
        /* Each value is 32+v encoded as a UTF-8 character (max U+07FF in xterm). */
        uint32_t vals[3] = { cb + 32u, x + 32u, y + 32u };
        size_t n = 0;
        memcpy(out, "\x1b[M", 3);
        n = 3;
        for (size_t i = 0; i < 3; i++) {
            if (vals[i] > 0x7FF) {
                return 0;
            }
            size_t k = utf8_encode(vals[i], out + n, outsz - n - 1);
            if (k == 0) {
                return 0;
            }
            n += k;
        }
        out[n] = '\0';
        return n;
    }
    case TE_MOUSE_ENC_DEFAULT:
    default:
        if (x + 32u > 255u || y + 32u > 255u) {
            return 0;   /* not representable in a single byte */
        }
        out[0] = '\x1b';
        out[1] = '[';
        out[2] = 'M';
        out[3] = (char)(unsigned char)(cb + 32u);
        out[4] = (char)(unsigned char)(x + 32u);
        out[5] = (char)(unsigned char)(y + 32u);
        out[6] = '\0';
        return 6;
    }
    return rc == TE_OK ? strlen(out) : 0;
}
