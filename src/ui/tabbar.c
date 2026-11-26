#include "ui/tabbar.h"

#include <string.h>

#include "ui/paint.h"
#include "util/str.h"

void tabbar_init(te_tabbar_ui *ui)
{
    memset(ui, 0, sizeof *ui);
    ui->menu.hover = -1;
}

static te_menu_item hinted(int id, const char *label, const te_keymap *keys, te_action act, bool enabled)
{
    char hint[24] = { 0 };
    if (keys == nullptr || act == TE_ACT_NONE || keymap_describe(keys, act, hint, sizeof hint) != TE_OK) {
        hint[0] = '\0';
    }
    return menu_item(id, label, hint, enabled);
}

size_t tabbar_menu_items(const te_tabbar_menu_ctx *ctx, te_menu_item *out, size_t max)
{
    if (ctx == nullptr || out == nullptr || max < TE_MENU_MAX_ITEMS || ctx->tab >= ctx->ntabs) {
        return 0;
    }
    const te_keymap *k = ctx->keys;
    bool first = ctx->tab == 0;
    bool last = ctx->tab + 1 >= ctx->ntabs;
    size_t n = 0;
    out[n++] = hinted(TE_TB_RENAME, "Rename tab...", k, TE_ACT_RENAME_TAB, true);
    out[n++] = menu_item(TE_TB_RESET_NAME, "Reset name", "", ctx->named);

    out[n] = hinted(TE_TB_SPLIT_RIGHT, "Split right", k, TE_ACT_CLONE_SPLIT_RIGHT, true);
    out[n++].separator_before = true;
    out[n++] = hinted(TE_TB_SPLIT_DOWN, "Split down", k, TE_ACT_CLONE_SPLIT_DOWN, true);

    out[n] = hinted(TE_TB_DUPLICATE, "Duplicate tab", k, TE_ACT_CLONE_TAB, true);
    out[n++].separator_before = true;
    if (ctx->can_reconnect) {
        out[n++] = menu_item(TE_TB_RECONNECT, "Reconnect", "", true);
    }

    out[n] = hinted(TE_TB_MOVE_LEFT, "Move left", k, TE_ACT_MOVE_TAB_LEFT, !first);
    out[n++].separator_before = true;
    out[n++] = hinted(TE_TB_MOVE_RIGHT, "Move right", k, TE_ACT_MOVE_TAB_RIGHT, !last);

    out[n] = menu_item(TE_TB_CLOSE, "Close tab", "", true);
    out[n].danger = true;
    out[n++].separator_before = true;
    out[n] = menu_item(TE_TB_CLOSE_OTHERS, "Close other tabs", "", ctx->ntabs > 1);
    out[n++].danger = true;
    out[n] = menu_item(TE_TB_CLOSE_RIGHT, "Close tabs to the right", "", !last);
    out[n++].danger = true;
    return n;
}

void tabbar_edit_begin(te_tabbar_ui *ui, size_t idx, const char *current)
{
    menu_close(&ui->menu);
    memset(&ui->edit, 0, sizeof ui->edit);
    ui->edit.kind = TE_FIELD_TEXT;
    ui->edit.maxlen = TE_TAB_NAME_MAX;
    /* A long automatic title is cut to the name limit; field_insert drops a
     * character the cut left incomplete, and any control characters. */
    if (current != nullptr && current[0] != '\0') {
        char tmp[TE_TAB_NAME_MAX + 1] = { 0 };
        (void)str_copy(tmp, sizeof tmp, current);
        (void)field_insert(&ui->edit, tmp);
    }
    ui->edit.cursor = strlen(ui->edit.value);
    ui->edit_all = ui->edit.value[0] != '\0';   /* like a browser: typing replaces the old name */
    ui->edit_tab = idx;
    ui->editing = true;
}

te_tb_edit tabbar_edit_key(te_tabbar_ui *ui, te_key key, uint32_t mods)
{
    (void)mods;
    if (!ui->editing) {
        return TE_TB_EDIT_NONE;
    }
    bool all = ui->edit_all;
    ui->edit_all = false;
    if (all && (key == TE_KEY_BACKSPACE || key == TE_KEY_DELETE)) {
        memset(ui->edit.value, 0, sizeof ui->edit.value);   /* the selection goes */
        ui->edit.cursor = 0;
        return TE_TB_EDIT_CHANGED;
    }
    switch (key) {
    case TE_KEY_ENTER:     return TE_TB_EDIT_SUBMIT;
    case TE_KEY_ESCAPE:    return TE_TB_EDIT_CANCEL;
    case TE_KEY_BACKSPACE: field_backspace(&ui->edit); return TE_TB_EDIT_CHANGED;
    case TE_KEY_DELETE:    field_delete(&ui->edit); return TE_TB_EDIT_CHANGED;
    case TE_KEY_LEFT:      if (all) ui->edit.cursor = 0; else field_move(&ui->edit, -1); return TE_TB_EDIT_CHANGED;
    case TE_KEY_RIGHT:     field_move(&ui->edit, 1); return TE_TB_EDIT_CHANGED;
    case TE_KEY_HOME:      ui->edit.cursor = 0; return TE_TB_EDIT_CHANGED;
    case TE_KEY_END:       ui->edit.cursor = strlen(ui->edit.value); return TE_TB_EDIT_CHANGED;
    default:
        ui->edit_all = all;   /* modifiers and other keys keep the selection */
        return TE_TB_EDIT_NONE;   /* swallowed: nothing reaches the terminal */
    }
}

te_tb_edit tabbar_edit_text(te_tabbar_ui *ui, const char *utf8)
{
    if (!ui->editing || utf8 == nullptr) {
        return TE_TB_EDIT_NONE;
    }
    if (ui->edit_all) {
        /* Replace the selected name, but only with something printable. */
        te_form_field probe = { .kind = TE_FIELD_TEXT, .maxlen = TE_TAB_NAME_MAX };
        if (!field_insert(&probe, utf8)) {
            return TE_TB_EDIT_NONE;
        }
        ui->edit = probe;
        ui->edit_all = false;
        return TE_TB_EDIT_CHANGED;
    }
    return field_insert(&ui->edit, utf8) ? TE_TB_EDIT_CHANGED : TE_TB_EDIT_NONE;
}

void tabbar_edit_value(const te_tabbar_ui *ui, char *out, size_t outsz)
{
    if (out == nullptr || outsz == 0) {
        return;
    }
    out[0] = '\0';
    const char *s = ui->edit.value;
    size_t len = strnlen(s, sizeof ui->edit.value);
    size_t b = 0;
    while (b < len && (s[b] == ' ' || s[b] == '\t')) {
        b++;
    }
    while (len > b && (s[len - 1] == ' ' || s[len - 1] == '\t')) {
        len--;
    }
    size_t n = len - b;
    if (n >= outsz) {
        n = outsz - 1;
        while (n > 0 && ((unsigned char)s[b + n] & 0xC0u) == 0x80u) {
            n--;   /* never cut inside a character */
        }
    }
    memcpy(out, s + b, n);
    out[n] = '\0';
}

void tabbar_edit_end(te_tabbar_ui *ui)
{
    ui->editing = false;
    ui->edit_all = false;
    memset(&ui->edit, 0, sizeof ui->edit);
}

bool tabbar_captures(const te_tabbar_ui *ui)
{
    return ui->menu.open || ui->editing || ui->confirm.open;
}

void tabbar_reset(te_tabbar_ui *ui)
{
    menu_close(&ui->menu);
    tabbar_edit_end(ui);
    ui->pressed = false;
    ui->dragging = false;
    ui->confirm.open = false;
}

size_t tabbar_drag_slot(size_t from, float x, float area_x, float tw, size_t ntabs)
{
    if (ntabs == 0 || tw <= 0.0f || from >= ntabs) {
        return from;
    }
    float rel = (x - area_x) / tw;
    if (rel < 0.0f) {
        return 0;
    }
    size_t slot = rel >= (float)ntabs ? ntabs - 1 : (size_t)rel;
    return slot;
}

/* Byte length of name's longest prefix within max_cols cells and max_bytes bytes, on a character boundary. */
static size_t fit_prefix(const char *name, size_t max_cols, size_t max_bytes)
{
    size_t n = ui_text_fit(name, max_cols);
    if (n > max_bytes) {
        n = max_bytes;
        while (n > 0 && ((unsigned char)name[n] & 0xC0) == 0x80) {
            n--;
        }
    }
    return n;
}

void tabbar_window_title(char *out, size_t outsz, const char *name, bool exited)
{
    if (out == nullptr || outsz == 0) {
        return;
    }
    static const char suffix[] = " (disconnected)";
    if (name == nullptr || name[0] == '\0') {
        (void)str_copy(out, outsz, APP_DISPLAY_NAME);
        return;
    }
    char cut[256] = { 0 };   /* window titles beyond ~120 columns are never seen anyway */
    (void)str_copy_n(cut, sizeof cut, name, fit_prefix(name, 120, sizeof cut - 1));
    if (str_fmt(out, outsz, "%s | %s%s", APP_DISPLAY_NAME, cut, exited ? suffix : "") != TE_OK) {
        (void)str_copy(out, outsz, APP_DISPLAY_NAME);   /* never show half a character */
    }
}

void tabbar_quit_message(char *out, size_t outsz, const char *const *names, size_t n)
{
    if (out == nullptr || outsz == 0) {
        return;
    }
    out[0] = '\0';
    char list[200] = { 0 };
    size_t shown = 0;
    for (size_t i = 0; names != nullptr && i < n && shown < 3; i++) {
        const char *nm = names[i];
        if (nm == nullptr || nm[0] == '\0') {
            continue;
        }
        char cut[64] = { 0 };
        size_t len = fit_prefix(nm, 24, sizeof cut - 4);   /* room for the ellipsis */
        if (str_copy_n(cut, sizeof cut, nm, len) != TE_OK) {
            break;
        }
        if (len < strlen(nm)) {
            (void)str_append(cut, sizeof cut, "\xE2\x80\xA6");   /* ... */
        }
        if ((shown > 0 && str_append(list, sizeof list, ", ") != TE_OK) || str_append(list, sizeof list, cut) != TE_OK) {
            break;
        }
        shown++;
    }
    if (shown > 0 && n > shown) {
        char more[32] = { 0 };
        (void)str_fmt(more, sizeof more, " and %zu more", n - shown);
        (void)str_append(list, sizeof list, more);
    }
    char open[24] = { 0 };
    (void)str_fmt(open, sizeof open, n == 1 ? "1 session is" : "%zu sessions are", n);
    (void)str_fmt(out, outsz, "%s still open%s%s%s. Close %s and end %s?", open,
                  shown > 0 ? " (" : "", list, shown > 0 ? ")" : "", APP_DISPLAY_NAME, n == 1 ? "it" : "them");
}
