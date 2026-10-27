#include "vt/parser.h"

#include <string.h>

#include "util/mem.h"

void vt_parser_init(vt_parser *p, const vt_callbacks *cb, void *user)
{
    memset(p, 0, sizeof *p);
    p->state = VT_GROUND;
    p->cb = cb;
    p->user = user;
}

void vt_parser_destroy(vt_parser *p)
{
    if (p == nullptr) {
        return;
    }
    TE_FREE(p->str);
    p->str_len = 0;
    p->str_cap = 0;
}

static void clear(vt_parser *p)
{
    memset(&p->params, 0, sizeof p->params);
    p->params_overflow = false;
    p->ninter = 0;
    p->inter_overflow = false;
    p->priv = 0;
}

static void str_reset(vt_parser *p)
{
    p->str_len = 0;
    p->str_overflow = false;
}

static void str_put(vt_parser *p, uint8_t b)
{
    if (p->str_overflow) {
        return;
    }
    /* Reserve one byte for the terminator. */
    if (p->str_len + 1 >= p->str_cap) {
        if (p->str_cap >= VT_MAX_STRING) {
            p->str_overflow = true;
            return;
        }
        size_t ncap = p->str_cap == 0 ? 256 : p->str_cap * 2;
        if (ncap > VT_MAX_STRING) {
            ncap = VT_MAX_STRING;
        }
        void *buf = p->str;
        if (te_realloc_array(&buf, p->str_cap, ncap, 1) != TE_OK) {
            p->str_overflow = true;
            return;
        }
        p->str = buf;
        p->str_cap = ncap;
    }
    p->str[p->str_len++] = (char)b;
    p->str[p->str_len] = '\0';
}

static void collect(vt_parser *p, uint8_t b)
{
    if (p->ninter < VT_MAX_INTERMEDIATES) {
        p->inter[p->ninter++] = b;
    } else {
        p->inter_overflow = true;
    }
}

static void param(vt_parser *p, uint8_t b)
{
    vt_params *pp = &p->params;
    if (pp->n == 0) {
        pp->n = 1;
        pp->v[0] = 0;
        pp->sub[0] = false;
    }
    if (b >= '0' && b <= '9') {
        uint32_t v = (uint32_t)pp->v[pp->n - 1] * 10u + (uint32_t)(b - '0');
        pp->v[pp->n - 1] = (uint16_t)(v > VT_MAX_PARAM_VALUE ? VT_MAX_PARAM_VALUE : v);
        return;
    }
    /* ';' or ':' starts a new parameter. */
    if (pp->n >= VT_MAX_PARAMS) {
        p->params_overflow = true;
        return;
    }
    pp->v[pp->n] = 0;
    pp->sub[pp->n] = (b == ':');
    pp->n++;
}

static void csi_dispatch(vt_parser *p, uint8_t final)
{
    if (p->params_overflow || p->inter_overflow || p->cb->csi == nullptr) {
        return;
    }
    p->cb->csi(p->user, &p->params, p->inter, p->ninter, p->priv, final);
}

static void esc_dispatch(vt_parser *p, uint8_t final)
{
    if (p->inter_overflow || p->cb->esc == nullptr) {
        return;
    }
    p->cb->esc(p->user, p->inter, p->ninter, final);
}

static void osc_end(vt_parser *p)
{
    if (!p->str_overflow && p->cb->osc != nullptr) {
        p->cb->osc(p->user, p->str != nullptr ? p->str : "", p->str_len);
    }
    str_reset(p);
}

static void apc_end(vt_parser *p)
{
    if (!p->str_overflow && p->cb->apc != nullptr) {
        p->cb->apc(p->user, p->str != nullptr ? p->str : "", p->str_len);
    }
    str_reset(p);
}

static void dcs_end(vt_parser *p)
{
    if (!p->str_overflow && !p->params_overflow && !p->inter_overflow && p->cb->dcs != nullptr) {
        p->cb->dcs(p->user, &p->params, p->inter, p->ninter, p->priv, p->dcs_final,
                   p->str != nullptr ? p->str : "", p->str_len);
    }
    str_reset(p);
}

/* Leave the current state; string states dispatch their payload first. */
static void leave(vt_parser *p)
{
    if (p->state == VT_OSC) {
        osc_end(p);
    } else if (p->state == VT_DCS_PASS) {
        dcs_end(p);
    } else if (p->state == VT_APC) {
        apc_end(p);
    } else if (p->state == VT_DCS_IGNORE || p->state == VT_SOS_PM_APC) {
        str_reset(p);
    }
}

/* Abort the current string without dispatching it. CAN and SUB cancel the
 * sequence in progress (unlike ESC/ST, which terminate and deliver it). */
static void cancel(vt_parser *p)
{
    if (p->state == VT_OSC || p->state == VT_DCS_PASS || p->state == VT_APC ||
        p->state == VT_DCS_IGNORE || p->state == VT_SOS_PM_APC) {
        str_reset(p);
    }
}

static void execute(vt_parser *p, uint8_t b)
{
    if (p->cb->execute != nullptr) {
        p->cb->execute(p->user, b);
    }
}

static void print(vt_parser *p, uint32_t cp)
{
    if (p->cb->print != nullptr) {
        p->cb->print(p->user, cp);
    }
}

static bool is_c0(uint8_t b)
{
    return b <= 0x17 || b == 0x19 || (b >= 0x1C && b <= 0x1F);
}

static void ground_utf8(vt_parser *p, uint8_t b)
{
    uint32_t cp = 0;
    te_utf8_res r = utf8_feed(&p->utf8, b, &cp);
    switch (r) {
    case TE_UTF8_MORE:
        break;
    case TE_UTF8_DONE:
        /* C1 controls (U+0080..U+009F) are deliberately ignored, not executed. */
        if (cp >= 0xA0) {
            print(p, cp);
        }
        break;
    case TE_UTF8_ERROR:
        print(p, TE_UTF8_REPLACEMENT);
        break;
    case TE_UTF8_ERROR_REPROCESS:
        print(p, TE_UTF8_REPLACEMENT);
        if (b >= 0x80) {
            ground_utf8(p, b);
        }
        break;
    }
}

static void step(vt_parser *p, uint8_t b)
{
    /* Non-ASCII bytes: UTF-8 text in ground, payload in strings, otherwise ignored. */
    if (b >= 0x80) {
        switch (p->state) {
        case VT_GROUND:
            ground_utf8(p, b);
            break;
        case VT_OSC:
        case VT_DCS_PASS:
        case VT_APC:
            str_put(p, b);
            break;
        default:
            break;
        }
        return;
    }

    /* An ASCII byte interrupts a partial UTF-8 sequence. */
    if (p->utf8.need != 0) {
        p->utf8.need = 0;
        print(p, TE_UTF8_REPLACEMENT);
    }

    /* "Anywhere" transitions. */
    if (b == 0x18 || b == 0x1A) {
        cancel(p);
        execute(p, b);
        p->state = VT_GROUND;
        return;
    }
    if (b == 0x1B) {
        leave(p);
        clear(p);
        p->state = VT_ESCAPE;
        return;
    }

    switch (p->state) {
    case VT_GROUND:
        if (b < 0x20) {
            execute(p, b);
        } else if (b < 0x7F) {
            print(p, b);
        }
        break;

    case VT_ESCAPE:
        if (is_c0(b)) {
            execute(p, b);
        } else if (b >= 0x20 && b <= 0x2F) {
            collect(p, b);
            p->state = VT_ESCAPE_INTER;
        } else if (b == '[') {
            clear(p);
            p->state = VT_CSI_ENTRY;
        } else if (b == ']') {
            str_reset(p);
            p->state = VT_OSC;
        } else if (b == 'P') {
            clear(p);
            str_reset(p);
            p->state = VT_DCS_ENTRY;
        } else if (b == '_') {
            str_reset(p);
            p->state = VT_APC;
        } else if (b == 'X' || b == '^') {
            p->state = VT_SOS_PM_APC;
        } else if (b >= 0x30 && b <= 0x7E) {
            esc_dispatch(p, b);
            p->state = VT_GROUND;
        }
        break;

    case VT_ESCAPE_INTER:
        if (is_c0(b)) {
            execute(p, b);
        } else if (b >= 0x20 && b <= 0x2F) {
            collect(p, b);
        } else if (b >= 0x30 && b <= 0x7E) {
            esc_dispatch(p, b);
            p->state = VT_GROUND;
        }
        break;

    case VT_CSI_ENTRY:
        if (is_c0(b)) {
            execute(p, b);
        } else if (b >= 0x20 && b <= 0x2F) {
            collect(p, b);
            p->state = VT_CSI_INTER;
        } else if ((b >= '0' && b <= '9') || b == ';' || b == ':') {
            param(p, b);
            p->state = VT_CSI_PARAM;
        } else if (b >= 0x3C && b <= 0x3F) {
            p->priv = b;
            p->state = VT_CSI_PARAM;
        } else if (b >= 0x40 && b <= 0x7E) {
            csi_dispatch(p, b);
            p->state = VT_GROUND;
        }
        break;

    case VT_CSI_PARAM:
        if (is_c0(b)) {
            execute(p, b);
        } else if ((b >= '0' && b <= '9') || b == ';' || b == ':') {
            param(p, b);
        } else if (b >= 0x3C && b <= 0x3F) {
            p->state = VT_CSI_IGNORE;
        } else if (b >= 0x20 && b <= 0x2F) {
            collect(p, b);
            p->state = VT_CSI_INTER;
        } else if (b >= 0x40 && b <= 0x7E) {
            csi_dispatch(p, b);
            p->state = VT_GROUND;
        }
        break;

    case VT_CSI_INTER:
        if (is_c0(b)) {
            execute(p, b);
        } else if (b >= 0x20 && b <= 0x2F) {
            collect(p, b);
        } else if (b >= 0x30 && b <= 0x3F) {
            p->state = VT_CSI_IGNORE;
        } else if (b >= 0x40 && b <= 0x7E) {
            csi_dispatch(p, b);
            p->state = VT_GROUND;
        }
        break;

    case VT_CSI_IGNORE:
        if (is_c0(b)) {
            execute(p, b);
        } else if (b >= 0x40 && b <= 0x7E) {
            p->state = VT_GROUND;
        }
        break;

    case VT_DCS_ENTRY:
    case VT_DCS_PARAM:
    case VT_DCS_INTER:
        if (b < 0x20 || b == 0x7F) {
            break; /* ignored */
        }
        if (b >= 0x20 && b <= 0x2F) {
            collect(p, b);
            p->state = VT_DCS_INTER;
        } else if ((b >= '0' && b <= '9') || b == ';' || b == ':') {
            if (p->state == VT_DCS_INTER) {
                p->state = VT_DCS_IGNORE;
            } else {
                param(p, b);
                p->state = VT_DCS_PARAM;
            }
        } else if (b >= 0x3C && b <= 0x3F) {
            if (p->state == VT_DCS_ENTRY) {
                p->priv = b;
                p->state = VT_DCS_PARAM;
            } else {
                p->state = VT_DCS_IGNORE;
            }
        } else if (b >= 0x40 && b <= 0x7E) {
            p->dcs_final = b;
            str_reset(p);
            p->state = VT_DCS_PASS;
        }
        break;

    case VT_DCS_PASS:
        if (b != 0x7F) {
            str_put(p, b);
        }
        break;

    case VT_DCS_IGNORE:
        break;

    case VT_OSC:
        if (b == 0x07) {
            osc_end(p);
            p->state = VT_GROUND;
        } else if (b >= 0x20) {
            str_put(p, b);
        }
        break;

    case VT_SOS_PM_APC:
        break;

    case VT_APC:
        if (b >= 0x20 && b != 0x7F) {
            str_put(p, b);
        }
        break;
    }
}

void vt_parser_feed(vt_parser *p, const uint8_t *data, size_t len)
{
    if (p == nullptr || data == nullptr) {
        return;
    }
    for (size_t i = 0; i < len; i++) {
        step(p, data[i]);
    }
}
