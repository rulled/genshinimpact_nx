/* Late host-allocation broker.
 *
 * Newlib remains the primary allocator.  Only an actual primary allocation
 * failure may borrow direct RW pages from the shared sparse backing pool.  The
 * linker routes public host allocation entry points here so Mesa/NVK, libc++,
 * SDL, and wrapper code all preserve normal ownership until the fragmented
 * arena can no longer satisfy a request.  Pool pointers are never passed to
 * newlib free/realloc/usable-size.
 */

#define _GNU_SOURCE

#include <errno.h>
#include <malloc.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "libc_shim.h"
#include "memory_broker.h"

void *__real_malloc(size_t size);
void *__real_calloc(size_t count, size_t size);
void *__real_realloc(void *pointer, size_t size);
void __real_free(void *pointer);
void *__real_memalign(size_t alignment, size_t size);
void *__real_aligned_alloc(size_t alignment, size_t size);
size_t __real_malloc_usable_size(void *pointer);

/* libnx implements C TLS through TPIDRRO_EL0, so this guard remains valid
 * while Android guest code owns TPIDR_EL0. */
static _Thread_local unsigned memory_broker_depth;

static int valid_power_of_two(size_t value) {
  return value && (value & (value - 1u)) == 0;
}

static void *host_pool_fallback(size_t size, size_t alignment, int clear) {
  if (!size) return NULL;
  if (memory_broker_depth) return NULL;
  ++memory_broker_depth;
  void *result = nx_sparse_pool_host_alloc_aligned(size, alignment);
  if (result && clear) memset(result, 0, size);
  --memory_broker_depth;
  return result;
}

void *nx_primary_malloc(size_t size) {
  return __real_malloc(size);
}

void *nx_primary_calloc(size_t count, size_t size) {
  return __real_calloc(count, size);
}

void *nx_primary_realloc(void *pointer, size_t size) {
  return __real_realloc(pointer, size);
}

void nx_primary_free(void *pointer) {
  __real_free(pointer);
}

void *nx_primary_memalign(size_t alignment, size_t size) {
  return __real_memalign(alignment, size);
}

size_t nx_primary_malloc_usable_size(void *pointer) {
  return pointer ? __real_malloc_usable_size(pointer) : 0;
}

void *__wrap_malloc(size_t size) {
  const int caller_errno = errno;
  void *result = __real_malloc(size);
  if (result || !size) return result;
  const int primary_errno = errno;
  result = host_pool_fallback(size, _Alignof(max_align_t), 0);
  errno = result ? caller_errno : primary_errno;
  return result;
}

void *__wrap_calloc(size_t count, size_t size) {
  const int caller_errno = errno;
  void *result = __real_calloc(count, size);
  if (result || !count || !size) return result;
  const int primary_errno = errno;
  if (count > SIZE_MAX / size) {
    errno = ENOMEM;
    return NULL;
  }
  result = host_pool_fallback(count * size, _Alignof(max_align_t), 1);
  errno = result ? caller_errno : primary_errno;
  return result;
}

void __wrap_free(void *pointer) {
  if (!pointer) return;
  if (nx_sparse_pool_owned_release(pointer)) return;
  /* Query/release deliberately fail closed while the current thread owns the
   * pool lock, and a live native stack makes its source temporarily non-RW.
   * In either case forwarding a pool address to newlib would corrupt its heap. */
  if (nx_sparse_pool_contains_address(pointer)) return;
  __real_free(pointer);
}

void *__wrap_realloc(void *pointer, size_t size) {
  if (!pointer) return __wrap_malloc(size);

  size_t pool_usable = 0;
  if (nx_sparse_pool_owned_query(pointer, NULL, &pool_usable)) {
    if (!size) {
      (void)nx_sparse_pool_owned_release(pointer);
      return NULL;
    }
    if (size <= pool_usable) return pointer;
    void *replacement = __wrap_malloc(size);
    if (!replacement) return NULL;
    memcpy(replacement, pointer, pool_usable < size ? pool_usable : size);
    (void)nx_sparse_pool_owned_release(pointer);
    return replacement;
  }
  if (nx_sparse_pool_contains_address(pointer)) {
    errno = ENOMEM;
    return NULL;
  }

  if (!size) return __real_realloc(pointer, 0);
  const int caller_errno = errno;
  void *result = __real_realloc(pointer, size);
  if (result) return result;
  const int primary_errno = errno;
  const size_t old_usable = __real_malloc_usable_size(pointer);
  result = host_pool_fallback(size, _Alignof(max_align_t), 0);
  if (!result) {
    errno = primary_errno;
    return NULL;
  }
  memcpy(result, pointer, old_usable < size ? old_usable : size);
  __real_free(pointer);
  errno = caller_errno;
  return result;
}

void *__wrap_memalign(size_t alignment, size_t size) {
  const int caller_errno = errno;
  void *result = __real_memalign(alignment, size);
  if (result || !size) return result;
  const int primary_errno = errno;
  if (!valid_power_of_two(alignment) || alignment < sizeof(void *)) {
    errno = EINVAL;
    return NULL;
  }
  result = host_pool_fallback(size, alignment, 0);
  errno = result ? caller_errno : primary_errno;
  return result;
}

void *__wrap_aligned_alloc(size_t alignment, size_t size) {
  const int caller_errno = errno;
  void *result = __real_aligned_alloc(alignment, size);
  if (result || !size) return result;
  const int primary_errno = errno;
  if (!valid_power_of_two(alignment) || size % alignment) {
    errno = EINVAL;
    return NULL;
  }
  result = host_pool_fallback(size, alignment, 0);
  errno = result ? caller_errno : primary_errno;
  return result;
}

int __wrap_posix_memalign(void **result_out, size_t alignment, size_t size) {
  if (!result_out || alignment < sizeof(void *) ||
      !valid_power_of_two(alignment))
    return EINVAL;

  const int caller_errno = errno;
  void *result = __real_memalign(alignment, size);
  if (result || !size) {
    *result_out = result;
    errno = caller_errno;
    return 0;
  }
  void *replacement = host_pool_fallback(size, alignment, 0);
  errno = caller_errno;
  if (!replacement) return ENOMEM;
  *result_out = replacement;
  return 0;
}

size_t __wrap_malloc_usable_size(void *pointer) {
  size_t usable = 0;
  if (nx_sparse_pool_owned_query(pointer, NULL, &usable)) return usable;
  if (nx_sparse_pool_contains_address(pointer)) return 0;
  return pointer ? __real_malloc_usable_size(pointer) : 0;
}
