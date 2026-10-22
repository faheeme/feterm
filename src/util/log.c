#include "util/log.h"

#include <stdarg.h>
#include <stdio.h>

static te_log_level g_level = TE_LOG_INFO;

void te_log_set_level(te_log_level lvl)
{
    g_level = lvl;
}

void te_log(te_log_level lvl, const char *fmt, ...)
{
    static const char *const names[] = { "debug", "info", "warn", "error" };
    if (lvl < g_level || fmt == nullptr) {
        return;
    }
    size_t idx = (size_t)lvl;
    if (idx >= TE_ARRAY_LEN(names)) {
        idx = TE_ARRAY_LEN(names) - 1;
    }
    char msg[1024] = { 0 };
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    if (n < 0) {
        return;
    }
    msg[sizeof msg - 1] = '\0';
    /* Strip control characters so log output cannot inject terminal escapes. */
    for (size_t i = 0; msg[i] != '\0'; i++) {
        unsigned char c = (unsigned char)msg[i];
        if (c < 0x20 || c == 0x7f) {
            msg[i] = '?';
        }
    }
    fprintf(stderr, "%s: %s: %s\n", APP_NAME, names[idx], msg);
}

const char *te_strerror(te_err e)
{
    switch (e) {
    case TE_OK:           return "success";
    case TE_ERR_NOMEM:    return "out of memory";
    case TE_ERR_INVAL:    return "invalid argument";
    case TE_ERR_RANGE:    return "value out of range";
    case TE_ERR_OVERFLOW: return "integer overflow";
    case TE_ERR_TRUNC:    return "truncated";
    case TE_ERR_IO:       return "I/O error";
    case TE_ERR_SYS:      return "system call failed";
    case TE_ERR_PARSE:    return "parse error";
    case TE_ERR_NOTFOUND: return "not found";
    case TE_ERR_PLATFORM: return "platform error";
    case TE_ERR_FONT:     return "font error";
    case TE_ERR_GL:       return "OpenGL error";
    case TE_ERR_CHILD:    return "child process error";
    }
    return "unknown error";
}
