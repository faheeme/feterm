/*
 * Tabs and split panes. Each tab holds a binary tree: leaves are panes,
 * inner nodes split their area side by side ("vertical" divider) or one
 * above the other, at an adjustable ratio. Pure geometry: no drawing here.
 */
#ifndef TE_LAYOUT_H
#define TE_LAYOUT_H

#include <stdbool.h>
#include "common.h"

#define TE_MAX_TABS           32
#define TE_MAX_PANES_PER_TAB  16
#define TE_LAYOUT_MIN_RATIO   0.1f
#define TE_LAYOUT_MAX_RATIO   0.9f

struct te_pane;

typedef struct te_node te_node;
struct te_node {
    te_node        *parent;
    te_node        *a, *b;        /* children of a split; nullptr for a leaf */
    struct te_pane *pane;         /* leaf only */
    bool            side_by_side; /* split: a | b (else a above b) */
    float           ratio;        /* share of the split given to a */
    int             x, y, w, h;
};

typedef struct te_tab {
    te_node        *root;
    struct te_pane *focus;
    bool            zoomed;       /* focus fills the whole tab */
    size_t          npanes;
} te_tab;

typedef struct te_layout {
    te_tab tabs[TE_MAX_TABS];
    size_t ntabs;
    size_t active;
    int    divider;               /* divider thickness in pixels */
} te_layout;

void layout_init(te_layout *l, int divider_px);
/* Frees the tree nodes (not the panes). */
void layout_destroy(te_layout *l);

/* Append a tab holding pane p and make it active. */
[[nodiscard]] te_err layout_add_tab(te_layout *l, struct te_pane *p);
/* Split the active tab's focused pane; p goes right of / below it and gets focus. */
[[nodiscard]] te_err layout_split(te_layout *l, struct te_pane *p, bool side_by_side);
/* Remove a pane from wherever it is. Returns true when no tabs remain. */
bool layout_remove(te_layout *l, struct te_pane *p);

te_tab *layout_active(te_layout *l);
struct te_pane *layout_focus(const te_layout *l);
/* Focus p, switching to its tab. */
void layout_set_focus(te_layout *l, struct te_pane *p);
void layout_select_tab(te_layout *l, size_t idx);
void layout_cycle_tab(te_layout *l, int dir);
void layout_cycle_focus(te_layout *l, int dir);
void layout_toggle_zoom(te_layout *l);
/* Move the nearest divider of the matching orientation by step (fraction of the split). */
bool layout_resize(te_layout *l, int dx, int dy, float step);

/* Compute rectangles for every pane in every tab within the given area. */
void layout_compute(te_layout *l, int x, int y, int w, int h);

/* Rectangle (x, y, w, h) computed for pane p. False if p is not in the layout. */
[[nodiscard]] bool layout_pane_rect(const te_layout *l, const struct te_pane *p, int r[4]);

size_t layout_tab_panes(const te_tab *t, struct te_pane **out, size_t max);
size_t layout_all_panes(const te_layout *l, struct te_pane **out, size_t max);
size_t layout_count_panes(const te_layout *l);
/* Panes visible in the active tab (only the focus when zoomed). */
size_t layout_visible_panes(const te_layout *l, struct te_pane **out, size_t max);
struct te_pane *layout_pane_at(const te_layout *l, int px, int py);
/* Index of the tab containing p, or -1. */
int layout_tab_of(const te_layout *l, const struct te_pane *p);

/* Divider under a point (within slop pixels), or nullptr. */
te_node *layout_divider_at(te_layout *l, int px, int py, int slop);
/* Move a divider so that it sits at the pointer. */
void layout_drag(te_node *split, int px, int py);
/* Divider rectangles of the active tab (x, y, w, h). */
size_t layout_dividers(const te_layout *l, int (*out)[4], size_t max);

#endif
