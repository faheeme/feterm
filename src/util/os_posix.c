#include "util/os.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#ifdef __APPLE__
#include <limits.h>
#include <mach-o/dyld.h>
#include <sys/xattr.h>
#include <time.h>
#endif

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

#ifdef __APPLE__
te_err os_self_exe(char *out, size_t outsz)
{
    if (out == nullptr || outsz < 2) {
        return TE_ERR_INVAL;
    }
    char raw[PATH_MAX] = { 0 };
    uint32_t len = (uint32_t)sizeof raw;
    if (_NSGetExecutablePath(raw, &len) != 0) {
        return TE_ERR_NOTFOUND;   /* longer than PATH_MAX */
    }
    raw[sizeof raw - 1] = '\0';
    char real[PATH_MAX] = { 0 };
    if (realpath(raw, real) == nullptr || real[0] != '/') {
        return TE_ERR_NOTFOUND;
    }
    real[sizeof real - 1] = '\0';
    return str_copy(out, outsz, real);
}

/* The quarantine attribute browsers set: Gatekeeper then checks the file before
 * it is run or opened as an app. Format: flags;hex time;agent;event id. */
void os_mark_from_internet(const char *path)
{
    if (path == nullptr || path[0] == '\0') {
        return;
    }
    char value[96] = { 0 };
    time_t now = time(nullptr);
    unsigned long long t = now > 0 ? (unsigned long long)now : 0u;
    if (str_fmt(value, sizeof value, "0081;%08llx;%s;", t, APP_DISPLAY_NAME) != TE_OK) {
        return;
    }
    (void)setxattr(path, "com.apple.quarantine", value, strlen(value), 0, XATTR_NOFOLLOW);
}
#else
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
#endif

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
