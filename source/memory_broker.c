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

#include "config.h"
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

/* ---- allocation-size histogram (diagnostic instrumentation) ----
 *
 * The NAK login-burst panic is open_memstream failing while dlmalloc is full,
 * even though the donor/pool still holds free pages.  Routing large public
 * allocations to the reclaimable broker pool would keep dlmalloc lean enough
 * for the tiny newlib-internal buffers, but only if the bytes that fill
 * dlmalloc are dominated by large allocations.  This histogram measures that:
 * cumulative calls/bytes/fallbacks per size bucket, and the peak *live* bytes
 * held in dlmalloc per bucket (added on a non-pool success, removed on free).
 * All counters are plain atomics; recording never allocates.
 *
 * At the frame-232 NAK panic (LoginMainPageContext + FSR burst), peak-live
 * dlmalloc residency was 472 MiB, of which 465 MiB sat in >=16 MiB
 * blocks (three allocations alone totalled 399MiB), while every sub-64KiB
 * bucket held under 1MiB live despite ~30k transient calls.  Large-allocation
 * routing to the pool is therefore implemented (OC_BROKER_LARGE_ALLOC_BYTES). */

#define HIST_BUCKETS 12
static const size_t hist_upper[HIST_BUCKETS] = {
  64, 256, 1024, 4096, 16384, 65536,
  262144, 1048576, 4194304, 16777216, 67108864, SIZE_MAX
};
static uint64_t hist_calls[HIST_BUCKETS];
static uint64_t hist_bytes[HIST_BUCKETS];
static uint64_t hist_fallback[HIST_BUCKETS];
static uint64_t hist_live[HIST_BUCKETS];
static uint64_t hist_peak_live[HIST_BUCKETS];
static uint64_t hist_total_live;
static uint64_t hist_peak_total_live;

static int hist_bucket(size_t n) {
  for (int i = 0; i < HIST_BUCKETS; ++i)
    if (n <= hist_upper[i]) return i;
  return HIST_BUCKETS - 1;
}

static void hist_peak_bump(uint64_t *peak, uint64_t value) {
  uint64_t seen = __atomic_load_n(peak, __ATOMIC_RELAXED);
  while (value > seen &&
         !__atomic_compare_exchange_n(peak, &seen, value, 0,
                                      __ATOMIC_RELAXED, __ATOMIC_RELAXED))
    ;
}

static void hist_record_request(size_t requested, int fell_back) {
  const int b = hist_bucket(requested);
  __atomic_add_fetch(&hist_calls[b], 1, __ATOMIC_RELAXED);
  __atomic_add_fetch(&hist_bytes[b], requested, __ATOMIC_RELAXED);
  if (fell_back) __atomic_add_fetch(&hist_fallback[b], 1, __ATOMIC_RELAXED);
}

/* Track live dlmalloc residency by usable size so add/remove stay consistent
 * for the same pointer regardless of the original requested size. */
static void hist_live_add(void *pointer) {
  if (!pointer) return;
  const size_t usable = __real_malloc_usable_size(pointer);
  if (!usable) return;
  const int b = hist_bucket(usable);
  const uint64_t bucket_live =
    __atomic_add_fetch(&hist_live[b], usable, __ATOMIC_RELAXED);
  hist_peak_bump(&hist_peak_live[b], bucket_live);
  const uint64_t total =
    __atomic_add_fetch(&hist_total_live, usable, __ATOMIC_RELAXED);
  hist_peak_bump(&hist_peak_total_live, total);
}

static void hist_live_remove_size(size_t usable) {
  if (!usable) return;
  const int b = hist_bucket(usable);
  /* Clamp to zero: frees of pointers allocated before this instrumentation
   * existed (or through unwrapped paths) would otherwise underflow the
   * unsigned counters and produce nonsense peaks. */
  uint64_t seen = __atomic_load_n(&hist_live[b], __ATOMIC_RELAXED);
  while (seen && !__atomic_compare_exchange_n(
             &hist_live[b], &seen,
             seen > usable ? seen - usable : 0, 0,
             __ATOMIC_RELAXED, __ATOMIC_RELAXED))
    ;
  uint64_t total = __atomic_load_n(&hist_total_live, __ATOMIC_RELAXED);
  while (total && !__atomic_compare_exchange_n(
              &hist_total_live, &total,
              total > usable ? total - usable : 0, 0,
              __ATOMIC_RELAXED, __ATOMIC_RELAXED))
    ;
}

static void hist_live_remove(void *pointer) {
  if (!pointer) return;
  hist_live_remove_size(__real_malloc_usable_size(pointer));
}

/* Serve a large public request directly from the reclaimable pool so it never
 * lands in dlmalloc.  Returns NULL when the pool cannot serve it, in which
 * case the caller keeps its original dlmalloc path. */
static void *host_pool_route(size_t size, size_t alignment, int clear) {
  void *result = host_pool_fallback(size, alignment, clear);
  if (result) hist_record_request(size, 1);
  return result;
}

void memory_broker_histogram_report(FILE *out) {
  if (!out) return;
  fprintf(out,
          "alloc_histogram peak_live_total=%lluMiB now_live_total=%lluMiB "
          "large_threshold=%zuKiB\n",
          (unsigned long long)(__atomic_load_n(&hist_peak_total_live,
                                               __ATOMIC_RELAXED) /
                               (1024ull * 1024ull)),
          (unsigned long long)(__atomic_load_n(&hist_total_live,
                                               __ATOMIC_RELAXED) /
                               (1024ull * 1024ull)),
          OC_BROKER_LARGE_ALLOC_BYTES / 1024);
  fprintf(out, "  bucket        calls        req_MiB   fallbacks  peak_live_MiB\n");
  for (int i = 0; i < HIST_BUCKETS; ++i) {
    const unsigned long long calls =
      __atomic_load_n(&hist_calls[i], __ATOMIC_RELAXED);
    if (!calls && !__atomic_load_n(&hist_peak_live[i], __ATOMIC_RELAXED))
      continue;
    fprintf(out,
            "  <=%-10zu %10llu %12llu %11llu %13llu\n",
            hist_upper[i],
            calls,
            (unsigned long long)(__atomic_load_n(&hist_bytes[i],
                                                 __ATOMIC_RELAXED) /
                                 (1024ull * 1024ull)),
            (unsigned long long)__atomic_load_n(&hist_fallback[i],
                                                __ATOMIC_RELAXED),
            (unsigned long long)(__atomic_load_n(&hist_peak_live[i],
                                                 __ATOMIC_RELAXED) /
                                 (1024ull * 1024ull)));
  }
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
  if (size >= OC_BROKER_LARGE_ALLOC_BYTES) {
    void *pooled = host_pool_route(size, _Alignof(max_align_t), 0);
    if (pooled) {
      errno = caller_errno;
      return pooled;
    }
  }
  void *result = __real_malloc(size);
  if (result || !size) {
    if (result) hist_live_add(result);
    if (size) hist_record_request(size, 0);
    return result;
  }
  const int primary_errno = errno;
  result = host_pool_fallback(size, _Alignof(max_align_t), 0);
  errno = result ? caller_errno : primary_errno;
  hist_record_request(size, 1);
  return result;
}

void *__wrap_calloc(size_t count, size_t size) {
  const int caller_errno = errno;
  if (count && size && count <= SIZE_MAX / size &&
      count * size >= OC_BROKER_LARGE_ALLOC_BYTES) {
    void *pooled = host_pool_route(count * size, _Alignof(max_align_t), 1);
    if (pooled) {
      errno = caller_errno;
      return pooled;
    }
  }
  void *result = __real_calloc(count, size);
  if (result || !count || !size) {
    if (result) {
      hist_live_add(result);
      hist_record_request(count * size, 0);
    }
    return result;
  }
  const int primary_errno = errno;
  if (count > SIZE_MAX / size) {
    errno = ENOMEM;
    return NULL;
  }
  result = host_pool_fallback(count * size, _Alignof(max_align_t), 1);
  errno = result ? caller_errno : primary_errno;
  hist_record_request(count * size, 1);
  return result;
}

void __wrap_free(void *pointer) {
  if (!pointer) return;
  if (nx_sparse_pool_owned_release(pointer)) return;
  /* Query/release deliberately fail closed while the current thread owns the
   * pool lock, and a live native stack makes its source temporarily non-RW.
   * In either case forwarding a pool address to newlib would corrupt its heap. */
  if (nx_sparse_pool_contains_address(pointer)) return;
  hist_live_remove(pointer);
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

  if (!size) {
    hist_live_remove(pointer);
    return __real_realloc(pointer, 0);
  }
  const int caller_errno = errno;
  /* A growth that crosses the large threshold migrates the block out of
   * dlmalloc into the reclaimable pool (copy + free) so the primary heap
   * shrinks back once big buffers are released. */
  if (size >= OC_BROKER_LARGE_ALLOC_BYTES) {
    const size_t old_usable = __real_malloc_usable_size(pointer);
    void *pooled = host_pool_route(size, _Alignof(max_align_t), 0);
    if (pooled) {
      memcpy(pooled, pointer, old_usable < size ? old_usable : size);
      hist_live_remove(pointer);
      __real_free(pointer);
      errno = caller_errno;
      return pooled;
    }
  }
  const size_t prev_usable = __real_malloc_usable_size(pointer);
  void *result = __real_realloc(pointer, size);
  if (result) {
    /* dlmalloc may resize in place or move; re-balance live tracking by the
     * usable-size delta.  Removing the old residency uses the pre-realloc
     * usable size captured above. */
    hist_live_remove_size(prev_usable);
    hist_live_add(result);
    hist_record_request(size, 0);
    return result;
  }
  const int primary_errno = errno;
  const size_t old_usable = prev_usable;
  result = host_pool_fallback(size, _Alignof(max_align_t), 0);
  if (!result) {
    errno = primary_errno;
    return NULL;
  }
  memcpy(result, pointer, old_usable < size ? old_usable : size);
  hist_live_remove(pointer);
  __real_free(pointer);
  errno = caller_errno;
  hist_record_request(size, 1);
  return result;
}

void *__wrap_memalign(size_t alignment, size_t size) {
  const int caller_errno = errno;
  if (size >= OC_BROKER_LARGE_ALLOC_BYTES && valid_power_of_two(alignment) &&
      alignment >= sizeof(void *)) {
    void *pooled = host_pool_route(size, alignment, 0);
    if (pooled) {
      errno = caller_errno;
      return pooled;
    }
  }
  void *result = __real_memalign(alignment, size);
  if (result || !size) {
    if (result) {
      hist_live_add(result);
      hist_record_request(size, 0);
    }
    return result;
  }
  const int primary_errno = errno;
  if (!valid_power_of_two(alignment) || alignment < sizeof(void *)) {
    errno = EINVAL;
    return NULL;
  }
  result = host_pool_fallback(size, alignment, 0);
  errno = result ? caller_errno : primary_errno;
  hist_record_request(size, 1);
  return result;
}

void *__wrap_aligned_alloc(size_t alignment, size_t size) {
  const int caller_errno = errno;
  if (size >= OC_BROKER_LARGE_ALLOC_BYTES && valid_power_of_two(alignment) &&
      size % alignment == 0) {
    void *pooled = host_pool_route(size, alignment, 0);
    if (pooled) {
      errno = caller_errno;
      return pooled;
    }
  }
  void *result = __real_aligned_alloc(alignment, size);
  if (result || !size) {
    if (result) {
      hist_live_add(result);
      hist_record_request(size, 0);
    }
    return result;
  }
  const int primary_errno = errno;
  if (!valid_power_of_two(alignment) || size % alignment) {
    errno = EINVAL;
    return NULL;
  }
  result = host_pool_fallback(size, alignment, 0);
  errno = result ? caller_errno : primary_errno;
  hist_record_request(size, 1);
  return result;
}

int __wrap_posix_memalign(void **result_out, size_t alignment, size_t size) {
  if (!result_out || alignment < sizeof(void *) ||
      !valid_power_of_two(alignment))
    return EINVAL;

  const int caller_errno = errno;
  if (size >= OC_BROKER_LARGE_ALLOC_BYTES) {
    void *pooled = host_pool_route(size, alignment, 0);
    if (pooled) {
      errno = caller_errno;
      *result_out = pooled;
      return 0;
    }
  }
  void *result = __real_memalign(alignment, size);
  if (result || !size) {
    *result_out = result;
    errno = caller_errno;
    if (result) {
      hist_live_add(result);
      hist_record_request(size, 0);
    }
    return 0;
  }
  void *replacement = host_pool_fallback(size, alignment, 0);
  errno = caller_errno;
  if (!replacement) return ENOMEM;
  *result_out = replacement;
  hist_record_request(size, 1);
  return 0;
}

size_t __wrap_malloc_usable_size(void *pointer) {
  size_t usable = 0;
  if (nx_sparse_pool_owned_query(pointer, NULL, &usable)) return usable;
  if (nx_sparse_pool_contains_address(pointer)) return 0;
  return pointer ? __real_malloc_usable_size(pointer) : 0;
}
