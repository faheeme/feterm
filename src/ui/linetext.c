#include "ui/linetext.h"

#include "util/utf8.h"

size_t linetext_append(const te_cell *cells, uint16_t n, uint32_t row, char *buf, te_textpos *map,
                       size_t len, size_t cap)
{
    if (cells == nullptr || buf == nullptr || map == nullptr || cap == 0 || len >= cap) {
        if (buf != nullptr && cap > 0) {
            buf[cap - 1] = '\0';
        }
        return len;
    }
    for (uint16_t x = 0; x < n; x++) {
        const te_cell *c = &cells[x];
        if ((c->attrs & TE_ATTR_SPACER) != 0) {
            continue;
        }
        uint32_t cps[1 + TE_CELL_MAX_COMBINING] = { c->cp != 0 ? c->cp : ' ', c->comb[0], c->comb[1] };
        for (size_t k = 0; k < TE_ARRAY_LEN(cps); k++) {
            if (k > 0 && cps[k] == 0) {
                break;
            }
            char enc[4] = { 0 };
            size_t m = utf8_encode(cps[k], enc, sizeof enc);
            if (m == 0 || m > sizeof enc) {
                enc[0] = '?';
                m = 1;
            }
            if (len + m + 1 > cap) {
                buf[len] = '\0';
                return len;
            }
            for (size_t b = 0; b < m; b++) {
                buf[len] = enc[b];
                map[len].row = row;
                map[len].col = x;
                len++;
            }
        }
    }
    buf[len] = '\0';
    return len;
}

uint16_t linetext_trimmed(const te_cell *cells, uint16_t n)
{
    while (n > 0 && cells[n - 1].cp == 0) {
        n--;
    }
    return n;
}
