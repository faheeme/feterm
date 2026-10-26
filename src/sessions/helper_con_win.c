/* Console side of feterm-helper.exe (see helper_con.h). */
#include "sessions/helper_con.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>

#include "util/str.h"

#define RECORDS 128   /* input records taken per read */

static void query_size(te_hcon *c)
{
    CONSOLE_SCREEN_BUFFER_INFO info;
    if (GetConsoleScreenBufferInfo(c->out, &info)) {
        int w = info.srWindow.Right - info.srWindow.Left + 1;
        int h = info.srWindow.Bottom - info.srWindow.Top + 1;
        c->cols = (uint16_t)TE_CLAMP(w, 1, UINT16_MAX);
        c->rows = (uint16_t)TE_CLAMP(h, 1, UINT16_MAX);
    }
}

bool hcon_open(te_hcon *c)
{
    memset(c, 0, sizeof *c);
    c->in = GetStdHandle(STD_INPUT_HANDLE);
    c->out = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD in_mode = 0;
    DWORD out_mode = 0;
    if (c->in == nullptr || c->in == INVALID_HANDLE_VALUE || c->out == nullptr || c->out == INVALID_HANDLE_VALUE ||
        !GetConsoleMode(c->in, &in_mode) || !GetConsoleMode(c->out, &out_mode)) {
        c->in = nullptr;
        c->out = nullptr;
        return false;
    }
    (void)SetConsoleCP(CP_UTF8);
    (void)SetConsoleOutputCP(CP_UTF8);
    /* Raw: no line editing, no echo, Ctrl+C is a byte; keys arrive as VT sequences. */
    in_mode = ENABLE_VIRTUAL_TERMINAL_INPUT | ENABLE_WINDOW_INPUT;
    out_mode = ENABLE_PROCESSED_OUTPUT | ENABLE_VIRTUAL_TERMINAL_PROCESSING | DISABLE_NEWLINE_AUTO_RETURN;
    if (!SetConsoleMode(c->in, in_mode) || !SetConsoleMode(c->out, out_mode)) {
        c->in = nullptr;
        c->out = nullptr;
        return false;
    }
    query_size(c);
    /* ConPTY titles the pane with this program's path; an empty title lets FeTerm show the session name. */
    (void)SetConsoleTitleW(L"");
    wchar_t name[64] = { 0 };
    if (hcon_break_name(GetCurrentProcessId(), name, TE_ARRAY_LEN(name)) == TE_OK) {
        c->brk = CreateEventW(nullptr, FALSE, FALSE, name);
    }
    return true;
}

void hcon_close(te_hcon *c)
{
    if (c->brk != nullptr) {
        CloseHandle(c->brk);
        c->brk = nullptr;
    }
    c->in = nullptr;    /* the standard handles are not ours to close */
    c->out = nullptr;
}

bool hcon_write(te_hcon *c, const void *data, size_t len)
{
    const uint8_t *p = data;
    while (len > 0) {
        DWORD want = (DWORD)TE_MIN(len, (size_t)65536);
        DWORD put = 0;
        if (!WriteFile(c->out, p, want, &put, nullptr) || put == 0) {
            return false;
        }
        p += put;
        len -= put;
    }
    return true;
}

void hcon_say(te_hcon *c, const char *msg)
{
    (void)hcon_write(c, msg, strlen(msg));
}

void hcon_sayf(te_hcon *c, const char *fmt, ...)
{
    char buf[512] = { 0 };
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (n > 0) {
        size_t len = (size_t)n < sizeof buf ? (size_t)n : sizeof buf - 1;
        (void)hcon_write(c, buf, len);
    }
}

/* Appends code point cp as UTF-8 if it fits. */
static bool put_utf8(uint8_t *buf, size_t cap, size_t *len, uint32_t cp)
{
    uint8_t tmp[4];
    size_t n = 0;
    if (cp < 0x80) {
        tmp[n++] = (uint8_t)cp;
    } else if (cp < 0x800) {
        tmp[n++] = (uint8_t)(0xC0 | (cp >> 6));
        tmp[n++] = (uint8_t)(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        tmp[n++] = (uint8_t)(0xE0 | (cp >> 12));
        tmp[n++] = (uint8_t)(0x80 | ((cp >> 6) & 0x3F));
        tmp[n++] = (uint8_t)(0x80 | (cp & 0x3F));
    } else {
        tmp[n++] = (uint8_t)(0xF0 | (cp >> 18));
        tmp[n++] = (uint8_t)(0x80 | ((cp >> 12) & 0x3F));
        tmp[n++] = (uint8_t)(0x80 | ((cp >> 6) & 0x3F));
        tmp[n++] = (uint8_t)(0x80 | (cp & 0x3F));
    }
    if (*len + n > cap) {
        return false;
    }
    memcpy(buf + *len, tmp, n);
    *len += n;
    return true;
}

ssize_t hcon_read(te_hcon *c, uint8_t *buf, size_t cap, bool *resized)
{
    *resized = false;
    DWORD waiting = 0;
    if (!GetNumberOfConsoleInputEvents(c->in, &waiting)) {
        return -1;
    }
    if (waiting == 0) {
        return 0;
    }
    /* Every record yields at most 4 bytes, so taking cap / 4 of them never overflows buf. */
    INPUT_RECORD rec[RECORDS];
    DWORD want = (DWORD)TE_MIN((size_t)RECORDS, cap / 4);
    DWORD got = 0;
    if (want == 0 || !ReadConsoleInputW(c->in, rec, want, &got)) {
        return -1;
    }
    size_t len = 0;
    for (DWORD i = 0; i < got; i++) {
        if (rec[i].EventType == WINDOW_BUFFER_SIZE_EVENT) {
            *resized = true;
            continue;
        }
        if (rec[i].EventType != KEY_EVENT || !rec[i].Event.KeyEvent.bKeyDown) {
            continue;
        }
        uint32_t u = rec[i].Event.KeyEvent.uChar.UnicodeChar;
        if (u == 0) {
            continue;   /* a key without a character (Shift alone); VT input sends sequences as chars */
        }
        if (u >= 0xD800 && u <= 0xDBFF) {
            c->surrogate = (uint16_t)u;
            continue;
        }
        uint32_t cp = u;
        if (u >= 0xDC00 && u <= 0xDFFF) {
            if (c->surrogate == 0) {
                continue;   /* a stray low half */
            }
            cp = 0x10000u + (((uint32_t)c->surrogate - 0xD800u) << 10) + (u - 0xDC00u);
        }
        c->surrogate = 0;
        if (!put_utf8(buf, cap, &len, cp)) {
            break;   /* cannot happen: cap / 4 records at most */
        }
    }
    if (*resized) {
        query_size(c);
    }
    return (ssize_t)len;
}
