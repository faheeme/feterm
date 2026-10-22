#include "app/layout.h"

#include <string.h>

#include "util/mem.h"

void layout_init(te_layout *l, int divider_px)
{
    memset(l, 0, sizeof *l);
    l->divider = divider_px > 0 ? divider_px : 1;
}

static void free_tree(te_node *n)
{
    if (n == nullptr) {
        return;
    }
    free_tree(n->a);
    free_tree(n->b);
    n->a = nullptr;
    n->b = nullptr;
    free(n);
}

void layout_destroy(te_layout *l)
{
    for (size_t i = 0; i < l->ntabs; i++) {
        free_tree(l->tabs[i].root);
        l->tabs[i].root = nullptr;
    }
    l->ntabs = 0;
    l->active = 0;
}

static te_node *new_leaf(struct te_pane *p)
{
    te_node *n = te_calloc(1, sizeof *n);
    if (n != nullptr) {
        n->pane = p;
        n->ratio = 0.5f;
    }
    return n;
}

te_err layout_add_tab(te_layout *l, struct te_pane *p)
{
    if (l == nullptr || p == nullptr) {
        return TE_ERR_INVAL;
    }
    if (l->ntabs >= TE_MAX_TABS) {
        return TE_ERR_RANGE;
    }
    te_node *n = new_leaf(p);
    if (n == nullptr) {
        return TE_ERR_NOMEM;
    }
    te_tab *t = &l->tabs[l->ntabs];
    memset(t, 0, sizeof *t);
    t->root = n;
    t->focus = p;
    t->npanes = 1;
    l->active = l->ntabs;
    l->ntabs++;
    return TE_OK;
}

te_tab *layout_active(te_layout *l)
{
    return l->ntabs > 0 && l->active < l->ntabs ? &l->tabs[l->active] : nullptr;
}

struct te_pane *layout_focus(const te_layout *l)
{
    return l->ntabs > 0 && l->active < l->ntabs ? l->tabs[l->active].focus : nullptr;
}

static te_node *find_leaf(te_node *n, const struct te_pane *p)
{
    if (n == nullptr) {
        return nullptr;
    }
    if (n->a == nullptr) {
        return n->pane == p ? n : nullptr;
    }
    te_node *r = find_leaf(n->a, p);
    return r != nullptr ? r : find_leaf(n->b, p);
}

te_err layout_split(te_layout *l, struct te_pane *p, bool side_by_side)
{
    te_tab *t = layout_active(l);
    if (t == nullptr || p == nullptr) {
        return TE_ERR_INVAL;
    }
    if (t->npanes >= TE_MAX_PANES_PER_TAB) {
        return TE_ERR_RANGE;
    }
    te_node *leaf = find_leaf(t->root, t->focus);
    if (leaf == nullptr) {
        return TE_ERR_NOTFOUND;
    }
    te_node *old = new_leaf(leaf->pane);
    te_node *fresh = new_leaf(p);
    if (old == nullptr || fresh == nullptr) {
        TE_FREE(old);
        TE_FREE(fresh);
        return TE_ERR_NOMEM;
    }
    /* The leaf becomes the split node so its parent link stays valid. */
    leaf->pane = nullptr;
    leaf->a = old;
    leaf->b = fresh;
    leaf->side_by_side = side_by_side;
    leaf->ratio = 0.5f;
    old->parent = leaf;
    fresh->parent = leaf;
    t->focus = p;
    t->zoomed = false;
    t->npanes++;
    return TE_OK;
}

static te_node *first_leaf(te_node *n)
{
    while (n != nullptr && n->a != nullptr) {
        n = n->a;
    }
    return n;
}

bool layout_remove(te_layout *l, struct te_pane *p)
{
    for (size_t ti = 0; ti < l->ntabs; ti++) {
        te_tab *t = &l->tabs[ti];
        te_node *leaf = find_leaf(t->root, p);
        if (leaf == nullptr) {
            continue;
        }
        te_node *parent = leaf->parent;
        if (parent == nullptr) {
            /* Last pane of this tab: drop the tab. */
            free_tree(t->root);
            memmove(&l->tabs[ti], &l->tabs[ti + 1], (l->ntabs - ti - 1) * sizeof l->tabs[0]);
            l->ntabs--;
            if (l->ntabs == 0) {
                l->active = 0;
                return true;
            }
            if (l->active > ti || l->active >= l->ntabs) {
                l->active = l->active > 0 ? l->active - 1 : 0;
            }
            return false;
        }
        /* Replace the parent split with the sibling subtree. */
        te_node *sib = parent->a == leaf ? parent->b : parent->a;
        te_node *grand = parent->parent;
        sib->parent = grand;
        if (grand == nullptr) {
            t->root = sib;
        } else if (grand->a == parent) {
            grand->a = sib;
        } else {
            grand->b = sib;
        }
        free(leaf);
        free(parent);
        t->npanes--;
        if (t->focus == p) {
            te_node *f = first_leaf(sib);
            t->focus = f != nullptr ? f->pane : nullptr;
        }
        t->zoomed = false;
        return false;
    }
    return l->ntabs == 0;
}

int layout_tab_of(const te_layout *l, const struct te_pane *p)
{
    for (size_t i = 0; i < l->ntabs; i++) {
        if (find_leaf(l->tabs[i].root, p) != nullptr) {
            return (int)i;
        }
    }
    return -1;
}

void layout_set_focus(te_layout *l, struct te_pane *p)
{
    int ti = layout_tab_of(l, p);
    if (ti < 0) {
        return;
    }
    l->active = (size_t)ti;
    l->tabs[ti].focus = p;
}

void layout_select_tab(te_layout *l, size_t idx)
{
    if (idx < l->ntabs) {
        l->active = idx;
    }
}

void layout_cycle_tab(te_layout *l, int dir)
{
    if (l->ntabs == 0) {
        return;
    }
    if (dir > 0) {
        l->active = (l->active + 1) % l->ntabs;
    } else {
        l->active = (l->active + l->ntabs - 1) % l->ntabs;
    }
}

static size_t collect(te_node *n, struct te_pane **out, size_t max, size_t k)
{
    if (n == nullptr) {
        return k;
    }
    if (n->a == nullptr) {
        if (k < max) {
            out[k] = n->pane;
        }
        return k + 1;
    }
    k = collect(n->a, out, max, k);
    return collect(n->b, out, max, k);
}

size_t layout_tab_panes(const te_tab *t, struct te_pane **out, size_t max)
{
    size_t n = collect(t->root, out, max, 0);
    return n < max ? n : max;
}

size_t layout_all_panes(const te_layout *l, struct te_pane **out, size_t max)
{
    size_t k = 0;
    for (size_t i = 0; i < l->ntabs; i++) {
        k = collect(l->tabs[i].root, out, max, k);
    }
    return k < max ? k : max;
}

size_t layout_count_panes(const te_layout *l)
{
    size_t n = 0;
    for (size_t i = 0; i < l->ntabs; i++) {
        n += l->tabs[i].npanes;
    }
    return n;
}

void layout_cycle_focus(te_layout *l, int dir)
{
    te_tab *t = layout_active(l);
    if (t == nullptr) {
        return;
    }
    struct te_pane *list[TE_MAX_PANES_PER_TAB] = { 0 };
    size_t n = layout_tab_panes(t, list, TE_ARRAY_LEN(list));
    for (size_t i = 0; i < n; i++) {
        if (list[i] == t->focus) {
            size_t j = dir > 0 ? (i + 1) % n : (i + n - 1) % n;
            t->focus = list[j];
            t->zoomed = false;
            return;
        }
    }
}

void layout_toggle_zoom(te_layout *l)
{
    te_tab *t = layout_active(l);
    if (t != nullptr && t->npanes > 1) {
        t->zoomed = !t->zoomed;
    }
}

bool layout_resize(te_layout *l, int dx, int dy, float step)
{
    te_tab *t = layout_active(l);
    if (t == nullptr) {
        return false;
    }
    te_node *n = find_leaf(t->root, t->focus);
    bool want_side = dx != 0;
    for (te_node *p = n != nullptr ? n->parent : nullptr; p != nullptr; p = p->parent) {
        if (p->side_by_side == want_side) {
            float d = (float)(want_side ? dx : dy) * step;
            p->ratio = TE_CLAMP(p->ratio + d, TE_LAYOUT_MIN_RATIO, TE_LAYOUT_MAX_RATIO);
            return true;
        }
    }
    return false;
}

static void compute_node(te_node *n, int x, int y, int w, int h, int div)
{
    if (n == nullptr) {
        return;
    }
    n->x = x;
    n->y = y;
    n->w = w > 0 ? w : 0;
    n->h = h > 0 ? h : 0;
    if (n->a == nullptr) {
        return;
    }
    if (n->side_by_side) {
        int aw = (int)((float)(w - div) * n->ratio);
        compute_node(n->a, x, y, aw, h, div);
        compute_node(n->b, x + aw + div, y, w - aw - div, h, div);
    } else {
        int ah = (int)((float)(h - div) * n->ratio);
        compute_node(n->a, x, y, w, ah, div);
        compute_node(n->b, x, y + ah + div, w, h - ah - div, div);
    }
}

void layout_compute(te_layout *l, int x, int y, int w, int h)
{
    for (size_t i = 0; i < l->ntabs; i++) {
        te_tab *t = &l->tabs[i];
        compute_node(t->root, x, y, w, h, l->divider);
        if (t->zoomed) {
            /* The zoomed pane takes the whole tab; the others keep their split sizes. */
            te_node *leaf = find_leaf(t->root, t->focus);
            if (leaf != nullptr) {
                leaf->x = x;
                leaf->y = y;
                leaf->w = w > 0 ? w : 0;
                leaf->h = h > 0 ? h : 0;
            }
        }
    }
}

bool layout_pane_rect(const te_layout *l, const struct te_pane *p, int r[4])
{
    for (size_t i = 0; i < l->ntabs; i++) {
        const te_node *leaf = find_leaf(l->tabs[i].root, p);
        if (leaf != nullptr) {
            r[0] = leaf->x;
            r[1] = leaf->y;
            r[2] = leaf->w;
            r[3] = leaf->h;
            return true;
        }
    }
    return false;
}

size_t layout_visible_panes(const te_layout *l, struct te_pane **out, size_t max)
{
    if (l->ntabs == 0 || l->active >= l->ntabs || max == 0) {
        return 0;
    }
    const te_tab *t = &l->tabs[l->active];
    if (t->zoomed && t->focus != nullptr) {
        out[0] = t->focus;
        return 1;
    }
    return layout_tab_panes(t, out, max);
}

static te_node *leaf_at(te_node *n, int px, int py)
{
    if (n == nullptr || px < n->x || py < n->y || px >= n->x + n->w || py >= n->y + n->h) {
        return nullptr;
    }
    if (n->a == nullptr) {
        return n;
    }
    te_node *r = leaf_at(n->a, px, py);
    return r != nullptr ? r : leaf_at(n->b, px, py);
}

struct te_pane *layout_pane_at(const te_layout *l, int px, int py)
{
    if (l->ntabs == 0 || l->active >= l->ntabs) {
        return nullptr;
    }
    const te_tab *t = &l->tabs[l->active];
    if (t->zoomed) {
        return t->focus;
    }
    te_node *n = leaf_at(t->root, px, py);
    return n != nullptr ? n->pane : nullptr;
}

static void divider_rect(const te_node *n, int div, int r[4])
{
    if (n->side_by_side) {
        r[0] = n->a->x + n->a->w;
        r[1] = n->y;
        r[2] = div;
        r[3] = n->h;
    } else {
        r[0] = n->x;
        r[1] = n->a->y + n->a->h;
        r[2] = n->w;
        r[3] = div;
    }
}

static te_node *divider_find(te_node *n, int px, int py, int slop, int div)
{
    if (n == nullptr || n->a == nullptr) {
        return nullptr;
    }
    int r[4] = { 0 };
    divider_rect(n, div, r);
    if (px >= r[0] - slop && px < r[0] + r[2] + slop && py >= r[1] - slop && py < r[1] + r[3] + slop) {
        return n;
    }
    te_node *hit = divider_find(n->a, px, py, slop, div);
    return hit != nullptr ? hit : divider_find(n->b, px, py, slop, div);
}

te_node *layout_divider_at(te_layout *l, int px, int py, int slop)
{
    te_tab *t = layout_active(l);
    if (t == nullptr || t->zoomed) {
        return nullptr;
    }
    return divider_find(t->root, px, py, slop, l->divider);
}

void layout_drag(te_node *s, int px, int py)
{
    if (s == nullptr || s->a == nullptr) {
        return;
    }
    float r = s->side_by_side ? (s->w > 0 ? (float)(px - s->x) / (float)s->w : 0.5f)
                              : (s->h > 0 ? (float)(py - s->y) / (float)s->h : 0.5f);
    s->ratio = TE_CLAMP(r, TE_LAYOUT_MIN_RATIO, TE_LAYOUT_MAX_RATIO);
}

static size_t collect_dividers(const te_node *n, int div, int (*out)[4], size_t max, size_t k)
{
    if (n == nullptr || n->a == nullptr) {
        return k;
    }
    if (k < max) {
        divider_rect(n, div, out[k]);
    }
    k++;
    k = collect_dividers(n->a, div, out, max, k);
    return collect_dividers(n->b, div, out, max, k);
}

size_t layout_dividers(const te_layout *l, int (*out)[4], size_t max)
{
    if (l->ntabs == 0 || l->active >= l->ntabs || l->tabs[l->active].zoomed) {
        return 0;
    }
    size_t n = collect_dividers(l->tabs[l->active].root, l->divider, out, max, 0);
    return n < max ? n : max;
}
