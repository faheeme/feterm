/*
 * PCRE2 syntax highlighting for network-device CLIs.
 *
 * Profiles live in highlight/<name>.ini (bundled data dir, then the user's
 * config dir, which overrides a bundled profile with the same name):
 *
 *   [profile]
 *   name     = cisco
 *   inherits = network-common        ; up to 4 inherits keys
 *   detect   = Cisco IOS Software    ; strong: switches to this profile in auto mode
 *   prompt   = ^[\w.-]+[#>]\s?$      ; weak: switches only from the fallback profile
 *
 *   [rule.interface]
 *   regex    = \b(?:Gi|Te)\S*\d\b
 *   fg       = blue                  ; palette name, color0..color255, or #rrggbb
 *   bg       = ...
 *   bold / italic / underline / strike / dim = true
 *   priority = 50                    ; higher wins where matches overlap
 *   group    = 1                     ; colour only this capture group (default 0)
 *   override = false                 ; true: recolour even text the program coloured
 *   ignore_case = true
 *   enabled  = false                 ; disable a rule inherited from a parent
 *
 * A rule with the same name as an inherited one replaces it.
 * Matching runs on rendered text, so escape sequences never interfere.
 * ReDoS protection: match, depth and heap limits on every match; a rule
 * that hits a limit three times is disabled and reported.
 */
#ifndef TE_HIGHLIGHT_H
#define TE_HIGHLIGHT_H

#include <stdbool.h>
#include "common.h"
#include "vt/term.h"

#define HL_MAX_PROFILES   64
#define HL_MAX_RULES      256
#define HL_MAX_PATTERNS   8
#define HL_MAX_INHERITS   4
#define HL_MAX_REGEX      1024
#define HL_MAX_COLS       2048

typedef struct te_hl_style {
    uint32_t fg;        /* TE_COLOR_* encoding; TE_COLOR_DEFAULT = leave unchanged */
    uint32_t bg;
    uint16_t attrs;     /* TE_ATTR_* bits to add */
    bool     override;
} te_hl_style;

typedef struct te_hl_span {
    uint16_t start, end;   /* columns, end exclusive */
    uint16_t style;        /* >= 1, see hl_style() */
} te_hl_span;

typedef enum { TE_HL_OFF, TE_HL_AUTO, TE_HL_PINNED } te_hl_mode;

typedef struct te_highlighter te_highlighter;

[[nodiscard]] te_err hl_create(te_highlighter **out);
void hl_destroy(te_highlighter **h);

/* Load every profile from the data directories. Bad rules are logged and skipped. */
[[nodiscard]] te_err hl_load_all(te_highlighter *h);

size_t hl_profile_count(const te_highlighter *h);
const char *hl_profile_name(const te_highlighter *h, size_t i);

/* Profile used in auto mode until a device is detected; nullptr or "none" for none. */
void hl_set_fallback(te_highlighter *h, const char *name);
/* mode PINNED needs a profile name; returns TE_ERR_NOTFOUND if it does not exist. */
[[nodiscard]] te_err hl_set_mode(te_highlighter *h, te_hl_mode mode, const char *profile);
te_hl_mode hl_mode(const te_highlighter *h);
const char *hl_active_name(const te_highlighter *h);   /* nullptr when nothing is active */

/* Back to the fallback profile (e.g. the local shell prompt reappeared). */
void hl_auto_reset(te_highlighter *h);
/* Cycle auto -> off -> each profile pinned -> auto. Returns a status message. */
const char *hl_cycle(te_highlighter *h);
/* True once after the active profile changed (auto detection), so the caller can redraw. */
bool hl_take_changed(te_highlighter *h);

/*
 * Highlight one line of cells (n columns). Returns the number of spans and
 * points *spans at an internal buffer valid until the next call. In auto
 * mode new lines are also checked for device detection patterns.
 */
size_t hl_line(te_highlighter *h, const te_cell *cells, uint16_t n, const te_hl_span **spans);
const te_hl_style *hl_style(const te_highlighter *h, uint16_t idx);

/* Column just after the prompt on this line, or -1 if no prompt is recognised. */
int hl_prompt_end(te_highlighter *h, const te_cell *cells, uint16_t n);

#endif
