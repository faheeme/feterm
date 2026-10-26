#ifndef TE_LOG_H
#define TE_LOG_H

#include "common.h"

typedef enum { TE_LOG_DEBUG, TE_LOG_INFO, TE_LOG_WARN, TE_LOG_ERROR } te_log_level;

void te_log_set_level(te_log_level lvl);
void te_log(te_log_level lvl, const char *fmt, ...) TE_PRINTF(2, 3);

#define LOG_D(...) te_log(TE_LOG_DEBUG, __VA_ARGS__)
#define LOG_I(...) te_log(TE_LOG_INFO, __VA_ARGS__)
#define LOG_W(...) te_log(TE_LOG_WARN, __VA_ARGS__)
#define LOG_E(...) te_log(TE_LOG_ERROR, __VA_ARGS__)

#endif
