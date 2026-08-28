#ifndef GENSHIN_MEMORY_BROKER_H
#define GENSHIN_MEMORY_BROKER_H

#include <stddef.h>

/* Bypass the linker's host-OOM fallback.  Guest allocator shims use these
 * entry points so a failed primary allocation is attributed to the guest
 * owner and released through the guest ABI rather than silently becoming a
 * host allocation. */
void *nx_primary_malloc(size_t size);
void *nx_primary_calloc(size_t count, size_t size);
void *nx_primary_realloc(void *pointer, size_t size);
void nx_primary_free(void *pointer);
void *nx_primary_memalign(size_t alignment, size_t size);
size_t nx_primary_malloc_usable_size(void *pointer);

#endif
