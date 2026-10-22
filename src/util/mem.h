/* Checked allocation wrappers. Every size computation is overflow-checked. */
#ifndef TE_MEM_H
#define TE_MEM_H

#include <stdlib.h>
#include "common.h"

/* Free and null the pointer so it can never be used or freed again. */
#define TE_FREE(p) do { free(p); (p) = nullptr; } while (0)

/* Returns nullptr on failure, on zero size, or if count*size overflows. Memory is zeroed. */
[[nodiscard]] void *te_calloc(size_t count, size_t size);

/* Returns nullptr on failure or zero size. Memory is NOT zeroed. */
[[nodiscard]] void *te_malloc(size_t size);

/*
 * Resize *ptr to count*size bytes. On failure *ptr is untouched (still valid,
 * still owned by the caller). New bytes beyond old_count are zeroed.
 */
[[nodiscard]] te_err te_realloc_array(void **ptr, size_t old_count, size_t new_count, size_t size);

#endif
