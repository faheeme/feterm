/*
 * Modal dialog: a title, an optional message, a column of fields (text,
 * number, choice, checkbox) and OK / Cancel buttons. Keyboard: Tab and
 * Shift+Tab (or Up/Down) move between fields, Left/Right or Space change a
 * choice, Enter submits, Esc cancels. Fields and buttons can be clicked.
 *
 * Text editing is UTF-8 aware and bounded: nothing longer than the field's
 * limit is ever accepted, and control characters are dropped.
 */
#ifndef TE_FORM_H
#define TE_FORM_H

#include <stdbool.h>
#include "common.h"
#include "platform/platform.h"
#include "ui/paint.h"

#define TE_FORM_MAX_FIELDS 24
#define TE_FORM_VALUE_MAX  1024

typedef enum { TE_FIELD_TEXT, TE_FIELD_NUMBER, TE_FIELD_CHOICE, TE_FIELD_CHECK, TE_FIELD_SECRET } te_field_kind;

typedef struct te_form_field {
    int                 id;
    te_field_kind       kind;
    char                label[40];
    char                value[TE_FORM_VALUE_MAX];
    size_t              cursor;       /* byte offset into value */
    size_t              maxlen;       /* bytes, < TE_FORM_VALUE_MAX */
    char                placeholder[64];
    const char *const  *choices;      /* not owned; must outlive the form */
    size_t              nchoices;
    size_t              choice;
    bool                checked;
    bool                hidden;
} te_form_field;

typedef enum { TE_FORM_NONE, TE_FORM_SUBMIT, TE_FORM_CANCEL, TE_FORM_CHANGED } te_form_result;

typedef struct te_form {
    bool           open;
    int            purpose;           /* what the caller opened it for */
    char           title[96];
    char           message[320];      /* shown above the fields (wrapped) */
    char           ok_label[24];
    bool           danger;            /* OK is a destructive action */
    char           error[200];        /* validation message from the caller */
    te_form_field  f[TE_FORM_MAX_FIELDS];
    size_t         n;
    int            focus;             /* field index; n = OK, n + 1 = Cancel */
    float          x, y, w, h;        /* set by form_layout */
    float          row_h;             /* field row height (smaller in a short window) */
} te_form;

void form_init(te_form *f, int purpose, const char *title, const char *ok_label);
/* Adds a field (nullptr when full). value may be nullptr. */
te_form_field *form_add(te_form *f, int id, te_field_kind kind, const char *label, const char *value);
te_form_field *form_get(te_form *f, int id);
const te_form_field *form_cget(const te_form *f, int id);
void form_set_choices(te_form_field *fl, const char *const *choices, size_t n, size_t selected);
void form_show(te_form *f, int id, bool visible);
void form_focus_first(te_form *f);
void form_set_error(te_form *f, const char *msg);
/* Overwrite every field value (passwords) before the form is reused or closed. */
void form_wipe(te_form *f);

const char *form_value(const te_form *f, int id);          /* "" if missing */
size_t form_choice(const te_form *f, int id);
bool form_checked(const te_form *f, int id);

te_form_result form_key(te_form *f, te_key key, uint32_t mods);
te_form_result form_text(te_form *f, const char *utf8);    /* typed or pasted text */
te_form_result form_click(te_form *f, float x, float y, const te_painter *p);

void form_layout(te_form *f, const te_painter *p, float win_w, float win_h);
void form_draw(const te_form *f, const te_painter *p, const te_ui_colors *c);

/* Pure text-editing primitives (exposed for tests). */
bool field_insert(te_form_field *fl, const char *utf8);
void field_backspace(te_form_field *fl);
void field_delete(te_form_field *fl);
void field_move(te_form_field *fl, int dir);

#endif
