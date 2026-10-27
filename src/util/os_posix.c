#include "util/os.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "util/str.h"

bool os_path_is_absolute(const char *path)
{
    return path != nullptr && path[0] == '/';
}

te_err os_home_dir(char *out, size_t outsz)
{
    const char *home = getenv("HOME");
    if (home == nullptr || home[0] != '/') {
        return TE_ERR_NOTFOUND;
    }
    return str_copy(out, outsz, home);
}

te_err os_config_base(char *out, size_t outsz)
{
    const char *xdg = getenv("XDG_CONFIG_HOME");
    if (xdg != nullptr && xdg[0] == '/') {
        return str_copy(out, outsz, xdg);
    }
    const char *home = getenv("HOME");
    if (home == nullptr || home[0] != '/') {
        return TE_ERR_NOTFOUND;
    }
    return str_fmt(out, outsz, "%s/.config", home);
}

te_err os_self_exe(char *out, size_t outsz)
{
    if (out == nullptr || outsz < 2) {
        return TE_ERR_INVAL;
    }
    ssize_t n = readlink("/proc/self/exe", out, outsz - 1);
    if (n <= 0 || (size_t)n >= outsz - 1) {
        return TE_ERR_NOTFOUND;
    }
    out[n] = '\0';
    /* A rebuilt or upgraded binary shows up as "... (deleted)"; it can still be run via /proc. */
    if (out[0] != '/' || strstr(out, " (deleted)") != nullptr) {
        return str_copy(out, outsz, "/proc/self/exe");
    }
    return TE_OK;
}

void os_mark_from_internet(const char *path)
{
    (void)path;   /* Mark-of-the-Web is a Windows concept; nothing to do here */
}

void os_harden_process(void)
{
    /* The DLL/exe search-order planting this guards against is Windows-specific. */
}

bool os_hostname(char *out, size_t outsz)
{
    if (out == nullptr || outsz == 0) {
        return false;
    }
    if (gethostname(out, outsz) != 0) {
        out[0] = '\0';
        return false;
    }
    out[outsz - 1] = '\0';
    return out[0] != '\0';
}
