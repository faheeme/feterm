/* SDL3 backend: one code path for Wayland and X11 (SDL picks at runtime). */
#include "platform/platform.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdatomic.h>
#include <string.h>
#include <unistd.h>

#include <SDL3/SDL.h>

#include "platform/icon_data.h"
#include "util/log.h"
#include "util/mem.h"
#include "util/str.h"

#define TE_ICON_MAX 512   /* largest embedded icon side we accept */

struct te_platform {
    SDL_Window    *win;
    SDL_GLContext  gl;
    SDL_Cursor    *ibeam;
    SDL_Cursor    *hand;
    SDL_Cursor    *arrow;
    te_pointer     pointer;
    uint32_t       wake_event;     /* registered SDL user event type */

    /* fd watcher thread: polls every armed fd plus a wake pipe */
    SDL_Thread    *thread;
    atomic_int     fds[TE_PLAT_MAX_FDS];     /* -1: slot free */
    atomic_bool    armed[TE_PLAT_MAX_FDS];
    atomic_int     nfds;
    int            wake_pipe[2];
    atomic_bool    quit;
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

static void wake_thread(te_platform *p)
{
    if (p->wake_pipe[1] >= 0) {
        ssize_t w = write(p->wake_pipe[1], "w", 1);
        (void)w;   /* the pipe is non-blocking; a full pipe already means "wake up" */
    }
}

static int SDLCALL fd_thread(void *arg)
{
    te_platform *p = arg;
    while (!atomic_load(&p->quit)) {
        struct pollfd pfd[TE_PLAT_MAX_FDS + 1];
        int tags[TE_PLAT_MAX_FDS + 1] = { 0 };
        nfds_t n = 0;
        pfd[n] = (struct pollfd){ .fd = p->wake_pipe[0], .events = POLLIN, .revents = 0 };
        tags[n++] = -1;
        int count = atomic_load(&p->nfds);
        for (int i = 0; i < count && i < TE_PLAT_MAX_FDS; i++) {
            if (atomic_load(&p->armed[i]) && atomic_load(&p->fds[i]) >= 0) {
                pfd[n] = (struct pollfd){ .fd = atomic_load(&p->fds[i]), .events = POLLIN, .revents = 0 };
                tags[n++] = i;
            }
        }
        int r = poll(pfd, n, -1);
        if (r < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }
        for (nfds_t k = 0; k < n; k++) {
            if (pfd[k].revents == 0) {
                continue;
            }
            if (tags[k] < 0) {
                char drain[64];
                while (read(p->wake_pipe[0], drain, sizeof drain) > 0) {
                }
                continue;
            }
            /* Disarm until the main thread has drained this fd and re-armed it. */
            atomic_store(&p->armed[tags[k]], false);
            SDL_Event ev;
            SDL_zero(ev);
            ev.type = p->wake_event;
            ev.user.code = tags[k];
            if (!SDL_PushEvent(&ev)) {
                LOG_W("SDL_PushEvent failed: %s", SDL_GetError());
                atomic_store(&p->armed[tags[k]], true);
            }
        }
    }
    return 0;
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
    p->wake_pipe[0] = -1;
    p->wake_pipe[1] = -1;
    atomic_init(&p->quit, false);
    atomic_init(&p->nfds, 0);
    for (size_t i = 0; i < TE_PLAT_MAX_FDS; i++) {
        atomic_init(&p->fds[i], -1);
        atomic_init(&p->armed[i], false);
    }

    (void)SDL_SetAppMetadata(APP_DISPLAY_NAME, APP_VERSION, o->app_id != nullptr ? o->app_id : APP_ID);
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS)) {
        LOG_E("SDL_Init: %s", SDL_GetError());
        TE_FREE(p);
        return TE_ERR_PLATFORM;
    }
    LOG_D("video driver: %s", SDL_GetCurrentVideoDriver());

    (void)SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
    (void)SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    (void)SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
    (void)SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    (void)SDL_GL_SetAttribute(SDL_GL_ALPHA_SIZE, o->transparent ? 8 : 0);

    SDL_WindowFlags flags = SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY |
                            SDL_WINDOW_HIDDEN;
    if (o->transparent) {
        flags |= SDL_WINDOW_TRANSPARENT;
    }
    p->win = SDL_CreateWindow(o->title != nullptr ? o->title : APP_DISPLAY_NAME, o->width, o->height, flags);
    if (p->win == nullptr) {
        LOG_E("SDL_CreateWindow: %s", SDL_GetError());
        plat_destroy(&p);
        return TE_ERR_PLATFORM;
    }
    set_window_icon(p->win);
    p->gl = SDL_GL_CreateContext(p->win);
    if (p->gl == nullptr) {
        LOG_E("SDL_GL_CreateContext (OpenGL ES 3.0): %s", SDL_GetError());
        plat_destroy(&p);
        return TE_ERR_PLATFORM;
    }
    if (!SDL_GL_MakeCurrent(p->win, p->gl)) {
        LOG_E("SDL_GL_MakeCurrent: %s", SDL_GetError());
        plat_destroy(&p);
        return TE_ERR_PLATFORM;
    }
    if (!SDL_GL_SetSwapInterval(1)) {
        LOG_D("vsync unavailable: %s", SDL_GetError());
    }
    p->wake_event = SDL_RegisterEvents(1);
    if (p->wake_event == 0) {
        LOG_E("SDL_RegisterEvents: %s", SDL_GetError());
        plat_destroy(&p);
        return TE_ERR_PLATFORM;
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
    if (p->thread != nullptr) {
        atomic_store(&p->quit, true);
        wake_thread(p);
        SDL_WaitThread(p->thread, nullptr);
        p->thread = nullptr;
    }
    for (size_t i = 0; i < 2; i++) {
        if (p->wake_pipe[i] >= 0) {
            close(p->wake_pipe[i]);
            p->wake_pipe[i] = -1;
        }
    }
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
    (void)SDL_ShowWindow(p->win);
}

static float pixel_density(te_platform *p)
{
    float d = SDL_GetWindowPixelDensity(p->win);
    return d > 0.0f ? d : 1.0f;
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
    default:
        if (p->wake_event != 0 && e->type == p->wake_event) {
            ev->type = TE_EV_FD_READY;
            ev->fd.tag = e->user.code;
            return true;
        }
        return false;
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
    if (p == nullptr || fd < 0 || tag == nullptr) {
        return TE_ERR_INVAL;
    }
    int count = atomic_load(&p->nfds);
    int n = -1;
    for (int i = 0; i < count; i++) {
        if (atomic_load(&p->fds[i]) < 0) {
            n = i;   /* reuse a freed slot */
            break;
        }
    }
    if (n < 0) {
        if (count >= TE_PLAT_MAX_FDS) {
            return TE_ERR_RANGE;
        }
        n = count;
    }
    if (p->wake_pipe[0] < 0 && pipe2(p->wake_pipe, O_CLOEXEC | O_NONBLOCK) != 0) {
        return TE_ERR_SYS;
    }
    atomic_store(&p->fds[n], fd);
    atomic_store(&p->armed[n], true);
    if (n == count) {
        atomic_store(&p->nfds, n + 1);
    }
    *tag = n;
    if (p->thread == nullptr) {
        p->thread = SDL_CreateThread(fd_thread, "fd-watch", p);
        if (p->thread == nullptr) {
            LOG_E("SDL_CreateThread: %s", SDL_GetError());
            atomic_store(&p->fds[n], -1);
            return TE_ERR_PLATFORM;
        }
    } else {
        wake_thread(p);
    }
    return TE_OK;
}

void plat_fd_rearm(te_platform *p, int tag)
{
    if (p == nullptr || tag < 0 || tag >= atomic_load(&p->nfds)) {
        return;
    }
    atomic_store(&p->armed[tag], true);
    wake_thread(p);
}

void plat_unwatch_fd(te_platform *p, int tag)
{
    if (p == nullptr || tag < 0 || tag >= atomic_load(&p->nfds)) {
        return;
    }
    atomic_store(&p->armed[tag], false);
    atomic_store(&p->fds[tag], -1);
    wake_thread(p);
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
