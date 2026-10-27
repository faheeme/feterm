#include "pty/conpty_win.h"

#include <string.h>

#include "util/log.h"

typedef HRESULT (WINAPI *create_pc_fn)(COORD size, HANDLE in, HANDLE out, DWORD flags, te_hpcon *pc);
typedef HRESULT (WINAPI *resize_pc_fn)(te_hpcon pc, COORD size);
typedef void (WINAPI *close_pc_fn)(te_hpcon pc);

static struct {
    bool         loaded;
    create_pc_fn create;
    resize_pc_fn resize;
    close_pc_fn  close;
} conpty;

/* GetProcAddress hands back a FARPROC; copy it into the real function type. */
static bool load_sym(HMODULE m, const char *name, void *fn_out, size_t fn_size)
{
    FARPROC f = GetProcAddress(m, name);
    if (f == nullptr || fn_size != sizeof f) {
        return false;
    }
    memcpy(fn_out, &f, fn_size);
    return true;
}

bool conpty_load(void)
{
    if (conpty.loaded) {
        return conpty.create != nullptr;
    }
    conpty.loaded = true;
    HMODULE k = GetModuleHandleW(L"kernel32.dll");
    if (k == nullptr || !load_sym(k, "CreatePseudoConsole", &conpty.create, sizeof conpty.create) ||
        !load_sym(k, "ResizePseudoConsole", &conpty.resize, sizeof conpty.resize) ||
        !load_sym(k, "ClosePseudoConsole", &conpty.close, sizeof conpty.close)) {
        conpty.create = nullptr;
        LOG_E("this Windows has no pseudo console (ConPTY needs Windows 10 version 1809 or newer)");
        return false;
    }
    return true;
}

HRESULT conpty_create(COORD size, HANDLE in, HANDLE out, DWORD flags, te_hpcon *pc)
{
    return conpty.create != nullptr ? conpty.create(size, in, out, flags, pc) : E_NOTIMPL;
}

HRESULT conpty_resize(te_hpcon pc, COORD size)
{
    return conpty.resize != nullptr ? conpty.resize(pc, size) : E_NOTIMPL;
}

void conpty_close(te_hpcon pc)
{
    if (conpty.close != nullptr && pc != nullptr) {
        conpty.close(pc);
    }
}

bool conpty_on_wine(void)
{
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    return ntdll != nullptr && GetProcAddress(ntdll, "wine_get_version") != nullptr;
}
