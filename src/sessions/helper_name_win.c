/* The Break event's name, shared by feterm.exe (pty_win.c) and feterm-helper.exe. */
#include "sessions/helper_con.h"

#include "util/str.h"

te_err hcon_break_name(unsigned long pid, wchar_t *out, size_t outsz)
{
    if (out == nullptr || outsz == 0) {
        return TE_ERR_INVAL;
    }
    out[0] = L'\0';
    char name[64] = { 0 };
    te_err rc = str_fmt(name, sizeof name, "Local\\%s-break-%lu", APP_NAME, pid);
    if (rc != TE_OK) {
        return rc;
    }
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, name, -1, out, (int)TE_MIN(outsz, (size_t)INT_MAX));
    if (n <= 0) {
        out[0] = L'\0';
        return TE_ERR_TRUNC;
    }
    return TE_OK;
}
