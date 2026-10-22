/* Shared definitions: error codes and small helpers used by every module. */
#ifndef TE_COMMON_H
#define TE_COMMON_H

#include <stddef.h>
#include <stdint.h>
#include <stdckdint.h>

#include "app_name.h"

typedef enum te_err {
    TE_OK = 0,
    TE_ERR_NOMEM,
    TE_ERR_INVAL,
    TE_ERR_RANGE,
    TE_ERR_OVERFLOW,
    TE_ERR_TRUNC,
    TE_ERR_IO,
    TE_ERR_SYS,
    TE_ERR_PARSE,
    TE_ERR_NOTFOUND,
    TE_ERR_PLATFORM,
    TE_ERR_FONT,
    TE_ERR_GL,
    TE_ERR_CHILD,
    TE_ERR_EXISTS,
    TE_ERR_PERM,
} te_err;

[[nodiscard]] const char *te_strerror(te_err e);

#define TE_ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))
#define TE_MIN(a, b) ((a) < (b) ? (a) : (b))
#define TE_MAX(a, b) ((a) > (b) ? (a) : (b))
#define TE_CLAMP(v, lo, hi) ((v) < (lo) ? (lo) : ((v) > (hi) ? (hi) : (v)))

#endif
