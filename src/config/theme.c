#include "config/theme.h"

#include <dirent.h>
#include <string.h>

#include "config/ini.h"
#include "config/paths.h"
#include "util/log.h"
#include "util/str.h"

void theme_defaults(te_theme *t)
{
    static const uint32_t mocha[16] = {
        0x45475a, 0xf38ba8, 0xa6e3a1, 0xf9e2af, 0x89b4fa, 0xf5c2e7, 0x94e2d5, 0xbac2de,
        0x585b70, 0xf38ba8, 0xa6e3a1, 0xf9e2af, 0x89b4fa, 0xf5c2e7, 0x94e2d5, 0xa6adc8,
    };
    memset(t, 0, sizeof *t);
    (void)str_copy(t->name, sizeof t->name, "builtin");
    memcpy(t->ansi, mocha, sizeof mocha);
    t->fg = 0xcdd6f4;
    t->bg = 0x1e1e2e;
    t->cursor = 0xf5e0dc;
    t->cursor_text = 0x1e1e2e;
    t->selection_bg = 0x585b70;
    t->selection_fg = 0xcdd6f4;
    t->search_bg = 0xf9e2af;
    t->search_fg = 0x1e1e2e;
    t->search_current_bg = 0xfab387;
}

static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool color_parse(const char *s, uint32_t *rgb)
{
    if (s == nullptr || rgb == nullptr) {
        return false;
    }
    if (s[0] == '#') {
        s++;
    }
    if (strnlen(s, 7) != 6) {
        return false;
    }
    uint32_t v = 0;
    for (size_t i = 0; i < 6; i++) {
        int h = hexval(s[i]);
        if (h < 0) {
            return false;
        }
        v = (v << 4) | (uint32_t)h;
    }
    *rgb = v;
    return true;
}

static te_err theme_kv(void *user, const char *section, const char *key, const char *value,
                       const char *path, unsigned line)
{
    te_theme *t = user;
    (void)section;
    uint32_t c = 0;
    if (!color_parse(value, &c)) {
        LOG_W("%s:%u: invalid colour '%s' (expected #rrggbb)", path, line, value);
        return TE_OK;
    }
    if (strncmp(key, "color", 5) == 0) {
        long idx = 0;
        if (!str_to_long(key + 5, 0, 15, &idx)) {
            LOG_W("%s:%u: palette index must be color0..color15", path, line);
            return TE_OK;
        }
        t->ansi[(size_t)idx] = c;
    } else if (strcmp(key, "foreground") == 0) {
        t->fg = c;
    } else if (strcmp(key, "background") == 0) {
        t->bg = c;
    } else if (strcmp(key, "cursor") == 0) {
        t->cursor = c;
    } else if (strcmp(key, "cursor_text") == 0) {
        t->cursor_text = c;
    } else if (strcmp(key, "selection_background") == 0) {
        t->selection_bg = c;
    } else if (strcmp(key, "selection_foreground") == 0) {
        t->selection_fg = c;
    } else if (strcmp(key, "search_background") == 0) {
        t->search_bg = c;
    } else if (strcmp(key, "search_foreground") == 0) {
        t->search_fg = c;
    } else if (strcmp(key, "search_current_background") == 0) {
        t->search_current_bg = c;
    } else {
        LOG_W("%s:%u: unknown theme key '%s'", path, line, key);
    }
    return TE_OK;
}

te_err theme_load(te_theme *t, const char *name)
{
    if (t == nullptr) {
        return TE_ERR_INVAL;
    }
    theme_defaults(t);
    if (name == nullptr || name[0] == '\0' || strcmp(name, "builtin") == 0) {
        return TE_OK;
    }
    if (!paths_is_safe_name(name)) {
        LOG_W("theme name '%s' contains illegal characters", name);
        return TE_ERR_INVAL;
    }
    char rel[256] = { 0 };
    char path[TE_PATH_MAX] = { 0 };
    te_err rc = str_fmt(rel, sizeof rel, "themes/%s.ini", name);
    if (rc != TE_OK) {
        return rc;
    }
    rc = paths_find_data(rel, path, sizeof path);
    if (rc != TE_OK) {
        LOG_W("theme '%s' not found, using built-in theme", name);
        return rc;
    }
    te_theme tmp = *t;
    rc = ini_parse_file(path, theme_kv, &tmp);
    if (rc != TE_OK) {
        LOG_W("failed to read theme %s: %s", path, te_strerror(rc));
        return rc;
    }
    *t = tmp;
    (void)str_copy(t->name, sizeof t->name, name);
    return TE_OK;
}

size_t theme_list(char names[][128], size_t max)
{
    char dirs[3][TE_PATH_MAX] = { { 0 } };
    size_t nd = paths_data_dirs(dirs, 3);
    size_t n = 0;
    for (size_t d = 0; d < nd; d++) {
        char path[TE_PATH_MAX] = { 0 };
        if (str_fmt(path, sizeof path, "%s/themes", dirs[d]) != TE_OK) {
            continue;
        }
        DIR *dir = opendir(path);
        if (dir == nullptr) {
            continue;
        }
        struct dirent *e = nullptr;
        while ((e = readdir(dir)) != nullptr && n < max) {
            size_t len = strnlen(e->d_name, sizeof e->d_name);
            if (len <= 4 || len >= 128 || strcmp(e->d_name + len - 4, ".ini") != 0 || !paths_is_safe_name(e->d_name)) {
                continue;
            }
            char stem[128] = { 0 };
            if (str_copy_n(stem, sizeof stem, e->d_name, len - 4) != TE_OK) {
                continue;
            }
            bool dup = false;
            for (size_t i = 0; i < n && !dup; i++) {
                dup = strcmp(names[i], stem) == 0;
            }
            if (!dup) {
                (void)str_copy(names[n++], 128, stem);
            }
        }
        closedir(dir);
    }
    /* Insertion sort: lists are small. */
    for (size_t i = 1; i < n; i++) {
        char tmp[128] = { 0 };
        (void)str_copy(tmp, sizeof tmp, names[i]);
        size_t j = i;
        while (j > 0 && strcmp(names[j - 1], tmp) > 0) {
            (void)str_copy(names[j], 128, names[j - 1]);
            j--;
        }
        (void)str_copy(names[j], 128, tmp);
    }
    return n;
}

void theme_build_palette(const te_theme *t, uint32_t pal[256])
{
    for (size_t i = 0; i < 16; i++) {
        pal[i] = t->ansi[i];
    }
    static const uint32_t steps[6] = { 0x00, 0x5f, 0x87, 0xaf, 0xd7, 0xff };
    size_t idx = 16;
    for (size_t r = 0; r < 6; r++) {
        for (size_t g = 0; g < 6; g++) {
            for (size_t b = 0; b < 6; b++) {
                pal[idx++] = (steps[r] << 16) | (steps[g] << 8) | steps[b];
            }
        }
    }
    for (uint32_t i = 0; i < 24; i++) {
        uint32_t v = 8 + i * 10;
        pal[idx++] = (v << 16) | (v << 8) | v;
    }
}
