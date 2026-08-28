/* Android and Unity guest symbol bindings. */

#define _GNU_SOURCE

#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <stdarg.h>
#include <string.h>
#include <strings.h>
#include <malloc.h>
#include <fcntl.h>
#include <unistd.h>
#include <ctype.h>
#include <math.h>
#include <pthread.h>
#include <time.h>
#include <wchar.h>
#include <wctype.h>
#include <errno.h>
#include <limits.h>
#include <locale.h>
#include <setjmp.h>
#include <fenv.h>
#include <fnmatch.h>
#include <zlib.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <dirent.h>
#include <getopt.h>
#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <switch.h>
#include "config.h"
#include "error.h"
#include "so_util.h"
#include "util.h"
#include "libc_shim.h"
#include "asset_pack.h"
#include "opensles.h"
#include "imports.h"
#include "unity_imports.h"
#include "vulkan_bridge.h"
#include "vulkan_egl_stubs.h"
#include "genshin_compat.h"
#include "android_asset.h"
#include "android_native_unity.h"
#include "jni_fake.h"
#include "android_log_sink.h"
#include "memory_broker.h"

extern int *__errno(void);

static int guest_printf_noop(const char *fmt, ...) { (void)fmt; return 0; }
static int guest_puts_noop(const char *text) { (void)text; return 0; }

int __android_log_print(int prio, const char *tag, const char *fmt, ...) {
  va_list args;
  va_start(args, fmt);
  const int result = android_log_sink_vprint(0, prio, tag, fmt, args);
  va_end(args);
  return result;
}
int __android_log_write(int prio, const char *tag, const char *text) {
  return android_log_sink_write(0, prio, tag, text);
}
int __android_log_vprint(int prio, const char *tag, const char *fmt, va_list va) {
  return android_log_sink_vprint(0, prio, tag, fmt, va);
}

/* Unity 2017's DynamicHeapAllocator ultimately imports malloc or memalign.
 * Its OOM reporter returns NULL to callers which are not uniformly null-safe,
 * so retain the real allocator contract and fall back to the sparse pool. */
static void *nx_guest_malloc(size_t size) {
  const int caller_errno = errno;
  void *result = nx_primary_malloc(size);
  if (result) return result;

  const int allocator_errno = errno;
  result = nx_sparse_pool_spill_alloc(size);
  if (result) {
    errno = caller_errno;
    return result;
  }

  errno = allocator_errno;
  return NULL;
}

static void *nx_guest_calloc(size_t count, size_t size) {
  const int caller_errno = errno;
  void *result = nx_primary_calloc(count, size);
  if (result || !count || !size) return result;

  const int allocator_errno = errno;
  if (count > SIZE_MAX / size) {
    errno = allocator_errno;
    return NULL;
  }
  const size_t total = count * size;
  result = nx_sparse_pool_spill_alloc(total);
  if (result) {
    memset(result, 0, total);
    errno = caller_errno;
    return result;
  }

  errno = allocator_errno;
  return NULL;
}

static void *nx_guest_memalign(size_t alignment, size_t size) {
  const int caller_errno = errno;
  void *result = nx_primary_memalign(alignment, size);
  if (result) return result;

  const int allocator_errno = errno;
  result = nx_sparse_pool_spill_alloc_aligned(size, alignment);
  if (result) {
    errno = caller_errno;
    return result;
  }

  errno = allocator_errno;
  return NULL;
}

static int nx_guest_posix_memalign(void **out, size_t alignment, size_t size) {
  if (!out || alignment < sizeof(void *) ||
      (alignment & (alignment - 1u)) != 0)
    return EINVAL;
  const int caller_errno = errno;
  void *result = nx_guest_memalign(alignment, size);
  if (!result) {
    const int allocator_errno = errno;
    errno = caller_errno;
    return allocator_errno == EINVAL ? EINVAL : ENOMEM;
  }
  *out = result;
  errno = caller_errno;
  return 0;
}

static void nx_guest_free(void *pointer) {
  if (!pointer) return;
  if (nx_sparse_pool_spill_release(pointer)) return;
  if (nx_sparse_pool_contains_address(pointer)) return;
  nx_primary_free(pointer);
}

static void *nx_guest_realloc(void *pointer, size_t size) {
  if (!pointer) return nx_guest_malloc(size);
  size_t usable = 0;
  if (!nx_sparse_pool_spill_query(pointer, NULL, &usable)) {
    if (nx_sparse_pool_contains_address(pointer)) {
      errno = ENOMEM;
      return NULL;
    }
    if (!size) return nx_primary_realloc(pointer, 0);

    /* newlib can reject an otherwise tiny growth when its fixed arena is
     * fragmented.  realloc failure leaves the original allocation live, so
     * migrate it through the same guest recovery path used by malloc instead
     * of returning NULL to Unity containers which assume allocator progress. */
    const int caller_errno = errno;
    const size_t old_usable = nx_primary_malloc_usable_size(pointer);
    void *replacement = nx_primary_realloc(pointer, size);
    if (replacement) return replacement;
    const int allocator_errno = errno;
    replacement = nx_guest_malloc(size);
    if (!replacement) {
      errno = allocator_errno;
      return NULL;
    }
    memcpy(replacement, pointer, old_usable < size ? old_usable : size);
    nx_primary_free(pointer);
    errno = caller_errno;
    return replacement;
  }
  if (!size) {
    (void)nx_sparse_pool_spill_release(pointer);
    return NULL;
  }
  /* Dynamic spill allocations are page-granular.  Keep sub-page vector
   * growth in place rather than asking the physical broker for a replacement;
   * this is both valid realloc behaviour and avoids a needless mapping at the
   * most memory-intensive data-loading boundary.  Copy the complete usable
   * span on a later cross-page growth so every prior in-place byte survives. */
  if (size <= usable) return pointer;
  void *replacement = nx_guest_malloc(size);
  if (!replacement) return NULL;
  memcpy(replacement, pointer, usable);
  (void)nx_sparse_pool_spill_release(pointer);
  return replacement;
}

static size_t nx_guest_malloc_usable_size(void *pointer) {
  size_t usable = 0;
  if (nx_sparse_pool_spill_query(pointer, NULL, &usable)) return usable;
  if (nx_sparse_pool_contains_address(pointer)) return 0;
  return nx_primary_malloc_usable_size(pointer);
}
void __stack_chk_fail_fake(void) { abort(); }
static void __cxa_pure_virtual_fake(void) { abort(); }
static int nx_daylight_data;
static long nx_timezone_data;
static char nx_timezone_name[] = "UTC";
static char *nx_tzname_data[2] = { nx_timezone_name, nx_timezone_name };

int  __cxa_atexit_fake(void (*fn)(void *), void *arg, void *dso) { (void)fn; (void)arg; (void)dso; return 0; }
void __cxa_finalize_fake(void *dso) { (void)dso; }

/* Bionic pthread objects contain pointers to libnx-backed allocations. */

/* Bionic only promises its own object alignment, not pointer alignment.  All
 * embedded host-pointer loads/stores therefore use memcpy and a single lock.
 * This also makes lazy materialization of static initializers atomic: two
 * guest threads can no longer install different native locks/conditions. */
static Mutex g_bionic_pthread_storage_lock;
static void guest_gc_critical_enter(void);
static void guest_gc_critical_leave(void);

/* The registry lock is wrapper state which Android's asynchronous GC signal
 * can never interrupt safely.  Publish before each nonblocking acquisition
 * attempt and retain the publication only for the actual owner.  A blocking
 * mutexLock() advertised every queued waiter as critical; Unity counts a
 * successful pthread_kill immediately, so one such waiter could leave its GC
 * suspend/resume acknowledgement pending for tens of seconds.  A waiter is
 * safe to stop while it is not enqueued on the Horizon mutex, and the second
 * post-pause check in the GC bridge closes the publication race. */
static void bionic_pthread_storage_lock(void) {
  for (;;) {
    guest_gc_critical_enter();
    if (mutexTryLock(&g_bionic_pthread_storage_lock)) return;
    guest_gc_critical_leave();
    svcSleepThread(0);
  }
}

static void bionic_pthread_storage_unlock(void) {
  mutexUnlock(&g_bionic_pthread_storage_lock);
  guest_gc_critical_leave();
}

void nx_pthread_storage_get_diagnostics(NxPthreadStorageDiagnostics *out) {
  if (!out) return;
  out->lock_address = (uintptr_t)&g_bionic_pthread_storage_lock;
  out->lock_word = __atomic_load_n(
    (const uint32_t *)&g_bionic_pthread_storage_lock, __ATOMIC_RELAXED);
}

static int pthread_error_n2b(int result) {
  switch (result) {
    case EDEADLK: return 35;
    case ETIMEDOUT: return 110;
    case ENOTSUP: return 95;
    case EOVERFLOW: return 75;
    case EOWNERDEAD: return 130;
    case ENOTRECOVERABLE: return 131;
    default: return result;
  }
}

static uintptr_t bionic_pthread_storage_load(const void *storage) {
  uintptr_t value = 0;
  if (storage) memcpy(&value, storage, sizeof(value));
  return value;
}

static void bionic_pthread_storage_store(void *storage, uintptr_t value) {
  memcpy(storage, &value, sizeof(value));
}

typedef enum {
  NX_PTHREAD_BIND_MUTEX = 1,
  NX_PTHREAD_BIND_COND = 2,
} NxPthreadBindingKind;

typedef struct NxPthreadBinding {
  void *storage;
  void *host;
  NxPthreadBindingKind kind;
  struct NxPthreadBinding *next;
} NxPthreadBinding;

/* Guest pthread storage is opaque and may contain any byte pattern before an
 * explicit init call.  A side registry keyed by the guest object's address is
 * therefore the only authority for whether a high word is one of our host
 * pointers.  This prevents stale malloc contents from ever being dereferenced
 * as a pthread object.  All helpers below require the storage lock. */
#define NX_PTHREAD_BINDING_BUCKETS 256u
_Static_assert((NX_PTHREAD_BINDING_BUCKETS &
                (NX_PTHREAD_BINDING_BUCKETS - 1u)) == 0,
               "pthread registry bucket count must be a power of two");
static NxPthreadBinding
  *g_bionic_pthread_bindings[NX_PTHREAD_BINDING_BUCKETS];

static size_t pthread_binding_bucket(const void *storage,
                                     NxPthreadBindingKind kind) {
  uintptr_t key = (uintptr_t)storage >> 3;
  key ^= key >> 17;
  key ^= (uintptr_t)kind * UINT64_C(0x9e3779b97f4a7c15);
  return (size_t)key & (NX_PTHREAD_BINDING_BUCKETS - 1u);
}

static NxPthreadBinding *pthread_binding_find_locked(
    const void *storage, NxPthreadBindingKind kind) {
  const size_t bucket = pthread_binding_bucket(storage, kind);
  for (NxPthreadBinding *binding = g_bionic_pthread_bindings[bucket];
       binding; binding = binding->next) {
    if (binding->storage == storage && binding->kind == kind) return binding;
  }
  return NULL;
}

static int pthread_binding_add_locked(void *storage, void *host,
                                      NxPthreadBindingKind kind) {
  if (pthread_binding_find_locked(storage, kind)) return EBUSY;
  NxPthreadBinding *binding = calloc(1, sizeof(*binding));
  if (!binding) return ENOMEM;
  binding->storage = storage;
  binding->host = host;
  binding->kind = kind;
  const size_t bucket = pthread_binding_bucket(storage, kind);
  binding->next = g_bionic_pthread_bindings[bucket];
  g_bionic_pthread_bindings[bucket] = binding;
  return 0;
}

static void pthread_binding_unlink_locked(NxPthreadBinding *binding) {
  const size_t bucket = pthread_binding_bucket(binding->storage, binding->kind);
  NxPthreadBinding **cursor = &g_bionic_pthread_bindings[bucket];
  while (*cursor && *cursor != binding) cursor = &(*cursor)->next;
  if (*cursor) *cursor = binding->next;
}

static int bionic_mutex_static_type(uintptr_t raw) {
  /* arm64 Bionic stores the mutex type in bits 14-15 of an otherwise-zero
   * untouched static initializer.  No other low value is a static mutex. */
  if (raw == 0) return 0;
  if (raw == UINT32_C(0x4000)) return 1;
  if (raw == UINT32_C(0x8000)) return 2;
  return -1;
}

static int create_host_mutex(int bionic_type, pthread_mutex_t **out) {
  pthread_mutex_t *m = calloc(1, sizeof(*m));
  if (!m) return ENOMEM;
  pthread_mutexattr_t attr;
  int result = pthread_mutexattr_init(&attr);
  const int attr_initialized = result == 0;
  if (result == 0 && bionic_type == 1)
    result = pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
  if (result == 0 && bionic_type == 2)
    result = pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_ERRORCHECK);
  if (result == 0) result = pthread_mutex_init(m, &attr);
  if (result == 0) *out = m;
  if (attr_initialized) pthread_mutexattr_destroy(&attr);
  if (result != 0) free(m);
  return pthread_error_n2b(result);
}

int pthread_mutex_init_fake(pthread_mutex_t **uid, const int *attr) {
  if (!uid) return EINVAL;
  const int type = attr ? *attr : 0;
  if (type < 0 || type > 2) return EINVAL;
  pthread_mutex_t *m = NULL;
  int result = create_host_mutex(type, &m);
  if (result == 0) {
    /* pthread_mutex_init() initializes object storage; it must not inspect the
     * bytes supplied by malloc/new first.  Treating an arbitrary high first
     * word as one of our host pointers made libc++ recursive_mutex creation
     * spuriously return EBUSY during Unity constructors.  Reinitializing a
     * genuinely live POSIX mutex is undefined; the side registry can reject
     * that case without mistaking arbitrary bytes for a live object. */
    bionic_pthread_storage_lock();
    result = pthread_binding_add_locked(uid, m, NX_PTHREAD_BIND_MUTEX);
    if (result == 0) bionic_pthread_storage_store(uid, (uintptr_t)m);
    bionic_pthread_storage_unlock();
    if (result != 0) {
      (void)pthread_mutex_destroy(m);
      free(m);
    }
  }
  return pthread_error_n2b(result);
}
int pthread_mutex_destroy_fake(pthread_mutex_t **uid) {
  if (!uid) return EINVAL;
  bionic_pthread_storage_lock();
  const uintptr_t raw = bionic_pthread_storage_load(uid);
  NxPthreadBinding *binding =
    pthread_binding_find_locked(uid, NX_PTHREAD_BIND_MUTEX);
  if (!binding && bionic_mutex_static_type(raw) >= 0) {
    bionic_pthread_storage_store(uid, 0);
    bionic_pthread_storage_unlock();
    return 0;
  }
  if (!binding || raw != (uintptr_t)binding->host) {
    bionic_pthread_storage_unlock();
    return EINVAL;
  }
  pthread_mutex_t *host = binding->host;
  const int result = pthread_mutex_destroy(host);
  if (result == 0) {
    pthread_binding_unlink_locked(binding);
    bionic_pthread_storage_store(uid, 0);
  }
  bionic_pthread_storage_unlock();
  if (result == 0) {
    free(binding);
    free(host);
  }
  return pthread_error_n2b(result);
}
static int ensure_mutex(pthread_mutex_t **uid) {
  if (!uid) return EINVAL;
  bionic_pthread_storage_lock();
  const uintptr_t raw = bionic_pthread_storage_load(uid);
  NxPthreadBinding *binding =
    pthread_binding_find_locked(uid, NX_PTHREAD_BIND_MUTEX);
  int result = binding && raw == (uintptr_t)binding->host ? 0 : EINVAL;
  const int static_type = bionic_mutex_static_type(raw);
  if (!binding && static_type >= 0) {
    /* Bionic's static recursive/error-check initializers encode the type in
     * the otherwise-null first word. */
    pthread_mutex_t *host = NULL;
    result = create_host_mutex(static_type, &host);
    if (result == 0) {
      result = pthread_binding_add_locked(uid, host, NX_PTHREAD_BIND_MUTEX);
      if (result == 0) {
        bionic_pthread_storage_store(uid, (uintptr_t)host);
      } else {
        (void)pthread_mutex_destroy(host);
        free(host);
      }
    }
  }
  bionic_pthread_storage_unlock();
  return pthread_error_n2b(result);
}
static pthread_mutex_t *guest_mutex(pthread_mutex_t **uid) {
  /* Every caller first succeeds through ensure_mutex(), which validates this
   * word against the registry while locked.  Concurrent destroy/reinit of a
   * mutex in use is outside the POSIX contract, so no second global lookup is
   * needed on the hot lock path. */
  const uintptr_t raw = bionic_pthread_storage_load(uid);
  return raw >= 0x10000u ? (pthread_mutex_t *)raw : NULL;
}
int pthread_mutex_lock_fake(pthread_mutex_t **uid) {
  int result = ensure_mutex(uid);
  pthread_mutex_t *host = result == 0 ? guest_mutex(uid) : NULL;
  return result != 0 ? result : host ? pthread_error_n2b(pthread_mutex_lock(host)) : EINVAL;
}
int pthread_mutex_trylock_fake(pthread_mutex_t **uid) {
  int result = ensure_mutex(uid);
  pthread_mutex_t *host = result == 0 ? guest_mutex(uid) : NULL;
  return result != 0 ? result : host ? pthread_error_n2b(pthread_mutex_trylock(host)) : EINVAL;
}
int pthread_mutex_unlock_fake(pthread_mutex_t **uid) {
  int result = ensure_mutex(uid);
  pthread_mutex_t *host = result == 0 ? guest_mutex(uid) : NULL;
  return result != 0 ? result : host ? pthread_error_n2b(pthread_mutex_unlock(host)) : EINVAL;
}
/* Linux UAPI/Android clock IDs are 0 for realtime and 1 for monotonic.  They
 * must not be confused with newlib's CLOCK_REALTIME=1/CLOCK_MONOTONIC=4. */
enum {
  BIONIC_COND_CLOCK_REALTIME = 0,
  BIONIC_COND_CLOCK_MONOTONIC = 1,
};
#define BIONIC_COND_CLOCK_MASK ((BionicPthreadCondAttr)0x0002)
#define BIONIC_CONDATTR_DESTROYED ((BionicPthreadCondAttr)0xdeada11d)
_Static_assert(sizeof(BionicPthreadCondAttr) == 8,
               "Android arm64 pthread_condattr_t must be 64-bit");

typedef struct {
  pthread_cond_t host;
  clockid_t clock_id;
} NxCond;

int pthread_condattr_init_fake(BionicPthreadCondAttr *attr) {
  if (!attr) return EINVAL;
  /* PTHREAD_PROCESS_PRIVATE and CLOCK_REALTIME are both zero in Bionic. */
  *attr = 0;
  return 0;
}

int pthread_condattr_destroy_fake(BionicPthreadCondAttr *attr) {
  if (!attr) return EINVAL;
  *attr = BIONIC_CONDATTR_DESTROYED;
  return 0;
}

int pthread_condattr_setclock_fake(BionicPthreadCondAttr *attr,
                                   int bionic_clock_id) {
  if (!attr || (bionic_clock_id != BIONIC_COND_CLOCK_REALTIME &&
                bionic_clock_id != BIONIC_COND_CLOCK_MONOTONIC))
    return EINVAL;
  *attr = (*attr & ~BIONIC_COND_CLOCK_MASK) |
          ((BionicPthreadCondAttr)bionic_clock_id << 1);
  return 0;
}

static clockid_t cond_host_clock(const BionicPthreadCondAttr *attr) {
  return attr && (*attr & BIONIC_COND_CLOCK_MASK)
           ? CLOCK_MONOTONIC : CLOCK_REALTIME;
}

static int bionic_cond_static_initializer(uintptr_t raw) {
  /* Bionic publishes exactly these two inline initializers on arm64. */
  return raw == 0 || raw == (uintptr_t)BIONIC_COND_CLOCK_MASK;
}

static int create_host_cond(const BionicPthreadCondAttr *attr, NxCond **out) {
  NxCond *c = calloc(1, sizeof(*c));
  if (!c) return ENOMEM;

  c->clock_id = cond_host_clock(attr);
  pthread_condattr_t host_attr;
  int result = pthread_condattr_init(&host_attr);
  const int host_attr_initialized = (result == 0);
  if (result == 0) result = pthread_condattr_setclock(&host_attr, c->clock_id);
  if (result == 0) result = pthread_cond_init(&c->host, &host_attr);
  if (host_attr_initialized) pthread_condattr_destroy(&host_attr);
  if (result != 0) { free(c); return pthread_error_n2b(result); }
  *out = c;
  return 0;
}
int pthread_cond_init_fake(pthread_cond_t **cnd,
                           const BionicPthreadCondAttr *attr) {
  if (!cnd) return EINVAL;
  NxCond *host = NULL;
  int result = create_host_cond(attr, &host);
  if (result == 0) {
    /* As with pthread_mutex_init(), explicit initialization overwrites
     * indeterminate object storage.  Only lazy operations interpret the
     * Bionic static-initializer encodings already present in that storage. */
    bionic_pthread_storage_lock();
    result = pthread_binding_add_locked(cnd, host, NX_PTHREAD_BIND_COND);
    if (result == 0) bionic_pthread_storage_store(cnd, (uintptr_t)host);
    bionic_pthread_storage_unlock();
    if (result != 0) {
      (void)pthread_cond_destroy(&host->host);
      free(host);
    }
  }
  return pthread_error_n2b(result);
}
static int ensure_cond(pthread_cond_t **cnd) {
  if (!cnd) return EINVAL;
  bionic_pthread_storage_lock();
  const uintptr_t raw = bionic_pthread_storage_load(cnd);
  NxPthreadBinding *binding =
    pthread_binding_find_locked(cnd, NX_PTHREAD_BIND_COND);
  int result = binding && raw == (uintptr_t)binding->host ? 0 : EINVAL;
  if (!binding && bionic_cond_static_initializer(raw)) {
    /* PTHREAD_COND_INITIALIZER_MONOTONIC_NP stores bit 1 in the first word.
     * Preserve that clock when lazily replacing the inline initializer. */
    const BionicPthreadCondAttr static_attr = (BionicPthreadCondAttr)raw;
    NxCond *host = NULL;
    result = create_host_cond(&static_attr, &host);
    if (result == 0) {
      result = pthread_binding_add_locked(cnd, host, NX_PTHREAD_BIND_COND);
      if (result == 0) {
        bionic_pthread_storage_store(cnd, (uintptr_t)host);
      } else {
        (void)pthread_cond_destroy(&host->host);
        free(host);
      }
    }
  }
  bionic_pthread_storage_unlock();
  return pthread_error_n2b(result);
}
static NxCond *nx_cond(pthread_cond_t **cnd) {
  /* ensure_cond() validates registry ownership immediately before this load. */
  const uintptr_t raw = bionic_pthread_storage_load(cnd);
  return raw >= 0x10000u ? (NxCond *)raw : NULL;
}
int pthread_cond_broadcast_fake(pthread_cond_t **cnd) {
  int result = ensure_cond(cnd);
  NxCond *host = result == 0 ? nx_cond(cnd) : NULL;
  return result != 0 ? result : host ? pthread_error_n2b(pthread_cond_broadcast(&host->host)) : EINVAL;
}
int pthread_cond_signal_fake(pthread_cond_t **cnd) {
  int result = ensure_cond(cnd);
  NxCond *host = result == 0 ? nx_cond(cnd) : NULL;
  return result != 0 ? result : host ? pthread_error_n2b(pthread_cond_signal(&host->host)) : EINVAL;
}
int pthread_cond_destroy_fake(pthread_cond_t **cnd) {
  if (!cnd) return EINVAL;
  bionic_pthread_storage_lock();
  const uintptr_t raw = bionic_pthread_storage_load(cnd);
  NxPthreadBinding *binding =
    pthread_binding_find_locked(cnd, NX_PTHREAD_BIND_COND);
  if (!binding && bionic_cond_static_initializer(raw)) {
    bionic_pthread_storage_store(cnd, 0);
    bionic_pthread_storage_unlock();
    return 0;
  }
  if (!binding || raw != (uintptr_t)binding->host) {
    bionic_pthread_storage_unlock();
    return EINVAL;
  }
  NxCond *c = binding->host;
  int result = pthread_cond_destroy(&c->host);
  if (result == 0) {
    pthread_binding_unlink_locked(binding);
    bionic_pthread_storage_store(cnd, 0);
  }
  bionic_pthread_storage_unlock();
  if (result == 0) {
    free(binding);
    free(c);
  }
  return pthread_error_n2b(result);
}
int pthread_cond_wait_fake(pthread_cond_t **cnd, pthread_mutex_t **mtx) {
  int result = ensure_cond(cnd);
  if (result != 0) return result;
  result = ensure_mutex(mtx);
  if (result != 0) return result;
  NxCond *c = nx_cond(cnd);
  pthread_mutex_t *host_mutex = guest_mutex(mtx);
  if (!c || !host_mutex) return EINVAL;
  /* The GC bridge pauses and captures the native Horizon thread directly,
   * including while it is inside a host pthread boundary, so no timed-wait
   * emulation is needed here.  Preserve the real POSIX blocking contract so
   * Unity's downloader workers sleep until a signal instead of competing
   * continuously with TLS, FS, and render work. */
  return pthread_error_n2b(pthread_cond_wait(&c->host, host_mutex));
}
int pthread_cond_timedwait_fake(pthread_cond_t **cnd, pthread_mutex_t **mtx, const struct timespec *t) {
  int result = ensure_cond(cnd);
  if (result != 0) return result;
  result = ensure_mutex(mtx);
  if (result != 0) return result;
  if (t && (t->tv_nsec < 0 || t->tv_nsec >= 1000000000L)) return EINVAL;
  NxCond *c = nx_cond(cnd);
  pthread_mutex_t *host_mutex = guest_mutex(mtx);
  if (!c || !host_mutex) return EINVAL;
  /* Bionic and newlib use the same LP64 timespec layout.  NxCond was created
   * with the translated Bionic clock, so the guest absolute deadline can be
   * passed through without polling or changing its timeout semantics.  Keep a
   * defensive null fallback for callers that previously relied on the shim's
   * tolerant behavior. */
  const int wait_result = t
    ? pthread_cond_timedwait(&c->host, host_mutex, t)
    : pthread_cond_wait(&c->host, host_mutex);
  return pthread_error_n2b(wait_result);
}

int pthread_once_fake(volatile int *once, void (*init)(void)) {
  if (!once || !init) return -1;
  enum { ONCE_UNINITIALIZED = 0, ONCE_RUNNING = 1, ONCE_COMPLETE = 2 };
  for (;;) {
    const int state = __atomic_load_n(once, __ATOMIC_ACQUIRE);
    if (state == ONCE_COMPLETE) return 0;
    if (state == ONCE_UNINITIALIZED) {
      int expected = ONCE_UNINITIALIZED;
      if (__atomic_compare_exchange_n(once, &expected, ONCE_RUNNING, 0,
                                      __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
        (*init)();
        __atomic_store_n(once, ONCE_COMPLETE, __ATOMIC_RELEASE);
        return 0;
      }
    }
    /* The initializer may call into code which needs another host thread, so
     * yield instead of monopolizing the core with a tight spin. */
    svcSleepThread(0);
  }
}

int pthread_mutexattr_init_fake(int *a) { if (a) *a = 0; return 0; }
int pthread_mutexattr_settype_fake(int *a, int t) { if (a) *a = t; return 0; }

int pthread_storage_self_test(void) {
  /* Heap-allocated Bionic pthread objects contain indeterminate bytes until
   * their explicit init call.  Use values that resemble host pointers so the
   * test detects any regression to the old "non-zero means live" heuristic. */
  uintptr_t mutex_storage[2] = {
    UINTPTR_MAX - UINT64_C(0x1357), 0
  };
  int recursive = 1;
  if (pthread_mutex_init_fake((pthread_mutex_t **)&mutex_storage[0],
                              &recursive) != 0)
    return 0;
  if (pthread_mutex_lock_fake((pthread_mutex_t **)&mutex_storage[0]) != 0) {
    (void)pthread_mutex_destroy_fake((pthread_mutex_t **)&mutex_storage[0]);
    return 0;
  }
  if (pthread_mutex_lock_fake((pthread_mutex_t **)&mutex_storage[0]) != 0) {
    (void)pthread_mutex_unlock_fake((pthread_mutex_t **)&mutex_storage[0]);
    (void)pthread_mutex_destroy_fake((pthread_mutex_t **)&mutex_storage[0]);
    return 0;
  }
  if (pthread_mutex_unlock_fake((pthread_mutex_t **)&mutex_storage[0]) != 0 ||
      pthread_mutex_unlock_fake((pthread_mutex_t **)&mutex_storage[0]) != 0 ||
      pthread_mutex_destroy_fake((pthread_mutex_t **)&mutex_storage[0]) != 0 ||
      mutex_storage[0] != 0 || mutex_storage[1] != 0)
    return 0;

  uintptr_t static_mutex_storage[2] = {UINT32_C(0x4000), 0};
  if (pthread_mutex_lock_fake((pthread_mutex_t **)&static_mutex_storage[0]) != 0 ||
      pthread_mutex_lock_fake((pthread_mutex_t **)&static_mutex_storage[0]) != 0 ||
      pthread_mutex_unlock_fake((pthread_mutex_t **)&static_mutex_storage[0]) != 0 ||
      pthread_mutex_unlock_fake((pthread_mutex_t **)&static_mutex_storage[0]) != 0 ||
      pthread_mutex_destroy_fake((pthread_mutex_t **)&static_mutex_storage[0]) != 0 ||
      static_mutex_storage[0] != 0 || static_mutex_storage[1] != 0)
    return 0;

  /* A low but non-ABI value is neither a host pointer nor a valid untouched
   * static initializer; it must never be silently materialized as NORMAL. */
  uintptr_t invalid_mutex_storage[2] = {UINT32_C(1), 0};
  if (pthread_mutex_lock_fake((pthread_mutex_t **)&invalid_mutex_storage[0]) != EINVAL ||
      pthread_mutex_destroy_fake((pthread_mutex_t **)&invalid_mutex_storage[0]) != EINVAL ||
      invalid_mutex_storage[0] != UINT32_C(1) || invalid_mutex_storage[1] != 0)
    return 0;

  uintptr_t cond_storage = UINTPTR_MAX - UINT64_C(0x2468);
  BionicPthreadCondAttr cond_attr;
  if (pthread_condattr_init_fake(&cond_attr) != 0 ||
      pthread_condattr_setclock_fake(&cond_attr,
                                     BIONIC_COND_CLOCK_MONOTONIC) != 0 ||
      pthread_cond_init_fake((pthread_cond_t **)&cond_storage, &cond_attr) != 0)
    return 0;
  if (pthread_cond_destroy_fake((pthread_cond_t **)&cond_storage) != 0 ||
      pthread_condattr_destroy_fake(&cond_attr) != 0 || cond_storage != 0)
    return 0;

  /* Bionic also exposes a monotonic static condition initializer (raw bit 1).
   * Lazy materialization must retain its clock instead of defaulting to
   * realtime. */
  uintptr_t static_cond_storage = (uintptr_t)BIONIC_COND_CLOCK_MASK;
  if (ensure_cond((pthread_cond_t **)&static_cond_storage) != 0) return 0;
  NxCond *static_cond = nx_cond((pthread_cond_t **)&static_cond_storage);
  if (!static_cond || static_cond->clock_id != CLOCK_MONOTONIC) {
    (void)pthread_cond_destroy_fake((pthread_cond_t **)&static_cond_storage);
    return 0;
  }
  if (pthread_cond_destroy_fake((pthread_cond_t **)&static_cond_storage) != 0 ||
      static_cond_storage != 0)
    return 0;

  void *rwlock_storage = (void *)(UINTPTR_MAX - UINT64_C(0x3579));
  if (pthread_rwlock_init_fake(&rwlock_storage, NULL) != 0 ||
      pthread_rwlock_rdlock_fake(&rwlock_storage) != 0 ||
      pthread_rwlock_unlock_fake(&rwlock_storage) != 0 ||
      pthread_rwlock_destroy_fake(&rwlock_storage) != 0 || rwlock_storage)
    return 0;

  return 1;
}

#define ATTR_MAGIC 0x41545452 /* 'ATTR' */
typedef struct { uint32_t magic; uint32_t detach; size_t stacksize; } OurAttr;

int pthread_attr_init_fake(void *a) { if (a) { OurAttr *o = a; o->magic = ATTR_MAGIC; o->detach = 0; o->stacksize = 0; } return 0; }
int pthread_attr_destroy_fake(void *a) { (void)a; return 0; }
int pthread_attr_setdetachstate_fake(void *a, int s) { if (a) { OurAttr *o = a; if (o->magic == ATTR_MAGIC) o->detach = (uint32_t)s; } return 0; }
int pthread_attr_setstacksize_fake(void *a, size_t s) { if (a) { OurAttr *o = a; if (o->magic == ATTR_MAGIC) o->stacksize = s; } return 0; }

#define THREAD_START_MAGIC UINT64_C(0x5453544152544e58) /* "TSTARTNX" */

typedef enum {
  NX_GUEST_THREAD_CREATING = 0,
  NX_GUEST_THREAD_LIVE,
  NX_GUEST_THREAD_EXITING,
  NX_GUEST_THREAD_EXITED,
} NxGuestThreadState;

/* This object deliberately outlives ThreadStart.  libnx's pthread_t owns the
 * native Thread and its handle until pthread_join(), so collectors take a
 * strong reference before pausing a target and join/reaping waits for those
 * references to drain. */
struct NxGuestThreadRef {
  struct NxGuestThreadRef *next;
  pthread_t pthread;
  Thread *native_thread;
  Handle handle;
  uintptr_t guest_thread_pointer;
  uintptr_t guest_entry;
  uint64_t signal_mask;
  uint32_t gc_critical_depth;
  unsigned borrowers;
  unsigned child_ready;
  unsigned creator_published;
  unsigned creator_returned;
  unsigned detached;
  unsigned external;
  unsigned reap_claimed;
  unsigned reap_failed;
  size_t stack_size;
  void *stack_backing;
  size_t stack_backing_size;
  NxGuestThreadState state;
};

typedef struct {
  uint64_t magic;
  void *(*entry)(void *);
  void *arg;
  NxGuestThreadRef *thread_ref;
  uintptr_t host_thread_pointer;
  uintptr_t guest_thread_pointer;
  uint8_t tls[BIONIC_TLS_SIZE] __attribute__((aligned(16)));
} ThreadStart;

/* This is host C TLS (TPIDRRO_EL0-backed), so it remains available while the
 * guest owns TPIDR_EL0.  It is NULL on main/SDL/other non-wrapper threads. */
static _Thread_local ThreadStart *current_thread_start;
/* Host C TLS is TPIDRRO_EL0-backed on libnx and remains readable while Unity
 * owns TPIDR_EL0.  Keep the external-thread registry identity here as well as
 * in the pthread key: the key supplies the exit destructor, while this pointer
 * makes repeated guest pthread_self() calls lock-free and avoids entering
 * newlib pthread-specific lookup with the guest TLS pointer installed. */
static _Thread_local NxGuestThreadRef *current_external_thread_ref;
/* arm64 Bionic's sigset_t is one 64-bit word.  Host C TLS remains available
 * while TPIDR_EL0 points at the Android TLS block. */
static _Thread_local uint64_t guest_signal_mask;
static _Thread_local uint32_t guest_gc_critical_depth;

static NxGuestThreadRef *current_guest_thread_ref_fast(void) {
  if (current_thread_start && current_thread_start->thread_ref)
    return current_thread_start->thread_ref;
  return current_external_thread_ref;
}

static void guest_gc_critical_enter(void) {
  const uint32_t depth = ++guest_gc_critical_depth;
  if (depth != 1) return;
  NxGuestThreadRef *ref = current_guest_thread_ref_fast();
  if (ref)
    __atomic_store_n(&ref->gc_critical_depth, depth, __ATOMIC_RELEASE);
}

static void guest_gc_critical_leave(void) {
  if (!guest_gc_critical_depth) return;
  const uint32_t depth = --guest_gc_critical_depth;
  if (depth) return;
  NxGuestThreadRef *ref = current_guest_thread_ref_fast();
  if (ref)
    __atomic_store_n(&ref->gc_critical_depth, 0, __ATOMIC_RELEASE);
}

void nx_guest_gc_critical_enter(void) {
  guest_gc_critical_enter();
}

void nx_guest_gc_critical_leave(void) {
  guest_gc_critical_leave();
}

static Mutex guest_thread_lock;
static CondVar guest_thread_cond;
static NxGuestThreadRef *guest_threads;
static pthread_t detached_reaper_thread;
enum { REAPER_STOPPED = 0, REAPER_STARTING, REAPER_RUNNING };
static unsigned detached_reaper_state;
static pthread_key_t external_thread_key;
enum { EXTERNAL_KEY_STOPPED = 0, EXTERNAL_KEY_STARTING, EXTERNAL_KEY_READY };
static unsigned external_thread_key_state;

#define GUEST_THREAD_DEFAULT_STACK ((size_t)1u << 20)
#define GUEST_THREAD_FALLBACK_STACK ((size_t)512u << 10)
#define GUEST_THREAD_BACKING_OVERHEAD ((size_t)64u << 10)

enum {
  THREAD_FAILURE_REAPER = 1,
  THREAD_FAILURE_RECORDS,
  THREAD_FAILURE_ATTR_INIT,
  THREAD_FAILURE_ATTR_STACK,
  THREAD_FAILURE_HOST_CREATE,
};

static uint64_t guest_thread_create_calls;
static uint64_t guest_thread_create_successes;
static uint64_t guest_thread_create_failures;
static uint64_t guest_thread_fallback_successes;
static uint64_t guest_thread_detached_creates;
static uint64_t guest_thread_reaped;
static uint64_t guest_thread_reap_failures;
static uint64_t guest_thread_stack_bytes;
static uint64_t guest_thread_peak_stack_bytes;
static uint64_t guest_thread_last_requested_stack;
static uint64_t guest_thread_last_attempted_stack;
static int32_t guest_thread_last_host_error;
static int32_t guest_thread_last_failure_stage;

static void run_guest_key_destructors(void);

#define THREAD_DIAG_ADD(field, value) \
  __atomic_fetch_add(&(field), (uint64_t)(value), __ATOMIC_RELAXED)
#define THREAD_DIAG_LOAD(field) \
  __atomic_load_n(&(field), __ATOMIC_RELAXED)

static void update_guest_thread_stack_peak(uint64_t current) {
  uint64_t observed = THREAD_DIAG_LOAD(guest_thread_peak_stack_bytes);
  while (current > observed &&
         !__atomic_compare_exchange_n(&guest_thread_peak_stack_bytes,
                                      &observed, current, 1,
                                      __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {
  }
}

void nx_guest_thread_get_diagnostics(NxGuestThreadDiagnostics *out) {
  if (!out) return;
  memset(out, 0, sizeof(*out));
  out->create_calls = THREAD_DIAG_LOAD(guest_thread_create_calls);
  out->create_successes = THREAD_DIAG_LOAD(guest_thread_create_successes);
  out->create_failures = THREAD_DIAG_LOAD(guest_thread_create_failures);
  out->fallback_successes = THREAD_DIAG_LOAD(guest_thread_fallback_successes);
  out->detached_creates = THREAD_DIAG_LOAD(guest_thread_detached_creates);
  out->reaped_threads = THREAD_DIAG_LOAD(guest_thread_reaped);
  out->reap_failures = THREAD_DIAG_LOAD(guest_thread_reap_failures);
  out->stack_bytes = THREAD_DIAG_LOAD(guest_thread_stack_bytes);
  out->peak_stack_bytes = THREAD_DIAG_LOAD(guest_thread_peak_stack_bytes);
  out->last_requested_stack =
    THREAD_DIAG_LOAD(guest_thread_last_requested_stack);
  out->last_attempted_stack =
    THREAD_DIAG_LOAD(guest_thread_last_attempted_stack);
  out->last_host_error =
    __atomic_load_n(&guest_thread_last_host_error, __ATOMIC_RELAXED);
  out->last_failure_stage =
    __atomic_load_n(&guest_thread_last_failure_stage, __ATOMIC_RELAXED);

  mutexLock(&guest_thread_lock);
  for (NxGuestThreadRef *ref = guest_threads; ref; ref = ref->next) {
    ++out->registered_threads;
    if (ref->external) ++out->external_threads;
    if (ref->detached && !ref->reap_claimed) ++out->detached_pending;
    switch (ref->state) {
      case NX_GUEST_THREAD_CREATING:
      case NX_GUEST_THREAD_LIVE: ++out->live_threads; break;
      case NX_GUEST_THREAD_EXITING: ++out->exiting_threads; break;
      case NX_GUEST_THREAD_EXITED: ++out->exited_threads; break;
    }
  }
  mutexUnlock(&guest_thread_lock);

  struct mallinfo heap = mallinfo();
  out->heap_arena = heap.arena;
  out->heap_used = heap.uordblks;
  out->heap_free = heap.fordblks;
  out->heap_top_free = heap.keepcost;

  u64 value = 0;
  if (R_SUCCEEDED(svcGetInfo(&value, InfoType_FreeThreadCount,
                             CUR_PROCESS_HANDLE, 0))) {
    out->free_thread_count = value;
    out->free_thread_count_valid = 1;
  }
  u64 used = 0, total = 0;
  if (R_SUCCEEDED(svcGetInfo(&used, InfoType_UsedMemorySize,
                             CUR_PROCESS_HANDLE, 0)) &&
      R_SUCCEEDED(svcGetInfo(&total, InfoType_TotalMemorySize,
                             CUR_PROCESS_HANDLE, 0))) {
    out->process_memory_used = used;
    out->process_memory_total = total;
    out->process_memory_valid = 1;
  }
}

static void record_guest_thread_failure(int stage, int host_error,
                                        size_t requested, size_t attempted) {
  THREAD_DIAG_ADD(guest_thread_create_failures, 1);
  __atomic_store_n(&guest_thread_last_failure_stage, stage, __ATOMIC_RELAXED);
  __atomic_store_n(&guest_thread_last_host_error, host_error,
                   __ATOMIC_RELAXED);
  __atomic_store_n(&guest_thread_last_requested_stack, requested,
                   __ATOMIC_RELAXED);
  __atomic_store_n(&guest_thread_last_attempted_stack, attempted,
                   __ATOMIC_RELAXED);
}

static NxGuestThreadRef *find_guest_thread_locked(pthread_t thread) {
  for (NxGuestThreadRef *ref = guest_threads; ref; ref = ref->next)
    if (ref->pthread == thread) return ref;
  return NULL;
}

static void remove_guest_thread_locked(NxGuestThreadRef *target) {
  NxGuestThreadRef **link = &guest_threads;
  while (*link && *link != target) link = &(*link)->next;
  if (*link == target) *link = target->next;
}

NxGuestThreadRef *nx_guest_thread_acquire(pthread_t thread) {
  if (!thread) return NULL;
  mutexLock(&guest_thread_lock);
  NxGuestThreadRef *ref = find_guest_thread_locked(thread);
  if (!ref || ref->state != NX_GUEST_THREAD_LIVE || ref->reap_claimed) {
    mutexUnlock(&guest_thread_lock);
    return NULL;
  }
  ++ref->borrowers;
  mutexUnlock(&guest_thread_lock);
  return ref;
}

void nx_guest_thread_release(NxGuestThreadRef *ref) {
  if (!ref) return;
  mutexLock(&guest_thread_lock);
  if (ref->borrowers && --ref->borrowers == 0)
    condvarWakeAll(&guest_thread_cond);
  mutexUnlock(&guest_thread_lock);
}

int nx_guest_thread_snapshot(NxGuestThreadRef **refs, size_t capacity,
                             size_t *count_out) {
  if (!refs || !count_out) return EINVAL;
  *count_out = 0;

  mutexLock(&guest_thread_lock);
  size_t count = 0;
  for (NxGuestThreadRef *ref = guest_threads; ref; ref = ref->next) {
    if (ref->state == NX_GUEST_THREAD_LIVE && !ref->reap_claimed)
      ++count;
  }
  if (count > capacity) {
    mutexUnlock(&guest_thread_lock);
    return EAGAIN;
  }

  size_t index = 0;
  for (NxGuestThreadRef *ref = guest_threads; ref; ref = ref->next) {
    if (ref->state != NX_GUEST_THREAD_LIVE || ref->reap_claimed) continue;
    ++ref->borrowers;
    refs[index++] = ref;
  }
  mutexUnlock(&guest_thread_lock);
  *count_out = index;
  return 0;
}

pthread_t nx_guest_thread_pthread(const NxGuestThreadRef *ref) {
  return ref ? ref->pthread : (pthread_t)0;
}

Thread *nx_guest_thread_native(const NxGuestThreadRef *ref) {
  return ref ? ref->native_thread : NULL;
}

Handle nx_guest_thread_handle(const NxGuestThreadRef *ref) {
  return ref ? ref->handle : INVALID_HANDLE;
}

uintptr_t nx_guest_thread_pointer(const NxGuestThreadRef *ref) {
  return ref ? ref->guest_thread_pointer : 0;
}

uintptr_t nx_guest_thread_entry(const NxGuestThreadRef *ref) {
  return ref ? ref->guest_entry : 0;
}

uint64_t nx_guest_thread_signal_mask(const NxGuestThreadRef *ref) {
  return ref
    ? __atomic_load_n(&ref->signal_mask, __ATOMIC_ACQUIRE)
    : UINT64_MAX;
}

int nx_guest_thread_gc_critical(const NxGuestThreadRef *ref) {
  return ref &&
    __atomic_load_n(&ref->gc_critical_depth, __ATOMIC_ACQUIRE) != 0;
}

int nx_guest_thread_is_self(const NxGuestThreadRef *ref) {
  return ref && ref->pthread == pthread_self();
}

int nx_guest_thread_is_live(const NxGuestThreadRef *ref) {
  if (!ref) return 0;
  mutexLock(&guest_thread_lock);
  const int live = ref->state == NX_GUEST_THREAD_LIVE && !ref->reap_claimed;
  mutexUnlock(&guest_thread_lock);
  return live;
}

static int guest_context_frame_readable(uint64_t address, uint64_t stack,
                                        size_t bytes) {
  if (!address || (address & 15u) || address < stack ||
      address - stack > (UINT64_C(64) << 20) ||
      bytes > UINT64_MAX - address)
    return 0;
  MemoryInfo info;
  u32 page_info;
  if (R_FAILED(svcQueryMemory(&info, &page_info, address)) ||
      !(info.perm & Perm_R) || address < info.addr ||
      bytes > info.size || address - info.addr > info.size - bytes)
    return 0;
  return 1;
}

int nx_guest_thread_capture_context(pthread_t thread,
                                    NxGuestThreadContextSnapshot *out) {
  if (!out) return EINVAL;
  memset(out, 0, sizeof(*out));
  NxGuestThreadRef *ref = nx_guest_thread_acquire(thread);
  if (!ref) return ESRCH;
  if (nx_guest_thread_is_self(ref)) {
    nx_guest_thread_release(ref);
    return EDEADLK;
  }

  out->guest_thread_pointer = nx_guest_thread_pointer(ref);
  Thread *native = nx_guest_thread_native(ref);
  if (!native) {
    nx_guest_thread_release(ref);
    return ESRCH;
  }

  const Result pause_result = threadPause(native);
  out->pause_result = (uint32_t)pause_result;
  if (R_FAILED(pause_result)) {
    nx_guest_thread_release(ref);
    return EBUSY;
  }

  ThreadContext context;
  memset(&context, 0, sizeof(context));
  const Result dump_result = threadDumpContext(&context, native);
  out->dump_result = (uint32_t)dump_result;
  if (R_SUCCEEDED(dump_result)) {
    out->pc = context.pc.x;
    out->lr = context.lr;
    out->sp = context.sp;
    out->fp = context.fp;

    uint64_t frame = context.fp;
    while (out->frame_count < NX_GUEST_CONTEXT_MAX_FRAMES &&
           guest_context_frame_readable(frame, context.sp,
                                        2 * sizeof(uint64_t))) {
      uint64_t pair[2];
      memcpy(pair, (const void *)(uintptr_t)frame, sizeof(pair));
      out->frames[out->frame_count++] = pair[1];
      if (pair[0] <= frame || (pair[0] & 15u)) break;
      frame = pair[0];
    }
  }

  const Result resume_result = threadResume(native);
  out->resume_result = (uint32_t)resume_result;
  nx_guest_thread_release(ref);
  if (R_FAILED(dump_result) || R_FAILED(resume_result)) return EIO;
  return 0;
}

static NxGuestThreadRef *detached_reap_candidate_locked(void) {
  for (NxGuestThreadRef *ref = guest_threads; ref; ref = ref->next) {
    if (ref->detached && ref->creator_returned &&
        ref->state == NX_GUEST_THREAD_EXITED && !ref->borrowers &&
        !ref->reap_claimed && !ref->reap_failed)
      return ref;
  }
  return NULL;
}

static void *detached_reaper_main(void *opaque) {
  (void)opaque;
  for (;;) {
    mutexLock(&guest_thread_lock);
    NxGuestThreadRef *victim;
    while (!(victim = detached_reap_candidate_locked()))
      condvarWait(&guest_thread_cond, &guest_thread_lock);
    victim->reap_claimed = 1;
    const pthread_t thread = victim->pthread;
    mutexUnlock(&guest_thread_lock);

    const int result = pthread_join(thread, NULL);
    mutexLock(&guest_thread_lock);
    if (!result) {
      remove_guest_thread_locked(victim);
      condvarWakeAll(&guest_thread_cond);
      mutexUnlock(&guest_thread_lock);
      THREAD_DIAG_ADD(guest_thread_stack_bytes, -(int64_t)victim->stack_size);
      THREAD_DIAG_ADD(guest_thread_reaped, 1);
      if (victim->stack_backing)
        (void)nx_sparse_pool_thread_release(victim->stack_backing);
      free(victim);
    } else {
      /* A failed join makes the native object's lifetime unknowable.  Keep a
       * quarantined record instead of ever handing out a potentially stale
       * Thread/Handle or spinning forever retrying the same join. */
      victim->reap_failed = 1;
      condvarWakeAll(&guest_thread_cond);
      mutexUnlock(&guest_thread_lock);
      THREAD_DIAG_ADD(guest_thread_reap_failures, 1);
    }
  }
  return NULL;
}

static int ensure_detached_reaper(void) {
  mutexLock(&guest_thread_lock);
  while (detached_reaper_state == REAPER_STARTING)
    condvarWait(&guest_thread_cond, &guest_thread_lock);
  if (detached_reaper_state == REAPER_RUNNING) {
    mutexUnlock(&guest_thread_lock);
    return 0;
  }
  detached_reaper_state = REAPER_STARTING;
  mutexUnlock(&guest_thread_lock);

  /* newlib on this libnx has no detach syscall hook, so a single persistent
   * joinable worker reaps every guest thread which requests detached state. */
  const int result = pthread_create(&detached_reaper_thread, NULL,
                                    detached_reaper_main, NULL);
  mutexLock(&guest_thread_lock);
  detached_reaper_state = result ? REAPER_STOPPED : REAPER_RUNNING;
  condvarWakeAll(&guest_thread_cond);
  mutexUnlock(&guest_thread_lock);
  return result;
}

static inline uintptr_t imports_read_thread_pointer(void) {
  uintptr_t value;
  __asm__ volatile("mrs %0, s3_3_c13_c0_2" : "=r"(value));
  return value;
}

static inline void imports_write_thread_pointer(uintptr_t value) {
  __asm__ volatile("msr s3_3_c13_c0_2, %0" : : "r"(value) : "memory");
}

static void external_thread_ref_dtor(void *opaque) {
  NxGuestThreadRef *ref = opaque;
  if (!ref) return;

  if (current_external_thread_ref == ref)
    current_external_thread_ref = NULL;

  mutexLock(&guest_thread_lock);
  if (ref->state == NX_GUEST_THREAD_LIVE)
    ref->state = NX_GUEST_THREAD_EXITING;
  condvarWakeAll(&guest_thread_cond);
  /* This is the last point at which newlib still owns a valid native handle.
   * New acquisitions now fail; drain only leases which were already issued so
   * the host cannot close the handle underneath an in-flight pause/resume. */
  while (ref->borrowers)
    condvarWait(&guest_thread_cond, &guest_thread_lock);
  ref->state = NX_GUEST_THREAD_EXITED;
  remove_guest_thread_locked(ref);
  condvarWakeAll(&guest_thread_cond);
  mutexUnlock(&guest_thread_lock);
  free(ref);
}

static int ensure_external_thread_key(void) {
  mutexLock(&guest_thread_lock);
  while (external_thread_key_state == EXTERNAL_KEY_STARTING)
    condvarWait(&guest_thread_cond, &guest_thread_lock);
  if (external_thread_key_state == EXTERNAL_KEY_READY) {
    mutexUnlock(&guest_thread_lock);
    return 0;
  }
  external_thread_key_state = EXTERNAL_KEY_STARTING;
  mutexUnlock(&guest_thread_lock);

  const int result = pthread_key_create(&external_thread_key,
                                        external_thread_ref_dtor);
  mutexLock(&guest_thread_lock);
  external_thread_key_state = result ? EXTERNAL_KEY_STOPPED : EXTERNAL_KEY_READY;
  condvarWakeAll(&guest_thread_cond);
  mutexUnlock(&guest_thread_lock);
  return result;
}

/* Guest code records pthread_self() in IL2CPP's GC thread table.  The main
 * thread and host-owned SDL/callback threads do not pass through our create
 * trampoline, so register them lazily the first time that identity becomes
 * guest-visible.  A dedicated host key retires the record during newlib's
 * teardown; unlike wrapper threads, external records are never joined/reaped
 * by this layer. */
static pthread_t pthread_self_fake(void) {
  const pthread_t self = pthread_self();
  /* Wrapper-created threads already carry their stable registry record in
   * host C TLS.  Externally registered threads carry it in the destructor
   * key.  Avoid taking guest_thread_lock on every guest pthread_self(): an
   * asynchronous GC pause must not commonly catch a target owning the same
   * registry mutex which the collector needs to locate later targets. */
  if (current_thread_start && current_thread_start->thread_ref &&
      current_thread_start->thread_ref->pthread == self)
    return self;
  if (current_external_thread_ref &&
      current_external_thread_ref->pthread == self)
    return self;

  mutexLock(&guest_thread_lock);
  NxGuestThreadRef *existing = find_guest_thread_locked(self);
  mutexUnlock(&guest_thread_lock);
  if (existing) return self;

  /* Without an exit destructor we cannot promise that Thread/Handle remains
   * valid, so leave the identity unregistered and make later GC lookup fail
   * closed instead of retaining a stale LIVE record. */
  if (ensure_external_thread_key() != 0) return self;
  NxGuestThreadRef *ref = calloc(1, sizeof(*ref));
  if (!ref) return self;
  ref->pthread = self;
  ref->native_thread = threadGetSelf();
  ref->handle = threadGetCurHandle();
  ref->guest_thread_pointer = imports_read_thread_pointer();
  ref->signal_mask = guest_signal_mask;
  ref->child_ready = 1;
  ref->creator_published = 1;
  ref->creator_returned = 1;
  ref->external = 1;
  ref->state = NX_GUEST_THREAD_LIVE;

  mutexLock(&guest_thread_lock);
  existing = find_guest_thread_locked(self);
  NxGuestThreadRef *registered = NULL;
  if (!existing) {
    ref->next = guest_threads;
    guest_threads = ref;
    registered = ref;
    ref = NULL;
  }
  mutexUnlock(&guest_thread_lock);
  free(ref);
  if (registered && pthread_setspecific(external_thread_key, registered) != 0) {
    /* The pthread_self() caller has not returned yet, so this record cannot be
     * present in IL2CPP's thread vector.  Retire it before exposing identity. */
    mutexLock(&guest_thread_lock);
    registered->state = NX_GUEST_THREAD_EXITING;
    while (registered->borrowers)
      condvarWait(&guest_thread_cond, &guest_thread_lock);
    registered->state = NX_GUEST_THREAD_EXITED;
    remove_guest_thread_locked(registered);
    mutexUnlock(&guest_thread_lock);
    free(registered);
  } else if (registered) {
    current_external_thread_ref = registered;
  }
  return self;
}

static void begin_thread_exit(NxGuestThreadRef *ref) {
  if (!ref) return;
  mutexLock(&guest_thread_lock);
  if (ref->state == NX_GUEST_THREAD_LIVE)
    ref->state = NX_GUEST_THREAD_EXITING;
  condvarWakeAll(&guest_thread_cond);
  while (ref->borrowers)
    condvarWait(&guest_thread_cond, &guest_thread_lock);
  mutexUnlock(&guest_thread_lock);
}

static void finish_thread_exit(NxGuestThreadRef *ref) {
  if (!ref) return;
  mutexLock(&guest_thread_lock);
  ref->state = NX_GUEST_THREAD_EXITED;
  condvarWakeAll(&guest_thread_cond);
  mutexUnlock(&guest_thread_lock);
}

static void release_thread_start(ThreadStart *ts) {
  if (!ts || ts->magic != THREAD_START_MAGIC) return;
  begin_thread_exit(ts->thread_ref);
  /* Bionic runs C++ thread_local destructors before pthread-key destructors.
   * Both can call back into the Android binary, so drain them before replacing
   * TPIDR_EL0.  Clearing their host keys here also prevents host pthread
   * teardown from invoking them a second time with the libnx thread pointer. */
  nx_run_cxa_thread_destructors();
  run_guest_key_destructors();
  jni_thread_exit_cleanup();
  const uintptr_t current = imports_read_thread_pointer();
  if (current == ts->guest_thread_pointer) {
    /* The utility tracks non-wrapper guest TLS too.  Keep the saved value in
     * ThreadStart as a defensive fallback for a mismatched/stale tracker. */
    if (!restore_bionic_tls())
      imports_write_thread_pointer(ts->host_thread_pointer);
  } else if (current != ts->host_thread_pointer) {
    imports_write_thread_pointer(ts->host_thread_pointer);
  }
  finish_thread_exit(ts->thread_ref);
  current_thread_start = NULL;
  ts->magic = 0;
  free(ts);
}

static void *thread_trampoline(void *p) {
  ThreadStart *ts = (ThreadStart *)p;
  void *(*entry)(void *) = ts->entry;
  void *arg = ts->arg;
  NxGuestThreadRef *ref = ts->thread_ref;
  ts->host_thread_pointer = imports_read_thread_pointer();
  /* POSIX threads inherit their creator's signal mask.  pthread_create_fake
   * saved that mask in the stable registry record before starting this host
   * thread; install it before any guest instruction can alter or query it. */
  guest_signal_mask = __atomic_load_n(&ref->signal_mask, __ATOMIC_ACQUIRE);
  current_thread_start = ts;
  install_bionic_tls(ts->tls);          // this thread's OWN stack-guard block (tpidr_el0+0x28)
  ts->guest_thread_pointer = imports_read_thread_pointer();

  mutexLock(&guest_thread_lock);
  ref->native_thread = threadGetSelf();
  ref->handle = threadGetCurHandle();
  ref->guest_thread_pointer = ts->guest_thread_pointer;
  ref->child_ready = 1;
  if (ref->creator_published) ref->state = NX_GUEST_THREAD_LIVE;
  condvarWakeAll(&guest_thread_cond);
  /* Do not let the child enter guest code before pthread_create_fake has
   * published the returned pthread_t in the registry and to its caller. */
  while (!ref->creator_returned)
    condvarWait(&guest_thread_cond, &guest_thread_lock);
  mutexUnlock(&guest_thread_lock);

  void *result = entry(arg);
  /* Guest destructors/JNI cleanup still need the Android thread pointer; the
   * release path restores libnx's pointer before returning through pthread
   * startup and before newlib performs its own teardown. */
  release_thread_start(ts);
  return result;
}

static _Noreturn void pthread_exit_fake(void *retval) {
  ThreadStart *ts = current_thread_start;
  if (ts && ts->magic == THREAD_START_MAGIC) {
    /* release_thread_start restores TPIDR_EL0 before freeing its embedded TLS
     * block.  The host then owns joined/detached stack and pthread teardown. */
    release_thread_start(ts);
  } else {
    /* Main, SDL, and other host-created threads have no ThreadStart.  Restore
     * them only if install_bionic_tls registered the current TPIDR_EL0; on an
     * ordinary host thread this is a deliberate no-op. */
    nx_run_cxa_thread_destructors();
    run_guest_key_destructors();
    jni_thread_exit_cleanup();
    (void)restore_bionic_tls();
  }
  pthread_exit(retval);
  __builtin_unreachable();
}

/* libnx's pthread_attr_setstack path treats the supplied size as backing for
 * the requested stack plus newlib reentrancy/TLS metadata.  Reserve one 64 KiB
 * page-aligned overhead block so the usable stack is never smaller than the
 * Android request.  The backing remains owned by NxGuestThreadRef until join
 * has called threadClose and removed libnx's stack mirror. */
static int configure_guest_thread_stack(pthread_attr_t *attr,
                                        size_t usable_stack,
                                        void **backing_out,
                                        size_t *backing_size_out) {
  if (backing_out) *backing_out = NULL;
  if (backing_size_out) *backing_size_out = 0;
  if (!attr || usable_stack > SIZE_MAX - GUEST_THREAD_BACKING_OVERHEAD)
    return EOVERFLOW;

  const size_t backing_size =
    usable_stack + GUEST_THREAD_BACKING_OVERHEAD;
  void *backing = nx_sparse_pool_thread_alloc(backing_size);
  if (backing) {
    /* New pthread stacks must not inherit stale guest or compiler objects. */
    memset(backing, 0, backing_size);
    const int result = pthread_attr_setstack(attr, backing, backing_size);
    if (result == 0) {
      if (backing_out) *backing_out = backing;
      if (backing_size_out) *backing_size_out = backing_size;
      return 0;
    }
    (void)nx_sparse_pool_thread_release(backing);
  }
  return pthread_attr_setstacksize(attr, usable_stack);
}

int pthread_create_fake(pthread_t *thread, const void *bionic_attr, void *entry, void *arg) {
  THREAD_DIAG_ADD(guest_thread_create_calls, 1);
  if (!thread || !entry) {
    record_guest_thread_failure(THREAD_FAILURE_RECORDS, EINVAL, 0, 0);
    return EINVAL;
  }
  size_t stack = 0;
  int detached = 0;
  if (bionic_attr) {
    const OurAttr *o = bionic_attr;
    if (o->magic == ATTR_MAGIC) {
      stack = o->stacksize;
      detached = o->detach == 1; /* bionic PTHREAD_CREATE_DETACHED */
    }
  }
  if (detached) {
    const int reaper_result = ensure_detached_reaper();
    if (reaper_result) {
      record_guest_thread_failure(THREAD_FAILURE_REAPER, reaper_result,
                                  stack, 0);
      return pthread_error_n2b(reaper_result);
    }
  }

  NxGuestThreadRef *ref = calloc(1, sizeof(*ref));
  ThreadStart *ts = calloc(1, sizeof(*ts));
  if (!ref || !ts) {
    free(ref);
    free(ts);
    record_guest_thread_failure(THREAD_FAILURE_RECORDS, ENOMEM, stack, 0);
    return ENOMEM;
  }
  ref->handle = INVALID_HANDLE;
  ref->detached = detached;
  ref->guest_entry = (uintptr_t)entry;
  /* pthread_create inherits the calling thread's blocked-signal set. */
  ref->signal_mask = guest_signal_mask;
  ref->state = NX_GUEST_THREAD_CREATING;
  ts->magic = THREAD_START_MAGIC;
  ts->entry = (void *(*)(void *))entry;
  ts->arg = arg;
  ts->thread_ref = ref;
  const size_t requested_stack = stack;
  /* Bionic's LP64 default is approximately 1 MiB.  Retain a conservative
   * 512-KiB floor for explicitly small engine workers, but do not double every
   * std::thread allocation: the installer keeps dozens alive concurrently. */
  if (!stack) stack = GUEST_THREAD_DEFAULT_STACK;
  if (stack < GUEST_THREAD_FALLBACK_STACK)
    stack = GUEST_THREAD_FALLBACK_STACK;
  stack = (stack + 0xFFFFu) & ~(size_t)0xFFFFu;   // round to 64KB
  pthread_attr_t attr;
  int r = pthread_attr_init(&attr);
  if (r != 0) {
    free(ref);
    free(ts);
    record_guest_thread_failure(THREAD_FAILURE_ATTR_INIT, r,
                                requested_stack, stack);
    return pthread_error_n2b(r);
  }
  void *stack_backing = NULL;
  size_t stack_backing_size = 0;
  r = configure_guest_thread_stack(&attr, stack, &stack_backing,
                                   &stack_backing_size);
  pthread_t created = (pthread_t)0;
  if (r == 0)
    r = pthread_create(&created, &attr, thread_trampoline, ts);
  if (r == ENOMEM && requested_stack <= GUEST_THREAD_DEFAULT_STACK &&
      stack > GUEST_THREAD_FALLBACK_STACK) {
    /* libnx collapses allocation, resource-limit, and svcCreateThread errors
     * into ENOMEM.  A smaller Android-compatible worker stack can recover from
     * heap fragmentation; resource-limit failures still fail deterministically. */
    if (stack_backing) {
      (void)nx_sparse_pool_thread_release(stack_backing);
      stack_backing = NULL;
      stack_backing_size = 0;
    }
    pthread_attr_destroy(&attr);
    const int reinit_result = pthread_attr_init(&attr);
    const int resize_result = reinit_result ? reinit_result :
      configure_guest_thread_stack(
        &attr, GUEST_THREAD_FALLBACK_STACK, &stack_backing,
        &stack_backing_size);
    if (resize_result == 0) {
      stack = GUEST_THREAD_FALLBACK_STACK;
      r = pthread_create(&created, &attr, thread_trampoline, ts);
      if (r == 0)
        THREAD_DIAG_ADD(guest_thread_fallback_successes, 1);
    } else {
      r = resize_result;
    }
  }
  pthread_attr_destroy(&attr);
  if (r != 0) {
    if (stack_backing)
      (void)nx_sparse_pool_thread_release(stack_backing);
    free(ref);
    free(ts);
    record_guest_thread_failure(
      r == EINVAL ? THREAD_FAILURE_ATTR_STACK : THREAD_FAILURE_HOST_CREATE,
      r, requested_stack, stack);
    return pthread_error_n2b(r);
  }

  mutexLock(&guest_thread_lock);
  ref->pthread = created;
  ref->stack_size = stack;
  ref->stack_backing = stack_backing;
  ref->stack_backing_size = stack_backing_size;
  ref->next = guest_threads;
  guest_threads = ref;
  ref->creator_published = 1;
  if (ref->child_ready) ref->state = NX_GUEST_THREAD_LIVE;
  condvarWakeAll(&guest_thread_cond);
  while (!ref->child_ready)
    condvarWait(&guest_thread_cond, &guest_thread_lock);
  ref->state = NX_GUEST_THREAD_LIVE;
  *thread = created;
  /* Publish accounting before releasing the child.  A very short detached
   * worker can otherwise exit and be reaped before its stack reservation is
   * added, transiently wrapping the unsigned byte counter. */
  THREAD_DIAG_ADD(guest_thread_create_successes, 1);
  if (detached) THREAD_DIAG_ADD(guest_thread_detached_creates, 1);
  const uint64_t current_stack =
    THREAD_DIAG_ADD(guest_thread_stack_bytes, stack) + stack;
  update_guest_thread_stack_peak(current_stack);
  ref->creator_returned = 1;
  condvarWakeAll(&guest_thread_cond);
  mutexUnlock(&guest_thread_lock);
  return 0;
}
int pthread_join_fake(pthread_t thread, void **retval) {
  if (!thread) return ESRCH;
  if (thread == pthread_self()) return 35; /* Bionic EDEADLK. */

  mutexLock(&guest_thread_lock);
  NxGuestThreadRef *ref = find_guest_thread_locked(thread);
  if (!ref) {
    mutexUnlock(&guest_thread_lock);
    return pthread_error_n2b(pthread_join(thread, retval));
  }
  if (ref->external || ref->detached || ref->reap_claimed) {
    mutexUnlock(&guest_thread_lock);
    return EINVAL;
  }
  ref->reap_claimed = 1;
  while (ref->borrowers)
    condvarWait(&guest_thread_cond, &guest_thread_lock);
  mutexUnlock(&guest_thread_lock);

  const int result = pthread_join(thread, retval);
  mutexLock(&guest_thread_lock);
  if (!result) {
    remove_guest_thread_locked(ref);
    condvarWakeAll(&guest_thread_cond);
    mutexUnlock(&guest_thread_lock);
    THREAD_DIAG_ADD(guest_thread_stack_bytes, -(int64_t)ref->stack_size);
    THREAD_DIAG_ADD(guest_thread_reaped, 1);
    if (ref->stack_backing)
      (void)nx_sparse_pool_thread_release(ref->stack_backing);
    free(ref);
  } else {
    ref->reap_failed = 1;
    condvarWakeAll(&guest_thread_cond);
    mutexUnlock(&guest_thread_lock);
    THREAD_DIAG_ADD(guest_thread_reap_failures, 1);
  }
  return pthread_error_n2b(result);
}

int pthread_detach_fake(pthread_t thread) {
  if (!thread) return ESRCH;
  const int reaper_result = ensure_detached_reaper();
  if (reaper_result) return pthread_error_n2b(reaper_result);

  mutexLock(&guest_thread_lock);
  NxGuestThreadRef *ref = find_guest_thread_locked(thread);
  if (!ref) {
    mutexUnlock(&guest_thread_lock);
    return ESRCH;
  }
  if (ref->external || ref->detached || ref->reap_claimed) {
    mutexUnlock(&guest_thread_lock);
    return EINVAL;
  }
  ref->detached = 1;
  condvarWakeAll(&guest_thread_cond);
  mutexUnlock(&guest_thread_lock);
  return 0;
}
static int update_guest_signal_mask(int how, const void *set, void *old) {
  if (old) memcpy(old, &guest_signal_mask, sizeof(guest_signal_mask));
  if (!set) return 0;
  uint64_t value;
  memcpy(&value, set, sizeof(value));
  switch (how) {
    case 0: guest_signal_mask |= value; break;  /* SIG_BLOCK */
    case 1: guest_signal_mask &= ~value; break; /* SIG_UNBLOCK */
    case 2: guest_signal_mask = value; break;   /* SIG_SETMASK */
    default: return EINVAL;
  }
  /* pthread_kill_gc must decide whether signal 30 is deliverable without
   * taking the guest-thread registry mutex: the target may be changing its
   * mask specifically while holding a GC-internal lock. */
  NxGuestThreadRef *ref = current_thread_start
    ? current_thread_start->thread_ref : current_external_thread_ref;
  if (ref)
    __atomic_store_n(&ref->signal_mask, guest_signal_mask, __ATOMIC_RELEASE);
  return 0;
}

int pthread_sigmask_fake(int how, const void *set, void *old) {
  return update_guest_signal_mask(how, set, old);
}

/* Multiplex bionic TLS keys over one libnx key. */
#define FAKE_KEYS_MAX 128
static pthread_mutex_t g_key_mutex = PTHREAD_MUTEX_INITIALIZER;
static struct {
  unsigned used;
  uint32_t generation;
  void (*dtor)(void *);
} g_key_table[FAKE_KEYS_MAX];
static pthread_key_t g_master_key;
static int g_master_key_ready;
typedef struct {
  void *values[FAKE_KEYS_MAX];
  uint32_t generations[FAKE_KEYS_MAX];
} KeyValues;

static void destroy_guest_key_values(KeyValues *kv) {
  for (int iter = 0; iter < 4; iter++) {     // POSIX: rerun while dtors set new values
    int again = 0;
    for (int i = 0; i < FAKE_KEYS_MAX; i++) {
      void *v = kv->values[i];
      /* used/generation/dtor form one logical key generation.  Snapshot them
       * under the creator/deleter lock, then drop it before invoking guest
       * code so a destructor may safely call the pthread-key APIs itself. */
      pthread_mutex_lock(&g_key_mutex);
      const unsigned used = g_key_table[i].used;
      const uint32_t generation = g_key_table[i].generation;
      void (*dtor)(void *) = g_key_table[i].dtor;
      pthread_mutex_unlock(&g_key_mutex);
      if (used && kv->generations[i] == generation && dtor && v) {
        kv->values[i] = NULL;
        dtor(v);
        again = 1;
      }
    }
    if (!again) break;
  }
}

static void master_key_dtor(void *p) {
  KeyValues *kv = p;
  destroy_guest_key_values(kv);
  free(kv);
}

static void run_guest_key_destructors(void) {
  if (!__atomic_load_n(&g_master_key_ready, __ATOMIC_ACQUIRE)) return;
  KeyValues *kv = pthread_getspecific(g_master_key);
  if (!kv) return;

  /* Keep kv installed during callbacks so pthread_setspecific_fake updates the
   * same value array and the next of the four passes observes it. */
  destroy_guest_key_values(kv);
  (void)pthread_setspecific(g_master_key, NULL);
  free(kv);
}

int pthread_key_create_fake(unsigned *key, void (*dtor)(void *)) {
  pthread_mutex_lock(&g_key_mutex);
  if (!g_master_key_ready) {
    if (pthread_key_create(&g_master_key, master_key_dtor) != 0) {
      pthread_mutex_unlock(&g_key_mutex);
      return EAGAIN;
    }
    __atomic_store_n(&g_master_key_ready, 1, __ATOMIC_RELEASE);
  }
  for (unsigned i = 0; i < FAKE_KEYS_MAX; i++) {
    if (!g_key_table[i].used) {
      uint32_t generation = g_key_table[i].generation + 1;
      if (!generation) generation = 1;
      g_key_table[i].generation = generation;
      __atomic_store_n(&g_key_table[i].dtor, dtor, __ATOMIC_RELEASE);
      __atomic_store_n(&g_key_table[i].used, 1, __ATOMIC_RELEASE);
      *key = i + 1;                 // 1-based: a zeroed key is invalid
      pthread_mutex_unlock(&g_key_mutex);
      return 0;
    }
  }
  pthread_mutex_unlock(&g_key_mutex);
  return EAGAIN;
}

int pthread_key_delete_fake(unsigned key) {
  if (key == 0 || key > FAKE_KEYS_MAX) return EINVAL;
  pthread_mutex_lock(&g_key_mutex);
  if (!g_key_table[key - 1].used) {
    pthread_mutex_unlock(&g_key_mutex);
    return EINVAL;
  }
  __atomic_store_n(&g_key_table[key - 1].used, 0, __ATOMIC_RELEASE);
  __atomic_store_n(&g_key_table[key - 1].dtor, NULL, __ATOMIC_RELEASE);
  pthread_mutex_unlock(&g_key_mutex);
  return 0;
}

void *pthread_getspecific_fake(unsigned key) {
  if (key == 0 || key > FAKE_KEYS_MAX ||
      !__atomic_load_n(&g_master_key_ready, __ATOMIC_ACQUIRE)) return NULL;
  const unsigned index = key - 1;
  if (!__atomic_load_n(&g_key_table[index].used, __ATOMIC_ACQUIRE)) return NULL;
  const uint32_t generation =
    __atomic_load_n(&g_key_table[index].generation, __ATOMIC_ACQUIRE);
  KeyValues *kv = pthread_getspecific(g_master_key);
  return kv && kv->generations[index] == generation ? kv->values[index] : NULL;
}

int pthread_setspecific_fake(unsigned key, const void *value) {
  if (key == 0 || key > FAKE_KEYS_MAX ||
      !__atomic_load_n(&g_master_key_ready, __ATOMIC_ACQUIRE)) return EINVAL;
  const unsigned index = key - 1;
  if (!__atomic_load_n(&g_key_table[index].used, __ATOMIC_ACQUIRE)) return EINVAL;
  const uint32_t generation =
    __atomic_load_n(&g_key_table[index].generation, __ATOMIC_ACQUIRE);
  KeyValues *kv = pthread_getspecific(g_master_key);
  if (!kv) {
    kv = calloc(1, sizeof(*kv));
    if (!kv) return ENOMEM;
    pthread_setspecific(g_master_key, kv);
  }
  kv->values[index] = (void *)value;
  kv->generations[index] = generation;
  return 0;
}

static int ret0_i(void) { return 0; }
static unsigned ret0_u(void) { return 0; }
typedef struct {
  uint32_t flags;
  uint32_t reserved;
  void *handler;
  uint64_t mask;
  void *restorer;
} GuestSigAction;
_Static_assert(sizeof(GuestSigAction) == 32, "arm64 bionic sigaction ABI");

enum {
  BIONIC_ENOSYS = 38,
  BIONIC_EOPNOTSUPP = 95,
};
#define BIONIC_SA_ONSTACK UINT32_C(0x08000000)
#define GENSHIN_GC_SIGNAL_NUMBER 30

static Mutex guest_signal_action_lock;
static GuestSigAction guest_signal_actions[65];

static void *signal_fake(int signal_number, void *handler) {
  if (signal_number <= 0 || signal_number >= 65) {
    errno = EINVAL;
    return (void *)(intptr_t)-1;
  }
  mutexLock(&guest_signal_action_lock);
  void *previous = guest_signal_actions[signal_number].handler;
  guest_signal_actions[signal_number].flags = 0x10000000u; /* SA_RESTART */
  guest_signal_actions[signal_number].handler = handler;
  guest_signal_actions[signal_number].mask = 0;
  guest_signal_actions[signal_number].restorer = NULL;
  mutexUnlock(&guest_signal_action_lock);
  return previous;
}

static int sigaction_fake(int signal_number, const void *action, void *old_action) {
  if (signal_number <= 0 || signal_number >= 65) {
    errno = EINVAL;
    return -1;
  }
  GuestSigAction incoming;
  if (action) {
    memcpy(&incoming, action, sizeof(incoming));
    if ((incoming.flags & BIONIC_SA_ONSTACK) != 0 &&
        signal_number != GENSHIN_GC_SIGNAL_NUMBER) {
      /* Horizon cannot enter arbitrary handlers on an alternate POSIX signal
       * stack.  The pinned client GC signal is handled by pthread_kill_gc. */
      errno = BIONIC_ENOSYS;
      return -1;
    }
  }
  mutexLock(&guest_signal_action_lock);
  const GuestSigAction previous = guest_signal_actions[signal_number];
  if (action) memcpy(&guest_signal_actions[signal_number], &incoming,
                     sizeof(GuestSigAction));
  if (old_action) memcpy(old_action, &previous, sizeof(previous));
  mutexUnlock(&guest_signal_action_lock);
  return 0;
}

static int sigaltstack_fake(const void *stack, void *old_stack) {
  (void)stack;
  (void)old_stack;
  errno = BIONIC_ENOSYS;
  return -1;
}

static int sigsuspend_fake(const void *mask) {
  (void)mask;
  /* No asynchronous POSIX signal delivery exists.  sigsuspend never returns
     success, so report the same interruption shape instead of a false zero. */
  errno = EINTR;
  return -1;
}

static FILE *popen_unsupported(const char *command, const char *mode) {
  (void)command;
  (void)mode;
  errno = BIONIC_ENOSYS;
  return NULL;
}

static int pclose_unsupported(FILE *stream) {
  (void)stream;
  errno = BIONIC_ENOSYS;
  return -1;
}

static int system_unsupported(const char *command) {
  if (!command) return 0; /* no command processor is available */
  errno = BIONIC_ENOSYS;
  return -1;
}

static int sigemptyset_fake(void *set) {
  if (!set) { errno = EINVAL; return -1; }
  const uint64_t empty = 0;
  memcpy(set, &empty, sizeof(empty));
  return 0;
}

static int sigfillset_fake(void *set) {
  if (!set) { errno = EINVAL; return -1; }
  const uint64_t full = UINT64_MAX;
  memcpy(set, &full, sizeof(full));
  return 0;
}

static int sigaddset_fake(void *set, int signal_number) {
  if (!set || signal_number <= 0 || signal_number > 64) {
    errno = EINVAL; return -1;
  }
  uint64_t value;
  memcpy(&value, set, sizeof(value));
  value |= UINT64_C(1) << (signal_number - 1);
  memcpy(set, &value, sizeof(value));
  return 0;
}

static int sigdelset_fake(void *set, int signal_number) {
  if (!set || signal_number <= 0 || signal_number > 64) {
    errno = EINVAL; return -1;
  }
  uint64_t value;
  memcpy(&value, set, sizeof(value));
  value &= ~(UINT64_C(1) << (signal_number - 1));
  memcpy(set, &value, sizeof(value));
  return 0;
}
static int mlock_stub(const void *addr, size_t len) { (void)addr; (void)len; return 0; }
static int sigprocmask_stub(int how, const void *set, void *old) {
  const int result = update_guest_signal_mask(how, set, old);
  if (result) { errno = result; return -1; }
  return 0;
}
static int tcgetattr_stub(int fd, void *termios_data) {
  (void)fd; (void)termios_data; errno = ENOTTY; return -1;
}
static int tcsetattr_stub(int fd, int action, const void *termios_data) {
  (void)fd; (void)action; (void)termios_data; errno = ENOTTY; return -1;
}
static int fchown_stub(int fd, unsigned uid, unsigned gid) {
  (void)fd; (void)uid; (void)gid; return 0;
}
static int pthread_attr_getschedparam_stub(const void *attr, void *param) {
  (void)attr;
  if (!param) return EINVAL;
  /* Android/Linux struct sched_param contains exactly one 32-bit priority. */
  *(int32_t *)param = 0;
  return 0;
}
static int pthread_attr_setschedparam_stub(void *attr, const void *param) {
  (void)attr;
  return param ? 0 : EINVAL;
}
static int pthread_mutex_timedlock_fake(pthread_mutex_t **mutex,
                                        const struct timespec *absolute_time) {
  int result = ensure_mutex(mutex);
  if (result != 0) return result;
  pthread_mutex_t *host = guest_mutex(mutex);
  if (!host) return EINVAL;
  for (;;) {
    result = pthread_mutex_trylock(host);
    if (result != EBUSY) return pthread_error_n2b(result);
    if (absolute_time) {
      struct timespec now;
      clock_gettime(CLOCK_REALTIME, &now);
      if (now.tv_sec > absolute_time->tv_sec ||
          (now.tv_sec == absolute_time->tv_sec && now.tv_nsec >= absolute_time->tv_nsec))
        return 110; /* Bionic ETIMEDOUT. */
    }
    svcSleepThread(1000000ull);
  }
}

static DIR *opendir_fake(const char *path) {
  void *packed = asset_pack_opendir_path(path);
  if (packed) return (DIR *)packed;
  char nb[600];
  if (path && path[0] == '/') { snprintf(nb, sizeof nb, "sdmc:%s", path); path = nb; }
  return opendir(path);
}
static int chmod_stub(const char *path, int mode) { (void)path; (void)mode; return 0; }
static int dup2_fake(int source, int target) {
  if (source == target) {
    uint32_t stripe;
    if (!nx_fd_route_source_lock(source, &stripe)) return -1;
    int result;
    if (fakefd_is_fake(source)) {
      result = fakefd_is_live(source) ? source : (errno = EBADF, -1);
    } else if (asset_pack_fd_is(source)) {
      result = source;
    } else if (nx_epoll_is_fd(source)) {
      errno = BIONIC_EOPNOTSUPP;
      result = -1;
    } else {
      result = fcntl(source, F_GETFD, 0) < 0 ? -1 : source;
    }
    int saved = errno;
    nx_fd_route_source_unlock(stripe);
    errno = saved;
    return result;
  }

  NxFdRoutePair guard;
  if (!nx_fd_route_pair_begin(source, target, &guard)) return -1;
  int result = -1;
  int stable_source = -1;
  int source_fake = fakefd_is_fake(source);
  int source_asset = asset_pack_fd_is(source);
  int source_epoll = nx_epoll_is_fd(source);
  int target_fake = fakefd_is_fake(target);
  int target_epoll = nx_epoll_is_fd(target);

  /* Validate and pin the source route before touching any target state.  Both
   * route stripes remain locked until the atomic replacement finishes. */
  if ((source_fake && !fakefd_is_live(source)) ||
      (!source_fake && !source_asset && !source_epoll &&
       fcntl(source, F_GETFD, 0) < 0)) {
    errno = EBADF;
    goto finished;
  }
  if (source_epoll || target_epoll || (target_fake && !source_fake)) {
    errno = BIONIC_EOPNOTSUPP;
    goto finished;
  }
  if (!source_fake && !source_asset && ra_flush_detach(source) < 0)
    goto finished;
  if (ra_flush_detach(target) < 0) goto finished;

  /* Keep the source open description stable after releasing the ordered route
   * mutexes.  This is required before a packed target can drain active reads:
   * those reads must be able to validate their stale tickets and drop pins. */
  if (source_fake) {
    stable_source = fakefd_dup(source);
  } else if (source_asset) {
    stable_source = asset_pack_dup_fd(source);
  } else {
    stable_source = fcntl(source, F_DUPFD_CLOEXEC, 0);
  }
  if (stable_source < 0) goto finished;
  fd_metadata_copy(source, stable_source);

  nx_epoll_forget_fd(target);
  android_native_looper_forget_fd(target);
  nx_fd_route_pair_release(&guard);
  if (stable_source == target) {
    /* A free target can be selected while creating the stable duplicate. */
    result = target;
  } else if (source_fake) {
    result = fakefd_dup2(stable_source, target);
  } else if (source_asset) {
    result = asset_pack_dup2_fd(stable_source, target);
  } else {
    /* A packed target owns a registry record in addition to its native fd. */
    if (asset_pack_fd_is(target) && asset_pack_close_fd(target) < 0)
      goto finished;
    result = dup2(stable_source, target);
  }
  if (result >= 0) {
    fd_metadata_copy(stable_source, target);
    network_track_duplicate(source, target);
  }

finished: {
    int saved = errno;
    if (stable_source >= 0 && stable_source != target) {
      if (source_fake) (void)fakefd_close(stable_source);
      else if (source_asset) (void)asset_pack_close_fd(stable_source);
      else (void)close(stable_source);
      fd_metadata_copy(-1, stable_source);
    }
    nx_fd_route_replace_end(&guard);
    errno = saved;
  }
  return result;
}
int dup_fake(int fd) {
  uint32_t stripe;
  if (!nx_fd_route_source_lock(fd, &stripe)) return -1;
  int result;
  if (fakefd_is_fake(fd)) {
    result = fakefd_dup(fd);
  } else if (asset_pack_fd_is(fd)) {
    result = asset_pack_dup_fd(fd);
  } else if (ra_flush_detach(fd) < 0) {
    result = -1;
  } else {
    result = dup(fd);
  }
  if (result >= 0) {
    fd_metadata_copy(fd, result);
    network_track_duplicate(fd, result);
  }
  int saved = errno;
  nx_fd_route_source_unlock(stripe);
  errno = saved;
  return result;
}
static int uname_fake(void *buf) {
  if (!buf) { errno = EFAULT; return -1; }
  char (*field)[65] = buf;
  memset(buf, 0, 6 * 65);
  snprintf(field[0], 65, "Linux");
  snprintf(field[1], 65, "switch");
  snprintf(field[2], 65, "4.14.0");
  snprintf(field[3], 65, "#1 SMP PREEMPT");
  snprintf(field[4], 65, "aarch64");
  snprintf(field[5], 65, "localdomain");
  return 0;
}
static long sysconf_pass(int n) { return sysconf_fake(n); }
static int readlink_stub(const char *p, char *b, size_t n) { (void)p; (void)b; (void)n; errno = EINVAL; return -1; }
static int link_stub(const char *a, const char *b) {
  (void)a; (void)b; errno = BIONIC_EOPNOTSUPP; return -1;
}
static int symlink_stub(const char *a, const char *b) {
  (void)a; (void)b; errno = BIONIC_EOPNOTSUPP; return -1;
}
static int fchmod_stub(int fd, int m) { (void)fd; (void)m; return 0; }

/* A bounded userspace sendfile.  nx_pread leaves the input descriptor position
 * unchanged, allowing both the optional-offset and partial-write cases to
 * commit exactly the number of bytes accepted by the output. */
static long sendfile_fake(int output_fd, int input_fd, long *offset,
                          size_t count) {
  if (count == 0) return 0;
  if (fakefd_is_fake(input_fd) || nx_epoll_is_fd(input_fd) ||
      nx_epoll_is_fd(output_fd)) {
    errno = BIONIC_EOPNOTSUPP;
    return -1;
  }
  if (asset_pack_fd_is(output_fd)) { errno = EBADF; return -1; }

  long cursor;
  if (offset) {
    cursor = *offset;
    if (cursor < 0) { errno = EINVAL; return -1; }
  } else {
    cursor = z_lseek(input_fd, 0, SEEK_CUR);
    if (cursor < 0) return -1;
  }
  if (count > (size_t)LONG_MAX) count = (size_t)LONG_MAX;
  if (count > (size_t)(LONG_MAX - cursor))
    count = (size_t)(LONG_MAX - cursor);
  if (count == 0) return 0;

  const size_t capacity = count < 65536u ? count : 65536u;
  unsigned char *buffer = malloc(capacity);
  if (!buffer) { errno = ENOMEM; return -1; }

  size_t total = 0;
  int failed = 0;
  int failure_errno = 0;
  while (total < count) {
    const size_t wanted = count - total < capacity ? count - total : capacity;
    const long received = nx_pread(input_fd, buffer, wanted, cursor);
    if (received < 0) {
      failed = 1;
      failure_errno = errno;
      break;
    }
    if (received == 0) break;

    size_t written = 0;
    while (written < (size_t)received) {
      const long result = write_fake(output_fd, buffer + written,
                                     (size_t)received - written);
      if (result <= 0) {
        failed = 1;
        failure_errno = result < 0 ? errno : EIO;
        break;
      }
      written += (size_t)result;
      total += (size_t)result;
      cursor += result;
    }
    if (failed) break;
  }

  if (offset) {
    *offset = cursor;
  } else if (z_lseek(input_fd, cursor, SEEK_SET) < 0) {
    if (!failed) {
      failed = 1;
      failure_errno = errno;
    }
  }
  free(buffer);
  if (failed && total == 0) {
    errno = failure_errno;
    return -1;
  }
  if (failed) errno = failure_errno;
  return (long)total;
}

/* JNI device fields can be null. */
static int z_strcmp(const char *a, const char *b) {
  if (a == b) return 0;
  if (!a) return -1;
  if (!b) return 1;
  return strcmp(a, b);
}
static int z_strncmp(const char *a, const char *b, size_t n) {
  if (a == b || n == 0) return 0;
  if (!a) return -1;
  if (!b) return 1;
  return strncmp(a, b, n);
}
static char *z_strstr(const char *h, const char *n) {
  if (!h || !n) return NULL;
  return strstr(h, n);
}
static char *z_strchr(const char *s, int c) { return s ? strchr(s, c) : NULL; }
static char *z_strrchr(const char *s, int c) { return s ? strrchr(s, c) : NULL; }
static size_t z_strlen(const char *s) { return s ? strlen(s) : 0; }

/* switch-mesa may report a zero-sized window surface. */
static EGLBoolean egl_QuerySurface_fake(EGLDisplay d, EGLSurface s, EGLint attr, EGLint *val) {
#ifdef VULKAN_ONLY
  return nx_eglQuerySurface_stub(d, s, attr, val);
#else
  EGLBoolean r = eglQuerySurface(d, s, attr, val);
  if (val) {
    if (attr == 0x3057 /*EGL_WIDTH*/  && *val <= 0) { *val = screen_width;  r = EGL_TRUE; }
    if (attr == 0x3056 /*EGL_HEIGHT*/ && *val <= 0) { *val = screen_height; r = EGL_TRUE; }
  }
  return r;
#endif
}

DynLibFunction dynlib_functions[] = {
  /* Android Unity dynamically loads Vulkan.  These anchors also retain NVK
   * when the homebrew ELF is linked with --gc-sections. */
  { "vkGetInstanceProcAddr", (uintptr_t)&nx_vkGetInstanceProcAddr },
  { "vkGetDeviceProcAddr", (uintptr_t)&nx_vkGetDeviceProcAddr },
  { "vkCreateInstance", (uintptr_t)&nx_vkCreateInstance },
  { "vkEnumerateInstanceExtensionProperties", (uintptr_t)&nx_vkEnumerateInstanceExtensionProperties },
  { "vkCreateAndroidSurfaceKHR", (uintptr_t)&nx_vkCreateAndroidSurfaceKHR },
  /* liblog and C++ runtime */
  { "__android_log_print", (uintptr_t)&__android_log_print },
  { "__android_log_write", (uintptr_t)&__android_log_write },
  { "__android_log_vprint", (uintptr_t)&__android_log_vprint },
  { "android_set_abort_message", (uintptr_t)&android_set_abort_message_fake },
  { "__cxa_atexit", (uintptr_t)&__cxa_atexit_fake },
  { "__cxa_finalize", (uintptr_t)&__cxa_finalize_fake },
  { "__cxa_pure_virtual", (uintptr_t)&__cxa_pure_virtual_fake },
  { "__stack_chk_fail", (uintptr_t)&__stack_chk_fail_fake },
  { "__errno", (uintptr_t)&__errno },

  /* Fortify */
  { "__memmove_chk", (uintptr_t)&__memmove_chk_fake },
  { "__strlen_chk", (uintptr_t)&__strlen_chk_fake },
  { "__vsnprintf_chk", (uintptr_t)&__vsnprintf_chk_fake },
  { "__FD_SET_chk", (uintptr_t)&__FD_SET_chk_fake },
  { "__FD_ISSET_chk", (uintptr_t)&__FD_ISSET_chk_fake },

  /* Bionic */
  { "__system_property_get", (uintptr_t)&__system_property_get_fake },
  { "__system_property_find", (uintptr_t)&__system_property_find_fake },
  { "__system_property_read", (uintptr_t)&__system_property_read_fake },
  { "getauxval", (uintptr_t)&getauxval_fake },
  { "syscall", (uintptr_t)&syscall_fake },
  { "dl_iterate_phdr", (uintptr_t)&so_dl_iterate_phdr },
  { "__ctype_get_mb_cur_max", (uintptr_t)&__ctype_get_mb_cur_max_fake },
  { "sysconf", (uintptr_t)&sysconf_pass },
  { "uname", (uintptr_t)&uname_fake },
  { "openlog", (uintptr_t)&ret0_i },
  { "closelog", (uintptr_t)&ret0_i },
  { "syslog", (uintptr_t)&ret0_i },
  { "abort", (uintptr_t)&abort },

  /* Memory */
  { "malloc", (uintptr_t)&nx_guest_malloc },
  { "calloc", (uintptr_t)&nx_guest_calloc },
  { "realloc", (uintptr_t)&nx_guest_realloc },
  { "free", (uintptr_t)&nx_guest_free },
  { "memalign", (uintptr_t)&nx_guest_memalign },
  { "posix_memalign", (uintptr_t)&nx_guest_posix_memalign },
  { "mmap", (uintptr_t)&mmap_fake },
  { "munmap", (uintptr_t)&munmap_fake },
  { "mprotect", (uintptr_t)&mprotect_fake },
  { "madvise", (uintptr_t)&madvise_fake },
  { "mremap", (uintptr_t)&mremap_fake },

  /* Strings */
  { "memchr", (uintptr_t)&memchr }, { "memcmp", (uintptr_t)&memcmp },
  { "memcpy", (uintptr_t)&memcpy }, { "memmove", (uintptr_t)&memmove },
  { "memset", (uintptr_t)&memset },
  { "strcat", (uintptr_t)&strcat }, { "strchr", (uintptr_t)&z_strchr },
  { "strcmp", (uintptr_t)&z_strcmp }, { "strcpy", (uintptr_t)&strcpy },
  { "strlen", (uintptr_t)&z_strlen },
  { "strncmp", (uintptr_t)&z_strncmp }, { "strncpy", (uintptr_t)&strncpy },
  { "strrchr", (uintptr_t)&z_strrchr }, { "strstr", (uintptr_t)&z_strstr },
  { "strtod", (uintptr_t)&strtod }, { "strtof", (uintptr_t)&strtof },
  { "strtol", (uintptr_t)&strtol }, { "strtold", (uintptr_t)&strtold },
  { "strtoll", (uintptr_t)&strtoll }, { "strtoul", (uintptr_t)&strtoul },
  { "strtoull", (uintptr_t)&strtoull }, { "atoi", (uintptr_t)&atoi },
  { "qsort", (uintptr_t)&qsort },

  /* Locale */
  { "wcslen", (uintptr_t)&wcslen }, { "wmemchr", (uintptr_t)&wmemchr },
  { "wmemcmp", (uintptr_t)&wmemcmp }, { "wcstod", (uintptr_t)&wcstod },
  { "wcstof", (uintptr_t)&wcstof }, { "wcstol", (uintptr_t)&wcstol },
  { "wcstold", (uintptr_t)&wcstold }, { "wcstoll", (uintptr_t)&wcstoll },
  { "wcstoul", (uintptr_t)&wcstoul }, { "wcstoull", (uintptr_t)&wcstoull },
  { "btowc", (uintptr_t)&btowc }, { "wctob", (uintptr_t)&wctob },
  { "mbrlen", (uintptr_t)&mbrlen }, { "mbrtowc", (uintptr_t)&mbrtowc },
  { "mbtowc", (uintptr_t)&mbtowc }, { "mbsrtowcs", (uintptr_t)&mbsrtowcs },
  { "wcrtomb", (uintptr_t)&wcrtomb }, { "mbsnrtowcs", (uintptr_t)&mbsnrtowcs_fake },
  { "wcsnrtombs", (uintptr_t)&wcsnrtombs_fake },
  { "setlocale", (uintptr_t)&setlocale }, { "localeconv", (uintptr_t)&localeconv },
  { "newlocale", (uintptr_t)&newlocale_fake }, { "freelocale", (uintptr_t)&freelocale_fake },
  { "uselocale", (uintptr_t)&uselocale_fake },
  { "iswalpha_l", (uintptr_t)&iswalpha_l_fake }, { "iswblank_l", (uintptr_t)&iswblank_l_fake },
  { "iswcntrl_l", (uintptr_t)&iswcntrl_l_fake }, { "iswdigit_l", (uintptr_t)&iswdigit_l_fake },
  { "iswlower_l", (uintptr_t)&iswlower_l_fake }, { "iswprint_l", (uintptr_t)&iswprint_l_fake },
  { "iswpunct_l", (uintptr_t)&iswpunct_l_fake }, { "iswspace_l", (uintptr_t)&iswspace_l_fake },
  { "iswupper_l", (uintptr_t)&iswupper_l_fake }, { "iswxdigit_l", (uintptr_t)&iswxdigit_l_fake },
  { "towlower_l", (uintptr_t)&towlower_l_fake }, { "towupper_l", (uintptr_t)&towupper_l_fake },
  { "strcoll_l", (uintptr_t)&strcoll_l_fake }, { "strxfrm_l", (uintptr_t)&strxfrm_l_fake },
  { "strftime_l", (uintptr_t)&strftime_l_fake }, { "strtold_l", (uintptr_t)&strtold_l_fake },
  { "strtoll_l", (uintptr_t)&strtoll_l_fake }, { "strtoull_l", (uintptr_t)&strtoull_l_fake },
  { "wcscoll_l", (uintptr_t)&wcscoll_l_fake }, { "wcsxfrm_l", (uintptr_t)&wcsxfrm_l_fake },

  /* Formatted I/O */
  { "printf", (uintptr_t)&guest_printf_noop }, { "puts", (uintptr_t)&guest_puts_noop },
  { "snprintf", (uintptr_t)&snprintf }, { "sprintf", (uintptr_t)&sprintf },
  { "swprintf", (uintptr_t)&swprintf }, { "vsnprintf", (uintptr_t)&vsnprintf },
  { "vasprintf", (uintptr_t)&vasprintf },
  { "sscanf", (uintptr_t)&sscanf }, { "vsscanf", (uintptr_t)&vsscanf },

  /* Math */
  { "acosf", (uintptr_t)&acosf }, { "asinf", (uintptr_t)&asinf },
  { "atan2f", (uintptr_t)&atan2f }, { "cosf", (uintptr_t)&cosf },
  { "sinf", (uintptr_t)&sinf }, { "tanf", (uintptr_t)&tanf },
  { "expf", (uintptr_t)&expf }, { "logf", (uintptr_t)&logf },
  { "powf", (uintptr_t)&powf }, { "pow", (uintptr_t)&pow },
  { "fmodf", (uintptr_t)&fmodf }, { "sincosf", (uintptr_t)&sincosf_fake },

  /* Time */
  { "clock_gettime", (uintptr_t)&clock_gettime_fake },
  { "clock_getres", (uintptr_t)&clock_getres_fake },
  { "gettimeofday", (uintptr_t)&gettimeofday },
  { "gmtime", (uintptr_t)&gmtime }, { "gmtime_r", (uintptr_t)&gmtime_r },
  { "localtime", (uintptr_t)&localtime }, { "localtime_r", (uintptr_t)&localtime_r },
  { "mktime", (uintptr_t)&mktime }, { "time", (uintptr_t)&time },
  { "timegm", (uintptr_t)&timegm_fake },
  { "nanosleep", (uintptr_t)&nanosleep }, { "usleep", (uintptr_t)&usleep },
  { "getenv", (uintptr_t)&getenv_fake },

  /* Standard I/O */
  { "__sF", (uintptr_t)&fake_sF },
  { "fopen", (uintptr_t)&fopen_fake }, { "fclose", (uintptr_t)&fclose_fake },
  { "fread", (uintptr_t)&fread_fake }, { "fwrite", (uintptr_t)&fwrite_fake },
  { "fseek", (uintptr_t)&fseek_fake }, { "fseeko", (uintptr_t)&fseeko },
  { "ftell", (uintptr_t)&ftell_fake }, { "ftello", (uintptr_t)&ftello },
  { "fflush", (uintptr_t)&fflush_fake }, { "fprintf", (uintptr_t)&fprintf_fake },
  { "vfprintf", (uintptr_t)&vfprintf_fake }, { "fputc", (uintptr_t)&fputc_fake },
  { "fputs", (uintptr_t)&fputs_fake }, { "fgets", (uintptr_t)&fgets_fake },
  { "feof", (uintptr_t)&feof_fake }, { "ferror", (uintptr_t)&ferror_fake },
  { "fileno", (uintptr_t)&fileno_fake }, { "remove", (uintptr_t)&remove_fake },
  { "clearerr", (uintptr_t)&clearerr_fake }, { "fscanf", (uintptr_t)&fscanf_fake },
  { "rewind", (uintptr_t)&rewind_fake }, { "freopen", (uintptr_t)&freopen_fake },
  { "ungetc", (uintptr_t)&ungetc_fake }, { "setvbuf", (uintptr_t)&setvbuf_fake },
  { "setbuf", (uintptr_t)&setbuf_fake }, { "fputwc", (uintptr_t)&fputwc_fake },
  { "getwc", (uintptr_t)&getwc_fake }, { "ungetwc", (uintptr_t)&ungetwc_fake },
  { "rename", (uintptr_t)&rename_fake },
  { "fchown", (uintptr_t)&fchown_stub },

  /* Filesystem */
  { "open", (uintptr_t)&open_fake },
  { "close", (uintptr_t)&close_fake }, { "read", (uintptr_t)&read_fake },
  { "write", (uintptr_t)&write_fake },
  { "lseek", (uintptr_t)&z_lseek }, { "pipe", (uintptr_t)&pipe_fake },
  { "poll", (uintptr_t)&poll_fake }, { "select", (uintptr_t)&select_fake },
  { "dup", (uintptr_t)&dup_fake }, { "dup2", (uintptr_t)&dup2_fake },
  { "fcntl", (uintptr_t)&fcntl_shim },
  { "ioctl", (uintptr_t)&ioctl_fake }, { "isatty", (uintptr_t)&isatty },
  { "stat", (uintptr_t)&stat_fake }, { "fstat", (uintptr_t)&fstat_fake },
  { "lstat", (uintptr_t)&lstat_fake }, { "statfs", (uintptr_t)&statfs_fake },
  { "access", (uintptr_t)&access_fake },
  { "mkdir", (uintptr_t)&mkdir_fake }, { "rmdir", (uintptr_t)&rmdir_fake },
  { "unlink", (uintptr_t)&unlink_fake }, { "getcwd", (uintptr_t)&getcwd_fake },
  { "chmod", (uintptr_t)&chmod_stub }, { "fchmod", (uintptr_t)&fchmod_stub },
  { "truncate", (uintptr_t)&truncate_fake },
  { "ftruncate", (uintptr_t)&nx_ftruncate }, { "fsync", (uintptr_t)&nx_fsync },
  { "flock", (uintptr_t)&flock_fake }, { "futimens", (uintptr_t)&futimens_fake },
  { "link", (uintptr_t)&link_stub }, { "symlink", (uintptr_t)&symlink_stub },
  { "readlink", (uintptr_t)&readlink_stub }, { "utime", (uintptr_t)&utime_fake },
  { "utimes", (uintptr_t)&utimes_fake },
  { "sendfile", (uintptr_t)&sendfile_fake },
  { "opendir", (uintptr_t)&opendir_fake }, { "closedir", (uintptr_t)&closedir_fake },
  { "readdir", (uintptr_t)&readdir_fake },
  { "realpath", (uintptr_t)&realpath_fake },
  { "strerror", (uintptr_t)&strerror_fake }, { "strerror_r", (uintptr_t)&strerror_r_fake },

  /* Signals */
  { "signal", (uintptr_t)&signal_fake }, { "sigaction", (uintptr_t)&sigaction_fake },
  { "sigaltstack", (uintptr_t)&sigaltstack_fake },
  { "sigaddset", (uintptr_t)&sigaddset_fake }, { "sigemptyset", (uintptr_t)&sigemptyset_fake },
  { "sigdelset", (uintptr_t)&sigdelset_fake }, { "sigfillset", (uintptr_t)&sigfillset_fake },
  { "sigsuspend", (uintptr_t)&sigsuspend_fake },
  { "setjmp", (uintptr_t)&setjmp }, { "longjmp", (uintptr_t)&longjmp },

  /* Process */
  { "getpid", (uintptr_t)&getpid_fake }, { "gettid", (uintptr_t)&gettid_fake },
  { "getuid", (uintptr_t)&ret0_u },
  { "getgid", (uintptr_t)&ret0_u },
  { "geteuid", (uintptr_t)&ret0_u }, { "getegid", (uintptr_t)&ret0_u },
  { "getpwuid", (uintptr_t)&getpwuid_fake }, { "getpwuid_r", (uintptr_t)&getpwuid_r_fake },
  { "kill", (uintptr_t)&kill_fake },
  { "sched_yield", (uintptr_t)&sched_yield_fake },

  /* Dynamic loader */
  { "dlopen", (uintptr_t)&dlopen_fake }, { "dlclose", (uintptr_t)&dlclose_fake },
  { "dlerror", (uintptr_t)&dlerror_fake }, { "dlsym", (uintptr_t)&dlsym_fake },

  /* Networking */
  { "socket", (uintptr_t)&socket_fake }, { "connect", (uintptr_t)&connect_fake },
  { "bind", (uintptr_t)&bind_fake }, { "listen", (uintptr_t)&listen_fake },
  { "accept", (uintptr_t)&accept_fake }, { "send", (uintptr_t)&send_fake },
  { "recv", (uintptr_t)&recv_fake },
  { "recvfrom", (uintptr_t)&recvfrom_fake }, { "shutdown", (uintptr_t)&shutdown_fake },
  { "recvmsg", (uintptr_t)&recvmsg_fake }, { "sendmsg", (uintptr_t)&sendmsg_fake },
  { "setsockopt", (uintptr_t)&setsockopt_fake }, { "getsockopt", (uintptr_t)&getsockopt_fake },
  { "getsockname", (uintptr_t)&getsockname_fake }, { "getpeername", (uintptr_t)&getpeername_fake },
  { "getaddrinfo", (uintptr_t)&getaddrinfo_fake }, { "freeaddrinfo", (uintptr_t)&freeaddrinfo_fake },
  { "getnameinfo", (uintptr_t)&getnameinfo_fake }, { "gethostname", (uintptr_t)&gethostname_fake },
  { "if_nametoindex", (uintptr_t)&if_nametoindex_fake },
  { "gethostbyname", (uintptr_t)&gethostbyname_fake },
  { "gethostbyaddr", (uintptr_t)&gethostbyaddr_fake },
  { "inet_addr", (uintptr_t)&inet_addr },
  { "inet_pton", (uintptr_t)&inet_pton_shim },
  { "inet_ntop", (uintptr_t)&inet_ntop_shim },

  /* SDK plug-ins share the process' libc/zlib networking utilities. */
  { "gai_strerror", (uintptr_t)&gai_strerror },
  { "fnmatch", (uintptr_t)&fnmatch },
  { "mlock", (uintptr_t)&mlock_stub },
  { "sigprocmask", (uintptr_t)&sigprocmask_stub },
  { "tcgetattr", (uintptr_t)&tcgetattr_stub },
  { "tcsetattr", (uintptr_t)&tcsetattr_stub },

  /* Android NDK asset manager backed by the extracted/first-boot packed tree. */
  { "AAssetManager_fromJava", (uintptr_t)&AAssetManager_fromJava },
  { "AAssetManager_open", (uintptr_t)&AAssetManager_open },
  { "AAssetManager_openDir", (uintptr_t)&AAssetManager_openDir },
  { "AAsset_read", (uintptr_t)&AAsset_read },
  { "AAsset_seek", (uintptr_t)&AAsset_seek },
  { "AAsset_seek64", (uintptr_t)&AAsset_seek64 },
  { "AAsset_close", (uintptr_t)&AAsset_close },
  { "AAsset_getBuffer", (uintptr_t)&AAsset_getBuffer },
  { "AAsset_getLength", (uintptr_t)&AAsset_getLength },
  { "AAsset_getLength64", (uintptr_t)&AAsset_getLength64 },
  { "AAsset_getRemainingLength", (uintptr_t)&AAsset_getRemainingLength },
  { "AAsset_getRemainingLength64", (uintptr_t)&AAsset_getRemainingLength64 },
  { "AAsset_openFileDescriptor", (uintptr_t)&AAsset_openFileDescriptor },
  { "AAsset_openFileDescriptor64", (uintptr_t)&AAsset_openFileDescriptor64 },
  { "AAsset_isAllocated", (uintptr_t)&AAsset_isAllocated },
  { "AAssetDir_getNextFileName", (uintptr_t)&AAssetDir_getNextFileName },
  { "AAssetDir_rewind", (uintptr_t)&AAssetDir_rewind },
  { "AAssetDir_close", (uintptr_t)&AAssetDir_close },

  /* Common third-party SDK dependencies (HoYo networking, CRI and GME). */
  { "crc32", (uintptr_t)&crc32 },
  { "deflate", (uintptr_t)&deflate },
  { "deflateEnd", (uintptr_t)&deflateEnd },
  { "deflateInit_", (uintptr_t)&deflateInit_ },
  { "deflateInit2_", (uintptr_t)&deflateInit2_ },
  { "get_crc_table", (uintptr_t)&get_crc_table },
  { "inflate", (uintptr_t)&inflate },
  { "inflateEnd", (uintptr_t)&inflateEnd },
  { "inflateInit_", (uintptr_t)&inflateInit_ },
  { "inflateInit2_", (uintptr_t)&inflateInit2_ },
  { "inflateReset", (uintptr_t)&inflateReset },
  { "zError", (uintptr_t)&zError },
  { "zlibVersion", (uintptr_t)&zlibVersion },
  { "isalnum", (uintptr_t)&isalnum },
  { "isalpha", (uintptr_t)&isalpha },
  { "isgraph", (uintptr_t)&isgraph },
  { "islower", (uintptr_t)&islower },
  { "isprint", (uintptr_t)&isprint },
  { "isupper", (uintptr_t)&isupper },
  { "isxdigit", (uintptr_t)&isxdigit },
  { "tolower", (uintptr_t)&tolower },
  { "toupper", (uintptr_t)&toupper },

  /* Threads */
  { "pthread_create", (uintptr_t)&pthread_create_fake }, { "pthread_join", (uintptr_t)&pthread_join_fake },
  { "pthread_detach", (uintptr_t)&pthread_detach_fake }, { "pthread_exit", (uintptr_t)&pthread_exit_fake },
  { "pthread_self", (uintptr_t)&pthread_self_fake }, { "pthread_kill", (uintptr_t)&pthread_kill_gc },
  { "pthread_key_create", (uintptr_t)&pthread_key_create_fake }, { "pthread_key_delete", (uintptr_t)&pthread_key_delete_fake },
  { "pthread_getspecific", (uintptr_t)&pthread_getspecific_fake }, { "pthread_setspecific", (uintptr_t)&pthread_setspecific_fake },
  { "pthread_once", (uintptr_t)&pthread_once_fake },
  { "pthread_mutex_init", (uintptr_t)&pthread_mutex_init_fake },
  { "pthread_mutex_destroy", (uintptr_t)&pthread_mutex_destroy_fake },
  { "pthread_mutex_lock", (uintptr_t)&pthread_mutex_lock_fake },
  { "pthread_mutex_trylock", (uintptr_t)&pthread_mutex_trylock_fake },
  { "pthread_mutex_unlock", (uintptr_t)&pthread_mutex_unlock_fake },
  { "pthread_mutexattr_init", (uintptr_t)&pthread_mutexattr_init_fake },
  { "pthread_mutexattr_settype", (uintptr_t)&pthread_mutexattr_settype_fake },
  { "pthread_mutexattr_destroy", (uintptr_t)&ret0_i },
  { "pthread_condattr_init", (uintptr_t)&pthread_condattr_init_fake },
  { "pthread_condattr_destroy", (uintptr_t)&pthread_condattr_destroy_fake },
  { "pthread_condattr_setclock", (uintptr_t)&pthread_condattr_setclock_fake },
  { "pthread_cond_init", (uintptr_t)&pthread_cond_init_fake },
  { "pthread_cond_destroy", (uintptr_t)&pthread_cond_destroy_fake },
  { "pthread_cond_broadcast", (uintptr_t)&pthread_cond_broadcast_fake },
  { "pthread_cond_signal", (uintptr_t)&pthread_cond_signal_fake },
  { "pthread_cond_wait", (uintptr_t)&pthread_cond_wait_fake },
  { "pthread_cond_timedwait", (uintptr_t)&pthread_cond_timedwait_fake },
  { "pthread_rwlock_init", (uintptr_t)&pthread_rwlock_init_fake },
  { "pthread_rwlock_destroy", (uintptr_t)&pthread_rwlock_destroy_fake },
  { "pthread_rwlock_rdlock", (uintptr_t)&pthread_rwlock_rdlock_fake },
  { "pthread_rwlock_wrlock", (uintptr_t)&pthread_rwlock_wrlock_fake },
  { "pthread_rwlock_unlock", (uintptr_t)&pthread_rwlock_unlock_fake },
  { "pthread_attr_init", (uintptr_t)&pthread_attr_init_fake },
  { "pthread_attr_destroy", (uintptr_t)&pthread_attr_destroy_fake },
  { "pthread_attr_setdetachstate", (uintptr_t)&pthread_attr_setdetachstate_fake },
  { "pthread_attr_setstacksize", (uintptr_t)&pthread_attr_setstacksize_fake },
  { "pthread_attr_getschedparam", (uintptr_t)&pthread_attr_getschedparam_stub },
  { "pthread_attr_setschedparam", (uintptr_t)&pthread_attr_setschedparam_stub },
  { "pthread_sigmask", (uintptr_t)&pthread_sigmask_fake },
  { "sem_init", (uintptr_t)&sem_init_fake }, { "sem_destroy", (uintptr_t)&sem_destroy_fake },
  { "sem_post", (uintptr_t)&sem_post_fake }, { "sem_wait", (uintptr_t)&sem_wait_fake },
  { "sem_getvalue", (uintptr_t)&sem_getvalue_fake },
  { "sem_timedwait", (uintptr_t)&sem_timedwait_fake },

  /* EGL */
#ifdef VULKAN_ONLY
  /* Unity renders through Vulkan, but CRI performs an Android EGL/GLES
   * capability probe when its optional video plug-ins are loaded.  Never let
   * that probe reach the NVK SDK's intentional hard-failure EGL anchors. */
  { "eglGetDisplay", (uintptr_t)&nx_eglGetDisplay_stub },
  { "eglInitialize", (uintptr_t)&nx_eglInitialize_stub },
  { "eglTerminate", (uintptr_t)&nx_eglTerminate_stub },
  { "eglChooseConfig", (uintptr_t)&nx_eglChooseConfig_stub },
  { "eglGetConfigAttrib", (uintptr_t)&nx_eglGetConfigAttrib_stub },
  { "eglCreateContext", (uintptr_t)&nx_eglCreateContext_stub },
  { "eglCreatePbufferSurface", (uintptr_t)&nx_eglCreatePbufferSurface_stub },
  { "eglCreateWindowSurface", (uintptr_t)&nx_eglCreateWindowSurface_stub },
  { "eglMakeCurrent", (uintptr_t)&nx_eglMakeCurrent_stub },
  { "eglDestroyContext", (uintptr_t)&nx_eglDestroyContext_stub },
  { "eglDestroySurface", (uintptr_t)&nx_eglDestroySurface_stub },
  { "eglGetCurrentContext", (uintptr_t)&nx_eglGetCurrentContext_stub },
  { "eglGetCurrentSurface", (uintptr_t)&nx_eglGetCurrentSurface_stub },
  { "eglGetError", (uintptr_t)&nx_eglGetError_stub },
  { "eglQueryString", (uintptr_t)&nx_eglQueryString_stub },
  { "eglSurfaceAttrib", (uintptr_t)&nx_eglSurfaceAttrib_stub },
  { "eglSwapBuffers", (uintptr_t)&nx_eglSwapBuffers_stub },
  { "eglSwapInterval", (uintptr_t)&nx_eglSwapInterval_stub },
  { "eglGetProcAddress", (uintptr_t)&nx_eglGetProcAddress_stub },
#else
  { "eglGetDisplay", (uintptr_t)&eglGetDisplay }, { "eglInitialize", (uintptr_t)&eglInitialize },
  { "eglTerminate", (uintptr_t)&eglTerminate },
  { "eglCreateContext", (uintptr_t)&eglCreateContext }, { "eglMakeCurrent", (uintptr_t)&eglMakeCurrent },
  { "eglDestroyContext", (uintptr_t)&eglDestroyContext }, { "eglDestroySurface", (uintptr_t)&eglDestroySurface },
  { "eglGetConfigAttrib", (uintptr_t)&eglGetConfigAttrib },
  { "eglCreateWindowSurface", (uintptr_t)&eglCreateWindowSurface },
  { "eglSwapBuffers", (uintptr_t)&eglSwapBuffers },
#endif
  { "eglQuerySurface", (uintptr_t)&egl_QuerySurface_fake },

  /* GLES2, resolved dynamically by Unity. */
#ifndef VULKAN_ONLY
  { "glActiveTexture", (uintptr_t)&glActiveTexture }, { "glAttachShader", (uintptr_t)&glAttachShader },
  { "glBindBuffer", (uintptr_t)&glBindBuffer }, { "glBindFramebuffer", (uintptr_t)&glBindFramebuffer },
  { "glBindRenderbuffer", (uintptr_t)&glBindRenderbuffer }, { "glBindTexture", (uintptr_t)&glBindTexture },
  { "glBlendEquationSeparate", (uintptr_t)&glBlendEquationSeparate },
  { "glBufferData", (uintptr_t)&glBufferData }, { "glClear", (uintptr_t)&glClear },
  { "glClearColor", (uintptr_t)&glClearColor }, { "glClearDepthf", (uintptr_t)&glClearDepthf },
  { "glClearStencil", (uintptr_t)&glClearStencil }, { "glColorMask", (uintptr_t)&glColorMask },
  { "glCompileShader", (uintptr_t)&glCompileShader }, { "glCompressedTexImage2D", (uintptr_t)&glCompressedTexImage2D },
  { "glCreateProgram", (uintptr_t)&glCreateProgram }, { "glCreateShader", (uintptr_t)&glCreateShader },
  { "glCullFace", (uintptr_t)&glCullFace }, { "glDeleteBuffers", (uintptr_t)&glDeleteBuffers },
  { "glDeleteFramebuffers", (uintptr_t)&glDeleteFramebuffers }, { "glDeleteProgram", (uintptr_t)&glDeleteProgram },
  { "glDeleteRenderbuffers", (uintptr_t)&glDeleteRenderbuffers }, { "glDeleteShader", (uintptr_t)&glDeleteShader },
  { "glDeleteTextures", (uintptr_t)&glDeleteTextures }, { "glDepthFunc", (uintptr_t)&glDepthFunc },
  { "glDepthMask", (uintptr_t)&glDepthMask }, { "glDisable", (uintptr_t)&glDisable },
  { "glDisableVertexAttribArray", (uintptr_t)&glDisableVertexAttribArray }, { "glDrawArrays", (uintptr_t)&glDrawArrays },
  { "glDrawElements", (uintptr_t)&glDrawElements }, { "glEnable", (uintptr_t)&glEnable },
  { "glEnableVertexAttribArray", (uintptr_t)&glEnableVertexAttribArray },
  { "glFramebufferRenderbuffer", (uintptr_t)&glFramebufferRenderbuffer },
  { "glFramebufferTexture2D", (uintptr_t)&glFramebufferTexture2D }, { "glGenBuffers", (uintptr_t)&glGenBuffers },
  { "glGenFramebuffers", (uintptr_t)&glGenFramebuffers }, { "glGenRenderbuffers", (uintptr_t)&glGenRenderbuffers },
  { "glGenTextures", (uintptr_t)&glGenTextures }, { "glGetAttribLocation", (uintptr_t)&glGetAttribLocation },
  { "glGetError", (uintptr_t)&glGetError }, { "glGetProgramiv", (uintptr_t)&glGetProgramiv },
  { "glGetIntegerv", (uintptr_t)&glGetIntegerv }, { "glGetString", (uintptr_t)&glGetString },
  { "glGetShaderiv", (uintptr_t)&glGetShaderiv }, { "glGetUniformLocation", (uintptr_t)&glGetUniformLocation },
  { "glLinkProgram", (uintptr_t)&glLinkProgram }, { "glPixelStorei", (uintptr_t)&glPixelStorei },
  { "glPolygonOffset", (uintptr_t)&glPolygonOffset }, { "glReadPixels", (uintptr_t)&glReadPixels },
  { "glRenderbufferStorage", (uintptr_t)&glRenderbufferStorage }, { "glScissor", (uintptr_t)&glScissor },
  { "glShaderSource", (uintptr_t)&glShaderSource },
  { "glStencilMask", (uintptr_t)&glStencilMask },
  { "glTexImage2D", (uintptr_t)&glTexImage2D }, { "glTexParameterf", (uintptr_t)&glTexParameterf },
  { "glTexParameteri", (uintptr_t)&glTexParameteri },
  { "glTexSubImage2D", (uintptr_t)&glTexSubImage2D }, { "glUniform1fv", (uintptr_t)&glUniform1fv },
  { "glUniform1i", (uintptr_t)&glUniform1i }, { "glUniform2fv", (uintptr_t)&glUniform2fv },
  { "glUniform3fv", (uintptr_t)&glUniform3fv }, { "glUniform4fv", (uintptr_t)&glUniform4fv },
  { "glUniformMatrix4fv", (uintptr_t)&glUniformMatrix4fv }, { "glUseProgram", (uintptr_t)&glUseProgram },
  { "glVertexAttribPointer", (uintptr_t)&glVertexAttribPointer }, { "glViewport", (uintptr_t)&glViewport },
#else
  { "glActiveTexture", (uintptr_t)&nx_glActiveTexture_stub },
  { "glBindBuffer", (uintptr_t)&nx_glBindBuffer_stub },
  { "glBindTexture", (uintptr_t)&nx_glBindTexture_stub },
  { "glBufferData", (uintptr_t)&nx_glBufferData_stub },
  { "glCompressedTexImage2D", (uintptr_t)&nx_glCompressedTexImage2D_stub },
  { "glDeleteBuffers", (uintptr_t)&nx_glDeleteBuffers_stub },
  { "glDeleteTextures", (uintptr_t)&nx_glDeleteTextures_stub },
  { "glGenBuffers", (uintptr_t)&nx_glGenBuffers_stub },
  { "glGenTextures", (uintptr_t)&nx_glGenTextures_stub },
  { "glGetError", (uintptr_t)&nx_glGetError_stub },
  { "glGetIntegerv", (uintptr_t)&nx_glGetIntegerv_stub },
  { "glGetShaderPrecisionFormat", (uintptr_t)&nx_glGetShaderPrecisionFormat_stub },
  { "glGetString", (uintptr_t)&nx_glGetString_stub },
  { "glTexImage2D", (uintptr_t)&nx_glTexImage2D_stub },
  { "glTexImage2DMultisample", (uintptr_t)&nx_glTexImage2DMultisample_stub },
  { "glTexStorage2DMultisample", (uintptr_t)&nx_glTexStorage2DMultisample_stub },
  { "glTexParameterf", (uintptr_t)&nx_glTexParameterf_stub },
  { "glTexParameteri", (uintptr_t)&nx_glTexParameteri_stub },
  { "glTexSubImage2D", (uintptr_t)&nx_glTexSubImage2D_stub },
  { "glMapBufferOES", (uintptr_t)&nx_glMapBufferOES_stub },
  { "glUnmapBufferOES", (uintptr_t)&nx_glUnmapBufferOES_stub },
  { "glMapBufferRange", (uintptr_t)&nx_glMapBufferRange_stub },
  { "glUnmapBuffer", (uintptr_t)&nx_glUnmapBuffer_stub },
#endif

  /* Android runtime */
  { "ALooper_prepare", (uintptr_t)&ALooper_prepare },
  { "ALooper_pollOnce", (uintptr_t)&ALooper_pollOnce },
  { "ANativeWindow_setBuffersGeometry", (uintptr_t)&ANativeWindow_setBuffersGeometry },
  { "ASensorEventQueue_enableSensor", (uintptr_t)&ASensorEventQueue_enableSensor },
  { "ASensorEventQueue_disableSensor", (uintptr_t)&ASensorEventQueue_disableSensor },
  { "ASensorEventQueue_setEventRate", (uintptr_t)&ASensorEventQueue_setEventRate },
  { "ASensorEventQueue_getEvents", (uintptr_t)&ASensorEventQueue_getEvents },

  /* CRIWARE imports the Android OpenSL ABI from its native plug-in. */
  { "slCreateEngine", (uintptr_t)&slCreateEngine },
  #define SL_IID(n) { "SL_IID_" #n, (uintptr_t)&SL_IID_##n }
  SL_IID(3DCOMMIT), SL_IID(3DDOPPLER), SL_IID(3DGROUPING), SL_IID(3DLOCATION),
  SL_IID(3DMACROSCOPIC), SL_IID(3DSOURCE), SL_IID(ANDROIDCONFIGURATION),
  SL_IID(ANDROIDACOUSTICECHOCANCELLATION), SL_IID(ANDROIDAUTOMATICGAINCONTROL),
  SL_IID(ANDROIDNOISESUPPRESSION),
  SL_IID(ANDROIDEFFECT), SL_IID(ANDROIDEFFECTCAPABILITIES), SL_IID(ANDROIDEFFECTSEND),
  SL_IID(ANDROIDSIMPLEBUFFERQUEUE), SL_IID(AUDIODECODERCAPABILITIES), SL_IID(AUDIOENCODER),
  SL_IID(AUDIOENCODERCAPABILITIES), SL_IID(AUDIOIODEVICECAPABILITIES), SL_IID(BASSBOOST),
  SL_IID(BUFFERQUEUE), SL_IID(DEVICEVOLUME), SL_IID(DYNAMICINTERFACEMANAGEMENT),
  SL_IID(DYNAMICSOURCE), SL_IID(EFFECTSEND), SL_IID(ENGINE), SL_IID(ENGINECAPABILITIES),
  SL_IID(ENVIRONMENTALREVERB), SL_IID(EQUALIZER), SL_IID(LED), SL_IID(METADATAEXTRACTION),
  SL_IID(METADATATRAVERSAL), SL_IID(MIDIMESSAGE), SL_IID(MIDIMUTESOLO), SL_IID(MIDITEMPO),
  SL_IID(MIDITIME), SL_IID(MUTESOLO), SL_IID(NULL), SL_IID(OBJECT), SL_IID(OUTPUTMIX),
  SL_IID(PITCH), SL_IID(PLAY), SL_IID(PLAYBACKRATE), SL_IID(PREFETCHSTATUS),
  SL_IID(PRESETREVERB), SL_IID(RATEPITCH), SL_IID(RECORD), SL_IID(SEEK), SL_IID(THREADSYNC),
  SL_IID(VIBRA), SL_IID(VIRTUALIZER), SL_IID(VISUALIZATION), SL_IID(VOLUME),
  #undef SL_IID
};

size_t dynlib_numfunctions = sizeof(dynlib_functions) / sizeof(*dynlib_functions);

static char  *strcasestr_shim(const char*h,const char*n){
  if(!n||!*n) return (char*)h;
  for(; *h; h++){ const char*a=h,*b=n;
    while(*a && *b && tolower((unsigned char)*a)==tolower((unsigned char)*b)){a++;b++;}
    if(!*b) return (char*)h; }
  return NULL;
}
static DynLibFunction supplemental_functions[] = {
  { "strcasestr", (uintptr_t)&strcasestr_shim },

  /* Bionic fortify/runtime data used by the merged Unity 2017 player. */
  { "__assert2", (uintptr_t)&nx_assert2 },
  { "__cxa_thread_atexit_impl", (uintptr_t)&nx_cxa_thread_atexit },
  { "__fgets_chk", (uintptr_t)&nx_fgets_chk },
  { "__memcpy_chk", (uintptr_t)&nx_memcpy_chk },
  { "__memset_chk", (uintptr_t)&nx_memset_chk },
  { "__open_2", (uintptr_t)&nx_open_2 },
  { "__read_chk", (uintptr_t)&nx_read_chk },
  { "__register_atfork", (uintptr_t)&nx_register_atfork },
  { "__stack_chk_guard", (uintptr_t)&nx_stack_chk_guard },
  { "__strcat_chk", (uintptr_t)&nx_strcat_chk },
  { "__strchr_chk", (uintptr_t)&nx_strchr_chk },
  { "__strcpy_chk", (uintptr_t)&nx_strcpy_chk },
  { "__strncpy_chk2", (uintptr_t)&nx_strncpy_chk2 },
  { "__strrchr_chk", (uintptr_t)&nx_strrchr_chk },
  { "__strncat_chk", (uintptr_t)&nx_strncat_chk },
  { "__vsprintf_chk", (uintptr_t)&nx_vsprintf_chk },
  { "__FD_CLR_chk", (uintptr_t)&nx_fd_clr_chk },
  { "__gnu_strerror_r", (uintptr_t)&nx_gnu_strerror_r },
  { "__umask_chk", (uintptr_t)&nx_umask_chk },
  { "__android_log_buf_write", (uintptr_t)&nx_android_log_buf_write },
  { "__android_log_assert", (uintptr_t)&nx_android_log_assert },
  { "__cmsg_nxthdr", (uintptr_t)&nx_cmsg_nxthdr },
  { "__libc_init", (uintptr_t)&nx_libc_init },
  { "environ", (uintptr_t)&nx_environ_ptr },
  { "stdin", (uintptr_t)&nx_stdin_ptr },
  { "stdout", (uintptr_t)&nx_stdout_ptr },
  { "stderr", (uintptr_t)&nx_stderr_ptr },
  { "daylight", (uintptr_t)&nx_daylight_data },
  { "timezone", (uintptr_t)&nx_timezone_data },
  { "tzname", (uintptr_t)&nx_tzname_data },
  { "optarg", (uintptr_t)&optarg },
  { "optind", (uintptr_t)&optind },

  /* Linux descriptor/event APIs used by libcurl, telemetry and IL2CPP. */
  { "epoll_create", (uintptr_t)&nx_epoll_create },
  { "epoll_create1", (uintptr_t)&nx_epoll_create1 },
  { "epoll_ctl", (uintptr_t)&nx_epoll_ctl },
  { "epoll_wait", (uintptr_t)&nx_epoll_wait },
  { "inotify_init1", (uintptr_t)&nx_inotify_init1 },
  { "inotify_add_watch", (uintptr_t)&nx_inotify_add_watch },
  { "signalfd", (uintptr_t)&nx_signalfd },
  { "eventfd", (uintptr_t)&nx_eventfd },
  { "sendto", (uintptr_t)&sendto_fake },
  { "socketpair", (uintptr_t)&socketpair_fake },

  /* Entropy is security-critical for TLS and account authentication. */
  { "arc4random", (uintptr_t)&nx_arc4random },
  { "arc4random_buf", (uintptr_t)&nx_arc4random_buf },
  { "getentropy", (uintptr_t)&nx_getentropy },
  { "getrandom", (uintptr_t)&getrandom_fake },

  /* Process-only Android facilities fail predictably on Horizon. */
  { "fork", (uintptr_t)&nx_fork }, { "execl", (uintptr_t)&nx_execl },
  { "execv", (uintptr_t)&nx_execv }, { "execve", (uintptr_t)&nx_execve },
  { "clone", (uintptr_t)&nx_clone },
  { "waitpid", (uintptr_t)&nx_waitpid }, { "getppid", (uintptr_t)&nx_getppid },
  { "popen", (uintptr_t)&popen_unsupported },
  { "pclose", (uintptr_t)&pclose_unsupported },
  { "system", (uintptr_t)&system_unsupported }, { "alarm", (uintptr_t)&alarm_fake },
  { "_exit", (uintptr_t)&exit },

  /* File, VM, resource, and pthread compatibility. */
  { "fstat64", (uintptr_t)&nx_fstat64 }, { "stat64", (uintptr_t)&stat_fake },
  { "lstat64", (uintptr_t)&lstat_fake },
  { "pread", (uintptr_t)&nx_pread }, { "pread64", (uintptr_t)&nx_pread },
  { "pwrite", (uintptr_t)&nx_pwrite }, { "writev", (uintptr_t)&nx_writev },
  { "fdatasync", (uintptr_t)&nx_fdatasync }, { "pipe2", (uintptr_t)&nx_pipe2 },
  { "getrlimit", (uintptr_t)&nx_getrlimit }, { "getrusage", (uintptr_t)&nx_getrusage },
  { "sysinfo", (uintptr_t)&nx_sysinfo }, { "pathconf", (uintptr_t)&nx_pathconf },
  { "mincore", (uintptr_t)&nx_mincore }, { "msync", (uintptr_t)&nx_msync },
  { "pthread_attr_getguardsize", (uintptr_t)&nx_pthread_attr_getguardsize },
  { "pthread_attr_setschedpolicy", (uintptr_t)&nx_pthread_attr_setschedpolicy },
  { "pthread_getschedparam", (uintptr_t)&nx_pthread_getschedparam },
  { "pthread_mutex_timedlock", (uintptr_t)&pthread_mutex_timedlock_fake },
  { "pthread_rwlock_destroy", (uintptr_t)&pthread_rwlock_destroy_fake },
  { "pthread_setschedparam", (uintptr_t)&nx_pthread_setschedparam },
  { "sched_get_priority_min", (uintptr_t)&nx_sched_get_priority_min },
  { "sched_get_priority_max", (uintptr_t)&nx_sched_get_priority_max },
  { "sched_getparam", (uintptr_t)&nx_sched_getparam },
  { "sched_getscheduler", (uintptr_t)&nx_sched_getscheduler },
  { "ALooper_addFd", (uintptr_t)&ALooper_addFd },
  { "ALooper_removeFd", (uintptr_t)&ALooper_removeFd },
  { "sigsetjmp", (uintptr_t)&nx_sigsetjmp }, { "siglongjmp", (uintptr_t)&nx_siglongjmp },

  /* Newlib-backed ISO/POSIX functions absent from the original table. */
  { "atof", (uintptr_t)&atof }, { "atoll", (uintptr_t)&atoll },
  { "cosh", (uintptr_t)&cosh }, { "exp2", (uintptr_t)&exp2 },
  { "feclearexcept", (uintptr_t)&feclearexcept }, { "fetestexcept", (uintptr_t)&fetestexcept },
  { "fputwc", (uintptr_t)&fputwc_fake }, { "freopen", (uintptr_t)&freopen_fake },
  { "fgetc", (uintptr_t)&fgetc_fake }, { "vfscanf", (uintptr_t)&vfscanf_fake },
  { "frexp", (uintptr_t)&frexp }, { "frexpf", (uintptr_t)&frexpf },
  { "getc", (uintptr_t)&fgetc_fake }, { "getwc", (uintptr_t)&getwc_fake },
  { "getopt_long", (uintptr_t)&getopt_long },
  { "inet_aton", (uintptr_t)&inet_aton }, { "inet_ntoa", (uintptr_t)&inet_ntoa },
  { "isspace", (uintptr_t)&isspace }, { "mallinfo", (uintptr_t)&mallinfo },
  { "malloc_usable_size", (uintptr_t)&nx_guest_malloc_usable_size },
  { "perror", (uintptr_t)&perror }, { "putchar", (uintptr_t)&putchar },
  { "rand", (uintptr_t)&rand }, { "random", (uintptr_t)&random },
  { "remainderf", (uintptr_t)&remainderf },
  { "rewind", (uintptr_t)&rewind_fake }, { "sincos", (uintptr_t)&sincos },
  { "sinh", (uintptr_t)&sinh }, { "sleep", (uintptr_t)&sleep },
  { "sqrt", (uintptr_t)&sqrt }, { "srand", (uintptr_t)&srand },
  { "scandir", (uintptr_t)&scandir_fake }, { "alphasort", (uintptr_t)&alphasort_fake },
  { "mkstemp", (uintptr_t)&mkstemp_fake }, { "sem_trywait", (uintptr_t)&sem_trywait_fake },
  { "strcoll", (uintptr_t)&strcoll }, { "strncasecmp", (uintptr_t)&strncasecmp },
  { "strncat", (uintptr_t)&strncat }, { "strpbrk", (uintptr_t)&strpbrk },
  { "strsep", (uintptr_t)&strsep }, { "strtok", (uintptr_t)&strtok },
  { "tanh", (uintptr_t)&tanh }, { "tanhf", (uintptr_t)&tanhf },
  { "iswctype", (uintptr_t)&iswctype }, { "wctype", (uintptr_t)&wctype },
  { "towupper", (uintptr_t)&towupper }, { "wcscoll", (uintptr_t)&wcscoll },
  { "wcsftime", (uintptr_t)&wcsftime }, { "wcsxfrm", (uintptr_t)&wcsxfrm },
  { "strxfrm", (uintptr_t)&strxfrm },
  { "regcomp", (uintptr_t)&nx_regcomp }, { "regexec", (uintptr_t)&nx_regexec },
  { "regfree", (uintptr_t)&nx_regfree },
  { "tmpfile", (uintptr_t)&tmpfile },
  { "tmpnam", (uintptr_t)&tmpnam }, { "tzset", (uintptr_t)&tzset },
  { "ungetc", (uintptr_t)&ungetc_fake }, { "ungetwc", (uintptr_t)&ungetwc_fake },
  { "vsprintf", (uintptr_t)&vsprintf },
};
static const size_t supplemental_numfunctions = sizeof(supplemental_functions)/sizeof(*supplemental_functions);

/* Shared by relocations and dlsym. */
static DynLibFunction *g_combined = NULL;
static int g_combined_n = 0;
static Mutex g_combined_lock;
static CondVar g_combined_cond;
enum {
  COMBINED_UNINITIALIZED = 0,
  COMBINED_BUILDING,
  COMBINED_READY,
  COMBINED_FAILED,
};
static int g_combined_state;

static int build_combined(void) {
  mutexLock(&g_combined_lock);
  while (g_combined_state == COMBINED_BUILDING) {
    const Result wait_result = condvarWait(&g_combined_cond,
                                           &g_combined_lock);
    if (R_FAILED(wait_result)) {
      mutexUnlock(&g_combined_lock);
      return 0;
    }
  }
  if (g_combined_state == COMBINED_READY) {
    mutexUnlock(&g_combined_lock);
    return 1;
  }
  if (g_combined_state == COMBINED_FAILED) {
    mutexUnlock(&g_combined_lock);
    return 0;
  }
  g_combined_state = COMBINED_BUILDING;
  mutexUnlock(&g_combined_lock);

  size_t total = dynlib_numfunctions;
  int valid = unity_dynlib_numfunctions >= 0 &&
              total <= SIZE_MAX - (size_t)unity_dynlib_numfunctions;
  if (valid) total += (size_t)unity_dynlib_numfunctions;
  if (valid) valid = total <= SIZE_MAX - supplemental_numfunctions;
  if (valid) total += supplemental_numfunctions;
  if (valid) valid = total <= INT_MAX &&
                     total <= SIZE_MAX / sizeof(DynLibFunction);

  DynLibFunction *combined = valid
    ? malloc(total * sizeof(DynLibFunction)) : NULL;
  if (!combined) valid = 0;
  size_t off = 0;
  if (valid) {
    memcpy(combined + off, dynlib_functions,
           dynlib_numfunctions * sizeof(DynLibFunction));
    off += dynlib_numfunctions;
    memcpy(combined + off, unity_dynlib_functions,
           (size_t)unity_dynlib_numfunctions * sizeof(DynLibFunction));
    off += (size_t)unity_dynlib_numfunctions;
    memcpy(combined + off, supplemental_functions,
           supplemental_numfunctions * sizeof(DynLibFunction));
  }

  mutexLock(&g_combined_lock);
  if (valid) {
    g_combined = combined;
    g_combined_n = (int)total;
    g_combined_state = COMBINED_READY;
  } else {
    free(combined);
    g_combined_state = COMBINED_FAILED;
  }
  (void)condvarWakeAll(&g_combined_cond);
  mutexUnlock(&g_combined_lock);
  return valid;
}

uintptr_t dynlib_find_export(const char *name) {
  if (!name) return 0;
  if (!build_combined()) return 0;
  for (int i = 0; i < g_combined_n; i++)
    if (strcmp(name, g_combined[i].symbol) == 0)
      return g_combined[i].func;
  return 0;
}

void resolve_module_imports(so_module *mod) {
  so_relocate(mod);
  if (!build_combined())
    fatal_error("Could not allocate the guest import resolver table.");
  so_resolve(mod, g_combined, g_combined_n);
}
