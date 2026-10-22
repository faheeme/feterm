#include "config/paths.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "util/str.h"

te_err paths_config_dir(char *out, size_t outsz)
{
    const char *xdg = getenv("XDG_CONFIG_HOME");
    if (xdg != nullptr && xdg[0] == '/') {
        return str_fmt(out, outsz, "%s/%s", xdg, APP_NAME);
    }
    const char *home = getenv("HOME");
    if (home == nullptr || home[0] != '/') {
        return TE_ERR_NOTFOUND;
    }
    return str_fmt(out, outsz, "%s/.config/%s", home, APP_NAME);
}

static te_err exe_dir(char *out, size_t outsz)
{
    char buf[TE_PATH_MAX] = { 0 };
    ssize_t n = readlink("/proc/self/exe", buf, sizeof buf - 1);
    if (n <= 0 || (size_t)n >= sizeof buf) {
        return TE_ERR_SYS;
    }
    buf[n] = '\0';
    char *slash = strrchr(buf, '/');
    if (slash == nullptr) {
        return TE_ERR_NOTFOUND;
    }
    *slash = '\0';
    return str_copy(out, outsz, buf);
}

static bool try_path(char *out, size_t outsz, const char *base, const char *rel)
{
    if (str_fmt(out, outsz, "%s/%s", base, rel) != TE_OK) {
        return false;
    }
    return access(out, R_OK) == 0;
}

te_err paths_find_data(const char *rel, char *out, size_t outsz)
{
    if (rel == nullptr || out == nullptr || outsz == 0 || rel[0] == '/' || strstr(rel, "..") != nullptr) {
        return TE_ERR_INVAL;
    }
    char base[TE_PATH_MAX] = { 0 };
    if (paths_config_dir(base, sizeof base) == TE_OK && try_path(out, outsz, base, rel)) {
        return TE_OK;
    }
    if (exe_dir(base, sizeof base) == TE_OK && str_append(base, sizeof base, "/data") == TE_OK &&
        try_path(out, outsz, base, rel)) {
        return TE_OK;
    }
    if (try_path(out, outsz, APP_DATADIR, rel)) {
        return TE_OK;
    }
    out[0] = '\0';
    return TE_ERR_NOTFOUND;
}

size_t paths_data_dirs(char dirs[][TE_PATH_MAX], size_t max)
{
    size_t n = 0;
    char base[TE_PATH_MAX] = { 0 };
    if (n < max && str_copy(dirs[n], TE_PATH_MAX, APP_DATADIR) == TE_OK) {
        n++;
    }
    if (n < max && exe_dir(base, sizeof base) == TE_OK &&
        str_fmt(dirs[n], TE_PATH_MAX, "%s/data", base) == TE_OK && strcmp(dirs[n], APP_DATADIR) != 0) {
        n++;
    }
    if (n < max && paths_config_dir(dirs[n], TE_PATH_MAX) == TE_OK) {
        n++;
    }
    return n;
}

bool paths_is_safe_name(const char *name)
{
    if (name == nullptr || name[0] == '\0' || name[0] == '.') {
        return false;
    }
    size_t n = 0;
    for (const char *p = name; *p != '\0'; p++, n++) {
        char c = *p;
        bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                  c == '_' || c == '-' || c == '.';
        if (!ok || n >= 128) {
            return false;
        }
    }
    return true;
}
