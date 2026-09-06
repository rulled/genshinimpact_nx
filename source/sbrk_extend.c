/* newlib heap ceiling bypass.
 *
 * The NRO's newlib heap can only grow between _end and the main-thread stack
 * guard, which is a small, fixed window.  Public allocations survive through
 * the memory broker's sparse-pool fallback, but newlib-internal allocations
 * (stdio FILE glues, open_memstream cookies, locale, ...) go through
 * _malloc_r -> _sbrk_r with no fallback.  Once the ceiling is reached, every
 * internal allocation returns ENOMEM; the NVK/NAK Rust code unwraps an
 * io::Error from open_memstream() and panics (observed at nak/from_nir.rs:338
 * during the login-screen pipeline burst).
 *
 * Wrapping _sbrk_r lets us extend the dlmalloc arena with non-contiguous
 * blocks from the shared sparse host pool.  Full dlmalloc (the build provides
 * __malloc_max_sbrked_mem) fences off the old top chunk and accepts the new
 * block as a separate chunk, so this stays heap-corruption free.
 *
 * Ownership: extensions carry the dedicated OC_POOL_OWNER_SBRK owner and are
 * pinned for the process lifetime.  dlmalloc places chunk headers inside the
 * borrowed pages and frees user pointers back into its own free lists, so the
 * wrapper-level free/realloc/usable-size paths must never recycle or forward
 * them (a wildcard pool release here handed arena pages back to the pool and
 * produced _free_r unlink faults through live chunks).  Every extension range
 * is therefore registered below; sbrk_extension_class() lets the broker
 * wrappers classify a pointer lock-free BEFORE any pool or newlib call:
 *   - INTERIOR -> native newlib path (the bytes stay dlmalloc-owned),
 *   - BASE     -> no-op / ENOMEM (dlmalloc never hands out the raw base).
 *
 * The extension is deliberately CAPPED.  An uncapped extension is a one-way
 * ratchet: the login shader-compile burst drained the whole heap-donor into
 * the dlmalloc arena (observed at 1254 MiB / 309 extensions in fatal.txt),
 * starving the guest and Unity into an OOM data abort.  Once the cap is
 * reached, _sbrk_r fails, dlmalloc's malloc fails, and the request falls
 * through to the broker's reclaimable sparse-pool fallback - exactly the
 * ownership model the upstream author relies on.  The cap only has to cover
 * the irreducible newlib-internal working set (stdio glues, open_memstream
 * cookies) that cannot use the broker.
 *
 * All _sbrk_r callers come from dlmalloc while it holds the recursive malloc
 * lock, so the wrapper itself stays serialized.  Free-path readers do NOT
 * hold that lock, so the registry is published with release/acquire atomics
 * and is append-only (extensions are never returned).  Statistics are kept in
 * plain globals: performing stdio logging here would allocate from the heap
 * we are in the middle of extending.
 */

#define _GNU_SOURCE

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/reent.h>

#include "libc_shim.h"
#include "sbrk_extend.h"

void *__real__sbrk_r(struct _reent *reent, ptrdiff_t incr);

/* Serve at least this much per extension so repeated small sbrk requests do
 * not fragment the sparse pool into page-sized slivers. */
#define SBRK_EXTENSION_BLOCK_MIN ((size_t)4 * 1024 * 1024)
#define SBRK_PAGE_MASK ((size_t)0xFFF)

/* Total budget the dlmalloc arena may borrow from the heap-donor pool.  This
 * covers the newlib-internal working set that has no broker fallback; every
 * larger Mesa/NAK/guest allocation overflows into the reclaimable broker pool
 * instead of permanently pinning donor pages here.  The donor cap is 1490 MiB,
 * so this leaves the bulk of it for the guest and Unity spill path.
 * SBRK_EXTENSION_MAX_RANGES must be >= TOTAL_MAX / BLOCK_MIN (the registry
 * below denies extensions once its slots run out, which can only happen past
 * the budget cap anyway). */
#define SBRK_EXTENSION_TOTAL_MAX ((size_t)256 * 1024 * 1024)

#define SBRK_EXTENSION_MAX_RANGES 64u

typedef struct {
  uintptr_t base;
  size_t size;
} SbrkExtensionRange;

static SbrkExtensionRange g_sbrk_ranges[SBRK_EXTENSION_MAX_RANGES];
/* Published number of populated slots.  Writers (under the malloc lock)
 * populate the entry first and publish the incremented count with RELEASE;
 * readers acquire the count and then observe fully-written entries. */
static unsigned g_sbrk_range_slots;

unsigned long g_sbrk_extension_count;
unsigned long g_sbrk_extension_bytes;
unsigned long g_sbrk_extension_denied;

SbrkExtensionClass sbrk_extension_class(const void *pointer) {
  const uintptr_t address = (uintptr_t)pointer;
  if (!address) return SBRK_EXTENSION_CLASS_NONE;
  const unsigned slots =
    __atomic_load_n(&g_sbrk_range_slots, __ATOMIC_ACQUIRE);
  for (unsigned i = 0; i < slots && i < SBRK_EXTENSION_MAX_RANGES; ++i) {
    /* Entries are immutable once published; the acquire on the slot count
     * orders these relaxed loads. */
    const uintptr_t base =
      __atomic_load_n(&g_sbrk_ranges[i].base, __ATOMIC_RELAXED);
    const size_t size =
      __atomic_load_n(&g_sbrk_ranges[i].size, __ATOMIC_RELAXED);
    if (!base || !size || size > UINTPTR_MAX - base) continue;
    if (address >= base && address < base + size) {
      return address == base ? SBRK_EXTENSION_CLASS_BASE
                             : SBRK_EXTENSION_CLASS_INTERIOR;
    }
  }
  return SBRK_EXTENSION_CLASS_NONE;
}

void *__wrap__sbrk_r(struct _reent *reent, ptrdiff_t incr) {
  void *result = __real__sbrk_r(reent, incr);
  if (result != (void *)-1 || incr <= 0) return result;

  size_t block = (size_t)incr;
  if (block < SBRK_EXTENSION_BLOCK_MIN) block = SBRK_EXTENSION_BLOCK_MIN;
  block = (block + SBRK_PAGE_MASK) & ~SBRK_PAGE_MASK;

  /* Enforce the cumulative cap so dlmalloc cannot ratchet the whole donor into
   * a non-reclaimable arena.  Past the cap, fail so the broker fallback (which
   * recycles pool pages on free) serves the request instead. */
  if (g_sbrk_extension_bytes >= SBRK_EXTENSION_TOTAL_MAX ||
      block > SBRK_EXTENSION_TOTAL_MAX - g_sbrk_extension_bytes ||
      g_sbrk_range_slots >= SBRK_EXTENSION_MAX_RANGES) {
    g_sbrk_extension_denied++;
    reent->_errno = ENOMEM;
    return (void *)-1;
  }

  void *extension = nx_sparse_pool_sbrk_alloc_aligned(block,
                                                      SBRK_PAGE_MASK + 1);
  if (!extension) {
    g_sbrk_extension_denied++;
    reent->_errno = ENOMEM;
    return (void *)-1;
  }

  const unsigned slot = g_sbrk_range_slots;
  g_sbrk_ranges[slot].base = (uintptr_t)extension;
  g_sbrk_ranges[slot].size = block;
  __atomic_store_n(&g_sbrk_range_slots, slot + 1, __ATOMIC_RELEASE);

  g_sbrk_extension_count++;
  g_sbrk_extension_bytes += (unsigned long)block;
  return extension;
}

void sbrk_extension_report(FILE *out) {
  if (!out || (!g_sbrk_extension_count && !g_sbrk_extension_denied)) return;
  fprintf(out, "sbrk_extensions=%lu bytes=%luMiB denied=%lu ranges=%u\n",
          g_sbrk_extension_count,
          g_sbrk_extension_bytes / (1024ul * 1024ul),
          g_sbrk_extension_denied, g_sbrk_range_slots);
}
