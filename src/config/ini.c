#include "config/ini.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "util/log.h"
#include "util/str.h"

/* Discard the remainder of an over-long line. */
static void skip_rest_of_line(FILE *fp)
{
    int c = 0;
    while ((c = fgetc(fp)) != EOF && c != '\n') {
    }
}

static FILE *open_checked(const char *path)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOCTTY);
    if (fd < 0) {
        return nullptr;
    }
    struct stat st = { 0 };
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
        close(fd);
        errno = EINVAL;
        return nullptr;
    }
    if (st.st_size > INI_MAX_FILE) {
        LOG_W("%s: file larger than %d bytes, ignored", path, INI_MAX_FILE);
        close(fd);
        errno = EFBIG;
        return nullptr;
    }
    if ((st.st_mode & S_IWOTH) != 0) {
        LOG_W("%s: file is world-writable; anyone can change your terminal configuration", path);
    }
    FILE *fp = fdopen(fd, "r");
    if (fp == nullptr) {
        close(fd);
    }
    return fp;
}

te_err ini_parse_file(const char *path, ini_handler cb, void *user)
{
    if (path == nullptr || cb == nullptr) {
        return TE_ERR_INVAL;
    }
    FILE *fp = open_checked(path);
    if (fp == nullptr) {
        return errno == ENOENT ? TE_ERR_NOTFOUND : TE_ERR_IO;
    }

    te_err rc = TE_OK;
    char line[INI_MAX_LINE] = { 0 };
    char section[INI_MAX_NAME] = { 0 };
    unsigned lineno = 0;

    while (fgets(line, (int)sizeof line, fp) != nullptr) {
        lineno++;
        size_t len = strnlen(line, sizeof line);
        if (len == sizeof line - 1 && line[len - 1] != '\n' && !feof(fp)) {
            LOG_W("%s:%u: line longer than %d bytes, ignored", path, lineno, INI_MAX_LINE - 1);
            skip_rest_of_line(fp);
            continue;
        }
        char *s = str_trim(line);
        if (*s == '\0' || *s == ';' || *s == '#') {
            continue;
        }
        if (*s == '[') {
            char *end = strchr(s, ']');
            if (end == nullptr) {
                LOG_W("%s:%u: unterminated section header", path, lineno);
                continue;
            }
            *end = '\0';
            char *name = str_trim(s + 1);
            if (str_copy(section, sizeof section, name) != TE_OK) {
                LOG_W("%s:%u: section name too long", path, lineno);
                section[0] = '\0';
            }
            continue;
        }
        char *eq = strchr(s, '=');
        if (eq == nullptr) {
            LOG_W("%s:%u: expected 'key = value'", path, lineno);
            continue;
        }
        *eq = '\0';
        char *key = str_trim(s);
        char *val = str_trim(eq + 1);
        if (*key == '\0' || strnlen(key, INI_MAX_NAME) >= INI_MAX_NAME) {
            LOG_W("%s:%u: empty or over-long key", path, lineno);
            continue;
        }
        rc = cb(user, section, key, val, path, lineno);
        if (rc != TE_OK) {
            break;
        }
    }
    if (rc == TE_OK && ferror(fp)) {
        rc = TE_ERR_IO;
    }
    fclose(fp);
    return rc;
}
