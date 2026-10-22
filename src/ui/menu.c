#include "ui/menu.h"

#include <string.h>

#include "util/str.h"

static float row_h(const te_painter *p)
{
    return p->cell_h * 1.5f;
}

static float sep_h(const te_painter *p)
{
    return p->cell_h * 0.6f;
}

te_menu_item menu_item(int id, const char *label, const char *hint, bool enabled)
{
    te_menu_item it = { .id = id, .enabled = enabled };
    (void)str_copy(it.label, sizeof it.label, label != nullptr ? label : "");
    (void)str_copy(it.hint, sizeof it.hint, hint != nullptr ? hint : "");
    return it;
}

void menu_open(te_menu *m, const te_menu_item *items, size_t n, float x, float y, float win_w, float win_h,
               const te_painter *p)
{
    memset(m, 0, sizeof *m);
    m->n = n < TE_MENU_MAX_ITEMS ? n : TE_MENU_MAX_ITEMS;
    memcpy(m->items, items, m->n * sizeof *items);
    m->hover = -1;
    size_t label_cols = 8;
    size_t hint_cols = 0;
    float h = p->cell_h * 0.5f;   /* top and bottom padding */
    for (size_t i = 0; i < m->n; i++) {
        label_cols = TE_MAX(label_cols, ui_text_cols(m->items[i].label));
        hint_cols = TE_MAX(hint_cols, ui_text_cols(m->items[i].hint));
        h += row_h(p) + (m->items[i].separator_before && i > 0 ? sep_h(p) : 0.0f);
    }
    float w = (float)(label_cols + 4 + (hint_cols > 0 ? hint_cols + 3 : 0)) * p->cell_w;
    m->w = w < win_w ? w : win_w;
    m->h = h < win_h ? h : win_h;
    /* Open down-right of the pointer; flip or slide to stay in the window. */
    m->x = x + m->w > win_w ? TE_MAX(0.0f, x - m->w) : x;
    m->y = y + m->h > win_h ? TE_MAX(0.0f, win_h - m->h) : y;
    m->open = m->n > 0;
}

void menu_close(te_menu *m)
{
    m->open = false;
    m->hover = -1;
}

bool menu_contains(const te_menu *m, float x, float y)
{
    return m->open && x >= m->x && x < m->x + m->w && y >= m->y && y < m->y + m->h;
}

int menu_index_at(const te_menu *m, float x, float y, const te_painter *p)
{
    if (!menu_contains(m, x, y)) {
        return -1;
    }
    float top = m->y + p->cell_h * 0.25f;
    for (size_t i = 0; i < m->n; i++) {
        if (m->items[i].separator_before && i > 0) {
            top += sep_h(p);
        }
        if (y >= top && y < top + row_h(p)) {
            return (int)i;
        }
        top += row_h(p);
    }
    return -1;
}

void menu_motion(te_menu *m, float x, float y, const te_painter *p)
{
    if (m->open) {
        m->hover = menu_index_at(m, x, y, p);
    }
}

int menu_click(te_menu *m, float x, float y, const te_painter *p)
{
    if (!m->open) {
        return TE_MENU_NOTHING;
    }
    if (!menu_contains(m, x, y)) {
        menu_close(m);
        return TE_MENU_CLOSED;
    }
    int i = menu_index_at(m, x, y, p);
    if (i < 0 || !m->items[i].enabled) {
        return TE_MENU_NOTHING;   /* padding, separator or a disabled item: stay open */
    }
    menu_close(m);
    return m->items[i].id;
}

static int step(const te_menu *m, int from, int dir)
{
    int n = (int)m->n;
    for (int k = 1; k <= n; k++) {
        int i = ((from + dir * k) % n + n) % n;
        if (m->items[i].enabled) {
            return i;
        }
    }
    return -1;
}

int menu_key(te_menu *m, te_key key)
{
    if (!m->open || m->n == 0) {
        return TE_MENU_NOTHING;
    }
    switch (key) {
    case TE_KEY_ESCAPE:
        menu_close(m);
        return TE_MENU_CLOSED;
    case TE_KEY_UP:
        m->hover = step(m, m->hover < 0 ? 0 : m->hover, -1);
        return TE_MENU_NOTHING;
    case TE_KEY_DOWN:
        m->hover = step(m, m->hover < 0 ? -1 : m->hover, 1);
        return TE_MENU_NOTHING;
    case TE_KEY_HOME:
        m->hover = step(m, -1, 1);
        return TE_MENU_NOTHING;
    case TE_KEY_END:
        m->hover = step(m, (int)m->n, -1);
        return TE_MENU_NOTHING;
    case TE_KEY_ENTER:
        if (m->hover >= 0 && (size_t)m->hover < m->n && m->items[m->hover].enabled) {
            int id = m->items[m->hover].id;
            menu_close(m);
            return id;
        }
        return TE_MENU_NOTHING;
    default:
        return TE_MENU_NOTHING;
    }
}

void menu_draw(const te_menu *m, const te_painter *p, const te_ui_colors *c)
{
    if (!m->open) {
        return;
    }
    float s = p->scale > 0.0f ? p->scale : 1.0f;
    /* A soft shadow, then the panel. */
    ui_rect(p, TE_LAYER_OVERLAY, m->x + 3.0f * s, m->y + 4.0f * s, m->w, m->h, 0x000000, 0.35f);
    ui_rect(p, TE_LAYER_OVERLAY, m->x, m->y, m->w, m->h, c->bg_alt, 1.0f);
    ui_frame(p, TE_LAYER_OVERLAY, m->x, m->y, m->w, m->h, c->border);
    float top = m->y + p->cell_h * 0.25f;
    size_t cols = (size_t)(m->w / p->cell_w);
    for (size_t i = 0; i < m->n; i++) {
        const te_menu_item *it = &m->items[i];
        if (it->separator_before && i > 0) {
            ui_rect(p, TE_LAYER_OVERLAY, m->x + p->cell_w * 0.5f, top + sep_h(p) / 2.0f, m->w - p->cell_w, s,
                    c->border, 1.0f);
            top += sep_h(p);
        }
        bool hot = (int)i == m->hover && it->enabled;
        if (hot) {
            ui_rect(p, TE_LAYER_OVERLAY, m->x + 2.0f * s, top, m->w - 4.0f * s, row_h(p),
                    it->danger ? c->danger : c->sel_bg, 1.0f);
        }
        uint32_t fg = !it->enabled ? c->muted : (hot ? (it->danger ? c->bg : c->sel_fg) : (it->danger ? c->danger : c->fg));
        float ty = top + (row_h(p) - p->cell_h) / 2.0f;
        size_t hint_cols = ui_text_cols(it->hint);
        ui_text(p, TE_LAYER_OVERLAY, m->x + p->cell_w * 1.5f, ty, it->label, cols > 3 + hint_cols ? cols - 3 - hint_cols : 1,
                fg, false);
        if (hint_cols > 0 && cols > hint_cols + 2) {
            ui_text(p, TE_LAYER_OVERLAY, m->x + m->w - (float)(hint_cols + 1) * p->cell_w - p->cell_w * 0.5f, ty,
                    it->hint, hint_cols, hot ? fg : c->muted, false);
        }
        top += row_h(p);
    }
}
