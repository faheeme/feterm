/* Mouse reporting to applications (xterm protocols 9/1000/1002/1003, encodings 1005/1006/1015/1016). */
#ifndef TE_MOUSE_H
#define TE_MOUSE_H

#include <stdbool.h>
#include "common.h"
#include "vt/term.h"

typedef enum { TE_MOUSE_PRESS, TE_MOUSE_RELEASE, TE_MOUSE_MOVE } te_mouse_action;

/* Button codes as xterm numbers them. */
enum {
    TE_MB_LEFT = 0, TE_MB_MIDDLE = 1, TE_MB_RIGHT = 2, TE_MB_NONE = 3,
    TE_MB_WHEEL_UP = 64, TE_MB_WHEEL_DOWN = 65, TE_MB_WHEEL_LEFT = 66, TE_MB_WHEEL_RIGHT = 67,
};

/* Should this event be reported in the current mode? */
[[nodiscard]] bool mouse_wants(const te_modes *m, te_mouse_action act, bool button_held);

/*
 * Encode one event. col/row are 0-based cells; px/py are 0-based pixels
 * inside the grid (used by SGR-pixel mode). Returns bytes written, or 0 if
 * the position cannot be expressed in the active encoding.
 */
[[nodiscard]] size_t mouse_encode(const te_modes *m, te_mouse_action act, int button, uint32_t mods,
                                  uint16_t col, uint16_t row, uint32_t px, uint32_t py,
                                  char *out, size_t outsz);

#endif
