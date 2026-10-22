#include "ui/form.h"

#include <string.h>

#include "util/str.h"
#include "util/utf8.h"

#define ROW_H(f, p) ((f)->row_h > 0.0f ? (f)->row_h : (p)->cell_h * 1.7f)
#define PAD(p)      ((p)->cell_w * 2.0f)

void form_init(te_form *f, int purpose, const char *title, const char *ok_label)
{
    memset(f, 0, sizeof *f);
    f->purpose = purpose;
    (void)str_copy(f->title, sizeof f->title, title != nullptr ? title : "");
    (void)str_copy(f->ok_label, sizeof f->ok_label, ok_label != nullptr ? ok_label : "OK");
    f->open = true;
}

te_form_field *form_add(te_form *f, int id, te_field_kind kind, const char *label, const char *value)
{
    if (f->n >= TE_FORM_MAX_FIELDS) {
        return nullptr;
    }
    te_form_field *fl = &f->f[f->n++];
    memset(fl, 0, sizeof *fl);
    fl->id = id;
    fl->kind = kind;
    fl->maxlen = TE_FORM_VALUE_MAX - 1;
    (void)str_copy(fl->label, sizeof fl->label, label != nullptr ? label : "");
    if (value != nullptr) {
        (void)str_copy(fl->value, sizeof fl->value, value);
        fl->cursor = strlen(fl->value);
    }
    return fl;
}

te_form_field *form_get(te_form *f, int id)
{
    for (size_t i = 0; i < f->n; i++) {
        if (f->f[i].id == id) {
            return &f->f[i];
        }
    }
    return nullptr;
}

const te_form_field *form_cget(const te_form *f, int id)
{
    for (size_t i = 0; i < f->n; i++) {
        if (f->f[i].id == id) {
            return &f->f[i];
        }
    }
    return nullptr;
}

void form_set_choices(te_form_field *fl, const char *const *choices, size_t n, size_t selected)
{
    if (fl == nullptr) {
        return;
    }
    fl->choices = choices;
    fl->nchoices = n;
    fl->choice = selected < n ? selected : 0;
}

void form_show(te_form *f, int id, bool visible)
{
    te_form_field *fl = form_get(f, id);
    if (fl != nullptr) {
        fl->hidden = !visible;
    }
    if (f->focus >= 0 && (size_t)f->focus < f->n && f->f[f->focus].hidden) {
        form_focus_first(f);
    }
}

void form_focus_first(te_form *f)
{
    for (size_t i = 0; i < f->n; i++) {
        if (!f->f[i].hidden) {
            f->focus = (int)i;
            return;
        }
    }
    f->focus = (int)f->n;   /* no fields: the OK button */
}

void form_set_error(te_form *f, const char *msg)
{
    (void)str_copy(f->error, sizeof f->error, msg != nullptr ? msg : "");
}

const char *form_value(const te_form *f, int id)
{
    const te_form_field *fl = form_cget(f, id);
    return fl != nullptr ? fl->value : "";
}

size_t form_choice(const te_form *f, int id)
{
    const te_form_field *fl = form_cget(f, id);
    return fl != nullptr ? fl->choice : 0;
}

bool form_checked(const te_form *f, int id)
{
    const te_form_field *fl = form_cget(f, id);
    return fl != nullptr && fl->checked;
}

/* ------------------------------------------------------------------------- */
/* Text editing */

static bool is_cont(char c)
{
    return ((unsigned char)c & 0xC0u) == 0x80u;
}

bool field_insert(te_form_field *fl, const char *utf8)
{
    /* Keep only complete, printable characters; numbers take digits only. */
    char clean[TE_FORM_VALUE_MAX] = { 0 };
    size_t k = 0;
    te_utf8_dec d = { 0 };
    size_t start = 0;
    for (size_t i = 0; utf8[i] != '\0' && k + 4 < sizeof clean; i++) {
        uint32_t cp = 0;
        te_utf8_res r = utf8_feed(&d, (uint8_t)utf8[i], &cp);
        if (r == TE_UTF8_MORE) {
            continue;
        }
        if (r == TE_UTF8_DONE && cp >= 0x20 && cp != 0x7F && !(cp >= 0x80 && cp <= 0x9F) &&
            (fl->kind != TE_FIELD_NUMBER || (cp >= '0' && cp <= '9'))) {
            size_t len = i + 1 - start;
            memcpy(clean + k, utf8 + start, len);
            k += len;
        }
        start = i + 1;
    }
    clean[k] = '\0';
    size_t cur = strlen(fl->value);
    if (k == 0 || cur + k > fl->maxlen || cur + k >= sizeof fl->value || fl->cursor > cur) {
        return false;
    }
    memmove(fl->value + fl->cursor + k, fl->value + fl->cursor, cur - fl->cursor + 1);
    memcpy(fl->value + fl->cursor, clean, k);
    fl->cursor += k;
    return true;
}

void field_backspace(te_form_field *fl)
{
    size_t cur = strlen(fl->value);
    if (fl->cursor == 0 || fl->cursor > cur) {
        return;
    }
    size_t s = fl->cursor - 1;
    while (s > 0 && is_cont(fl->value[s])) {
        s--;
    }
    memmove(fl->value + s, fl->value + fl->cursor, cur - fl->cursor + 1);
    fl->cursor = s;
}

void field_delete(te_form_field *fl)
{
    size_t cur = strlen(fl->value);
    if (fl->cursor >= cur) {
        return;
    }
    size_t e = fl->cursor + 1;
    while (e < cur && is_cont(fl->value[e])) {
        e++;
    }
    memmove(fl->value + fl->cursor, fl->value + e, cur - e + 1);
}

void field_move(te_form_field *fl, int dir)
{
    size_t cur = strlen(fl->value);
    if (dir < 0 && fl->cursor > 0) {
        fl->cursor--;
        while (fl->cursor > 0 && is_cont(fl->value[fl->cursor])) {
            fl->cursor--;
        }
    } else if (dir > 0 && fl->cursor < cur) {
        fl->cursor++;
        while (fl->cursor < cur && is_cont(fl->value[fl->cursor])) {
            fl->cursor++;
        }
    }
}

/* ------------------------------------------------------------------------- */
/* Input */

static void move_focus(te_form *f, int dir)
{
    int total = (int)f->n + 2;
    int i = f->focus;
    for (int k = 0; k < total; k++) {
        i = ((i + dir) % total + total) % total;
        if (i >= (int)f->n || !f->f[i].hidden) {
            f->focus = i;
            return;
        }
    }
}

static te_form_result cycle_choice(te_form_field *fl, int dir)
{
    if (fl->kind == TE_FIELD_CHECK) {
        fl->checked = !fl->checked;
        return TE_FORM_CHANGED;
    }
    if (fl->kind != TE_FIELD_CHOICE || fl->nchoices == 0) {
        return TE_FORM_NONE;
    }
    size_t n = fl->nchoices;
    fl->choice = dir > 0 ? (fl->choice + 1) % n : (fl->choice + n - 1) % n;
    return TE_FORM_CHANGED;
}

te_form_result form_key(te_form *f, te_key key, uint32_t mods)
{
    if (!f->open) {
        return TE_FORM_NONE;
    }
    te_form_field *fl = f->focus >= 0 && (size_t)f->focus < f->n ? &f->f[f->focus] : nullptr;
    bool text = fl != nullptr && (fl->kind == TE_FIELD_TEXT || fl->kind == TE_FIELD_NUMBER);
    switch (key) {
    case TE_KEY_ESCAPE:
        f->open = false;
        return TE_FORM_CANCEL;
    case TE_KEY_ENTER:
        if ((size_t)f->focus == f->n + 1) {
            f->open = false;
            return TE_FORM_CANCEL;
        }
        return TE_FORM_SUBMIT;   /* the caller validates and closes */
    case TE_KEY_TAB:
        move_focus(f, (mods & TE_MOD_SHIFT) != 0 ? -1 : 1);
        return TE_FORM_NONE;
    case TE_KEY_UP:
        move_focus(f, -1);
        return TE_FORM_NONE;
    case TE_KEY_DOWN:
        move_focus(f, 1);
        return TE_FORM_NONE;
    case TE_KEY_LEFT:
        if (text) {
            field_move(fl, -1);
            return TE_FORM_NONE;
        }
        if (fl != nullptr) {
            return cycle_choice(fl, -1);
        }
        if ((size_t)f->focus == f->n + 1) f->focus = (int)f->n;
        return TE_FORM_NONE;
    case TE_KEY_RIGHT:
        if (text) {
            field_move(fl, 1);
            return TE_FORM_NONE;
        }
        if (fl != nullptr) {
            return cycle_choice(fl, 1);
        }
        if ((size_t)f->focus == f->n) f->focus = (int)f->n + 1;
        return TE_FORM_NONE;
    case TE_KEY_HOME:
        if (text) fl->cursor = 0;
        return TE_FORM_NONE;
    case TE_KEY_END:
        if (text) fl->cursor = strlen(fl->value);
        return TE_FORM_NONE;
    case TE_KEY_BACKSPACE:
        if (text) {
            field_backspace(fl);
            return TE_FORM_CHANGED;
        }
        return TE_FORM_NONE;
    case TE_KEY_DELETE:
        if (text) {
            field_delete(fl);
            return TE_FORM_CHANGED;
        }
        return TE_FORM_NONE;
    case TE_KEY_CHAR:   /* printable keys (and Space for choices) arrive as text events */
    default:
        return TE_FORM_NONE;
    }
}

te_form_result form_text(te_form *f, const char *utf8)
{
    if (!f->open || utf8 == nullptr) {
        return TE_FORM_NONE;
    }
    te_form_field *fl = f->focus >= 0 && (size_t)f->focus < f->n ? &f->f[f->focus] : nullptr;
    if (fl == nullptr) {
        return TE_FORM_NONE;
    }
    if (fl->kind == TE_FIELD_CHOICE || fl->kind == TE_FIELD_CHECK) {
        return strcmp(utf8, " ") == 0 ? cycle_choice(fl, 1) : TE_FORM_NONE;
    }
    return field_insert(fl, utf8) ? TE_FORM_CHANGED : TE_FORM_NONE;
}

/* ------------------------------------------------------------------------- */
/* Layout and drawing */

static size_t label_cols(const te_form *f)
{
    size_t m = 0;
    for (size_t i = 0; i < f->n; i++) {
        m = TE_MAX(m, ui_text_cols(f->f[i].label));
    }
    return m;
}

/* Number of message lines when wrapped at cols. */
static size_t wrap_lines(const char *msg, size_t cols)
{
    if (msg[0] == '\0' || cols == 0) {
        return 0;
    }
    size_t lines = 1;
    size_t col = 0;
    for (const char *s = msg; *s != '\0'; s++) {
        if (*s == '\n' || col >= cols) {
            lines++;
            col = 0;
            if (*s == '\n') continue;
        }
        if (!is_cont(*s)) col++;
    }
    return lines;
}

static size_t visible_fields(const te_form *f)
{
    size_t v = 0;
    for (size_t i = 0; i < f->n; i++) {
        v += !f->f[i].hidden;
    }
    return v;
}

void form_layout(te_form *f, const te_painter *p, float win_w, float win_h)
{
    float maxw = win_w - 2.0f * p->cell_w;
    float w = p->cell_w * 70.0f;
    f->w = w < maxw ? w : maxw;
    size_t inner = (size_t)((f->w - 2.0f * PAD(p)) / p->cell_w);
    float fixed = p->cell_h * 2.4f;                               /* title */
    fixed += (float)wrap_lines(f->message, inner) * p->cell_h * 1.2f;
    fixed += f->error[0] != '\0' ? (float)wrap_lines(f->error, inner) * p->cell_h * 1.2f + p->cell_h * 0.4f : 0.0f;
    fixed += p->cell_h * 3.0f;                                    /* buttons */
    /* Rows shrink (down to 1.25 lines) before the dialog would overflow the window. */
    size_t nf = visible_fields(f);
    f->row_h = p->cell_h * 1.7f;
    if (nf > 0 && fixed + (float)nf * f->row_h > win_h - p->cell_h) {
        float fit = (win_h - p->cell_h - fixed) / (float)nf;
        f->row_h = TE_MAX(fit, p->cell_h * 1.25f);
    }
    float h = fixed + (float)nf * f->row_h;
    f->h = h < win_h - p->cell_h ? h : win_h - p->cell_h;
    f->x = (win_w - f->w) / 2.0f;
    f->y = (win_h - f->h) / 2.0f;
    if (f->y < 0.0f) f->y = 0.0f;
}

/* Geometry of the field rows and buttons, shared by drawing and clicking. */
typedef struct form_geo {
    float fields_top;
    float label_w;
    float btn_y, btn_h, ok_x, ok_w, cancel_x, cancel_w;
} form_geo;

static form_geo geometry(const te_form *f, const te_painter *p)
{
    form_geo g = { 0 };
    size_t inner = (size_t)((f->w - 2.0f * PAD(p)) / p->cell_w);
    g.fields_top = f->y + p->cell_h * 2.4f + (float)wrap_lines(f->message, inner) * p->cell_h * 1.2f;
    g.label_w = (float)(label_cols(f) + 2) * p->cell_w;
    g.btn_h = p->cell_h * 1.6f;
    g.btn_y = f->y + f->h - g.btn_h - p->cell_h * 0.7f;
    g.cancel_w = (float)(ui_text_cols("Cancel") + 4) * p->cell_w;
    g.ok_w = (float)(ui_text_cols(f->ok_label) + 4) * p->cell_w;
    g.cancel_x = f->x + f->w - PAD(p) - g.cancel_w;
    g.ok_x = g.cancel_x - p->cell_w - g.ok_w;
    return g;
}

te_form_result form_click(te_form *f, float x, float y, const te_painter *p)
{
    if (!f->open) {
        return TE_FORM_NONE;
    }
    if (x < f->x || x >= f->x + f->w || y < f->y || y >= f->y + f->h) {
        return TE_FORM_NONE;   /* clicks outside a dialog do nothing: it is modal */
    }
    form_geo g = geometry(f, p);
    if (y >= g.btn_y && y < g.btn_y + g.btn_h) {
        if (x >= g.ok_x && x < g.ok_x + g.ok_w) {
            f->focus = (int)f->n;
            return TE_FORM_SUBMIT;
        }
        if (x >= g.cancel_x && x < g.cancel_x + g.cancel_w) {
            f->open = false;
            return TE_FORM_CANCEL;
        }
        return TE_FORM_NONE;
    }
    float top = g.fields_top;
    for (size_t i = 0; i < f->n; i++) {
        te_form_field *fl = &f->f[i];
        if (fl->hidden) {
            continue;
        }
        if (y >= top && y < top + ROW_H(f, p)) {
            f->focus = (int)i;
            float fx = f->x + PAD(p) + g.label_w;
            if (fl->kind == TE_FIELD_CHOICE || fl->kind == TE_FIELD_CHECK) {
                /* Left half of a choice steps back, right half forward. */
                float mid = fx + (f->x + f->w - PAD(p) - fx) / 2.0f;
                return cycle_choice(fl, fl->kind == TE_FIELD_CHOICE && x < mid ? -1 : 1);
            }
            /* Put the text cursor near the click. */
            size_t want = x > fx ? (size_t)((x - fx) / p->cell_w) : 0;
            fl->cursor = ui_text_fit(fl->value, want);
            return TE_FORM_NONE;
        }
        top += ROW_H(f, p);
    }
    return TE_FORM_NONE;
}

static void draw_wrapped(const te_painter *p, float x, float y, const char *msg, size_t cols, uint32_t rgb)
{
    char line[512] = { 0 };
    const char *s = msg;
    while (*s != '\0') {
        size_t nl = strcspn(s, "\n");
        size_t fit = ui_text_fit(s, cols);
        size_t take = nl < fit ? nl : fit;
        if (take == 0 && *s != '\n') {
            take = 1;   /* a character wider than the line: draw it anyway */
        }
        if (str_copy_n(line, sizeof line, s, take) == TE_OK) {
            ui_text(p, TE_LAYER_OVERLAY, x, y, line, cols, rgb, false);
        }
        s += take;
        if (*s == '\n') s++;
        y += p->cell_h * 1.2f;
    }
}

void form_draw(const te_form *f, const te_painter *p, const te_ui_colors *c)
{
    if (!f->open) {
        return;
    }
    float s = p->scale > 0.0f ? p->scale : 1.0f;
    ui_rect(p, TE_LAYER_OVERLAY, f->x + 4.0f * s, f->y + 6.0f * s, f->w, f->h, 0x000000, 0.4f);
    ui_rect(p, TE_LAYER_OVERLAY, f->x, f->y, f->w, f->h, c->bg, 1.0f);
    ui_frame(p, TE_LAYER_OVERLAY, f->x, f->y, f->w, f->h, c->border);
    ui_rect(p, TE_LAYER_OVERLAY, f->x, f->y, f->w, 2.0f * s, f->danger ? c->danger : c->accent, 1.0f);
    size_t inner = (size_t)((f->w - 2.0f * PAD(p)) / p->cell_w);
    ui_text(p, TE_LAYER_OVERLAY, f->x + PAD(p), f->y + p->cell_h * 0.8f, f->title, inner, c->fg, true);
    draw_wrapped(p, f->x + PAD(p), f->y + p->cell_h * 2.4f, f->message, inner, c->fg);

    form_geo g = geometry(f, p);
    float top = g.fields_top;
    float fx = f->x + PAD(p) + g.label_w;
    float fw = f->x + f->w - PAD(p) - fx;
    size_t fcols = fw > p->cell_w * 2.0f ? (size_t)(fw / p->cell_w) - 1 : 1;
    for (size_t i = 0; i < f->n; i++) {
        const te_form_field *fl = &f->f[i];
        if (fl->hidden) {
            continue;
        }
        bool foc = (int)i == f->focus;
        float ty = top + (ROW_H(f, p) - p->cell_h) / 2.0f;
        ui_text(p, TE_LAYER_OVERLAY, f->x + PAD(p), ty, fl->label, (size_t)(g.label_w / p->cell_w), foc ? c->fg : c->muted, false);
        float by = top + p->cell_h * 0.15f;
        float bh = ROW_H(f, p) - p->cell_h * 0.3f;
        ui_rect(p, TE_LAYER_OVERLAY, fx, by, fw, bh, c->bg_alt, 1.0f);
        if (foc) {
            ui_frame(p, TE_LAYER_OVERLAY, fx, by, fw, bh, c->accent);
        }
        float tx = fx + p->cell_w * 0.5f;
        switch (fl->kind) {
        case TE_FIELD_TEXT:
        case TE_FIELD_NUMBER: {
            if (fl->value[0] == '\0' && !foc) {
                ui_text(p, TE_LAYER_OVERLAY, tx, ty, fl->placeholder, fcols, c->muted, false);
                break;
            }
            /* Scroll so the cursor stays visible. */
            char before[TE_FORM_VALUE_MAX] = { 0 };
            (void)str_copy_n(before, sizeof before, fl->value, fl->cursor);
            size_t ccol = ui_text_cols(before);
            const char *shown = fl->value;
            size_t skip_cols = ccol + 1 > fcols ? ccol + 1 - fcols : 0;
            while (skip_cols > 0 && *shown != '\0') {
                size_t one = ui_text_fit(shown, 1);
                if (one == 0) one = 1;
                shown += one;
                skip_cols--;
                ccol--;
            }
            ui_text(p, TE_LAYER_OVERLAY, tx, ty, shown, fcols, c->fg, false);
            if (foc) {
                ui_rect(p, TE_LAYER_OVERLAY, tx + (float)ccol * p->cell_w, ty, 2.0f * s, p->cell_h, c->accent, 1.0f);
            }
            break;
        }
        case TE_FIELD_CHOICE: {
            const char *v = fl->nchoices > 0 && fl->choices != nullptr ? fl->choices[fl->choice] : "";
            ui_text(p, TE_LAYER_OVERLAY, tx, ty, "\xe2\x97\x82", 1, foc ? c->accent : c->muted, false);   /* ◂ */
            ui_text(p, TE_LAYER_OVERLAY, tx + p->cell_w * 2.0f, ty, v, fcols > 4 ? fcols - 4 : 1, c->fg, false);
            ui_text(p, TE_LAYER_OVERLAY, fx + fw - p->cell_w * 1.5f, ty, "\xe2\x96\xb8", 1, foc ? c->accent : c->muted, false); /* ▸ */
            break;
        }
        case TE_FIELD_CHECK:
            ui_text(p, TE_LAYER_OVERLAY, tx, ty, fl->checked ? "[x] Yes" : "[ ] No", fcols, c->fg, false);
            break;
        default:
            break;
        }
        top += ROW_H(f, p);
    }
    if (f->error[0] != '\0') {
        draw_wrapped(p, f->x + PAD(p), top + p->cell_h * 0.3f, f->error, inner, c->danger);
    }
    /* Buttons */
    bool ok_foc = (size_t)f->focus == f->n;
    bool cancel_foc = (size_t)f->focus == f->n + 1;
    uint32_t okbg = f->danger ? c->danger : c->accent;
    ui_rect(p, TE_LAYER_OVERLAY, g.ok_x, g.btn_y, g.ok_w, g.btn_h, okbg, ok_foc ? 1.0f : 0.85f);
    if (ok_foc) {
        ui_frame(p, TE_LAYER_OVERLAY, g.ok_x - 2.0f * s, g.btn_y - 2.0f * s, g.ok_w + 4.0f * s, g.btn_h + 4.0f * s, c->fg);
    }
    float bty = g.btn_y + (g.btn_h - p->cell_h) / 2.0f;
    ui_text(p, TE_LAYER_OVERLAY, g.ok_x + p->cell_w * 2.0f, bty, f->ok_label, 20, c->bg, true);
    ui_rect(p, TE_LAYER_OVERLAY, g.cancel_x, g.btn_y, g.cancel_w, g.btn_h, c->bg_alt, 1.0f);
    ui_frame(p, TE_LAYER_OVERLAY, g.cancel_x, g.btn_y, g.cancel_w, g.btn_h, cancel_foc ? c->fg : c->border);
    ui_text(p, TE_LAYER_OVERLAY, g.cancel_x + p->cell_w * 2.0f, bty, "Cancel", 6, c->fg, false);
}
