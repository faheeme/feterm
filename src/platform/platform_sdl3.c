/* SDL3 backend: one code path for Wayland and X11 (SDL picks at runtime). */
#include "platform/platform.h"

#ifndef _WIN32
#include <dlfcn.h>
#endif
#include <string.h>

#include <SDL3/SDL.h>

#include "platform/fdwatch.h"
#include "platform/icon_data.h"
#include "util/log.h"
#include "util/mem.h"
#include "util/str.h"

#define TE_ICON_MAX 512   /* largest embedded icon side we accept */

#ifdef __APPLE__
bool plat_macos_free_close_key(void);   /* menu_mac.c */
#endif

struct te_platform {
    SDL_Window    *win;
    SDL_GLContext  gl;
    bool           alpha;          /* the window has an alpha channel the compositor can use */
    SDL_Cursor    *ibeam;
    SDL_Cursor    *hand;
    SDL_Cursor    *arrow;
    te_pointer     pointer;
    uint32_t       wake_event;     /* registered SDL user event type */
    uint32_t       dialog_event;   /* file dialog answers */

    te_fdwatch    *watch;          /* fds (POSIX) or HANDLEs (Windows) */
    unsigned       option_as_alt;  /* macOS: bit 0 left, bit 1 right Option is Alt */
};

/* Copies one embedded icon image into a new surface. Returns nullptr on failure. */
static SDL_Surface *icon_surface(const te_icon_image *img)
{
    if (img->rgba == nullptr || img->size <= 0 || img->size > TE_ICON_MAX) {
        return nullptr;
    }
    size_t side = (size_t)img->size;
    size_t row = side * 4;            /* cannot overflow: side <= TE_ICON_MAX */
    if (img->len != row * side) {
        return nullptr;
    }
    SDL_Surface *s = SDL_CreateSurface(img->size, img->size, SDL_PIXELFORMAT_RGBA32);
    if (s == nullptr) {
        LOG_D("SDL_CreateSurface (icon): %s", SDL_GetError());
        return nullptr;
    }
    if (s->pixels == nullptr || s->pitch < 0 || (size_t)s->pitch < row) {
        SDL_DestroySurface(s);
        s = nullptr;
        return nullptr;
    }
    uint8_t *dst = s->pixels;
    size_t pitch = (size_t)s->pitch;
    for (size_t y = 0; y < side; y++) {
        memcpy(dst + y * pitch, img->rgba + y * row, row);
    }
    return s;
}

/* Gives the window the logo. X11 and Wayland compositors with xdg-toplevel-icon
 * show it; elsewhere (e.g. GNOME) the icon comes from the desktop entry, so a
 * failure here only costs the runtime icon. */
static void set_window_icon(SDL_Window *win)
{
    SDL_Surface *icon = icon_surface(&te_icons[0]);
    if (icon == nullptr) {
        LOG_D("window icon unavailable");
        return;
    }
    for (size_t i = 1; i < TE_ARRAY_LEN(te_icons); i++) {
        SDL_Surface *alt = icon_surface(&te_icons[i]);
        if (alt == nullptr) {
            continue;
        }
        /* Takes its own reference to alt. */
        if (!SDL_AddSurfaceAlternateImage(icon, alt)) {
            LOG_D("SDL_AddSurfaceAlternateImage: %s", SDL_GetError());
        }
        SDL_DestroySurface(alt);
        alt = nullptr;
    }
    if (!SDL_SetWindowIcon(win, icon)) {
        LOG_D("SDL_SetWindowIcon: %s", SDL_GetError());
    }
    SDL_DestroySurface(icon);
    icon = nullptr;
}

static uint32_t map_mods(SDL_Keymod m)
{
    uint32_t r = 0;
    if ((m & SDL_KMOD_SHIFT) != 0) r |= TE_MOD_SHIFT;
    if ((m & SDL_KMOD_ALT) != 0)   r |= TE_MOD_ALT;
    if ((m & SDL_KMOD_CTRL) != 0)  r |= TE_MOD_CTRL;
    if ((m & SDL_KMOD_GUI) != 0)   r |= TE_MOD_SUPER;
    return r;
}

static te_key map_key(SDL_Keycode k, uint32_t *cp)
{
    *cp = 0;
    switch (k) {
    case SDLK_RETURN:
    case SDLK_KP_ENTER:  return TE_KEY_ENTER;
    case SDLK_ESCAPE:    return TE_KEY_ESCAPE;
    case SDLK_BACKSPACE: return TE_KEY_BACKSPACE;
    case SDLK_TAB:       return TE_KEY_TAB;
    case SDLK_INSERT:    return TE_KEY_INSERT;
    case SDLK_DELETE:    return TE_KEY_DELETE;
    case SDLK_HOME:      return TE_KEY_HOME;
    case SDLK_END:       return TE_KEY_END;
    case SDLK_PAGEUP:    return TE_KEY_PAGE_UP;
    case SDLK_PAGEDOWN:  return TE_KEY_PAGE_DOWN;
    case SDLK_UP:        return TE_KEY_UP;
    case SDLK_DOWN:      return TE_KEY_DOWN;
    case SDLK_LEFT:      return TE_KEY_LEFT;
    case SDLK_RIGHT:     return TE_KEY_RIGHT;
    case SDLK_F1:  return TE_KEY_F1;
    case SDLK_F2:  return TE_KEY_F2;
    case SDLK_F3:  return TE_KEY_F3;
    case SDLK_F4:  return TE_KEY_F4;
    case SDLK_F5:  return TE_KEY_F5;
    case SDLK_F6:  return TE_KEY_F6;
    case SDLK_F7:  return TE_KEY_F7;
    case SDLK_F8:  return TE_KEY_F8;
    case SDLK_F9:  return TE_KEY_F9;
    case SDLK_F10: return TE_KEY_F10;
    case SDLK_F11: return TE_KEY_F11;
    case SDLK_F12: return TE_KEY_F12;
    case SDLK_PAUSE: return TE_KEY_PAUSE;
    default:
        break;
    }
    /* Printable keys: SDL keycodes for them are the unshifted Unicode code point. */
    if ((k & SDLK_SCANCODE_MASK) == 0 && k >= 0x20 && k <= 0x10FFFF && k != 0x7F) {
        *cp = (uint32_t)k;
        return TE_KEY_CHAR;
    }
    return TE_KEY_NONE;
}

/* Creates the window and its GL context, with or without an alpha channel. */
static bool create_surface(te_platform *p, const te_platform_opts *o, bool alpha)
{
    (void)SDL_GL_SetAttribute(SDL_GL_ALPHA_SIZE, alpha ? 8 : 0);
    SDL_WindowFlags flags = SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY |
                            SDL_WINDOW_HIDDEN;
    if (alpha) {
        flags |= SDL_WINDOW_TRANSPARENT;
    }
    p->win = SDL_CreateWindow(o->title != nullptr ? o->title : APP_DISPLAY_NAME, o->width, o->height, flags);
    if (p->win == nullptr) {
        LOG_W("SDL_CreateWindow%s: %s", alpha ? " (transparent)" : "", SDL_GetError());
        return false;
    }
    p->gl = SDL_GL_CreateContext(p->win);
    if (p->gl == nullptr) {
        LOG_W("SDL_GL_CreateContext (OpenGL ES 3.0%s): %s", alpha ? ", alpha" : "", SDL_GetError());
        SDL_DestroyWindow(p->win);
        p->win = nullptr;
        return false;
    }
    p->alpha = alpha;
    return true;
}

#ifndef _WIN32
typedef unsigned long (*x_intern_atom_fn)(void *dpy, const char *name, int only_if_exists);
typedef unsigned long (*x_get_selection_owner_fn)(void *dpy, unsigned long atom);

/* Looks up a libX11 function; dlsym hands back an object pointer. */
static bool x11_sym(void *lib, const char *name, void *fn_out, size_t fn_size)
{
    void *sym = dlsym(lib, name);
    if (sym == nullptr || fn_size != sizeof sym) {
        return false;
    }
    memcpy(fn_out, &sym, fn_size);
    return true;
}

/*
 * A compositing manager owns the _NET_WM_CM_S<screen> selection (EWMH). Without
 * one, X11 ignores the alpha channel and a translucent window just looks dark.
 * libX11 is already loaded by SDL; RTLD_NOLOAD never loads anything new.
 */
static bool x11_compositor_running(SDL_Window *win)
{
    SDL_PropertiesID props = SDL_GetWindowProperties(win);
    if (props == 0) {
        return false;
    }
    void *dpy = SDL_GetPointerProperty(props, SDL_PROP_WINDOW_X11_DISPLAY_POINTER, nullptr);
    Sint64 screen = SDL_GetNumberProperty(props, SDL_PROP_WINDOW_X11_SCREEN_NUMBER, -1);
    if (dpy == nullptr || screen < 0 || screen > 255) {
        return false;
    }
    char atom_name[32] = { 0 };
    if (str_fmt(atom_name, sizeof atom_name, "_NET_WM_CM_S%d", (int)screen) != TE_OK) {
        return false;
    }
    void *lib = dlopen("libX11.so.6", RTLD_LAZY | RTLD_LOCAL | RTLD_NOLOAD);
    if (lib == nullptr) {
        LOG_D("libX11 not loaded: cannot check for a compositor");
        return false;
    }
    bool running = false;
    x_intern_atom_fn intern_atom = nullptr;
    x_get_selection_owner_fn selection_owner = nullptr;
    if (x11_sym(lib, "XInternAtom", &intern_atom, sizeof intern_atom) &&
        x11_sym(lib, "XGetSelectionOwner", &selection_owner, sizeof selection_owner)) {
        unsigned long atom = intern_atom(dpy, atom_name, 1);   /* only if it exists */
        running = atom != 0 && selection_owner(dpy, atom) != 0;
    }
    intern_atom = nullptr;
    selection_owner = nullptr;
    (void)dlclose(lib);
    lib = nullptr;
    return running;
}
#endif

bool plat_transparency_available(te_platform *p)
{
    if (p == nullptr || p->win == nullptr || !p->alpha) {
        return false;
    }
    const char *drv = SDL_GetCurrentVideoDriver();
    if (drv == nullptr) {
        return false;
    }
    if (strcmp(drv, "wayland") == 0) {
        return true;   /* every Wayland compositor blends alpha surfaces */
    }
#ifdef _WIN32
    if (strcmp(drv, "windows") == 0) {
        return true;   /* DWM always composites */
    }
#else
    if (strcmp(drv, "x11") == 0) {
        return x11_compositor_running(p->win);
    }
#endif
    return false;
}

te_err plat_create(te_platform **out, const te_platform_opts *o)
{
    if (out == nullptr || o == nullptr) {
        return TE_ERR_INVAL;
    }
    *out = nullptr;
    te_platform *p = te_calloc(1, sizeof *p);
    if (p == nullptr) {
        return TE_ERR_NOMEM;
    }
    (void)SDL_SetAppMetadata(APP_DISPLAY_NAME, APP_VERSION, o->app_id != nullptr ? o->app_id : APP_ID);
#ifdef _WIN32
    /* OpenGL ES through ANGLE (Direct3D 11), shipped next to the exe as libEGL.dll and
     * libGLESv2.dll: it works on every GPU, and in VMs through the WARP software renderer. */
    (void)SDL_SetHint(SDL_HINT_OPENGL_ES_DRIVER, "1");
#endif
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS)) {
        LOG_E("SDL_Init: %s", SDL_GetError());
        TE_FREE(p);
        return TE_ERR_PLATFORM;
    }
    LOG_D("video driver: %s", SDL_GetCurrentVideoDriver());

#ifdef __APPLE__
    /* No OpenGL ES on macOS: the native 4.1 core profile (see render/gl.h). */
    (void)SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    (void)SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, SDL_GL_CONTEXT_FORWARD_COMPATIBLE_FLAG);
    (void)SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 4);
    (void)SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 1);
#else
    (void)SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
    (void)SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    (void)SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
#endif
    (void)SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);

    /* Always ask for an alpha channel, so transparency can be switched on by a
     * config reload; fall back to an opaque window if the driver refuses. */
    if (!create_surface(p, o, true)) {
        LOG_W("transparent window unavailable; using an opaque one");
        if (!create_surface(p, o, false)) {
            LOG_E("could not create the window");
            plat_destroy(&p);
            return TE_ERR_PLATFORM;
        }
    }
    set_window_icon(p->win);
#ifdef __APPLE__
    if (plat_macos_free_close_key()) {
        LOG_D("macOS: Cmd+W freed from the Window menu for close_pane");
    } else {
        LOG_W("macOS: Window > Close not found; Cmd+W may close the window");
    }
#endif
    if (!SDL_GL_MakeCurrent(p->win, p->gl)) {
        LOG_E("SDL_GL_MakeCurrent: %s", SDL_GetError());
        plat_destroy(&p);
        return TE_ERR_PLATFORM;
    }
    if (!SDL_GL_SetSwapInterval(1)) {
        LOG_D("vsync unavailable: %s", SDL_GetError());
    }
    p->wake_event = SDL_RegisterEvents(2);
    p->dialog_event = p->wake_event != 0 ? p->wake_event + 1 : 0;
    if (p->wake_event == 0) {
        LOG_E("SDL_RegisterEvents: %s", SDL_GetError());
        plat_destroy(&p);
        return TE_ERR_PLATFORM;
    }
    te_err rc = fdwatch_create(&p->watch, p->wake_event);
    if (rc != TE_OK) {
        LOG_E("cannot start the fd watcher: %s", te_strerror(rc));
        plat_destroy(&p);
        return rc;
    }
    p->ibeam = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_TEXT);
    p->hand = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_POINTER);
    p->arrow = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_DEFAULT);
    p->pointer = TE_POINTER_TEXT;
    if (p->ibeam != nullptr) {
        (void)SDL_SetCursor(p->ibeam);
    }
    if (!SDL_StartTextInput(p->win)) {
        LOG_W("SDL_StartTextInput: %s", SDL_GetError());
    }
    *out = p;
    return TE_OK;
}

void plat_destroy(te_platform **pp)
{
    if (pp == nullptr || *pp == nullptr) {
        return;
    }
    te_platform *p = *pp;
    fdwatch_destroy(&p->watch);
    SDL_Cursor **cursors[] = { &p->ibeam, &p->hand, &p->arrow };
    for (size_t i = 0; i < TE_ARRAY_LEN(cursors); i++) {
        if (*cursors[i] != nullptr) {
            SDL_DestroyCursor(*cursors[i]);
            *cursors[i] = nullptr;
        }
    }
    if (p->gl != nullptr) {
        (void)SDL_GL_DestroyContext(p->gl);
        p->gl = nullptr;
    }
    if (p->win != nullptr) {
        SDL_DestroyWindow(p->win);
        p->win = nullptr;
    }
    SDL_Quit();
    TE_FREE(*pp);
}

void plat_show(te_platform *p)
{
#ifdef _WIN32
    /* Windows would cascade new windows from the top-left; start in the middle of the
     * screen instead (here, once the final size is set). Linux window managers place
     * windows themselves. */
    (void)SDL_SetWindowPosition(p->win, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
#endif
    (void)SDL_ShowWindow(p->win);
}

static float pixel_density(te_platform *p)
{
    float d = SDL_GetWindowPixelDensity(p->win);
    return d > 0.0f ? d : 1.0f;
}

/* A copy of a path from the system; on Windows with '/' separators, as every path in the app has. */
static char *path_dup(const char *path)
{
    char *s = str_dup(path);
#ifdef _WIN32
    for (char *c = s; c != nullptr && *c != '\0'; c++) {
        if (*c == '\\') {
            *c = '/';
        }
    }
#endif
    return s;
}

static bool translate(te_platform *p, const SDL_Event *e, te_event *ev)
{
    memset(ev, 0, sizeof *ev);
    float dens = pixel_density(p);
    switch (e->type) {
    case SDL_EVENT_QUIT:
    case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
        ev->type = TE_EV_QUIT;
        return true;
    case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
        ev->type = TE_EV_RESIZE;
        ev->resize.w = e->window.data1;
        ev->resize.h = e->window.data2;
        return true;
    case SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED:
        ev->type = TE_EV_SCALE;
        return true;
    case SDL_EVENT_WINDOW_EXPOSED:
        ev->type = TE_EV_EXPOSE;
        return true;
    case SDL_EVENT_WINDOW_FOCUS_GAINED:
    case SDL_EVENT_WINDOW_FOCUS_LOST:
        ev->type = TE_EV_FOCUS;
        ev->focus.focused = e->type == SDL_EVENT_WINDOW_FOCUS_GAINED;
        return true;
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP:
        ev->type = TE_EV_KEY;
        ev->key.key = map_key(e->key.key, &ev->key.cp);
        ev->key.mods = map_mods(e->key.mod);
#ifdef __APPLE__
        if (ev->key.key == TE_KEY_CHAR && (ev->key.mods & TE_MOD_ALT) != 0 &&
            (ev->key.mods & (TE_MOD_CTRL | TE_MOD_SUPER)) == 0) {
            bool as_alt = ((e->key.mod & SDL_KMOD_LALT) != 0 && (p->option_as_alt & 1u) != 0) ||
                          ((e->key.mod & SDL_KMOD_RALT) != 0 && (p->option_as_alt & 2u) != 0);
            if (!as_alt) {
                ev->key.mods &= ~(uint32_t)TE_MOD_ALT;   /* a character key: the text event carries it */
            }
        }
#endif
        ev->key.down = e->key.down;
        ev->key.repeat = e->key.repeat;
        return ev->key.key != TE_KEY_NONE;
    case SDL_EVENT_TEXT_INPUT:
        if (e->text.text == nullptr) {
            return false;
        }
        ev->type = TE_EV_TEXT;
        /* Over-long input is dropped rather than split mid-character. */
        if (str_copy(ev->text.text, sizeof ev->text.text, e->text.text) != TE_OK) {
            return false;
        }
        return true;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP:
        ev->type = TE_EV_MOUSE_BUTTON;
        ev->button.x = e->button.x * dens;
        ev->button.y = e->button.y * dens;
        ev->button.button = e->button.button;
        ev->button.clicks = e->button.clicks;
        ev->button.down = e->button.down;
        ev->button.mods = map_mods(SDL_GetModState());
        return true;
    case SDL_EVENT_MOUSE_MOTION:
        ev->type = TE_EV_MOUSE_MOTION;
        ev->motion.x = e->motion.x * dens;
        ev->motion.y = e->motion.y * dens;
        ev->motion.buttons = e->motion.state;
        ev->motion.mods = map_mods(SDL_GetModState());
        return true;
    case SDL_EVENT_MOUSE_WHEEL: {
        float dy = e->wheel.y;
        if (e->wheel.direction == SDL_MOUSEWHEEL_FLIPPED) {
            dy = -dy;
        }
        ev->type = TE_EV_MOUSE_WHEEL;
        ev->wheel.x = e->wheel.mouse_x * dens;
        ev->wheel.y = e->wheel.mouse_y * dens;
        ev->wheel.dy = dy;
        ev->wheel.mods = map_mods(SDL_GetModState());
        return true;
    }
    case SDL_EVENT_DROP_FILE:
        ev->type = TE_EV_DROP;
        ev->drop.path = e->drop.data != nullptr ? path_dup(e->drop.data) : nullptr;
        ev->drop.x = e->drop.x * dens;
        ev->drop.y = e->drop.y * dens;
        return true;
    default:
        if (p->wake_event != 0 && e->type == p->wake_event) {
            ev->type = TE_EV_FD_READY;
            ev->fd.tag = e->user.code;
            return true;
        }
        if (p->dialog_event != 0 && e->type == p->dialog_event && e->user.data1 != nullptr) {
            /* The result struct was built on the dialog's thread; hand it over whole. */
            te_event *res = e->user.data1;
            *ev = *res;
            free(res);
            return true;
        }
        return false;
    }
}

typedef struct dialog_req {
    uint32_t event_type;
    int      request;
} dialog_req;

/* SDL may call this on another thread: it only builds a heap event and posts it. */
static void SDLCALL dialog_done(void *userdata, const char *const *filelist, int filter)
{
    (void)filter;
    dialog_req *dr = userdata;
    te_event *ev = te_calloc(1, sizeof *ev);
    if (ev == nullptr || dr == nullptr) {
        free(ev);
        free(dr);
        return;
    }
    ev->type = TE_EV_DIALOG;
    ev->dialog.request = dr->request;
    if (filelist == nullptr) {
        ev->dialog.failed = true;
        (void)str_copy(ev->dialog.error, sizeof ev->dialog.error, SDL_GetError());
    } else {
        size_t n = 0;
        while (filelist[n] != nullptr && n < 1024) {
            n++;
        }
        if (n > 0) {
            ev->dialog.paths = te_calloc(n, sizeof *ev->dialog.paths);
            for (size_t i = 0; ev->dialog.paths != nullptr && i < n; i++) {
                ev->dialog.paths[i] = path_dup(filelist[i]);
                if (ev->dialog.paths[i] != nullptr) {
                    ev->dialog.n++;
                } else {
                    break;
                }
            }
        }
    }
    SDL_Event se;
    SDL_zero(se);
    se.type = dr->event_type;
    se.user.data1 = ev;
    free(dr);
    if (!SDL_PushEvent(&se)) {
        plat_event_free(ev);
        free(ev);
    }
}

void plat_file_dialog(te_platform *p, te_dialog_kind kind, const char *default_dir, int request)
{
    if (p == nullptr || p->dialog_event == 0) {
        return;
    }
    dialog_req *dr = te_calloc(1, sizeof *dr);
    if (dr == nullptr) {
        return;
    }
    dr->event_type = p->dialog_event;
    dr->request = request;
    switch (kind) {
    case TE_DIALOG_OPEN_FILES:
        SDL_ShowOpenFileDialog(dialog_done, dr, p->win, nullptr, 0, default_dir, true);
        break;
    case TE_DIALOG_OPEN_FOLDERS:
        SDL_ShowOpenFolderDialog(dialog_done, dr, p->win, default_dir, true);
        break;
    case TE_DIALOG_CHOOSE_FOLDER:
    default:
        SDL_ShowOpenFolderDialog(dialog_done, dr, p->win, default_dir, false);
        break;
    }
}

void plat_event_free(te_event *ev)
{
    if (ev == nullptr) {
        return;
    }
    if (ev->type == TE_EV_DROP) {
        TE_FREE(ev->drop.path);
    } else if (ev->type == TE_EV_DIALOG) {
        for (size_t i = 0; i < ev->dialog.n; i++) {
            TE_FREE(ev->dialog.paths[i]);
        }
        TE_FREE(ev->dialog.paths);
        ev->dialog.n = 0;
    }
}

bool plat_wait_event(te_platform *p, te_event *ev, int timeout_ms)
{
    SDL_Event e;
    SDL_zero(e);
    uint64_t start = SDL_GetTicks();
    for (;;) {
        bool got = false;
        if (timeout_ms == 0) {
            got = SDL_PollEvent(&e);
        } else if (timeout_ms < 0) {
            got = SDL_WaitEvent(&e);
        } else {
            uint64_t elapsed = SDL_GetTicks() - start;
            if (elapsed >= (uint64_t)timeout_ms) {
                return false;
            }
            got = SDL_WaitEventTimeout(&e, (Sint32)((uint64_t)timeout_ms - elapsed));
        }
        if (!got) {
            return false;
        }
        if (translate(p, &e, ev)) {
            return true;
        }
        /* Untranslated event: keep waiting within the same deadline. */
        if (timeout_ms == 0) {
            continue;
        }
    }
}

void plat_swap(te_platform *p)
{
    (void)SDL_GL_SwapWindow(p->win);
}

void plat_drawable_size(te_platform *p, int *w, int *h)
{
    *w = 0;
    *h = 0;
    if (!SDL_GetWindowSizeInPixels(p->win, w, h)) {
        *w = 0;
        *h = 0;
    }
}

void plat_set_size_px(te_platform *p, int w, int h)
{
    float d = pixel_density(p);
    int lw = (int)((float)w / d + 0.5f);
    int lh = (int)((float)h / d + 0.5f);
    if (lw > 0 && lh > 0) {
        (void)SDL_SetWindowSize(p->win, lw, lh);
    }
}

float plat_content_scale(te_platform *p)
{
    float s = SDL_GetWindowDisplayScale(p->win);
    if (!(s > 0.25f && s < 8.0f)) {
        s = 1.0f;
    }
    return s;
}

void plat_set_title(te_platform *p, const char *utf8)
{
    if (utf8 == nullptr) {
        return;
    }
    (void)SDL_SetWindowTitle(p->win, utf8[0] != '\0' ? utf8 : APP_DISPLAY_NAME);
}

void plat_bell(te_platform *p)
{
    (void)SDL_FlashWindow(p->win, SDL_FLASH_UNTIL_FOCUSED);
}

uint64_t plat_ticks_ms(void)
{
    return SDL_GetTicks();
}

/* Copy an SDL-owned string into a malloc'd one so callers only ever use free(). */
static char *take_sdl_string(char *s)
{
    if (s == nullptr) {
        return nullptr;
    }
    char *r = s[0] != '\0' ? str_dup(s) : nullptr;
    SDL_free(s);
    return r;
}

void plat_clipboard_set(te_platform *p, const char *utf8)
{
    (void)p;
    if (utf8 != nullptr && !SDL_SetClipboardText(utf8)) {
        LOG_W("clipboard: %s", SDL_GetError());
    }
}

char *plat_clipboard_get(te_platform *p)
{
    (void)p;
    return take_sdl_string(SDL_GetClipboardText());
}

void plat_primary_set(te_platform *p, const char *utf8)
{
    (void)p;
    if (utf8 != nullptr) {
        (void)SDL_SetPrimarySelectionText(utf8);
    }
}

char *plat_primary_get(te_platform *p)
{
    (void)p;
    return take_sdl_string(SDL_GetPrimarySelectionText());
}

te_err plat_watch_fd(te_platform *p, int fd, int *tag)
{
#ifdef _WIN32
    (void)p;
    (void)fd;
    (void)tag;
    return TE_ERR_PLATFORM;   /* Windows pipes are not waitable: use plat_watch_handle */
#else
    return p != nullptr ? fdwatch_add(p->watch, fd, tag) : TE_ERR_INVAL;
#endif
}

#ifdef _WIN32
te_err plat_watch_handle(te_platform *p, void *handle, int *tag)
{
    return p != nullptr ? fdwatch_add(p->watch, handle, tag) : TE_ERR_INVAL;
}
#endif

void plat_fd_rearm(te_platform *p, int tag)
{
    if (p != nullptr) {
        fdwatch_rearm(p->watch, tag);
    }
}

void plat_unwatch_fd(te_platform *p, int tag)
{
    if (p != nullptr) {
        fdwatch_remove(p->watch, tag);
    }
}

void plat_set_pointer(te_platform *p, te_pointer kind)
{
    if (p == nullptr || kind == p->pointer) {
        return;
    }
    SDL_Cursor *c = kind == TE_POINTER_HAND ? p->hand : kind == TE_POINTER_DEFAULT ? p->arrow : p->ibeam;
    if (c != nullptr && SDL_SetCursor(c)) {
        p->pointer = kind;
    }
}

bool plat_logo_rgba(int size, const uint8_t **rgba, size_t *len)
{
    for (size_t i = 0; i < TE_ARRAY_LEN(te_icons); i++) {
        if (te_icons[i].size == size && te_icons[i].rgba != nullptr &&
            te_icons[i].len == (size_t)size * (size_t)size * 4u) {
            *rgba = te_icons[i].rgba;
            *len = te_icons[i].len;
            return true;
        }
    }
    return false;
}

void plat_set_option_as_alt(te_platform *p, unsigned sides)
{
    if (p != nullptr) {
        p->option_as_alt = sides & 3u;
    }
}
