#include "input/keys.h"

#include <string.h>

#include "util/mem.h"
#include "util/str.h"
#include "util/utf8.h"

/* xterm modifier parameter: 1 + shift(1) + alt(2) + ctrl(4) + super/meta(8). */
static unsigned mod_param(uint32_t mods)
{
    unsigned p = 1;
    if ((mods & TE_MOD_SHIFT) != 0) p += 1;
    if ((mods & TE_MOD_ALT) != 0)   p += 2;
    if ((mods & TE_MOD_CTRL) != 0)  p += 4;
    if ((mods & TE_MOD_SUPER) != 0) p += 8;
    return p;
}

static size_t emit(char *out, size_t outsz, const char *s)
{
    size_t n = strlen(s);
    if (n >= outsz) {
        return 0;
    }
    memcpy(out, s, n);
    out[n] = '\0';
    return n;
}

static size_t emitf_csi_letter(char *out, size_t outsz, uint32_t mods, char letter, bool app)
{
    char buf[32] = { 0 };
    te_err rc = TE_OK;
    if (mods != 0) {
        rc = str_fmt(buf, sizeof buf, "\x1b[1;%u%c", mod_param(mods), letter);
    } else if (app) {
        rc = str_fmt(buf, sizeof buf, "\x1bO%c", letter);
    } else {
        rc = str_fmt(buf, sizeof buf, "\x1b[%c", letter);
    }
    return rc == TE_OK ? emit(out, outsz, buf) : 0;
}

static size_t emitf_tilde(char *out, size_t outsz, uint32_t mods, unsigned code)
{
    char buf[32] = { 0 };
    te_err rc = mods != 0 ? str_fmt(buf, sizeof buf, "\x1b[%u;%u~", code, mod_param(mods))
                          : str_fmt(buf, sizeof buf, "\x1b[%u~", code);
    return rc == TE_OK ? emit(out, outsz, buf) : 0;
}

/* Ctrl+key to a C0 control byte, following xterm. Returns -1 if none. */
static int ctrl_byte(uint32_t cp)
{
    if (cp >= 'a' && cp <= 'z') return (int)(cp - 'a' + 1);
    if (cp >= 'A' && cp <= 'Z') return (int)(cp - 'A' + 1);
    switch (cp) {
    case '@': case ' ': case '2': return 0;
    case '[': case '3':           return 27;
    case '\\': case '4':          return 28;
    case ']': case '5':           return 29;
    case '^': case '6': case '~': return 30;
    case '_': case '7': case '/': case '-': return 31;
    case '8': case '?':           return 127;
    default:                      return -1;
    }
}

/* ------------------------------------------------------------------------- */
/* Kitty keyboard protocol */

typedef struct kitty_key {
    unsigned code;     /* number in the escape sequence */
    char     final;    /* 'u', '~', or a letter for CSI 1;mods X forms */
} kitty_key;

static bool kitty_functional(te_key key, kitty_key *k)
{
    switch (key) {
    case TE_KEY_ESCAPE:    *k = (kitty_key){ 27, 'u' }; return true;
    case TE_KEY_ENTER:     *k = (kitty_key){ 13, 'u' }; return true;
    case TE_KEY_TAB:       *k = (kitty_key){ 9, 'u' }; return true;
    case TE_KEY_BACKSPACE: *k = (kitty_key){ 127, 'u' }; return true;
    case TE_KEY_INSERT:    *k = (kitty_key){ 2, '~' }; return true;
    case TE_KEY_DELETE:    *k = (kitty_key){ 3, '~' }; return true;
    case TE_KEY_PAGE_UP:   *k = (kitty_key){ 5, '~' }; return true;
    case TE_KEY_PAGE_DOWN: *k = (kitty_key){ 6, '~' }; return true;
    case TE_KEY_UP:        *k = (kitty_key){ 1, 'A' }; return true;
    case TE_KEY_DOWN:      *k = (kitty_key){ 1, 'B' }; return true;
    case TE_KEY_RIGHT:     *k = (kitty_key){ 1, 'C' }; return true;
    case TE_KEY_LEFT:      *k = (kitty_key){ 1, 'D' }; return true;
    case TE_KEY_HOME:      *k = (kitty_key){ 1, 'H' }; return true;
    case TE_KEY_END:       *k = (kitty_key){ 1, 'F' }; return true;
    case TE_KEY_F1:        *k = (kitty_key){ 1, 'P' }; return true;
    case TE_KEY_F2:        *k = (kitty_key){ 1, 'Q' }; return true;
    case TE_KEY_F3:        *k = (kitty_key){ 13, '~' }; return true;
    case TE_KEY_F4:        *k = (kitty_key){ 1, 'S' }; return true;
    case TE_KEY_F5:        *k = (kitty_key){ 15, '~' }; return true;
    case TE_KEY_F6:        *k = (kitty_key){ 17, '~' }; return true;
    case TE_KEY_F7:        *k = (kitty_key){ 18, '~' }; return true;
    case TE_KEY_F8:        *k = (kitty_key){ 19, '~' }; return true;
    case TE_KEY_F9:        *k = (kitty_key){ 20, '~' }; return true;
    case TE_KEY_F10:       *k = (kitty_key){ 21, '~' }; return true;
    case TE_KEY_F11:       *k = (kitty_key){ 23, '~' }; return true;
    case TE_KEY_F12:       *k = (kitty_key){ 24, '~' }; return true;
    case TE_KEY_CHAR:
    case TE_KEY_NONE:
    default:               return false;
    }
}

static size_t kitty_emit(char *out, size_t outsz, kitty_key k, unsigned mods, te_keyev ev, bool events)
{
    char buf[48] = { 0 };
    char evs[4] = { 0 };
    if (events && ev != TE_KEYEV_PRESS) {
        (void)str_fmt(evs, sizeof evs, ":%u", (unsigned)ev);
    }
    bool need_mods = mods > 1 || evs[0] != '\0';
    te_err rc = TE_OK;
    if (k.final == 'u' || k.final == '~') {
        rc = need_mods ? str_fmt(buf, sizeof buf, "\x1b[%u;%u%s%c", k.code, mods, evs, k.final)
                       : str_fmt(buf, sizeof buf, "\x1b[%u%c", k.code, k.final);
    } else {
        rc = need_mods ? str_fmt(buf, sizeof buf, "\x1b[1;%u%s%c", mods, evs, k.final)
                       : str_fmt(buf, sizeof buf, "\x1b[%c", k.final);
    }
    return rc == TE_OK ? emit(out, outsz, buf) : 0;
}

static size_t legacy_encode(te_key key, uint32_t cp, uint32_t mods, const te_modes *m, char *out, size_t outsz);

static size_t kitty_encode(te_key key, uint32_t cp, uint32_t mods, te_keyev ev, const te_modes *m, char *out,
                           size_t outsz)
{
    uint8_t f = m->kitty_flags;
    bool events = (f & 2u) != 0;
    bool all = (f & 8u) != 0;
    if (ev == TE_KEYEV_RELEASE && !events) {
        return 0;
    }
    unsigned mp = mod_param(mods);
    kitty_key k = { 0, 'u' };
    if (key == TE_KEY_CHAR) {
        bool text_mods = (mods & (TE_MOD_CTRL | TE_MOD_ALT | TE_MOD_SUPER)) == 0;
        if (!all && text_mods) {
            return 0;                       /* plain text: delivered as text input */
        }
        uint32_t c = cp;
        if (c >= 'A' && c <= 'Z') {
            c = c - 'A' + 'a';              /* the key, not the shifted character */
        }
        k.code = c;
        return kitty_emit(out, outsz, k, mp, ev, events);
    }
    if (!kitty_functional(key, &k)) {
        return 0;
    }
    bool enter_tab_bs = key == TE_KEY_ENTER || key == TE_KEY_TAB || key == TE_KEY_BACKSPACE;
    if (enter_tab_bs && !all) {
        /* Kept legacy (so "reset" can still be typed) unless modified; no release events. */
        if (ev == TE_KEYEV_RELEASE) {
            return 0;
        }
        if (mp == 1 || (key == TE_KEY_TAB && mp == 2)) {
            return legacy_encode(key, cp, mods, m, out, outsz);
        }
        return kitty_emit(out, outsz, k, mp, ev, events);
    }
    if (k.final != 'u' && mp == 1 && !(events && ev != TE_KEYEV_PRESS) && !all) {
        return legacy_encode(key, cp, mods, m, out, outsz);   /* arrows etc. unmodified: as before */
    }
    return kitty_emit(out, outsz, k, mp, ev, events);
}

size_t keys_encode(te_key key, uint32_t cp, uint32_t mods, const te_modes *m, char *out, size_t outsz)
{
    return keys_encode_ev(key, cp, mods, TE_KEYEV_PRESS, m, out, outsz);
}

size_t keys_encode_ev(te_key key, uint32_t cp, uint32_t mods, te_keyev ev, const te_modes *m, char *out,
                      size_t outsz)
{
    if (out == nullptr || outsz < 16 || m == nullptr) {
        return 0;
    }
    out[0] = '\0';
    if (m->kitty_flags != 0) {
        return kitty_encode(key, cp, mods, ev, m, out, outsz);
    }
    if (ev == TE_KEYEV_RELEASE) {
        return 0;
    }
    return legacy_encode(key, cp, mods, m, out, outsz);
}

static size_t legacy_encode(te_key key, uint32_t cp, uint32_t mods, const te_modes *m, char *out, size_t outsz)
{
    if (out == nullptr || outsz < 16 || m == nullptr) {
        return 0;
    }
    out[0] = '\0';
    bool alt = (mods & TE_MOD_ALT) != 0;
    bool ctrl = (mods & TE_MOD_CTRL) != 0;
    bool shift = (mods & TE_MOD_SHIFT) != 0;

    switch (key) {
    case TE_KEY_UP:    return emitf_csi_letter(out, outsz, mods, 'A', m->app_cursor);
    case TE_KEY_DOWN:  return emitf_csi_letter(out, outsz, mods, 'B', m->app_cursor);
    case TE_KEY_RIGHT: return emitf_csi_letter(out, outsz, mods, 'C', m->app_cursor);
    case TE_KEY_LEFT:  return emitf_csi_letter(out, outsz, mods, 'D', m->app_cursor);
    case TE_KEY_HOME:  return emitf_csi_letter(out, outsz, mods, 'H', m->app_cursor);
    case TE_KEY_END:   return emitf_csi_letter(out, outsz, mods, 'F', m->app_cursor);
    case TE_KEY_INSERT:    return emitf_tilde(out, outsz, mods, 2);
    case TE_KEY_DELETE:    return emitf_tilde(out, outsz, mods, 3);
    case TE_KEY_PAGE_UP:   return emitf_tilde(out, outsz, mods, 5);
    case TE_KEY_PAGE_DOWN: return emitf_tilde(out, outsz, mods, 6);
    case TE_KEY_F1: return emitf_csi_letter(out, outsz, mods, 'P', true);
    case TE_KEY_F2: return emitf_csi_letter(out, outsz, mods, 'Q', true);
    case TE_KEY_F3: return emitf_csi_letter(out, outsz, mods, 'R', true);
    case TE_KEY_F4: return emitf_csi_letter(out, outsz, mods, 'S', true);
    case TE_KEY_F5:  return emitf_tilde(out, outsz, mods, 15);
    case TE_KEY_F6:  return emitf_tilde(out, outsz, mods, 17);
    case TE_KEY_F7:  return emitf_tilde(out, outsz, mods, 18);
    case TE_KEY_F8:  return emitf_tilde(out, outsz, mods, 19);
    case TE_KEY_F9:  return emitf_tilde(out, outsz, mods, 20);
    case TE_KEY_F10: return emitf_tilde(out, outsz, mods, 21);
    case TE_KEY_F11: return emitf_tilde(out, outsz, mods, 23);
    case TE_KEY_F12: return emitf_tilde(out, outsz, mods, 24);
    case TE_KEY_ENTER:
        if (m->newline) {
            return emit(out, outsz, alt ? "\x1b\r\n" : "\r\n");
        }
        return emit(out, outsz, alt ? "\x1b\r" : "\r");
    case TE_KEY_ESCAPE:
        return emit(out, outsz, "\x1b");
    case TE_KEY_BACKSPACE:
        if (ctrl) {
            return emit(out, outsz, alt ? "\x1b\x08" : "\x08");
        }
        return emit(out, outsz, alt ? "\x1b\x7f" : "\x7f");
    case TE_KEY_TAB:
        if (shift) {
            return emit(out, outsz, "\x1b[Z");
        }
        return emit(out, outsz, alt ? "\x1b\t" : "\t");
    case TE_KEY_CHAR:
        break;
    case TE_KEY_NONE:
    default:
        return 0;
    }

    /* Printable keys only need special handling with Ctrl or Alt. */
    if (ctrl) {
        int c = ctrl_byte(cp);
        if (c < 0) {
            return 0;
        }
        size_t n = 0;
        if (alt) {
            out[n++] = '\x1b';
        }
        out[n++] = (char)c;
        out[n] = '\0';
        return n;
    }
    if (alt) {
        uint32_t ch = cp;
        if (shift && ch >= 'a' && ch <= 'z') {
            ch = ch - 'a' + 'A';
        }
        out[0] = '\x1b';
        size_t n = utf8_encode(ch, out + 1, outsz - 2);
        if (n == 0) {
            return 0;
        }
        out[n + 1] = '\0';
        return n + 1;
    }
    return 0;
}

char *keys_prepare_paste(const char *text, bool bracketed, bool sanitize, size_t *len)
{
    static const char start[] = "\x1b[200~";
    static const char end[] = "\x1b[201~";
    if (text == nullptr || len == nullptr) {
        return nullptr;
    }
    *len = 0;
    size_t in = strlen(text);
    size_t cap = 0;
    if (ckd_add(&cap, in, sizeof start + sizeof end + 1)) {
        return nullptr;
    }
    char *out = te_malloc(cap);
    if (out == nullptr) {
        return nullptr;
    }
    size_t o = 0;
    if (bracketed) {
        memcpy(out, start, sizeof start - 1);
        o += sizeof start - 1;
    }
    for (size_t i = 0; i < in; i++) {
        unsigned char c = (unsigned char)text[i];
        if (c == '\r' && i + 1 < in && text[i + 1] == '\n') {
            continue;                       /* CRLF -> CR */
        }
        if (c == '\n') {
            c = '\r';
        }
        /*
         * ESC is always removed inside bracketed paste so the payload cannot
         * close the bracket early and run commands. With sanitising on, every
         * other C0 control except TAB and CR is removed as well.
         */
        if (c == 0x1b && (bracketed || sanitize)) {
            continue;
        }
        if (sanitize && ((c < 0x20 && c != '\t' && c != '\r') || c == 0x7f)) {
            continue;
        }
        /* C1 controls (U+0080..U+009F) are CSI/OSC introducers too; a terminal in
         * 8-bit mode would act on them. They arrive as UTF-8 0xC2 0x80..0x9F. */
        if (sanitize && c == 0xC2 && i + 1 < in && (unsigned char)text[i + 1] >= 0x80 &&
            (unsigned char)text[i + 1] <= 0x9F) {
            i++;
            continue;
        }
        out[o++] = (char)c;
    }
    if (bracketed) {
        memcpy(out + o, end, sizeof end - 1);
        o += sizeof end - 1;
    }
    out[o] = '\0';
    *len = o;
    return out;
}
