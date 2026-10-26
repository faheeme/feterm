#include "util/os.h"

#include <stdlib.h>
#include <string.h>

#include "util/mem.h"
#include "util/str.h"

/* Copies a UTF-16 path into out as UTF-8 with '/' separators. */
static te_err put_path(const wchar_t *w, char *out, size_t outsz)
{
    char *u = te_w_narrow(w);
    if (u == nullptr) {
        return errno == ENOMEM ? TE_ERR_NOMEM : TE_ERR_INVAL;
    }
    for (char *c = u; *c != '\0'; c++) {
        if (*c == '\\') {
            *c = '/';
        }
    }
    te_err rc = str_copy(out, outsz, u);
    TE_FREE(u);
    if (rc == TE_OK && !os_path_is_absolute(out)) {
        out[0] = '\0';
        return TE_ERR_NOTFOUND;
    }
    return rc;
}

static te_err env_path(const wchar_t *name, char *out, size_t outsz)
{
    const wchar_t *v = _wgetenv(name);
    if (v == nullptr || v[0] == L'\0') {
        return TE_ERR_NOTFOUND;
    }
    return put_path(v, out, outsz);
}

bool os_path_is_absolute(const char *path)
{
    if (path == nullptr) {
        return false;
    }
    bool drive = ((path[0] >= 'A' && path[0] <= 'Z') || (path[0] >= 'a' && path[0] <= 'z')) && path[1] == ':' &&
                 (path[2] == '/' || path[2] == '\\');
    bool unc = (path[0] == '/' || path[0] == '\\') && (path[1] == '/' || path[1] == '\\');
    return drive || unc;
}

te_err os_home_dir(char *out, size_t outsz)
{
    return env_path(L"USERPROFILE", out, outsz);
}

te_err os_config_base(char *out, size_t outsz)
{
    return env_path(L"APPDATA", out, outsz);
}

te_err os_self_exe(char *out, size_t outsz)
{
    if (out == nullptr || outsz < 2) {
        return TE_ERR_INVAL;
    }
    /* Long paths: grow until the name fits (32767 is the Win32 maximum). */
    for (DWORD cap = MAX_PATH; cap <= 32768u; cap *= 2u) {
        wchar_t *w = te_calloc(cap, sizeof *w);
        if (w == nullptr) {
            return TE_ERR_NOMEM;
        }
        DWORD n = GetModuleFileNameW(nullptr, w, cap);
        if (n == 0) {
            TE_FREE(w);
            return TE_ERR_SYS;
        }
        if (n < cap) {
            te_err rc = put_path(w, out, outsz);
            TE_FREE(w);
            return rc;
        }
        TE_FREE(w);
    }
    return TE_ERR_TRUNC;
}
