/* Translate key presses into the byte sequences terminal applications expect. */
#ifndef TE_KEYS_H
#define TE_KEYS_H

#include "common.h"
#include "platform/platform.h"
#include "vt/term.h"

typedef enum { TE_KEYEV_PRESS = 1, TE_KEYEV_REPEAT = 2, TE_KEYEV_RELEASE = 3 } te_keyev;

/*
 * Encode a key event. Returns the number of bytes written to out (never more
 * than outsz), or 0 if the key should instead be delivered as text input (or
 * nothing is sent, e.g. a release in legacy mode). Honours the kitty keyboard
 * protocol flags in m->kitty_flags.
 */
[[nodiscard]] size_t keys_encode(te_key key, uint32_t cp, uint32_t mods, const te_modes *m, char *out, size_t outsz);
[[nodiscard]] size_t keys_encode_ev(te_key key, uint32_t cp, uint32_t mods, te_keyev ev, const te_modes *m,
                                    char *out, size_t outsz);

/*
 * Prepare pasted text for the pty: optionally strip control characters,
 * turn newlines into CR, and wrap in bracketed-paste markers.
 * Returns a heap buffer (caller frees) and its length in *len, or nullptr.
 */
[[nodiscard]] char *keys_prepare_paste(const char *text, bool bracketed, bool sanitize, size_t *len);

#endif
