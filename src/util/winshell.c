#include "util/winshell.h"

#include <string.h>

#include "util/str.h"

static const struct {
    const char *name;
    te_wsh_kind kind;
} presets[] = {
    { "auto", TE_WSH_AUTO },       { "pwsh", TE_WSH_PWSH },     { "powershell", TE_WSH_POWERSHELL },
    { "cmd", TE_WSH_CMD },         { "gitbash", TE_WSH_GITBASH }, { "wsl", TE_WSH_WSL },
    { "msys2", TE_WSH_MSYS2 },     { "cygwin", TE_WSH_CYGWIN }, { "nu", TE_WSH_NU },
};

static bool distro_ok(const char *s)
{
    size_t n = strlen(s);
    if (n == 0 || n >= TE_WSH_DISTRO_MAX || s[0] == '-') {
        return false;   /* "-..." would read as a wsl.exe option */
    }
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                  c == '.' || c == '_' || c == '-';
        if (!ok) {
            return false;
        }
    }
    return true;
}

te_wsh_kind winshell_parse(const char *value, char *distro, size_t distro_sz)
{
    if (distro != nullptr && distro_sz > 0) {
        distro[0] = '\0';
    }
    if (value == nullptr || value[0] == '\0') {
        return TE_WSH_AUTO;
    }
    if (strncmp(value, "wsl:", 4) == 0) {
        if (!distro_ok(value + 4) || distro == nullptr || str_copy(distro, distro_sz, value + 4) != TE_OK) {
            if (distro != nullptr && distro_sz > 0) {
                distro[0] = '\0';
            }
            return TE_WSH_INVALID;
        }
        return TE_WSH_WSL;
    }
    for (size_t i = 0; i < TE_ARRAY_LEN(presets); i++) {
        if (strcmp(value, presets[i].name) == 0) {
            return presets[i].kind;
        }
    }
    return TE_WSH_PATH;
}

const char *winshell_name(te_wsh_kind k)
{
    for (size_t i = 0; i < TE_ARRAY_LEN(presets); i++) {
        if (presets[i].kind == k && k != TE_WSH_AUTO) {
            return presets[i].name;
        }
    }
    return "";
}

te_err winshell_split_args(const char *s, te_wsh_args *out)
{
    if (out == nullptr) {
        return TE_ERR_INVAL;
    }
    memset(out, 0, sizeof *out);
    if (s == nullptr) {
        return TE_OK;
    }
    size_t at = 0;
    const char *p = s;
    for (;;) {
        while (*p == ' ' || *p == '\t') {
            p++;
        }
        if (*p == '\0') {
            return TE_OK;
        }
        if (out->argc >= TE_WSH_MAX_ARGS) {
            return TE_ERR_RANGE;
        }
        out->argv[out->argc++] = out->buf + at;
        bool quoted = false;
        while (*p != '\0' && (quoted || (*p != ' ' && *p != '\t'))) {
            if (*p == '"') {
                quoted = !quoted;
                p++;
                continue;
            }
            if (at + 1 >= sizeof out->buf) {
                return TE_ERR_RANGE;
            }
            out->buf[at++] = *p++;
        }
        if (quoted) {
            return TE_ERR_INVAL;   /* unbalanced quote */
        }
        if (at + 1 >= sizeof out->buf) {
            return TE_ERR_RANGE;
        }
        out->buf[at++] = '\0';
    }
}
