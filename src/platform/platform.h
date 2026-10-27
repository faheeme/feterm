/*
 * Windowing-system abstraction. Nothing outside src/platform/ may include a
 * toolkit header. The SDL3 backend runs natively on both Wayland and X11 and
 * picks one at runtime; a native backend only has to implement this file.
 */
#ifndef TE_PLATFORM_H
#define TE_PLATFORM_H

#include <stdbool.h>
#include "common.h"

enum { TE_MOD_SHIFT = 1u << 0, TE_MOD_ALT = 1u << 1, TE_MOD_CTRL = 1u << 2, TE_MOD_SUPER = 1u << 3 };
/* Held while clicking to open a link: Cmd on macOS (Ctrl+click is a right-click there), Ctrl elsewhere. */
#ifdef __APPLE__
#define TE_MOD_LINK      TE_MOD_SUPER
#define TE_MOD_LINK_NAME "Cmd"
#else
#define TE_MOD_LINK      TE_MOD_CTRL
#define TE_MOD_LINK_NAME "Ctrl"
#endif

typedef enum te_key {
    TE_KEY_NONE = 0,
    TE_KEY_CHAR,          /* printable key; unshifted code point in .cp */
    TE_KEY_ENTER, TE_KEY_ESCAPE, TE_KEY_BACKSPACE, TE_KEY_TAB,
    TE_KEY_INSERT, TE_KEY_DELETE, TE_KEY_HOME, TE_KEY_END, TE_KEY_PAGE_UP, TE_KEY_PAGE_DOWN,
    TE_KEY_UP, TE_KEY_DOWN, TE_KEY_LEFT, TE_KEY_RIGHT,
    TE_KEY_F1, TE_KEY_F2, TE_KEY_F3, TE_KEY_F4, TE_KEY_F5, TE_KEY_F6,
    TE_KEY_F7, TE_KEY_F8, TE_KEY_F9, TE_KEY_F10, TE_KEY_F11, TE_KEY_F12,
    TE_KEY_PAUSE,         /* never sent to programs; bindable (serial Break) */
} te_key;

typedef enum te_event_type {
    TE_EV_NONE = 0,
    TE_EV_QUIT,
    TE_EV_RESIZE,         /* drawable size changed */
    TE_EV_SCALE,          /* content scale changed */
    TE_EV_EXPOSE,
    TE_EV_FOCUS,
    TE_EV_KEY,
    TE_EV_TEXT,
    TE_EV_MOUSE_BUTTON,
    TE_EV_MOUSE_MOTION,
    TE_EV_MOUSE_WHEEL,
    TE_EV_FD_READY,       /* a watched file descriptor is readable (.fd.tag says which) */
    TE_EV_DROP,           /* a file dragged from the desktop was dropped (.drop; free with plat_event_free) */
    TE_EV_DIALOG,         /* a file dialog finished (.dialog; free with plat_event_free) */
} te_event_type;

typedef struct te_event {
    te_event_type type;
    union {
        struct { int w, h; } resize;
        struct { bool focused; } focus;
        struct { te_key key; uint32_t cp; uint32_t mods; bool down; bool repeat; } key;
        struct { char text[64]; } text;                         /* always NUL-terminated UTF-8 */
        struct { float x, y; uint8_t button; uint8_t clicks; bool down; uint32_t mods; } button;
        struct { float x, y; uint32_t buttons; uint32_t mods; } motion; /* physical pixels */
        struct { float x, y; float dy; uint32_t mods; } wheel;
        struct { int tag; } fd;
        struct { char *path; float x, y; } drop;                 /* path: heap, may be nullptr */
        struct { int request; char **paths; size_t n; bool failed; char error[160]; } dialog;   /* n == 0: cancelled */
    };
} te_event;

typedef struct te_platform te_platform;

typedef struct te_platform_opts {
    const char *title;
    const char *app_id;
    int         width, height;   /* logical pixels */
} te_platform_opts;

[[nodiscard]] te_err plat_create(te_platform **out, const te_platform_opts *o);
void plat_destroy(te_platform **p);

void plat_show(te_platform *p);
/* Wait for an event. timeout_ms < 0 waits forever; 0 polls. Returns true if *ev was filled. */
[[nodiscard]] bool plat_wait_event(te_platform *p, te_event *ev, int timeout_ms);
void plat_swap(te_platform *p);

void plat_drawable_size(te_platform *p, int *w, int *h);
void plat_set_size_px(te_platform *p, int w, int h);
float plat_content_scale(te_platform *p);
void plat_set_title(te_platform *p, const char *utf8);
void plat_bell(te_platform *p);
/* True if the desktop will actually show a translucent window: always on Wayland,
 * on X11 only while a compositor runs. Cheap enough to call on config reload. */
bool plat_transparency_available(te_platform *p);
uint64_t plat_ticks_ms(void);

/* Clipboard. Getters return a heap string the caller frees with free(), or nullptr. */
void plat_clipboard_set(te_platform *p, const char *utf8);
[[nodiscard]] char *plat_clipboard_get(te_platform *p);
void plat_primary_set(te_platform *p, const char *utf8);
[[nodiscard]] char *plat_primary_get(te_platform *p);

#define TE_PLAT_MAX_FDS 64

/*
 * Watch fd for readability. *tag identifies it in TE_EV_FD_READY. The event
 * is sent once, then again only after plat_fd_rearm(tag).
 */
[[nodiscard]] te_err plat_watch_fd(te_platform *p, int fd, int *tag);
#ifdef _WIN32
/* Windows: watch a waitable HANDLE instead (an event that is set while data is ready). */
[[nodiscard]] te_err plat_watch_handle(te_platform *p, void *handle, int *tag);
#endif
void plat_fd_rearm(te_platform *p, int tag);
/* Stop watching (call before closing the fd). The tag may be reused later. */
void plat_unwatch_fd(te_platform *p, int tag);

typedef enum { TE_POINTER_TEXT, TE_POINTER_HAND, TE_POINTER_DEFAULT } te_pointer;

/*
 * Native file dialogs (xdg-desktop-portal, or zenity as a fallback). The answer
 * arrives later as a TE_EV_DIALOG event carrying request.
 */
typedef enum { TE_DIALOG_OPEN_FILES, TE_DIALOG_OPEN_FOLDERS, TE_DIALOG_CHOOSE_FOLDER } te_dialog_kind;
void plat_file_dialog(te_platform *p, te_dialog_kind kind, const char *default_dir, int request);
/* The app logo as raw RGBA (size x size x 4 bytes, straight alpha); false if that size is not built in. */
[[nodiscard]] bool plat_logo_rgba(int size, const uint8_t **rgba, size_t *len);

/* Free what a TE_EV_DROP or TE_EV_DIALOG event owns (safe on any event). */
void plat_event_free(te_event *ev);
void plat_set_pointer(te_platform *p, te_pointer kind);
/*
 * macOS: which Option keys act as Alt (bit 0 left, bit 1 right). The others
 * type characters (Option+3 is # on a UK keyboard): key events for character
 * keys then drop TE_MOD_ALT and the composed text arrives as TE_EV_TEXT. Keys
 * held with Cmd or Ctrl, and non-character keys (Option+arrows), keep Alt.
 * Elsewhere this does nothing.
 */
void plat_set_option_as_alt(te_platform *p, unsigned sides);

#endif
