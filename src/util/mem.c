#include "util/mem.h"

#include <string.h>

void *te_calloc(size_t count, size_t size)
{
    if (count == 0 || size == 0) {
        return nullptr;
    }
    size_t total = 0;
    if (ckd_mul(&total, count, size)) {
        return nullptr;
    }
    return calloc(count, size);
}

void *te_malloc(size_t size)
{
    if (size == 0) {
        return nullptr;
    }
    return malloc(size);
}

te_err te_realloc_array(void **ptr, size_t old_count, size_t new_count, size_t size)
{
    if (ptr == nullptr || size == 0) {
        return TE_ERR_INVAL;
    }
    if (new_count == 0) {
        TE_FREE(*ptr);
        return TE_OK;
    }
    size_t total = 0;
    if (ckd_mul(&total, new_count, size)) {
        return TE_ERR_OVERFLOW;
    }
    void *tmp = realloc(*ptr, total);
    if (tmp == nullptr) {
        return TE_ERR_NOMEM; /* *ptr still valid and owned by caller */
    }
    if (new_count > old_count) {
        size_t old_bytes = 0;
        if (ckd_mul(&old_bytes, old_count, size) || old_bytes > total) {
            memset(tmp, 0, total);
        } else {
            memset((unsigned char *)tmp + old_bytes, 0, total - old_bytes);
        }
    }
    *ptr = tmp;
    return TE_OK;
}
