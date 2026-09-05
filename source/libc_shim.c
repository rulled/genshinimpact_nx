/* Bionic-to-newlib compatibility wrappers for the Android Unity modules.
 * Socket, file, memory and synchronization ABIs are translated here.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#define _GNU_SOURCE

#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <stdarg.h>
#include <string.h>
#include <math.h>
#include <errno.h>
#include <limits.h>
#include <ctype.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <malloc.h>
#include <wchar.h>
#include <wctype.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/socket.h>
#include <netdb.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <utime.h>
#include <pthread.h>
#include <switch.h>
#include <EGL/egl.h>

#include "config.h"
#include "error.h"
#include "imports.h"
#include "so_util.h"
#include "libc_shim.h"
#include "android_native_unity.h"
#include "vulkan_bridge.h"
#include "genshin_compat.h"
#include "asset_pack.h"
#include "device_profile.h"
#include "plugin_loader.h"
#include "unity_entrypoints.h"
#include "android_log_sink.h"
#include "memory_broker.h"

/* devkitA64's newlib hides these two declarations unless the optional POSIX
 * CPU-time feature macros are enabled, but its clockid_t ABI reserves 2/3. */
#ifndef CLOCK_PROCESS_CPUTIME_ID
#define CLOCK_PROCESS_CPUTIME_ID ((clockid_t)2)
#endif
#ifndef CLOCK_THREAD_CPUTIME_ID
#define CLOCK_THREAD_CPUTIME_ID ((clockid_t)3)
#endif

/* Fortify wrappers ignore the object-size argument. */
void *__memmove_chk_fake(void *dst, const void *src, size_t n, size_t dstlen) { (void)dstlen; return memmove(dst, src, n); }
size_t __strlen_chk_fake(const char *s, size_t slen) { (void)slen; return strlen(s); }
int __vsnprintf_chk_fake(char *s, size_t maxlen, int flag, size_t slen, const char *fmt, va_list va) { (void)flag; (void)slen; return vsnprintf(s, maxlen, fmt, va); }
void  __FD_SET_chk_fake(int fd, void *set, size_t setlen) {
  fd = fakefd_select_bit(fd);
  if (!set || fd < 0 || (size_t)fd / 8u >= setlen) abort();
  ((unsigned long *)set)[(unsigned)fd / (8u * sizeof(unsigned long))] |=
    1ul << ((unsigned)fd % (8u * sizeof(unsigned long)));
}
int   __FD_ISSET_chk_fake(int fd, const void *set, size_t setlen) {
  fd = fakefd_select_bit(fd);
  if (!set || fd < 0 || (size_t)fd / 8u >= setlen) abort();
  return (int)((((const unsigned long *)set)
    [(unsigned)fd / (8u * sizeof(unsigned long))] >>
    ((unsigned)fd % (8u * sizeof(unsigned long)))) & 1ul);
}

/* Android system properties queried by Unity.  Bionic keeps prop_info opaque;
 * callers obtain a stable pointer from __system_property_find and pass it back
 * to __system_property_read.  Fixed-size storage enforces the legacy public
 * PROP_NAME_MAX/PROP_VALUE_MAX ABI without allocating during early startup. */
enum {
  BIONIC_PROP_NAME_MAX = 32,
  BIONIC_PROP_VALUE_MAX = 92,
};

struct FakePropInfo {
  char name[BIONIC_PROP_NAME_MAX];
  char value[BIONIC_PROP_VALUE_MAX];
};
_Static_assert(sizeof(FakePropInfo) == 124,
               "bounded synthetic property record size");

static FakePropInfo system_properties[] = {
  { "ro.build.version.sdk", "33" },
  { "ro.build.version.release", "13" },
  { "ro.build.version.codename", "REL" },
  { "ro.build.id", "REL" },
  { "ro.build.display.id", "REL" },
  { "ro.build.host", "localhost" },
  { "ro.build.user", "nx" },
  { "ro.product.cpu.abi", "arm64-v8a" },
  { "ro.product.cpu.abilist", "arm64-v8a" },
  { "ro.product.cpu.abilist64", "arm64-v8a" },
  { "ro.product.cpu.abi2", "" },
  { "ro.product.model", "Switch" },
  { "ro.product.marketname", "Nintendo Switch" },
  { "ro.product.manufacturer", "Nintendo" },
  { "ro.product.brand", "Nintendo" },
  { "ro.product.name", "Switch" },
  { "ro.product.device", "Switch" },
  { "ro.product.board", "nx" },
  { "ro.hardware", "nx" },
  { "ro.board.platform", "nx" },
  { "ro.build.fingerprint", "Nintendo/Switch/Switch:13/REL/10007:user/release-keys" },
  { "ro.build.version.incremental", "10007" },
  { "ro.build.version.security_patch", "2023-01-01" },
  { "ro.build.characteristics", "default" },
  { "ro.build.type", "user" },
  { "ro.build.tags", "release-keys" },
  { "ro.debuggable", "0" },
  { "ro.secure", "1" },
  { "ro.kernel.qemu", "0" },
  { "ro.opengles.version", "196610" }, /* GLES 3.2 */
  { "dalvik.vm.heapsize", "512m" },
  { "persist.sys.timezone", "UTC" },
  { "persist.sys.device_name", "Nintendo Switch" },
};
static const FakePropInfo *find_system_property(const char *name) {
  if (!name) return NULL;
  for (size_t i = 0; i < sizeof(system_properties) / sizeof(system_properties[0]); ++i)
    if (!strcmp(system_properties[i].name, name)) return &system_properties[i];
  return NULL;
}

static int is_system_property_info(const FakePropInfo *info) {
  if (!info) return 0;
  for (size_t i = 0; i < sizeof(system_properties) / sizeof(system_properties[0]); ++i)
    if (info == &system_properties[i]) return 1;
  return 0;
}

static void copy_system_property_text(char *destination, const char *source,
                                      size_t capacity) {
  if (!destination || !capacity) return;
  const size_t length = strnlen(source, capacity - 1);
  memcpy(destination, source, length);
  destination[length] = '\0';
}

/* Overlay the optional device profile onto the synthetic property table
 * before the guest starts querying it.  The table pointers stay stable
 * (only values are rewritten in place, bounded by PROP_VALUE_MAX), so the
 * __system_property_read_fake identity validation keeps working. */
void libc_shim_apply_device_profile(void) {
  static const struct {
    const char *key;
    const char *prop;
  } map[] = {
    { "model", "ro.product.model" },
    { "device_name", "ro.product.marketname" },
    { "device_name", "persist.sys.device_name" },
    { "manufacturer", "ro.product.manufacturer" },
    { "brand", "ro.product.brand" },
    { "product", "ro.product.name" },
    { "device", "ro.product.device" },
    { "board", "ro.product.board" },
    { "hardware", "ro.hardware" },
    { "platform", "ro.board.platform" },
    { "fingerprint", "ro.build.fingerprint" },
    { "build_id", "ro.build.id" },
    { "display_id", "ro.build.display.id" },
    { "build_host", "ro.build.host" },
    { "build_user", "ro.build.user" },
    { "characteristics", "ro.build.characteristics" },
    { "version_release", "ro.build.version.release" },
    { "version_sdk", "ro.build.version.sdk" },
    { "security_patch", "ro.build.version.security_patch" },
    { "incremental", "ro.build.version.incremental" },
  };
  for (size_t m = 0; m < sizeof map / sizeof map[0]; ++m) {
    const char *value = device_profile_get(map[m].key);
    if (!value) continue;
    for (size_t i = 0;
         i < sizeof system_properties / sizeof system_properties[0]; ++i) {
      if (strcmp(system_properties[i].name, map[m].prop)) continue;
      copy_system_property_text(system_properties[i].value, value,
                                BIONIC_PROP_VALUE_MAX);
      break;
    }
  }
}

const FakePropInfo *__system_property_find_fake(const char *name) {
  /* The table never mutates, so lookups remain reentrant even when diagnostic
   * or crash-reporting code asks for a property during another query. */
  return find_system_property(name);
}

int __system_property_read_fake(const FakePropInfo *info, char *name,
                                char *value) {
  int length = 0;
  if (is_system_property_info(info)) {
    copy_system_property_text(name, info->name, BIONIC_PROP_NAME_MAX);
    copy_system_property_text(value, info->value, BIONIC_PROP_VALUE_MAX);
    length = (int)strnlen(info->value, BIONIC_PROP_VALUE_MAX - 1);
  } else {
    if (name) name[0] = '\0';
    if (value) value[0] = '\0';
  }
  return length;
}

int __system_property_get_fake(const char *name, char *value) {
  if (!value) return 0;
  const FakePropInfo *info = __system_property_find_fake(name);
  if (!info) {
    value[0] = '\0';
    return 0;
  }
  return __system_property_read_fake(info, NULL, value);
}
unsigned long getauxval_fake(unsigned long type) {
  static unsigned char random_bytes[16];
  static int random_ready;
  static const char platform[] = "aarch64";
  static const char execfn[] = "/switch/genshinimpact_nx/genshinimpact_nx.nro";
  switch (type) {
    case 6:  return 0x1000;                         /* AT_PAGESZ */
    case 11: case 12: case 13: case 14: return 0; /* UID/EUID/GID/EGID */
    case 15: return (unsigned long)(uintptr_t)platform; /* AT_PLATFORM */
    case 16: return (1ul << 0) | (1ul << 1) | (1ul << 3) | (1ul << 4) |
                    (1ul << 5) | (1ul << 6) | (1ul << 7); /* FP/ASIMD/crypto/CRC */
    case 17: return 100;                            /* AT_CLKTCK */
    case 23: return 0;                              /* AT_SECURE */
    case 25:
      if (!random_ready) { randomGet(random_bytes, sizeof random_bytes); random_ready = 1; }
      return (unsigned long)(uintptr_t)random_bytes; /* AT_RANDOM */
    case 26: return 0;                              /* AT_HWCAP2 */
    case 31: return (unsigned long)(uintptr_t)execfn; /* AT_EXECFN */
    default: errno = ENOENT; return 0;
  }
}

int gettid_fake(void) {
  u64 tid = 1;
  if (R_SUCCEEDED(svcGetThreadId(&tid, CUR_THREAD_HANDLE)) && tid)
    return (int)(tid & 0x7fffffff);
  return 1;
}

#define ARM64_SYS_IOCTL             29
#define ARM64_SYS_FACCESSAT         48
#define ARM64_SYS_OPENAT            56
#define ARM64_SYS_FSTAT             80
#define ARM64_SYS_FUTEX             98
#define ARM64_SYS_SCHED_SETAFFINITY 122
#define ARM64_SYS_PRCTL             167
#define ARM64_SYS_GETUID            174
#define ARM64_SYS_GETTID            178
#define ARM64_SYS_MPROTECT          226
#define ARM64_SYS_MINCORE           232
#define ARM64_SYS_PROCESS_VM_READV  270
#define ARM64_SYS_PROCESS_VM_WRITEV 271
#define ARM64_SYS_GETRANDOM         278

#define ARM64_AT_FDCWD              (-100)
#define ARM64_O_CREAT               0100
#define ARM64_PR_SET_VMA            0x53564d41ul
#define ARM64_PR_SET_VMA_ANON_NAME  0ul

long getrandom_fake(void *buffer, size_t size, unsigned flags) {
  const unsigned known = 1u /* GRND_NONBLOCK */ | 2u /* GRND_RANDOM */;
  if (flags & ~known) { errno = EINVAL; return -1; }
  if (size > (size_t)LONG_MAX) { errno = EINVAL; return -1; }
  if (size && !buffer) { errno = EFAULT; return -1; }
  if (size) randomGet(buffer, size);
  return (long)size;
}

/* Hashed futex wait queues for IL2CPP synchronization.  A bucket can contain
 * unrelated addresses, so each blocked thread contributes an explicit waiter
 * record instead of sharing one condition variable for the whole bucket. */
#define FUTEX_WAIT           0
#define FUTEX_WAKE           1
#define FUTEX_WAIT_BITSET    9
#define FUTEX_WAKE_BITSET    10
#define FUTEX_PRIVATE_FLAG   128
#define FUTEX_CLOCK_REALTIME 256
#define FUTEX_CMD_MASK       0x7f
#define FUTEX_BITSET_MATCH_ANY UINT32_MAX
#define FUTEX_BUCKETS        256
/* Horizon condition variables do not expose Linux's interruptible futex
 * sleep.  Bound every kernel sleep so an Android waiter can observe a futex
 * word which changed without a matching FUTEX_WAKE (or whose wake raced the
 * compatibility queue).  Returning zero after such a change is a permitted
 * spurious futex wake; Android's userspace loops recheck their predicate.
 *
 * This is also required by Unity's stop-the-world collector.  Its signed
 * suspend/resume acknowledgement words can reach zero before the collector's
 * host waiter has consumed the final wake.  An unbounded CondVar sleep then
 * leaves every mutator paused even though both acknowledgement predicates are
 * already satisfied. */
#define FUTEX_RECHECK_SLICE_NS UINT64_C(50000000)

volatile uint32_t g_futex_change_wakes;

enum {
  BIONIC_EINTR = 4,
  BIONIC_EAGAIN = 11,
  BIONIC_EFAULT = 14,
  BIONIC_EINVAL = 22,
  BIONIC_ENOSYS = 38,
  BIONIC_EOVERFLOW = 75,
  BIONIC_ETIMEDOUT = 110,
};

typedef struct FutexWaiter FutexWaiter;
struct FutexWaiter {
  FutexWaiter *previous;
  FutexWaiter *next;
  volatile int32_t *address;
  uint32_t bitset;
  CondVar condition;
  int queued;
  int awoken;
};

typedef struct {
  Mutex lock;
  FutexWaiter *first;
  FutexWaiter *last;
} FutexBucket;

typedef struct {
  int active;
  int realtime;
  struct timespec absolute;
} FutexDeadline;

/* Defined below and shared with process_vm_* range validation. */
static int nx_addr_readable(uintptr_t addr, size_t len);
/* The exact-client GC bridge defines this after its thread-snapshot state.
 * Invoke it only outside a futex-bucket lock. */
static void gc_maybe_release_worker_dependency(
  volatile int32_t *address, int expected);

/* libnx Mutex and CondVar objects are valid when statically zero-initialized. */
static FutexBucket futex_buckets[FUTEX_BUCKETS];

static FutexBucket *futex_bucket_for(const volatile int32_t *address) {
  const unsigned index = (unsigned)(((uintptr_t)address >> 4) &
                                    (FUTEX_BUCKETS - 1));
  return &futex_buckets[index];
}

static int futex_fail(int bionic_error) {
  errno = bionic_error;
  return -1;
}

static int futex_validate_address(const volatile int32_t *address) {
  const uintptr_t value = (uintptr_t)address;
  if (value & (sizeof(int32_t) - 1u))
    return futex_fail(BIONIC_EINVAL);
  if (!nx_addr_readable(value, sizeof(int32_t)))
    return futex_fail(BIONIC_EFAULT);
  return 0;
}

static int futex_copy_timeout(const struct timespec *source,
                              struct timespec *destination) {
  if (!nx_addr_readable((uintptr_t)source, sizeof(*source)))
    return futex_fail(BIONIC_EFAULT);
  memcpy(destination, source, sizeof(*destination));
  if (destination->tv_sec < 0 || destination->tv_nsec < 0 ||
      destination->tv_nsec >= 1000000000L)
    return futex_fail(BIONIC_EINVAL);
  return 0;
}

static int futex_clock_now(int realtime, struct timespec *now) {
  if (clock_gettime(realtime ? CLOCK_REALTIME : CLOCK_MONOTONIC, now) == 0)
    return 0;
  /* These clocks are mandatory on libnx.  Do not leak a host-only errno value
   * through Android's syscall ABI if the service nevertheless fails. */
  return futex_fail(BIONIC_EINVAL);
}

static int futex_add_relative_timeout(FutexDeadline *deadline,
                                      const struct timespec *relative) {
  struct timespec now;
  if (futex_clock_now(0, &now) != 0) return -1;
  deadline->active = 1;
  deadline->realtime = 0;
  if ((uint64_t)relative->tv_sec >
      (uint64_t)INT64_MAX - (uint64_t)now.tv_sec) {
    deadline->absolute.tv_sec = (time_t)INT64_MAX;
    deadline->absolute.tv_nsec = 999999999L;
    return 0;
  }
  deadline->absolute.tv_sec = now.tv_sec + relative->tv_sec;
  deadline->absolute.tv_nsec = now.tv_nsec + relative->tv_nsec;
  if (deadline->absolute.tv_nsec >= 1000000000L) {
    if (deadline->absolute.tv_sec == (time_t)INT64_MAX) {
      deadline->absolute.tv_nsec = 999999999L;
    } else {
      deadline->absolute.tv_sec++;
      deadline->absolute.tv_nsec -= 1000000000L;
    }
  }
  return 0;
}

static int futex_remaining_timeout(const FutexDeadline *deadline,
                                   uint64_t *remaining_ns) {
  struct timespec now;
  if (futex_clock_now(deadline->realtime, &now) != 0) return -1;
  if (now.tv_sec > deadline->absolute.tv_sec ||
      (now.tv_sec == deadline->absolute.tv_sec &&
       now.tv_nsec >= deadline->absolute.tv_nsec)) {
    *remaining_ns = 0;
    return 0;
  }
  uint64_t seconds = (uint64_t)(deadline->absolute.tv_sec - now.tv_sec);
  int64_t nanoseconds = (int64_t)deadline->absolute.tv_nsec -
                        (int64_t)now.tv_nsec;
  if (nanoseconds < 0) {
    seconds--;
    nanoseconds += 1000000000L;
  }
  if (seconds > UINT64_MAX / UINT64_C(1000000000)) {
    *remaining_ns = UINT64_MAX;
  } else {
    const uint64_t base = seconds * UINT64_C(1000000000);
    *remaining_ns = (uint64_t)nanoseconds > UINT64_MAX - base
      ? UINT64_MAX : base + (uint64_t)nanoseconds;
  }
  return 1;
}

static void futex_enqueue_locked(FutexBucket *bucket, FutexWaiter *waiter) {
  waiter->previous = bucket->last;
  waiter->next = NULL;
  if (bucket->last) bucket->last->next = waiter;
  else bucket->first = waiter;
  bucket->last = waiter;
  waiter->queued = 1;
}

static void futex_remove_locked(FutexBucket *bucket, FutexWaiter *waiter) {
  if (!waiter->queued) return;
  if (waiter->previous) waiter->previous->next = waiter->next;
  else bucket->first = waiter->next;
  if (waiter->next) waiter->next->previous = waiter->previous;
  else bucket->last = waiter->previous;
  waiter->previous = NULL;
  waiter->next = NULL;
  waiter->queued = 0;
}

static long futex_wait_impl(volatile int32_t *address, int expected,
                            uint32_t bitset, const FutexDeadline *deadline) {
  FutexBucket *bucket = futex_bucket_for(address);
  FutexWaiter waiter = {
    .address = address,
    .bitset = bitset,
  };

  mutexLock(&bucket->lock);
  /* The bucket lock serializes enqueue against FUTEX_WAKE; the atomic load
   * orders the user-space value check against the writer's preceding store. */
  if (__atomic_load_n(address, __ATOMIC_SEQ_CST) != expected) {
    mutexUnlock(&bucket->lock);
    return futex_fail(BIONIC_EAGAIN);
  }
  futex_enqueue_locked(bucket, &waiter);

  while (!waiter.awoken) {
    uint64_t wait_ns = FUTEX_RECHECK_SLICE_NS;
    if (deadline->active) {
      uint64_t remaining_ns = 0;
      const int deadline_state = futex_remaining_timeout(
        deadline, &remaining_ns);
      if (deadline_state < 0) {
        futex_remove_locked(bucket, &waiter);
        mutexUnlock(&bucket->lock);
        return -1;
      }
      if (deadline_state == 0) {
        futex_remove_locked(bucket, &waiter);
        mutexUnlock(&bucket->lock);
        return futex_fail(BIONIC_ETIMEDOUT);
      }
      if (remaining_ns < wait_ns) wait_ns = remaining_ns;
    }
    const Result wait_result = condvarWaitTimeout(
      &waiter.condition, &bucket->lock, wait_ns);

    if (waiter.awoken) break;
    /* A Linux futex word is the predicate; the kernel wait queue is only a
     * notification mechanism.  Let a changed predicate escape even when no
     * matching wake reached this emulated queue.  This is a legal spurious
     * success and, unlike an unconditional timeout, cannot consume a token or
     * violate a caller's absolute deadline. */
    if (__atomic_load_n(address, __ATOMIC_SEQ_CST) != expected) {
      futex_remove_locked(bucket, &waiter);
      mutexUnlock(&bucket->lock);
      __atomic_add_fetch(&g_futex_change_wakes, 1, __ATOMIC_RELAXED);
      return 0;
    }
    if (R_FAILED(wait_result) &&
        R_VALUE(wait_result) != R_VALUE(KERNELRESULT(TimedOut))) {
      futex_remove_locked(bucket, &waiter);
      mutexUnlock(&bucket->lock);
      return futex_fail(BIONIC_EINTR);
    }
    /* Successful condition-variable waits may be spurious.  Slice expiry is
     * not an Android timeout; re-evaluate the one persistent deadline above. */
  }

  futex_remove_locked(bucket, &waiter);
  mutexUnlock(&bucket->lock);
  return 0;
}

static long futex_wake_impl(volatile int32_t *address, int maximum,
                            uint32_t wake_bitset) {
  if (maximum <= 0) return 0;
  FutexBucket *bucket = futex_bucket_for(address);
  int woken = 0;
  __atomic_thread_fence(__ATOMIC_SEQ_CST);
  mutexLock(&bucket->lock);
  for (FutexWaiter *waiter = bucket->first, *next;
       waiter && woken < maximum; waiter = next) {
    next = waiter->next;
    if (waiter->address != address || !(waiter->bitset & wake_bitset))
      continue;
    futex_remove_locked(bucket, waiter);
    waiter->awoken = 1;
    (void)condvarWakeOne(&waiter->condition);
    woken++;
  }
  mutexUnlock(&bucket->lock);
  return woken;
}

static long futex_impl(volatile int32_t *address, int op, int expected,
                       const struct timespec *timeout, uint32_t val3) {
  const unsigned command = (unsigned)op & FUTEX_CMD_MASK;
  const unsigned known_bits = FUTEX_CMD_MASK | FUTEX_PRIVATE_FLAG |
                              FUTEX_CLOCK_REALTIME;
  if ((unsigned)op & ~known_bits) return futex_fail(BIONIC_ENOSYS);
  /* Android's 5.15 futex ABI accepts CLOCK_REALTIME only for the bitset wait
   * among the four operations implemented by this compatibility layer. */
  if ((op & FUTEX_CLOCK_REALTIME) && command != FUTEX_WAIT_BITSET)
    return futex_fail(BIONIC_ENOSYS);
  if ((command == FUTEX_WAIT_BITSET || command == FUTEX_WAKE_BITSET) &&
      val3 == 0)
    return futex_fail(BIONIC_EINVAL);
  if (command != FUTEX_WAIT && command != FUTEX_WAIT_BITSET &&
      command != FUTEX_WAKE && command != FUTEX_WAKE_BITSET)
    return futex_fail(BIONIC_ENOSYS);
  if (futex_validate_address(address) != 0) return -1;

  if (command == FUTEX_WAKE || command == FUTEX_WAKE_BITSET) {
    const uint32_t wake_bitset = command == FUTEX_WAKE
      ? FUTEX_BITSET_MATCH_ANY : val3;
    return futex_wake_impl(address, expected, wake_bitset);
  }

  FutexDeadline deadline = {0};
  if (timeout) {
    struct timespec copied_timeout;
    if (futex_copy_timeout(timeout, &copied_timeout) != 0) return -1;
    if (command == FUTEX_WAIT) {
      if (futex_add_relative_timeout(&deadline, &copied_timeout) != 0)
        return -1;
    } else {
      deadline.active = 1;
      deadline.realtime = (op & FUTEX_CLOCK_REALTIME) != 0;
      deadline.absolute = copied_timeout;
    }
  }
  const uint32_t wait_bitset = command == FUTEX_WAIT
    ? FUTEX_BITSET_MATCH_ANY : val3;
  gc_maybe_release_worker_dependency(address, expected);
  return futex_wait_impl(address, expected, wait_bitset, &deadline);
}

struct nx_iovec { void *iov_base; size_t iov_len; };

/* Validate process_vm_* and futex ranges without touching guest memory first. */
static int nx_addr_has_permissions(uintptr_t addr, size_t len, u32 permissions) {
  if (len > UINTPTR_MAX - addr) return 0;
  uintptr_t a = addr, end = addr + len;
  while (a < end) {
    MemoryInfo mi; u32 pi;
    if (R_FAILED(svcQueryMemory(&mi, &pi, a))) return 0;
    if (mi.type == 0) return 0;                 /* MemType_Unmapped */
    if ((mi.perm & permissions) != permissions) return 0;
    if (mi.size > UINTPTR_MAX - (uintptr_t)mi.addr) return 0;
    uintptr_t be = (uintptr_t)mi.addr + mi.size;
    if ((uintptr_t)mi.addr > a || be <= a) return 0;
    a = be;
  }
  return 1;
}

static int nx_addr_readable(uintptr_t addr, size_t len) {
  return nx_addr_has_permissions(addr, len, Perm_R);
}

static int nx_addr_writable(uintptr_t addr, size_t len) {
  return nx_addr_has_permissions(addr, len, Perm_W);
}

long syscall_fake(long number, ...) {
  switch (number) {
    case ARM64_SYS_IOCTL: {
      va_list va; va_start(va, number);
      int descriptor = va_arg(va, int);
      unsigned long request = va_arg(va, unsigned long);
      void *argument = va_arg(va, void *);
      va_end(va);
      return ioctl_fake(descriptor, request, argument);
    }
    case ARM64_SYS_FACCESSAT: {
      va_list va; va_start(va, number);
      int directory = va_arg(va, int);
      const char *path = va_arg(va, const char *);
      int mode = va_arg(va, int);
      va_end(va);
      /* Linux syscall 48 is the three-argument faccessat ABI.  This client
       * only uses AT_FDCWD; do not reinterpret another guest descriptor as a
       * host directory handle.  Absolute paths are independent of dirfd. */
      if (directory != ARM64_AT_FDCWD && (!path || path[0] != '/')) {
        errno = 38; /* Bionic ENOSYS. */
        return -1;
      }
      return access_fake(path, mode);
    }
    case ARM64_SYS_OPENAT: {
      va_list va; va_start(va, number);
      int directory = va_arg(va, int);
      const char *path = va_arg(va, const char *);
      int flags = va_arg(va, int);
      int mode = (flags & ARM64_O_CREAT) ? va_arg(va, int) : 0666;
      va_end(va);
      /* Keep openat on the tracked Android path layer so synthetic /proc,
       * packed assets, logical file sizes, and descriptor metadata all match
       * the ordinary open import. */
      if (directory != ARM64_AT_FDCWD && (!path || path[0] != '/')) {
        errno = 38; /* Bionic ENOSYS. */
        return -1;
      }
      return (flags & ARM64_O_CREAT)
        ? open_fake(path, flags, mode) : open_fake(path, flags);
    }
    case ARM64_SYS_FSTAT: {
      va_list va; va_start(va, number);
      int descriptor = va_arg(va, int);
      struct bionic_stat *status = va_arg(va, struct bionic_stat *);
      va_end(va);
      return fstat_fake(descriptor, status);
    }
    case ARM64_SYS_PRCTL: {
      va_list va; va_start(va, number);
      unsigned long option = va_arg(va, unsigned long);
      unsigned long argument2 = va_arg(va, unsigned long);
      unsigned long argument3 = va_arg(va, unsigned long);
      unsigned long argument4 = va_arg(va, unsigned long);
      unsigned long argument5 = va_arg(va, unsigned long);
      va_end(va);
      (void)argument3;
      (void)argument4;
      (void)argument5;
      /* PR_SET_VMA/PR_SET_VMA_ANON_NAME changes only Linux proc-map labels.
       * Horizon has no corresponding label API, but accepting the hint keeps
       * the mapping lifecycle identical without weakening protection. */
      if (option == ARM64_PR_SET_VMA &&
          argument2 == ARM64_PR_SET_VMA_ANON_NAME)
        return 0;
      errno = 38; /* Bionic ENOSYS. */
      return -1;
    }
    case ARM64_SYS_GETUID: return 0;
    case ARM64_SYS_GETTID: return gettid_fake();
    case ARM64_SYS_MPROTECT: {
      /* Android code is allowed to bypass the exported libc veneer and issue
       * the AArch64 Linux mprotect syscall directly. Keep that route on the
       * same broker as the ordinary mprotect import so sparse first-touch and
       * failure/errno behavior cannot diverge between the two ABIs. */
      va_list va; va_start(va, number);
      void *address = va_arg(va, void *);
      size_t length = va_arg(va, size_t);
      int protection = va_arg(va, int);
      va_end(va);
      return mprotect_fake(address, length, protection);
    }
    case ARM64_SYS_MINCORE: {
      va_list va; va_start(va, number);
      void *address = va_arg(va, void *);
      size_t length = va_arg(va, size_t);
      unsigned char *vector = va_arg(va, unsigned char *);
      va_end(va);
      return nx_mincore(address, length, vector);
    }
    case ARM64_SYS_GETRANDOM: {
      va_list va; va_start(va, number);
      void *buffer = va_arg(va, void *);
      size_t size = va_arg(va, size_t);
      unsigned flags = va_arg(va, unsigned);
      va_end(va);
      return getrandom_fake(buffer, size, flags);
    }
    case ARM64_SYS_FUTEX: {
      va_list va; va_start(va, number);
      volatile int32_t *uaddr = va_arg(va, volatile int32_t *);
      const int op  = va_arg(va, int);
      const int val = va_arg(va, int);
      const struct timespec *to = va_arg(va, const struct timespec *);
      volatile int32_t *uaddr2 = va_arg(va, volatile int32_t *);
      const uint32_t val3 = va_arg(va, unsigned int);
      va_end(va);
      (void)uaddr2; /* requeue/PI operations remain deliberately unsupported. */
      return futex_impl(uaddr, op, val, to, val3);
    }
    case ARM64_SYS_SCHED_SETAFFINITY:
      return 0; // affinity hints are advisory; pretend success
    case ARM64_SYS_PROCESS_VM_READV:
    case ARM64_SYS_PROCESS_VM_WRITEV: {
      /* Support validated copies within the current process. */
      va_list va; va_start(va, number);
      long pid                   = va_arg(va, long); (void)pid;
      const struct nx_iovec *liov   = va_arg(va, const struct nx_iovec *);
      unsigned long lcnt         = va_arg(va, unsigned long);
      const struct nx_iovec *riov   = va_arg(va, const struct nx_iovec *);
      unsigned long rcnt         = va_arg(va, unsigned long);
      unsigned long flags        = va_arg(va, unsigned long);
      va_end(va);
      if (pid != getpid_fake()) return futex_fail(3); /* Bionic ESRCH */
      if (flags || lcnt > 1024 || rcnt > 1024)
        return futex_fail(BIONIC_EINVAL);
      if ((lcnt && (!liov || lcnt > SIZE_MAX / sizeof(*liov) ||
                    !nx_addr_readable((uintptr_t)liov,
                                      lcnt * sizeof(*liov)))) ||
          (rcnt && (!riov || rcnt > SIZE_MAX / sizeof(*riov) ||
                    !nx_addr_readable((uintptr_t)riov,
                                      rcnt * sizeof(*riov)))))
        return futex_fail(BIONIC_EFAULT);
      int writing = (number == ARM64_SYS_PROCESS_VM_WRITEV);
      ssize_t total = 0;
      unsigned long li = 0, ri = 0; size_t lo = 0, ro = 0;
      while (li < lcnt && ri < rcnt) {
        size_t lrem = liov[li].iov_len - lo, rrem = riov[ri].iov_len - ro;
        size_t n = lrem < rrem ? lrem : rrem;
        const uintptr_t local_base = (uintptr_t)liov[li].iov_base;
        const uintptr_t remote_base = (uintptr_t)riov[ri].iov_base;
        if (lo > UINTPTR_MAX - local_base || ro > UINTPTR_MAX - remote_base) {
          if (total == 0) return futex_fail(BIONIC_EFAULT);
          return total;
        }
        char *lp = (char *)(local_base + lo);
        char *rp = (char *)(remote_base + ro);
        char *source = writing ? lp : rp;
        char *destination = writing ? rp : lp;
        if (!nx_addr_readable((uintptr_t)source, n) ||
            !nx_addr_writable((uintptr_t)destination, n)) {
          if (total == 0) { errno = EFAULT; return -1; }
          return total;
        }
        if (writing) memcpy(rp, lp, n); else memcpy(lp, rp, n);
        total += (ssize_t)n; lo += n; ro += n;
        if (lo == liov[li].iov_len) { li++; lo = 0; }
        if (ro == riov[ri].iov_len) { ri++; ro = 0; }
      }
      return total;
    }
  }
  errno = 38; /* Bionic ENOSYS. */
  return -1;
}

void sincosf_fake(float x, float *s, float *c) { *s = sinf(x); *c = cosf(x); }
void android_set_abort_message_fake(const char *msg) {
  android_log_sink_abort_message(msg);
}
size_t __ctype_get_mb_cur_max_fake(void) { return 1; }

#define BIONIC_SC_PAGESIZE 39
#define BIONIC_SC_PAGE_SIZE 40
#define BIONIC_SC_NPROCESSORS_CONF 96
#define BIONIC_SC_NPROCESSORS_ONLN 97
#define BIONIC_SC_PHYS_PAGES 98

long sysconf_fake(int name) {
  switch (name) {
    case BIONIC_SC_PAGESIZE:
    case BIONIC_SC_PAGE_SIZE: return 0x1000;
    case BIONIC_SC_NPROCESSORS_CONF:
    case BIONIC_SC_NPROCESSORS_ONLN: return 3;
    /* Keep Unity's initial reservations within the wrapper arenas. */
    case BIONIC_SC_PHYS_PAGES: return (512ll * 1024 * 1024) / 0x1000;
    default: return -1;
  }
}

/* Linux UAPI clock IDs used by Android/Bionic.  newlib assigns different
 * values to CLOCK_REALTIME and CLOCK_MONOTONIC, so guest IDs must never be
 * passed directly to the host libc. */
enum {
  BIONIC_CLOCK_REALTIME = 0,
  BIONIC_CLOCK_MONOTONIC = 1,
  BIONIC_CLOCK_PROCESS_CPUTIME_ID = 2,
  BIONIC_CLOCK_THREAD_CPUTIME_ID = 3,
  BIONIC_CLOCK_MONOTONIC_RAW = 4,
  BIONIC_CLOCK_REALTIME_COARSE = 5,
  BIONIC_CLOCK_MONOTONIC_COARSE = 6,
  BIONIC_CLOCK_BOOTTIME = 7,
  BIONIC_CLOCK_REALTIME_ALARM = 8,
  BIONIC_CLOCK_BOOTTIME_ALARM = 9,
  BIONIC_CLOCK_SGI_CYCLE = 10,
  BIONIC_CLOCK_TAI = 11,
};

typedef struct {
  clockid_t primary;
  clockid_t fallback;
  int has_fallback;
} BionicClockMap;

static int map_bionic_clock(int id, BionicClockMap *map) {
  map->has_fallback = 0;
  switch (id) {
    case BIONIC_CLOCK_REALTIME:
      map->primary = CLOCK_REALTIME;
      return 0;
    case BIONIC_CLOCK_MONOTONIC:
      map->primary = CLOCK_MONOTONIC;
      return 0;
    case BIONIC_CLOCK_PROCESS_CPUTIME_ID:
      map->primary = CLOCK_PROCESS_CPUTIME_ID;
      map->fallback = CLOCK_MONOTONIC;
      map->has_fallback = 1;
      return 0;
    case BIONIC_CLOCK_THREAD_CPUTIME_ID:
      map->primary = CLOCK_THREAD_CPUTIME_ID;
      map->fallback = CLOCK_MONOTONIC;
      map->has_fallback = 1;
      return 0;
    case BIONIC_CLOCK_MONOTONIC_RAW:
      map->primary = CLOCK_MONOTONIC_RAW;
      map->fallback = CLOCK_MONOTONIC;
      map->has_fallback = 1;
      return 0;
    case BIONIC_CLOCK_REALTIME_COARSE:
      map->primary = CLOCK_REALTIME_COARSE;
      map->fallback = CLOCK_REALTIME;
      map->has_fallback = 1;
      return 0;
    case BIONIC_CLOCK_MONOTONIC_COARSE:
      map->primary = CLOCK_MONOTONIC_COARSE;
      map->fallback = CLOCK_MONOTONIC;
      map->has_fallback = 1;
      return 0;
    case BIONIC_CLOCK_BOOTTIME:
      map->primary = CLOCK_BOOTTIME;
      map->fallback = CLOCK_MONOTONIC;
      map->has_fallback = 1;
      return 0;
    case BIONIC_CLOCK_REALTIME_ALARM:
      /* Alarm clocks differ only in wakeup behavior; querying them has the
       * same epoch as their non-alarm counterpart. */
      map->primary = CLOCK_REALTIME;
      return 0;
    case BIONIC_CLOCK_BOOTTIME_ALARM:
      map->primary = CLOCK_BOOTTIME;
      map->fallback = CLOCK_MONOTONIC;
      map->has_fallback = 1;
      return 0;
    case BIONIC_CLOCK_TAI:
      map->primary = CLOCK_TAI;
      map->fallback = CLOCK_REALTIME;
      map->has_fallback = 1;
      return 0;
    case BIONIC_CLOCK_SGI_CYCLE:
    default:
      errno = EINVAL;
      return -1;
  }
}

static int clock_error_allows_fallback(int error) {
  return error == EINVAL || error == ENOSYS || error == ENOTSUP;
}

static int bionic_clock_call(int id, struct timespec *value, int get_resolution) {
  BionicClockMap map;
  if (map_bionic_clock(id, &map) != 0) return -1;

  int result = get_resolution
    ? clock_getres(map.primary, value)
    : clock_gettime(map.primary, value);
  if (result == 0 || !map.has_fallback ||
      !clock_error_allows_fallback(errno))
    return result;

  return get_resolution
    ? clock_getres(map.fallback, value)
    : clock_gettime(map.fallback, value);
}

int clock_gettime_fake(int bionic_clock_id, struct timespec *value) {
  return bionic_clock_call(bionic_clock_id, value, 0);
}

int clock_getres_fake(int bionic_clock_id, struct timespec *value) {
  return bionic_clock_call(bionic_clock_id, value, 1);
}

#define LINUX_O_CREAT  0100
#define LINUX_O_EXCL   0200
#define LINUX_O_TRUNC  01000
#define LINUX_O_APPEND 02000
#define LINUX_O_NONBLOCK 04000
#define LINUX_O_DSYNC 010000
#define LINUX_O_ASYNC 020000
#define LINUX_O_DIRECT 040000
#define LINUX_O_DIRECTORY 0200000
#define LINUX_O_NOFOLLOW 0400000
#define LINUX_O_CLOEXEC 02000000
#define LINUX_O_SYNC 04010000

static int convert_open_flags(int flags) {
  int out = flags & 3;
  if (flags & LINUX_O_CREAT)  out |= O_CREAT;
  if (flags & LINUX_O_EXCL)   out |= O_EXCL;
  if (flags & LINUX_O_TRUNC)  out |= O_TRUNC;
  if (flags & LINUX_O_APPEND) out |= O_APPEND;
  if (flags & LINUX_O_NONBLOCK) out |= O_NONBLOCK;
  if (flags & (LINUX_O_DSYNC | LINUX_O_SYNC)) out |= O_SYNC;
  if (flags & LINUX_O_CLOEXEC) out |= O_CLOEXEC;
  if (flags & LINUX_O_DIRECTORY) out |= O_DIRECTORY;
  if (flags & LINUX_O_NOFOLLOW) out |= O_NOFOLLOW;
  return out;
}

/* Add the devoptab prefix omitted from managed paths. */
static const char *dev_abs(const char *in, char *buf, size_t n) {
  if (in && (!strcmp(in, "/etc/ssl/cert.pem") ||
             !strcmp(in, "/usr/local/ssl/cert.pem") ||
             !strcmp(in, "/etc/ssl/certs/ca-certificates.crt")))
    return CA_BUNDLE_PATH;
  if (!in || in[0] != '/') return in;
  snprintf(buf, n, "sdmc:%s", in);
  return buf;
}
/* Retry Android asset paths against the staged assets tree. */
static int assets_suffix_fallback(const char *path, char *out, size_t outsz) {
  const char *hit = NULL, *s;
  for (s = path; (s = strstr(s, "assets/")) != NULL; s++) {
    if (s == path || s[-1] == '/' || s[-1] == '!') hit = s;
  }
  if (!hit) return 0;
  struct stat st;
  snprintf(out, outsz, "%s", hit);
  return stat(out, &st) == 0;
}

/* Skip devoptab root paths that fsdev cannot create. */
static int safe_mkdir(const char *p) {
  if (!p || !*p) { errno = EINVAL; return -1; }
  const char *colon = strchr(p, ':');
  if (colon) {                       // has a "device:" prefix
    const char *in = colon + 1;      // the path inside the device
    while (*in == '/') in++;
    if (!*in) { errno = EEXIST; return 0; }  // "sdmc:" / "sdmc:/" -> root, skip
    if (!strchr(in, '/')) { errno = EEXIST; return 0; }
  }
  return mkdir(p, 0777);
}

/* Create parents below the game root. */
static void mkdir_p_dir(const char *dir) {
  if (!dir || !*dir) return;
  char tmp[640];   // >= dev_abs's 600B normalized path
  if (snprintf(tmp, sizeof(tmp), "%s", dir) <= 0) return;
  size_t skip;
  const size_t glen = strlen(GAME_HOME);
  if (strncmp(tmp, GAME_HOME, glen) == 0 && (tmp[glen] == '/' || tmp[glen] == '\0')) {
    skip = glen;                                  // only create *under* the game root
  } else {
    const char *colon = strchr(tmp, ':');         // unknown base: at least skip "device:"
    skip = colon ? (size_t)(colon + 1 - tmp) : 0;
  }
  for (char *p = tmp + skip + 1; *p; p++)
    if (*p == '/') { *p = '\0'; safe_mkdir(tmp); *p = '/'; }
  if (tmp[skip]) safe_mkdir(tmp);
}
static void mkdir_parents(const char *filepath) {
  char tmp[640];   // >= dev_abs's 600B normalized path
  snprintf(tmp, sizeof(tmp), "%s", filepath);
  char *last = strrchr(tmp, '/');
  if (!last || last == tmp) return;
  *last = '\0';
  mkdir_p_dir(tmp);
}

int mkdir_fake(const char *path, unsigned mode) {
  (void)mode;
  if (!path || !*path) { errno = EINVAL; return -1; }
  char _nb[600]; path = dev_abs(path, _nb, sizeof _nb);
  mkdir_p_dir(path);
  int r = safe_mkdir(path);
  if (r != 0 && errno == EEXIST) r = 0;
  return r;
}

int truncate_fake(const char *path, long length) {
  if (!path) { errno = EFAULT; return -1; }
  if (length < 0) { errno = EINVAL; return -1; }
  if (asset_pack_stat_path_info(path, NULL, NULL, NULL)) {
    errno = EROFS;
    return -1;
  }
  char normalized[600];
  return truncate(dev_abs(path, normalized, sizeof normalized), length);
}

int unlink_fake(const char *path) {
  if (!path) { errno = EFAULT; return -1; }
  if (asset_pack_stat_path_info(path, NULL, NULL, NULL)) {
    errno = EROFS;
    return -1;
  }
  char normalized[600];
  return unlink(dev_abs(path, normalized, sizeof normalized));
}

int rmdir_fake(const char *path) {
  if (!path) { errno = EFAULT; return -1; }
  if (asset_pack_stat_path_info(path, NULL, NULL, NULL)) {
    errno = EROFS;
    return -1;
  }
  char normalized[600];
  return rmdir(dev_abs(path, normalized, sizeof normalized));
}

int remove_fake(const char *path) {
  if (!path) { errno = EFAULT; return -1; }
  if (asset_pack_stat_path_info(path, NULL, NULL, NULL)) {
    errno = EROFS;
    return -1;
  }
  char normalized[600];
  return remove(dev_abs(path, normalized, sizeof normalized));
}

int rename_fake(const char *old_path, const char *new_path) {
  if (!old_path || !new_path) { errno = EFAULT; return -1; }
  if (asset_pack_stat_path_info(old_path, NULL, NULL, NULL) ||
      asset_pack_stat_path_info(new_path, NULL, NULL, NULL)) {
    errno = EROFS;
    return -1;
  }
  char old_normalized[600], new_normalized[600];
  return rename(dev_abs(old_path, old_normalized, sizeof old_normalized),
                dev_abs(new_path, new_normalized, sizeof new_normalized));
}

int utime_fake(const char *path, const void *times) {
  if (!path) { errno = EFAULT; return -1; }
  if (asset_pack_stat_path_info(path, NULL, NULL, NULL)) {
    errno = EROFS;
    return -1;
  }
  char normalized[600];
  return utime(dev_abs(path, normalized, sizeof normalized),
               (const struct utimbuf *)times);
}

int utimes_fake(const char *path, const void *times) {
  if (!path) { errno = EFAULT; return -1; }
  if (asset_pack_stat_path_info(path, NULL, NULL, NULL)) {
    errno = EROFS;
    return -1;
  }
  char normalized[600];
  return utimes(dev_abs(path, normalized, sizeof normalized),
                (const struct timeval *)times);
}

int flock_fake(int fd, int operation) {
  enum { B_LOCK_SH = 1, B_LOCK_EX = 2, B_LOCK_NB = 4, B_LOCK_UN = 8 };
  const int mode = operation & ~B_LOCK_NB;
  if (operation & ~(B_LOCK_SH | B_LOCK_EX | B_LOCK_NB | B_LOCK_UN) ||
      (mode != B_LOCK_SH && mode != B_LOCK_EX && mode != B_LOCK_UN)) {
    errno = EINVAL;
    return -1;
  }
  if (fakefd_is_fake(fd) || asset_pack_fd_is(fd) || nx_epoll_is_fd(fd)) {
    errno = 95; /* bionic EOPNOTSUPP */
    return -1;
  }
  struct flock lock = {
    .l_type = mode == B_LOCK_UN ? F_UNLCK :
              (mode == B_LOCK_SH ? F_RDLCK : F_WRLCK),
    .l_whence = SEEK_SET,
    .l_start = 0,
    .l_len = 0,
  };
  int result = fcntl(fd, (operation & B_LOCK_NB) ? F_SETLK : F_SETLKW, &lock);
  if (result < 0) {
    if (errno == ENOSYS) errno = 38;       /* bionic ENOSYS */
    else if (errno == EDEADLK) errno = 35; /* bionic EDEADLK */
    else if (errno == ENOLCK) errno = 37;  /* bionic ENOLCK */
    else if (errno == EOPNOTSUPP) errno = 95;
  }
  return result;
}

static char *fd_path_snapshot(int fd);

int futimens_fake(int fd, const void *times) {
  enum { B_UTIME_NOW = 1073741823, B_UTIME_OMIT = 1073741822 };
  typedef struct { int64_t sec, nsec; } BionicFileTimespec;
  if (asset_pack_fd_is(fd)) { errno = EROFS; return -1; }
  if (fakefd_is_fake(fd) || nx_epoll_is_fd(fd)) {
    errno = 95;
    return -1;
  }
  char *path = fd_path_snapshot(fd);
  if (!path) { errno = 38; return -1; }
  int result;
  if (!times) {
    result = utimes(path, NULL);
  } else {
    const BionicFileTimespec *input = times;
    struct timeval output[2];
    struct stat existing;
    struct timeval now;
    int need_existing = input[0].nsec == B_UTIME_OMIT ||
                        input[1].nsec == B_UTIME_OMIT;
    int need_now = input[0].nsec == B_UTIME_NOW ||
                   input[1].nsec == B_UTIME_NOW;
    if ((need_existing && stat(path, &existing) < 0) ||
        (need_now && gettimeofday(&now, NULL) < 0)) {
      free(path);
      return -1;
    }
    for (int i = 0; i < 2; i++) {
      if (input[i].nsec == B_UTIME_OMIT) {
        output[i].tv_sec = i ? existing.st_mtime : existing.st_atime;
        output[i].tv_usec = 0;
      } else if (input[i].nsec == B_UTIME_NOW) {
        output[i] = now;
      } else {
        if (input[i].nsec < 0 || input[i].nsec >= 1000000000) {
          free(path);
          errno = EINVAL;
          return -1;
        }
        output[i].tv_sec = (time_t)input[i].sec;
        output[i].tv_usec = (suseconds_t)(input[i].nsec / 1000);
      }
    }
    result = utimes(path, output);
  }
  int saved = errno;
  free(path);
  errno = saved;
  return result;
}

/* Short-lived descriptor-route serialization.  Replacement marks a stripe
 * while backend captures use a generation ticket.  Hash collisions only add
 * bounded serialization; blocking I/O never holds one of these locks. */
#define FD_ROUTE_STRIPES 128u
typedef struct {
  Mutex lock;
  CondVar cond;
  uint64_t sequence;
  int replacing;
} FdRouteStripe;
static FdRouteStripe g_fd_routes[FD_ROUTE_STRIPES];

static int fd_route_is_replacing(const FdRouteStripe *route) {
  return __atomic_load_n(&route->replacing, __ATOMIC_ACQUIRE);
}

static uint64_t fd_route_sequence(const FdRouteStripe *route) {
  return __atomic_load_n(&route->sequence, __ATOMIC_ACQUIRE);
}

static void fd_route_begin_locked(FdRouteStripe *route) {
  __atomic_store_n(&route->replacing, 1, __ATOMIC_RELEASE);
  (void)__atomic_add_fetch(&route->sequence, 1, __ATOMIC_ACQ_REL);
}

static void fd_route_finish_locked(FdRouteStripe *route) {
  /* Publish the completed generation before allowing lock-free validators to
   * observe a clear replacement flag. */
  (void)__atomic_add_fetch(&route->sequence, 1, __ATOMIC_ACQ_REL);
  __atomic_store_n(&route->replacing, 0, __ATOMIC_RELEASE);
  (void)condvarWakeAll(&route->cond);
}

static uint32_t fd_route_index(int fd) {
  uint32_t value = (uint32_t)fd;
  value ^= value >> 16;
  value *= UINT32_C(0x7feb352d);
  value ^= value >> 15;
  return value & (FD_ROUTE_STRIPES - 1u);
}

static void fd_route_wait_clear_locked(FdRouteStripe *route) {
  while (fd_route_is_replacing(route))
    (void)condvarWait(&route->cond, &route->lock);
}

int nx_fd_route_snapshot(int fd, NxFdRouteTicket *ticket) {
  if (!ticket) { errno = EINVAL; return 0; }
  const uint32_t stripe = fd_route_index(fd);
  FdRouteStripe *route = &g_fd_routes[stripe];
  mutexLock(&route->lock);
  fd_route_wait_clear_locked(route);
  ticket->stripe = stripe;
  ticket->sequence = fd_route_sequence(route);
  ticket->fd = fd;
  mutexUnlock(&route->lock);
  return 1;
}

int nx_fd_route_validate(const NxFdRouteTicket *ticket) {
  if (!ticket || ticket->stripe >= FD_ROUTE_STRIPES) return 0;
  const FdRouteStripe *route = &g_fd_routes[ticket->stripe];
  return !fd_route_is_replacing(route) &&
         fd_route_sequence(route) == ticket->sequence;
}

int nx_fd_route_source_lock(int fd, uint32_t *stripe_out) {
  if (!stripe_out) { errno = EINVAL; return 0; }
  const uint32_t stripe = fd_route_index(fd);
  FdRouteStripe *route = &g_fd_routes[stripe];
  mutexLock(&route->lock);
  fd_route_wait_clear_locked(route);
  *stripe_out = stripe;
  return 1;
}

void nx_fd_route_source_unlock(uint32_t stripe) {
  if (stripe < FD_ROUTE_STRIPES) mutexUnlock(&g_fd_routes[stripe].lock);
}

int nx_fd_route_replace_begin(int fd, NxFdRoutePair *guard) {
  if (!guard) { errno = EINVAL; return 0; }
  memset(guard, 0, sizeof(*guard));
  const uint32_t stripe = fd_route_index(fd);
  FdRouteStripe *route = &g_fd_routes[stripe];
  mutexLock(&route->lock);
  fd_route_wait_clear_locked(route);
  fd_route_begin_locked(route);
  guard->source_stripe = stripe;
  guard->target_stripe = stripe;
  guard->target_fd = fd;
  guard->state = 2; /* active, route mutex released */
  mutexUnlock(&route->lock);
  return 1;
}

int nx_fd_route_pair_begin(int source, int target, NxFdRoutePair *guard) {
  if (!guard) { errno = EINVAL; return 0; }
  memset(guard, 0, sizeof(*guard));
  const uint32_t source_stripe = fd_route_index(source);
  const uint32_t target_stripe = fd_route_index(target);
  const uint32_t first = source_stripe < target_stripe
    ? source_stripe : target_stripe;
  const uint32_t second = source_stripe < target_stripe
    ? target_stripe : source_stripe;
  for (;;) {
    mutexLock(&g_fd_routes[first].lock);
    if (second != first) mutexLock(&g_fd_routes[second].lock);
    if (!fd_route_is_replacing(&g_fd_routes[source_stripe]) &&
        !fd_route_is_replacing(&g_fd_routes[target_stripe])) break;
    const uint32_t wait_stripe =
      fd_route_is_replacing(&g_fd_routes[source_stripe])
      ? source_stripe : target_stripe;
    if (second != first) mutexUnlock(&g_fd_routes[second].lock);
    mutexUnlock(&g_fd_routes[first].lock);
    mutexLock(&g_fd_routes[wait_stripe].lock);
    fd_route_wait_clear_locked(&g_fd_routes[wait_stripe]);
    mutexUnlock(&g_fd_routes[wait_stripe].lock);
  }
  fd_route_begin_locked(&g_fd_routes[target_stripe]);
  guard->source_stripe = source_stripe;
  guard->target_stripe = target_stripe;
  guard->target_fd = target;
  guard->state = 1; /* both ordered route locks are still held */
  return 1;
}

void nx_fd_route_pair_release(NxFdRoutePair *guard) {
  if (!guard || guard->state != 1) return;
  const uint32_t first = guard->source_stripe < guard->target_stripe
    ? guard->source_stripe : guard->target_stripe;
  const uint32_t second = guard->source_stripe < guard->target_stripe
    ? guard->target_stripe : guard->source_stripe;
  if (second != first) mutexUnlock(&g_fd_routes[second].lock);
  mutexUnlock(&g_fd_routes[first].lock);
  guard->state = 2;
}

void nx_fd_route_replace_end(NxFdRoutePair *guard) {
  if (!guard || !guard->state || guard->target_stripe >= FD_ROUTE_STRIPES)
    return;
  FdRouteStripe *target = &g_fd_routes[guard->target_stripe];
  if (guard->state == 1) {
    fd_route_finish_locked(target);
    const uint32_t first = guard->source_stripe < guard->target_stripe
      ? guard->source_stripe : guard->target_stripe;
    const uint32_t second = guard->source_stripe < guard->target_stripe
      ? guard->target_stripe : guard->source_stripe;
    if (second != first) mutexUnlock(&g_fd_routes[second].lock);
    mutexUnlock(&g_fd_routes[first].lock);
  } else {
    mutexLock(&target->lock);
    fd_route_finish_locked(target);
    mutexUnlock(&target->lock);
  }
  guard->state = 0;
}

/* Read-ahead windows for Unity archives. */
#define RA_SLOTS 8
#define RA_WIN   (1u << 20)     /* 1 MB read-ahead window */
static struct RaCache {
  int  fd;           /* -1 == free */
  long pos;          /* virtual file position (what read/lseek observe) */
  long size;         /* file size (for SEEK_END) */
  long base;         /* file offset of buf[0] */
  long len;          /* valid bytes currently in buf */
  unsigned char *buf;
} g_ra[RA_SLOTS] = {
  { .fd = -1 }, { .fd = -1 }, { .fd = -1 }, { .fd = -1 },
  { .fd = -1 }, { .fd = -1 }, { .fd = -1 }, { .fd = -1 },
};
static Mutex g_ra_lock;
static NxReadAheadDiagnostics g_ra_diagnostics;

enum {
  RA_OWNER_ATTACH = 1,
  RA_OWNER_DETACH = 2,
  RA_OWNER_FLUSH = 3,
  RA_OWNER_READ = 4,
  RA_OWNER_SEEK = 5,
};

static void ra_lock(uint32_t kind, int fd, uint64_t request_bytes) {
  __atomic_fetch_add(&g_ra_diagnostics.lock_attempts, 1, __ATOMIC_RELAXED);
  __atomic_fetch_add(&g_ra_diagnostics.lock_waiters, 1, __ATOMIC_RELAXED);
  mutexLock(&g_ra_lock);
  __atomic_fetch_sub(&g_ra_diagnostics.lock_waiters, 1, __ATOMIC_RELAXED);
  __atomic_store_n(&g_ra_diagnostics.owner_started_tick,
                   armGetSystemTick(), __ATOMIC_RELAXED);
  __atomic_store_n(&g_ra_diagnostics.owner_request_bytes,
                   request_bytes, __ATOMIC_RELAXED);
  __atomic_store_n(&g_ra_diagnostics.owner_thread,
                   (uintptr_t)threadGetSelf(), __ATOMIC_RELAXED);
  __atomic_store_n(&g_ra_diagnostics.owner_handle,
                   (uint32_t)threadGetCurHandle(), __ATOMIC_RELAXED);
  __atomic_store_n(&g_ra_diagnostics.owner_kind, kind, __ATOMIC_RELAXED);
  __atomic_store_n(&g_ra_diagnostics.owner_fd, fd, __ATOMIC_RELAXED);
  __atomic_store_n(&g_ra_diagnostics.owner_active, 1, __ATOMIC_RELEASE);
  __atomic_fetch_add(&g_ra_diagnostics.lock_acquisitions, 1,
                     __ATOMIC_RELAXED);
}

static void ra_unlock(void) {
  /* Clear before releasing the mutex.  Clearing after mutexUnlock could race
   * and erase the next owner's freshly published record. */
  __atomic_store_n(&g_ra_diagnostics.owner_active, 0, __ATOMIC_RELEASE);
  mutexUnlock(&g_ra_lock);
}

void nx_read_ahead_get_diagnostics(NxReadAheadDiagnostics *out) {
  if (!out) return;
  out->lock_address = (uintptr_t)&g_ra_lock;
  out->owner_active = __atomic_load_n(&g_ra_diagnostics.owner_active,
                                      __ATOMIC_ACQUIRE);
#define RA_DIAG_LOAD(field) \
  out->field = __atomic_load_n(&g_ra_diagnostics.field, __ATOMIC_RELAXED)
  RA_DIAG_LOAD(lock_attempts);
  RA_DIAG_LOAD(lock_acquisitions);
  RA_DIAG_LOAD(lock_waiters);
  RA_DIAG_LOAD(owner_started_tick);
  RA_DIAG_LOAD(owner_request_bytes);
  RA_DIAG_LOAD(read_calls);
  RA_DIAG_LOAD(read_bytes);
  RA_DIAG_LOAD(read_failures);
  RA_DIAG_LOAD(owner_thread);
  RA_DIAG_LOAD(owner_handle);
  RA_DIAG_LOAD(owner_kind);
  RA_DIAG_LOAD(owner_fd);
#undef RA_DIAG_LOAD
  out->lock_word = __atomic_load_n(
    (const uint32_t *)&g_ra_lock, __ATOMIC_RELAXED);
}

/* Every lookup and every use of a returned slot happens under g_ra_lock. */
static struct RaCache *ra_find_locked(int fd) {
  if (fd < 0) return NULL;
  for (int i = 0; i < RA_SLOTS; i++) if (g_ra[i].fd == fd) return &g_ra[i];
  return NULL;
}
void ra_attach(int fd, long size) {
  ra_lock(RA_OWNER_ATTACH, fd, 0);
  for (int i = 0; i < RA_SLOTS; i++) if (g_ra[i].fd < 0) {
    if (!g_ra[i].buf) g_ra[i].buf = malloc(RA_WIN);
    if (g_ra[i].buf) { g_ra[i].fd = fd; g_ra[i].pos = 0; g_ra[i].size = size; g_ra[i].base = 0; g_ra[i].len = 0; }
    break;
  }
  ra_unlock();
}
static void ra_detach(int fd) {
  ra_lock(RA_OWNER_DETACH, fd, 0);
  struct RaCache *c = ra_find_locked(fd);
  if (c) c->fd = -1;   /* keep buf allocated for reuse */
  ra_unlock();
}

int ra_flush_detach(int fd) {
  int saved = 0;
  ra_lock(RA_OWNER_FLUSH, fd, 0);
  struct RaCache *c = ra_find_locked(fd);
  if (c) {
    if (lseek(fd, c->pos, SEEK_SET) < 0) {
      saved = errno;
    } else {
      c->fd = -1;       /* keep the allocation available for a future open */
    }
  }
  ra_unlock();
  if (saved) {
    errno = saved;
    return -1;
  }
  return 0;
}

/* Atomically determine whether fd owns a cache and, if so, complete its read.
 * This prevents close/reuse from changing the slot between lookup and use. */
static int ra_read_if_attached(int fd, const NxFdRouteTicket *ticket,
                               void *buf, size_t count, long *result) {
  if (!result) return 0;
  size_t done = 0;
  int failure = 0;
  ra_lock(RA_OWNER_READ, fd, count);
  if (!nx_fd_route_validate(ticket)) {
    ra_unlock();
    return -1; /* the caller must restart classification */
  }
  struct RaCache *c = ra_find_locked(fd);
  if (!c) {
    ra_unlock();
    return 0;
  }
  __atomic_fetch_add(&g_ra_diagnostics.read_calls, 1, __ATOMIC_RELAXED);
  if (!buf && count) {
    __atomic_fetch_add(&g_ra_diagnostics.read_failures, 1, __ATOMIC_RELAXED);
    ra_unlock();
    errno = EFAULT;
    *result = -1;
    return 1;
  }
  while (done < count) {
    const int cache_hit = c->len > 0 && c->pos >= c->base &&
                          c->pos - c->base < c->len;
    if (!cache_hit) {
      if (lseek(fd, c->pos, SEEK_SET) < 0) { failure = errno; break; }
      long r = 0;
      while (r < (long)RA_WIN) {
        long k = read(fd, c->buf + r, RA_WIN - (size_t)r);
        if (k < 0) { if (!r) failure = errno; break; }
        if (k == 0) break;
        r += k;
      }
      if (r <= 0) break;
      if (c->pos > LONG_MAX - r) { failure = EOVERFLOW; break; }
      c->base = c->pos; c->len = r;
    }
    long avail = c->len - (c->pos - c->base);
    if (avail <= 0) break;
    size_t n = (count - done < (size_t)avail) ? count - done : (size_t)avail;
    memcpy((char *)buf + done, c->buf + (c->pos - c->base), n);
    c->pos += n; done += n;
  }
  __atomic_fetch_add(&g_ra_diagnostics.read_bytes, done, __ATOMIC_RELAXED);
  if (failure)
    __atomic_fetch_add(&g_ra_diagnostics.read_failures, 1, __ATOMIC_RELAXED);
  ra_unlock();
  if (!done && failure) {
    errno = failure;
    *result = -1;
  } else {
    *result = (long)done;
  }
  return 1;
}

/* off_t is 64-bit on this target, so this also services lseek64. */
long z_lseek(int fd, long off, int whence) {
  uint32_t stripe;
  if (!nx_fd_route_source_lock(fd, &stripe)) return -1;

  /* A read-ahead descriptor owns a virtual cursor until it is transferred to
   * stdio or duplicated.  The route lock prevents close/reuse while that
   * cursor is inspected and updated. */
  ra_lock(RA_OWNER_SEEK, fd, 0);
  struct RaCache *c = ra_find_locked(fd);
  if (c) {
    long base = 0;
    int failure = 0;
    switch (whence) {
      case SEEK_SET: base = 0; break;
      case SEEK_CUR: base = c->pos; break;
      case SEEK_END: base = c->size; break;
      default: failure = EINVAL; break;
    }
    if (!failure &&
        ((off > 0 && base > LONG_MAX - off) ||
         (off < 0 && base < LONG_MIN - off)))
      failure = EOVERFLOW;
    const long np = failure ? -1 : base + off;
    if (!failure && np < 0) failure = EINVAL;
    if (!failure) c->pos = np;
    ra_unlock();
    nx_fd_route_source_unlock(stripe);
    if (failure) {
      errno = failure;
      return -1;
    }
    return np;
  }
  ra_unlock();

  AssetPackOperation asset = {0};
  FakeFdOperation fake = {0};
  long result;
  if (asset_pack_operation_acquire(fd, &asset)) {
    result = asset_pack_operation_lseek(&asset, off, whence);
    const int saved = errno;
    asset_pack_operation_release(&asset);
    errno = saved;
  } else if (fakefd_operation_acquire(fd, &fake)) {
    fakefd_operation_release(&fake);
    errno = ESPIPE;
    result = -1;
  } else if (fakefd_is_fake(fd) || nx_epoll_is_fd(fd)) {
    errno = EBADF;
    result = -1;
  } else {
    /* Keep fsdev's cursor on the original descriptor.  Duplicated devoptab
     * handles do not reliably carry a usable fileStruct on Switch hardware.
     * Bulk download files may have a bounded physical preallocation tail;
     * SEEK_END must expose the Android-visible logical length instead. */
    int64_t logical_size = 0;
    if (whence == SEEK_END && nx_file_io_logical_size(fd, &logical_size)) {
      if ((off > 0 && logical_size > LONG_MAX - off) ||
          (off < 0 && logical_size < LONG_MIN - off)) {
        errno = EOVERFLOW;
        result = -1;
      } else {
        const long logical_offset = (long)logical_size + off;
        result = logical_offset < 0
          ? (errno = EINVAL, -1) : lseek(fd, logical_offset, SEEK_SET);
      }
    } else {
      result = lseek(fd, off, whence);
    }
  }
  const int saved = errno;
  nx_fd_route_source_unlock(stripe);
  errno = saved;
  return result;
}

static const char *synthetic_proc(const char *path, size_t *size_out);  /* defined below */

/* Materialize synthetic procfs data behind a real descriptor. */
static int synth_proc_open(const char *path) {
  if (!path) return -1;
  if (strncmp(path, "/proc/", 6) && strncmp(path, "/sys/", 5)) return -1;
  static char buf[16384];
  int len;
  if (!strcmp(path, "/proc/self/maps") || !strcmp(path, "/proc/self/smaps")) {
    len = so_dump_maps(buf, sizeof buf);
  } else {
    size_t synth_size = 0;
    const char *s = synthetic_proc(path, &synth_size);
    if (!s) return -1;                                   // not /proc or /sys
    len = synth_size > INT_MAX ? INT_MAX : (int)synth_size;
    if (len > (int)sizeof buf) len = (int)sizeof buf;
    memcpy(buf, s, (size_t)len);
  }
  char safe[160]; size_t j = 0;
  for (const char *p = path; *p && j < sizeof safe - 1; p++) safe[j++] = (*p == '/') ? '_' : *p;
  safe[j] = '\0';
  char tf[256];
  snprintf(tf, sizeof tf, "%s/.synth%s", GAME_HOME, safe);
  int wfd = open(tf, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (wfd >= 0) { if (write(wfd, buf, (size_t)len) < 0) { /* best effort */ } close(wfd); }
  return open(tf, O_RDONLY);
}

/* Stable synthetic inode numbers for files reported as inode zero by fsdev. */
#define FD_INO_MAX 4096
static uint64_t g_fd_ino[FD_INO_MAX];
static char *g_fd_path[FD_INO_MAX];
static Mutex g_fd_metadata_lock;

static const char *unity_metadata_basename(const char *path) {
  if (!path) return NULL;
  const char *name = path;
  for (const char *p = path; *p; ++p)
    if (*p == '/' || *p == '\\') name = p + 1;
  if (!strcmp(name, "global-metadata.dat") ||
      !strcmp(name, "startup-metadata.dat")) return name;
  return NULL;
}

static int unity_global_metadata_cache_path(const char *path) {
  return path && unity_metadata_basename(path) &&
         strstr(path, "/files/il2cpp/Metadata/global-metadata.dat") != NULL;
}

static int fd_unity_metadata_name(int fd, char *name, size_t name_size) {
  if (!name || !name_size || fd < 0 || fd >= FD_INO_MAX) return 0;
  int found = 0;
  mutexLock(&g_fd_metadata_lock);
  const char *base = unity_metadata_basename(g_fd_path[fd]);
  if (base) {
    snprintf(name, name_size, "%s", base);
    found = 1;
  }
  mutexUnlock(&g_fd_metadata_lock);
  return found;
}

static uint64_t path_ino(const char *path) {
  uint64_t h = 1469598103934665603ULL;               // FNV-1a 64 offset basis
  for (const unsigned char *p = (const unsigned char *)path; *p; p++) { h ^= *p; h *= 1099511628211ULL; }
  return h ? h : 1;                                   // 0 means "no inode" -- avoid it
}
static void fd_ino_set(int fd, const char *path) {
  if (fd < 0 || fd >= FD_INO_MAX || !path) return;
  char *copy = strdup(path);
  mutexLock(&g_fd_metadata_lock);
  char *old = g_fd_path[fd];
  g_fd_path[fd] = copy;
  g_fd_ino[fd] = path_ino(path);
  mutexUnlock(&g_fd_metadata_lock);
  free(old);
}
static void fd_ino_clear(int fd) {
  if (fd < 0 || fd >= FD_INO_MAX) return;
  mutexLock(&g_fd_metadata_lock);
  char *old = g_fd_path[fd];
  g_fd_path[fd] = NULL;
  g_fd_ino[fd] = 0;
  mutexUnlock(&g_fd_metadata_lock);
  free(old);
}
static char *fd_path_snapshot(int fd) {
  if (fd < 0 || fd >= FD_INO_MAX) return NULL;
  mutexLock(&g_fd_metadata_lock);
  char *path = g_fd_path[fd] ? strdup(g_fd_path[fd]) : NULL;
  mutexUnlock(&g_fd_metadata_lock);
  return path;
}
void fd_metadata_copy(int source, int target) {
  if (target < 0 || target >= FD_INO_MAX) return;
  char *copy = NULL;
  uint64_t ino = 0;
  mutexLock(&g_fd_metadata_lock);
  if (source >= 0 && source < FD_INO_MAX) {
    if (g_fd_path[source]) copy = strdup(g_fd_path[source]);
    ino = g_fd_ino[source];
  }
  char *old = g_fd_path[target];
  g_fd_path[target] = copy;
  g_fd_ino[target] = ino;
  mutexUnlock(&g_fd_metadata_lock);
  free(old);
}

int open_fake(const char *path, int flags, ...) {
  if (!path) { errno = EFAULT; return -1; }
  const char *metadata_name = unity_metadata_basename(path);
  const int global_metadata_cache =
    unity_global_metadata_cache_path(path);
  int mode = 0666;
  if (flags & LINUX_O_CREAT) { va_list va; va_start(va, flags); mode = va_arg(va, int); va_end(va); }
  const int cvt = convert_open_flags(flags);
  const int writing = (flags & 3) != 0 || (flags & LINUX_O_CREAT);
  if (!writing) {
    /* Back Android random devices with libnx entropy. */
    if (!strcmp(path, "/dev/urandom") || !strcmp(path, "/dev/random")) {
      static int removed_legacy_entropy_file;
      if (!__atomic_exchange_n(&removed_legacy_entropy_file, 1,
                               __ATOMIC_ACQ_REL))
        (void)remove(GAME_HOME "/.synth_dev_random");
      return fakefd_random(flags & (LINUX_O_NONBLOCK | LINUX_O_CLOEXEC));
    }
    int sfd = synth_proc_open(path);
    if (sfd >= 0) { fd_ino_set(sfd, path); return sfd; }
    int packed_fd = asset_pack_open_path(path);
    if (packed_fd >= 0) {
      fd_ino_set(packed_fd, path);
      return packed_fd;
    }
  }
  /* Preserve literal virtual paths before adding the devoptab prefix. */
  char _nb[600]; path = dev_abs(path, _nb, sizeof _nb);
  const char *resolved_path = path;
  int fd = open(path, cvt, mode);
  if (fd < 0 && writing) {
    mkdir_parents(path);
    fd = open(path, cvt, mode);
  }
  char alt[512];
  if (fd < 0 && (flags & 3) == 0 && !(flags & LINUX_O_CREAT)) {
    if (assets_suffix_fallback(path, alt, sizeof(alt))) {
      fd = open(alt, cvt, mode);
      if (fd >= 0) resolved_path = alt;
    }
  }
  if (fd >= 0 && global_metadata_cache && !writing) {
    struct stat cache_stat;
    const int cache_stat_ok = fstat(fd, &cache_stat) == 0;
    if (!cache_stat_ok || cache_stat.st_size < 1024 * 1024) {
      close(fd);
      fd = -1;
      errno = ENOENT;
    }
  }
  char canonical[768];
  if (fd < 0 && metadata_name && !writing && !global_metadata_cache) {
    snprintf(canonical, sizeof canonical,
             GAME_HOME "/assets/bin/Data/Managed/Metadata/%s", metadata_name);
    fd = open(canonical, cvt, mode);
    if (fd >= 0) resolved_path = canonical;
  }
  if (fd >= 0) {
    fd_ino_set(fd, resolved_path);
    nx_file_io_track_open(fd, resolved_path, writing);
    struct stat _st;
    /* Cache large read-only assets. */
    if (fstat(fd, &_st) == 0 && !writing && _st.st_size >= (4 << 20))
      ra_attach(fd, (long)_st.st_size);
  }
  return fd;
}

int access_fake(const char *path, int mode) {
  (void)mode;
  if (!path) { errno = EFAULT; return -1; }
  if (!strcmp(path, "/etc/ssl/cert.pem") ||
      !strcmp(path, "/usr/local/ssl/cert.pem") ||
      !strcmp(path, "/etc/ssl/certs/ca-certificates.crt"))
    path = CA_BUNDLE_PATH;
  if (asset_pack_stat_path_info(path, NULL, NULL, NULL)) return 0;
  char native_path[600];
  path = dev_abs(path, native_path, sizeof native_path);
  struct stat status;
  return stat(path, &status) == 0 ? 0 : -1;
}
struct bionic_timespec { int64_t tv_sec; int64_t tv_nsec; };
struct bionic_stat {
  uint64_t st_dev; uint64_t st_ino; uint32_t st_mode; uint32_t st_nlink;
  uint32_t st_uid; uint32_t st_gid; uint64_t st_rdev; uint64_t __pad1;
  int64_t st_size; int32_t st_blksize; int32_t __pad2; int64_t st_blocks;
  struct bionic_timespec st_atim; struct bionic_timespec st_mtim; struct bionic_timespec st_ctim;
  uint32_t __unused4; uint32_t __unused5;
};

static void convert_stat(const struct stat *in, struct bionic_stat *out) {
  memset(out, 0, sizeof(*out));
  out->st_dev = in->st_dev; out->st_ino = in->st_ino;
  /* fsdev permissions do not describe SD write access. */
  out->st_mode = (in->st_mode & (uint32_t)S_IFMT) | 0777u;
  out->st_nlink = in->st_nlink; out->st_uid = in->st_uid; out->st_gid = in->st_gid;
  out->st_rdev = in->st_rdev; out->st_size = in->st_size; out->st_blksize = in->st_blksize;
  out->st_blocks = in->st_blocks;
  out->st_atim.tv_sec = in->st_atime; out->st_mtim.tv_sec = in->st_mtime; out->st_ctim.tv_sec = in->st_ctime;
}

int stat_fake(const char *path, struct bionic_stat *st) {
  uint64_t packed_size, packed_ino;
  int packed_directory;
  if (asset_pack_stat_path_info(path, &packed_size, &packed_ino, &packed_directory)) {
    memset(st, 0, sizeof(*st));
    st->st_ino = packed_ino;
    st->st_mode = (packed_directory ? S_IFDIR | 0555 : S_IFREG | 0444);
    st->st_nlink = 1;
    st->st_size = (int64_t)packed_size;
    st->st_blksize = 4096;
    st->st_blocks = (int64_t)((packed_size + 511) / 512);
    return 0;
  }
  char _nb[600]; path = dev_abs(path, _nb, sizeof _nb);
  struct stat real; int r = stat(path, &real);
  if (r != 0) {
    char alt[512];
    if (assets_suffix_fallback(path, alt, sizeof(alt))) r = stat(alt, &real);
  }
  if (r == 0) {
    convert_stat(&real, st);
    int64_t logical_size = 0;
    if (nx_file_io_logical_size_path(path, &logical_size)) {
      st->st_size = logical_size;
      st->st_blocks = logical_size / 512 + (logical_size % 512 != 0);
    }
    if (st->st_ino == 0) st->st_ino = path_ino(path);   // fsdev gives 0 -> synth
  }
  return r;
}
int fstat_fake(int fd, struct bionic_stat *st) {
  uint64_t packed_size, packed_ino;
  int packed_directory;
  if (asset_pack_fstat_fd(fd, &packed_size, &packed_ino, &packed_directory)) {
    memset(st, 0, sizeof(*st));
    st->st_ino = packed_ino;
    st->st_mode = (packed_directory ? S_IFDIR | 0555 : S_IFREG | 0444);
    st->st_nlink = 1;
    st->st_size = (int64_t)packed_size;
    st->st_blksize = 4096;
    st->st_blocks = (int64_t)((packed_size + 511) / 512);
    return 0;
  }
  struct stat real; const int r = fstat(fd, &real);
  if (r == 0) {
    convert_stat(&real, st);
    int64_t logical_size = 0;
    if (nx_file_io_logical_size(fd, &logical_size)) {
      st->st_size = logical_size;
      st->st_blocks = logical_size / 512 + (logical_size % 512 != 0);
    }
    if (st->st_ino == 0) {                               // mirror stat(path)'s inode
      uint64_t ino = (fd >= 0 && fd < FD_INO_MAX) ? g_fd_ino[fd] : 0;
      st->st_ino = ino ? ino : ((uint64_t)(fd + 1) * 2654435761ULL) | 1;
    }
  }
  return r;
}
int lstat_fake(const char *path, struct bionic_stat *st) { return stat_fake(path, st); }

struct bionic_dirent {
  uint64_t d_ino; int64_t d_off; uint16_t d_reclen; uint8_t d_type; char d_name[256];
};

#define BIONIC_DIRENT_SLOTS 128
typedef struct {
  void *stream;
  struct bionic_dirent entry;
} BionicDirentSlot;
static BionicDirentSlot g_dirent_slots[BIONIC_DIRENT_SLOTS];
static Mutex g_dirent_lock;

static BionicDirentSlot *dirent_slot_locked(void *stream, int create) {
  BionicDirentSlot *free_slot = NULL;
  for (size_t i = 0; i < BIONIC_DIRENT_SLOTS; ++i) {
    if (g_dirent_slots[i].stream == stream) return &g_dirent_slots[i];
    if (!g_dirent_slots[i].stream && !free_slot) free_slot = &g_dirent_slots[i];
  }
  if (create && free_slot) free_slot->stream = stream;
  return create ? free_slot : NULL;
}

void *readdir_fake(void *dirp) {
  if (!dirp) { errno = EBADF; return NULL; }
  mutexLock(&g_dirent_lock);
  BionicDirentSlot *slot = dirent_slot_locked(dirp, 1);
  if (!slot) { mutexUnlock(&g_dirent_lock); errno = EMFILE; return NULL; }
  struct bionic_dirent *out = &slot->entry;
  memset(out, 0, sizeof(*out));
  out->d_reclen = sizeof(*out);
  if (asset_pack_dir_is(dirp)) {
    const char *name = asset_pack_readdir_path(dirp, &out->d_type, &out->d_ino);
    if (!name) { mutexUnlock(&g_dirent_lock); return NULL; }
    snprintf(out->d_name, sizeof(out->d_name), "%s", name);
  } else {
    struct dirent *e = readdir((DIR *)dirp);
    if (!e) { mutexUnlock(&g_dirent_lock); return NULL; }
    out->d_ino = e->d_ino;
    out->d_type = e->d_type;
    snprintf(out->d_name, sizeof(out->d_name), "%s", e->d_name);
  }
  mutexUnlock(&g_dirent_lock);
  return out;
}

int closedir_fake(void *dirp) {
  const int result = asset_pack_dir_is(dirp) ? asset_pack_closedir_path(dirp)
                                              : closedir((DIR *)dirp);
  mutexLock(&g_dirent_lock);
  BionicDirentSlot *slot = dirent_slot_locked(dirp, 0);
  if (slot) memset(slot, 0, sizeof(*slot));
  mutexUnlock(&g_dirent_lock);
  return result;
}

int alphasort_fake(const void *left, const void *right) {
  const struct bionic_dirent *const *a = (const struct bionic_dirent *const *)left;
  const struct bionic_dirent *const *b = (const struct bionic_dirent *const *)right;
  return strcoll((*a)->d_name, (*b)->d_name);
}
int scandir_fake(const char *path, void ***names_out,
                 int (*filter)(const void *), int (*compare)(const void *, const void *)) {
  if (!names_out) { errno = EINVAL; return -1; }
  *names_out = NULL;
  DIR *dir = (DIR *)asset_pack_opendir_path(path);
  char native_path[600];
  if (!dir) dir = opendir(dev_abs(path, native_path, sizeof native_path));
  if (!dir) return -1;
  size_t count = 0, capacity = 16;
  struct bionic_dirent **names = calloc(capacity, sizeof(*names));
  if (!names) { closedir_fake(dir); errno = ENOMEM; return -1; }
  for (;;) {
    struct bionic_dirent *entry = readdir_fake(dir);
    if (!entry) break;
    if (filter && !filter(entry)) continue;
    struct bionic_dirent *copy = malloc(sizeof(*copy));
    if (!copy) { errno = ENOMEM; goto fail; }
    memcpy(copy, entry, sizeof(*copy));
    if (count == capacity) {
      capacity *= 2;
      void *grown = realloc(names, capacity * sizeof(*names));
      if (!grown) { free(copy); errno = ENOMEM; goto fail; }
      names = grown;
    }
    names[count++] = copy;
  }
  closedir_fake(dir);
  if (compare && count > 1) qsort(names, count, sizeof(*names), compare);
  *names_out = (void **)names;
  return (int)count;
fail:
  closedir_fake(dir);
  for (size_t i = 0; i < count; ++i) free(names[i]);
  free(names);
  return -1;
}

int mkstemp_fake(char *template_path) {
  if (!template_path) { errno = EINVAL; return -1; }
  const size_t length = strlen(template_path);
  if (length < 6 || strcmp(template_path + length - 6, "XXXXXX")) {
    errno = EINVAL;
    return -1;
  }
  static const char alphabet[] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
  for (int attempt = 0; attempt < 128; ++attempt) {
    uint32_t random_value;
    randomGet(&random_value, sizeof random_value);
    for (int i = 0; i < 6; ++i) {
      template_path[length - 6 + (size_t)i] = alphabet[random_value % (sizeof alphabet - 1)];
      random_value = random_value / (sizeof alphabet - 1) + random_value * 33u;
    }
    int fd = open_fake(template_path, 2 | LINUX_O_CREAT | LINUX_O_EXCL, 0600);
    if (fd >= 0) return fd;
    if (errno != EEXIST) return -1;
  }
  errno = EEXIST;
  return -1;
}

/* Locale handles use newlib's C locale. */
void *newlocale_fake(int mask, const char *locale, void *base) { (void)mask; (void)locale; (void)base; return (void *)1; }
void freelocale_fake(void *loc) { (void)loc; }
void *uselocale_fake(void *loc) { (void)loc; return (void *)1; }

#define WRAP_ISW_L(fn) int fn##_l_fake(int wc, void *loc) { (void)loc; return fn(wc); }
WRAP_ISW_L(iswalpha) WRAP_ISW_L(iswblank) WRAP_ISW_L(iswcntrl) WRAP_ISW_L(iswdigit)
WRAP_ISW_L(iswlower) WRAP_ISW_L(iswprint) WRAP_ISW_L(iswpunct) WRAP_ISW_L(iswspace)
WRAP_ISW_L(iswupper) WRAP_ISW_L(iswxdigit) WRAP_ISW_L(towlower) WRAP_ISW_L(towupper)

int strcoll_l_fake(const char *a, const char *b, void *loc) { (void)loc; return strcoll(a, b); }
size_t strxfrm_l_fake(char *dst, const char *src, size_t n, void *loc) { (void)loc; return strxfrm(dst, src, n); }
size_t strftime_l_fake(char *s, size_t max, const char *fmt, const void *tm, void *loc) { (void)loc; return strftime(s, max, fmt, (const struct tm *)tm); }
long double strtold_l_fake(const char *s, char **end, void *loc) { (void)loc; return strtold(s, end); }
long long strtoll_l_fake(const char *s, char **end, int base, void *loc) { (void)loc; return strtoll(s, end, base); }
unsigned long long strtoull_l_fake(const char *s, char **end, int base, void *loc) { (void)loc; return strtoull(s, end, base); }
int wcscoll_l_fake(const wchar_t *a, const wchar_t *b, void *loc) { (void)loc; return wcscoll(a, b); }
size_t wcsxfrm_l_fake(wchar_t *dst, const wchar_t *src, size_t n, void *loc) { (void)loc; return wcsxfrm(dst, src, n); }

size_t mbsnrtowcs_fake(wchar_t *dst, const char **src, size_t nms, size_t len, void *ps) {
  (void)ps;
  size_t i = 0; const char *s = *src;
  while (i < nms && s[i] && (!dst || i < len)) { if (dst) dst[i] = (unsigned char)s[i]; i++; }
  if (dst && i < len) { dst[i] = 0; *src = NULL; }
  return i;
}
size_t wcsnrtombs_fake(char *dst, const wchar_t **src, size_t nwc, size_t len, void *ps) {
  (void)ps;
  size_t i = 0; const wchar_t *s = *src;
  while (i < nwc && s[i] && (!dst || i < len)) { if (dst) dst[i] = (char)s[i]; i++; }
  if (dst && i < len) { dst[i] = 0; *src = NULL; }
  return i;
}

int posix_memalign_fake(void **out, size_t align, size_t size) {
  void *p = memalign(align, size);
  if (!p) return ENOMEM;
  *out = p;
  return 0;
}

/* Page-granular mmap emulation for Unity's aligned reservations. */
extern void  *g_mmap_arena_base;   // set by __libnx_initheap (main.c)
extern size_t g_mmap_arena_size;
extern uintptr_t g_kernel_heap_base;
extern size_t g_kernel_heap_bytes;
extern void *g_heap_donor_base;
extern size_t g_heap_donor_capacity;
extern size_t g_heap_donor_active_bytes;
extern size_t g_heap_donor_kernel_offset;
extern NxMemoryBackingBackend g_memory_backing_backend;

#define BIONIC_MAP_ANONYMOUS 0x20
#define BIONIC_MAP_SHARED    0x01
#define BIONIC_MAP_PRIVATE   0x02
#define MMAP_PAGE       0x1000u
#define MMAP_BIG_ALIGN  MMAP_ARENA_ALIGN
#define MMAP_BIG_THRESH ((size_t)64 * 1024 * 1024)
#define BIONIC_PROT_NONE 0x0
#define BIONIC_PROT_READ 0x1
#define BIONIC_PROT_WRITE 0x2

/* Exact 7.0.1 Unity allocator request: a 4 GiB slab space plus one 8 MiB
 * alignment guard.  Android reserves the address range and backs it on first
 * touch.  Keep the same split on Horizon: the whole range is only a libnx
 * virtual reservation, while the exact 8 MiB slab selected by Unity is mapped
 * with physical pages immediately before its first allocator-header write. */
#define UNITY_SLAB_RESERVATION_BYTES ((size_t)GENSHIN_UNITY_SLAB_MAP_BYTES)
#define UNITY_SLAB_ALIGNMENT        ((size_t)GENSHIN_UNITY_SLAB_ALIGNMENT)
#define UNITY_SLAB_CHUNK_BYTES       UNITY_SLAB_ALIGNMENT
#define UNITY_SLAB_CHUNK_COUNT \
  ((size_t)GENSHIN_UNITY_SLAB_USABLE_BYTES / UNITY_SLAB_CHUNK_BYTES)
_Static_assert(SIZE_MAX >= UINT64_C(0x100800000),
               "Unity slab reservation requires a 64-bit size_t");
_Static_assert((GENSHIN_UNITY_SLAB_MAP_BYTES & (MMAP_PAGE - 1u)) == 0,
               "Unity slab reservation must be page aligned");
_Static_assert((GENSHIN_UNITY_SLAB_ALIGNMENT &
                (GENSHIN_UNITY_SLAB_ALIGNMENT - 1u)) == 0,
               "Unity slab alignment must be a power of two");

static uint8_t *mmap_arena;    // 256MB-aligned usable base (published last)
static size_t   mmap_usable;   // usable bytes
static size_t   mmap_pages;    // usable / page
static uint8_t *mmap_used;     // 1 byte/page bitmap: reserved (address space)
static int      mmap_arena_initialized;
static Mutex    g_mmap_lock;   // zero-init == valid unlocked libnx mutex

/* g_mmap_lock exists only in this compatibility layer.  Android's native
 * madvise/mmap path therefore cannot be interrupted while owning it, whereas
 * a raw Horizon threadPause can stop its owner and leave IL2CPP's private GC
 * service blocked in mutexLock: the service waits in madvise_fake on this
 * symbol while a suspended target owns it.
 *
 * Mirror asynchronous signal semantics around this wrapper-only lock.  A
 * queued waiter must remain suspendible, so publish critical state for one
 * nonblocking attempt, clear it before yielding, and retain it only for the
 * actual owner.  gc_capture_slot_once performs a second post-pause check,
 * closing the publication race. */
static void mmap_broker_lock(void) {
  for (;;) {
    nx_guest_gc_critical_enter();
    if (mutexTryLock(&g_mmap_lock)) return;
    nx_guest_gc_critical_leave();
    svcSleepThread(0);
  }
}

static void mmap_broker_unlock(void) {
  mutexUnlock(&g_mmap_lock);
  nx_guest_gc_critical_leave();
}
static void    *unity_slab_reservation;
static size_t   unity_slab_reservation_size;
static uint8_t *unity_slab_aligned_base;
static uint8_t *unity_slab_prepared_base;
static uint8_t unity_slab_committed[UNITY_SLAB_CHUNK_COUNT];
static uint32_t unity_slab_donor_sources[UNITY_SLAB_CHUNK_COUNT];
typedef enum {
  UNITY_SLAB_EMPTY = 0,
  UNITY_SLAB_RESERVED,
  UNITY_SLAB_CLAIMED,
  UNITY_SLAB_RETIRED,
} UnitySlabState;
static UnitySlabState unity_slab_state;
static UnitySlabMmapDiagnostics unity_slab_diagnostics;

_Static_assert(UNITY_SLAB_CHUNK_COUNT == 512u,
               "exact Unity slab must contain 512 allocator chunks");

static void unity_slab_diag_increment(uint64_t *value) {
  if (*value != UINT64_MAX) ++*value;
}

static void unity_slab_diag_add(uint64_t *value, uint64_t addition) {
  if (UINT64_MAX - *value < addition)
    *value = UINT64_MAX;
  else
    *value += addition;
}

/* Two virtual partitions share one process-lifetime libnx reservation.  With
 * per-process system-resource memory it lives in Horizon's physical alias
 * region.  Stock hbloader processes use the stack mapping region instead and
 * draw from a dynamically resized heap-donor bank through svcMapMemory:
 *
 *  - one shared extent arena serves Unity's large PROT_NONE/mprotect ranges,
 *    guest allocations, Mesa/NVK, and caller-owned stacks. Sparse reservations
 *    grow from low addresses while dynamic allocations pack from high addresses;
 *    8 MiB segments receive physical pages only while live.
 *  - Unity's exact 4.008 GiB slab commits the selected 8 MiB chunks on demand.
 *
 * Keeping one master virtual reservation makes the layout atomic and prevents
 * libnx users from claiming either kernel-unmapped partition. */
static VirtmemReservation *oc_alias_layout_reservation;
static uint8_t *oc_base;
static size_t oc_pages;
static uint8_t *oc_used;
static uint8_t *oc_owned;
static uint8_t *oc_committed;
static uint32_t *oc_sparse_start_pages;
static uint32_t *oc_sparse_granule_sources;

static uint8_t *oc_dynamic_base;
static size_t oc_dynamic_pages;
static size_t oc_dynamic_segments;
static size_t oc_sparse_granules;
static size_t oc_dynamic_extent_capacity;
static size_t oc_alias_layout_bytes;
static uint8_t *oc_dynamic_segment_mapped;
static uint32_t *oc_dynamic_segment_live_pages;
static uint32_t *oc_dynamic_segment_sources;

static uint8_t *oc_donor_units_used;
static size_t oc_donor_unit_capacity;
static size_t oc_donor_active_units;
static uint64_t oc_donor_used_units;
static uint64_t oc_peak_donor_used_units;
static uint64_t oc_donor_grow_calls;
static uint64_t oc_donor_shrink_calls;
static uint32_t oc_donor_last_resize_result;

static uint64_t oc_reserved_pages;
static uint64_t oc_peak_reserved_pages;
static uint64_t oc_committed_pages;
static uint64_t oc_peak_committed_pages;
static uint64_t oc_dynamic_mapped_segments;
static uint64_t oc_peak_dynamic_mapped_segments;
static uint64_t oc_map_call_count;
static uint64_t oc_map_retry_count;
static uint32_t oc_last_map_result;
/* Code-alias unmap accounting.  Making an AliasCode mapping writable changes
 * its state to AliasCodeData; that transition cannot be reversed with
 * svcSetProcessMemoryPermission.  svcUnmapProcessCodeMemory accepts the writable
 * state directly. */
static uint64_t oc_backing_unmap_ok;
static uint64_t oc_backing_unmap_fail;
static uint64_t oc_spill_pages;
static uint64_t oc_peak_spill_pages;
static uint64_t oc_host_spill_pages;
static uint64_t oc_peak_host_spill_pages;
static uint64_t oc_thread_pool_pages;
static uint64_t oc_peak_thread_pool_pages;
static uint64_t oc_guest_allocation_failures;
static uint64_t oc_host_allocation_failures;
static uint64_t oc_thread_allocation_failures;
static uint64_t oc_allocation_slots_in_use;
static uint64_t oc_peak_allocation_slots_in_use;
static uint64_t oc_allocation_slot_exhaustions;

const char *g_oc_arena_failure_stage = "not attempted";

#define OC_DYNAMIC_MAX_PAGES \
  (OC_DYNAMIC_ARENA_BYTES / MMAP_PAGE)
#define OC_DYNAMIC_SEGMENT_PAGES (OC_DYNAMIC_SEGMENT_BYTES / MMAP_PAGE)
#define OC_DYNAMIC_MAX_SEGMENTS \
  (OC_DYNAMIC_ARENA_BYTES / OC_DYNAMIC_SEGMENT_BYTES)
#define OC_SPARSE_GRANULE_PAGES \
  (OC_SPARSE_COMMIT_GRANULE_BYTES / MMAP_PAGE)
#define OC_SPARSE_MAX_GRANULES \
  (OC_DYNAMIC_ARENA_BYTES / OC_SPARSE_COMMIT_GRANULE_BYTES)
#define OC_DONOR_UNITS_PER_SEGMENT \
  (OC_DYNAMIC_SEGMENT_BYTES / OC_HEAP_DONOR_UNIT_BYTES)
#define OC_DONOR_UNITS_PER_GRANULE \
  (OC_SPARSE_COMMIT_GRANULE_BYTES / OC_HEAP_DONOR_UNIT_BYTES)
/* Free extents must alternate with at least one occupied page.  Therefore an
 * N-page arena can contain at most ceil(N/2) simultaneous free runs. */
#define OC_DYNAMIC_MAX_EXTENT_CAPACITY \
  ((OC_DYNAMIC_MAX_PAGES + 1u) / 2u)
#define OC_ALIAS_LAYOUT_ALIGNMENT MMAP_ARENA_ALIGN
/* A 36-bit process exposes ~63.875 GiB of ASLR, which is sufficient for the
 * ~10 GiB sparse/slab layout when using the heap-donor backend.  Reject only
 * address spaces too small for the layout itself. */
#define OC_39BIT_ASLR_THRESHOLD_BYTES ((u64)2 * 1024 * 1024 * 1024)

typedef enum {
  OC_POOL_OWNER_NONE = 0,
  OC_POOL_OWNER_GUEST,
  OC_POOL_OWNER_HOST,
  OC_POOL_OWNER_THREAD,
} OcPoolOwner;

/* Ownership is indexed by the allocation's exact virtual start instead of a
 * separate fixed slot pool.  Retain the native request width because the
 * 39-bit arena intentionally exceeds 4 GiB. */
typedef struct {
  size_t requested;
  uint32_t pages;
  uint32_t owner;
} OcDynamicAllocation;

/* Heap-donor stacks live outside the dynamic arena and are indexed by their
 * first 64 KiB donor unit.  Retain size_t here because donor capacity is a
 * runtime property of the hbloader heap override. */
typedef struct {
  size_t requested;
  uint32_t units;
  uint32_t reserved;
} OcDonorAllocation;

typedef struct {
  size_t first;
  size_t pages;
  size_t donor_units;
  size_t requested;
  OcPoolOwner owner;
  int direct_donor;
} OcPoolAllocationView;

typedef struct {
  uint32_t first_page;
  uint32_t pages;
  uint32_t previous;
  uint32_t next;
} OcDynamicExtent;

static OcDynamicAllocation oc_dynamic_allocations[OC_DYNAMIC_MAX_PAGES];
static OcDynamicExtent
  oc_dynamic_extents[OC_DYNAMIC_MAX_EXTENT_CAPACITY + 1u];
static OcDonorAllocation *oc_donor_allocations;
static uint32_t oc_free_extent_node_head;
static size_t oc_free_extent_node_count;
static uint32_t oc_dynamic_extent_head;
static int oc_dynamic_metadata_ready;

_Static_assert((OC_DYNAMIC_ARENA_BYTES & (MMAP_PAGE - 1u)) == 0,
               "dynamic arena must be page aligned");
_Static_assert((OC_ALIAS_LAYOUT_ALIGNMENT &
                (OC_ALIAS_LAYOUT_ALIGNMENT - 1u)) == 0 &&
               (OC_ALIAS_LAYOUT_ALIGNMENT & (MMAP_PAGE - 1u)) == 0,
               "alias layout alignment must be a page-aligned power of two");
_Static_assert(OC_DYNAMIC_ARENA_BYTES % OC_ALIAS_LAYOUT_ALIGNMENT == 0,
               "shared alias arena must retain layout alignment");
_Static_assert((OC_DYNAMIC_SEGMENT_BYTES &
                (OC_DYNAMIC_SEGMENT_BYTES - 1u)) == 0 &&
               (OC_DYNAMIC_SEGMENT_BYTES & (MMAP_PAGE - 1u)) == 0,
               "dynamic segment must be a page-aligned power of two");
_Static_assert(OC_DYNAMIC_ARENA_BYTES % OC_DYNAMIC_SEGMENT_BYTES == 0,
               "dynamic arena must contain whole segments");
_Static_assert(OC_DYNAMIC_MAX_PAGES < UINT32_MAX &&
               OC_DYNAMIC_MAX_EXTENT_CAPACITY <= UINT32_MAX,
               "dynamic metadata indices must fit in uint32_t");
_Static_assert(sizeof(OcDynamicAllocation) == 16u &&
               sizeof(OcDynamicExtent) == 16u &&
               sizeof(OcDonorAllocation) == 16u,
               "ownership metadata must remain compact");
_Static_assert(OC_SPARSE_COMMIT_GRANULE_BYTES >= MMAP_PAGE &&
               (OC_SPARSE_COMMIT_GRANULE_BYTES &
                (OC_SPARSE_COMMIT_GRANULE_BYTES - 1u)) == 0 &&
               (OC_SPARSE_COMMIT_GRANULE_BYTES & (MMAP_PAGE - 1u)) == 0,
               "sparse commit granule must be a page-aligned power of two");
_Static_assert(OC_HEAP_DONOR_UNIT_BYTES >= MMAP_PAGE &&
               (OC_HEAP_DONOR_UNIT_BYTES &
                (OC_HEAP_DONOR_UNIT_BYTES - 1u)) == 0 &&
               OC_DYNAMIC_SEGMENT_BYTES % OC_HEAP_DONOR_UNIT_BYTES == 0 &&
               OC_SPARSE_COMMIT_GRANULE_BYTES %
                 OC_HEAP_DONOR_UNIT_BYTES == 0,
               "heap donor units must divide every mapping granule");
_Static_assert(OC_HEAP_DONOR_INITIAL_BYTES %
                 OC_HEAP_DONOR_UNIT_BYTES == 0 &&
               OC_HEAP_DONOR_GROW_BYTES %
                 OC_HEAP_DONOR_UNIT_BYTES == 0 &&
               OC_HEAP_DONOR_GROW_BYTES % ((size_t)2 * 1024 * 1024) == 0,
               "heap donor resize policy must preserve Horizon alignment");

static void oc_update_peak(uint64_t *peak, uint64_t value) {
  uint64_t observed = __atomic_load_n(peak, __ATOMIC_RELAXED);
  while (value > observed &&
         !__atomic_compare_exchange_n(peak, &observed, value, 1,
                                      __ATOMIC_RELAXED,
                                      __ATOMIC_RELAXED)) {
  }
}

static uint64_t *oc_pool_owner_pages(OcPoolOwner owner) {
  switch (owner) {
    case OC_POOL_OWNER_GUEST: return &oc_spill_pages;
    case OC_POOL_OWNER_HOST: return &oc_host_spill_pages;
    case OC_POOL_OWNER_THREAD: return &oc_thread_pool_pages;
    default: return NULL;
  }
}

static uint64_t *oc_pool_owner_peak(OcPoolOwner owner) {
  switch (owner) {
    case OC_POOL_OWNER_GUEST: return &oc_peak_spill_pages;
    case OC_POOL_OWNER_HOST: return &oc_peak_host_spill_pages;
    case OC_POOL_OWNER_THREAD: return &oc_peak_thread_pool_pages;
    default: return NULL;
  }
}

static uint64_t *oc_pool_owner_failures(OcPoolOwner owner) {
  switch (owner) {
    case OC_POOL_OWNER_GUEST: return &oc_guest_allocation_failures;
    case OC_POOL_OWNER_HOST: return &oc_host_allocation_failures;
    case OC_POOL_OWNER_THREAD: return &oc_thread_allocation_failures;
    default: return NULL;
  }
}

static void oc_pool_record_failure(OcPoolOwner owner) {
  uint64_t *failures = oc_pool_owner_failures(owner);
  if (failures) __atomic_add_fetch(failures, 1, __ATOMIC_RELAXED);
}

static int oc_address_range_state(const void *address, size_t length,
                                  unsigned permissions,
                                  unsigned memory_type) {
  const uintptr_t begin = (uintptr_t)address;
  if (!begin || !length || length > UINTPTR_MAX - begin) return 0;
  const uintptr_t end = begin + length;
  for (uintptr_t at = begin; at < end; ) {
    MemoryInfo info;
    u32 page_info;
    if (R_FAILED(svcQueryMemory(&info, &page_info, at)) ||
        info.type != memory_type ||
        (info.perm & permissions) != permissions || info.addr > at ||
        info.size > UINTPTR_MAX - info.addr)
      return 0;
    const uintptr_t span_end = (uintptr_t)info.addr + (uintptr_t)info.size;
    if (span_end <= at) return 0;
    at = span_end < end ? span_end : end;
  }
  return 1;
}

typedef struct {
  uint8_t *base;
  VirtmemReservation *reservation;
} OcVirtualLayout;

NxMemoryBackingBackend nx_memory_backing_backend(void) {
  return g_memory_backing_backend;
}

MemoryType nx_memory_backing_mapped_type(void) {
  return g_memory_backing_backend == NX_MEMORY_BACKEND_HEAP_ALIAS
    ? MemType_ModuleCodeMutable : MemType_Heap;
}

static size_t oc_select_dynamic_arena_bytes(void) {
  const u64 large_layout =
    (u64)OC_DYNAMIC_ARENA_BYTES + UNITY_SLAB_RESERVATION_BYTES;
  const u64 large_search = large_layout + OC_ALIAS_LAYOUT_ALIGNMENT;
  if (g_memory_backing_backend != NX_MEMORY_BACKEND_HEAP_ALIAS &&
      g_memory_backing_backend != NX_MEMORY_BACKEND_PHYSICAL) {
    g_oc_arena_failure_stage = "select:backend-none";
    return 0;
  }
  u64 aslr_size = 0;
  if (R_FAILED(svcGetInfo(&aslr_size, InfoType_AslrRegionSize,
                          CUR_PROCESS_HANDLE, 0))) {
    g_oc_arena_failure_stage = "select:aslr-query-failed";
    return 0;
  }
  if (aslr_size <= OC_39BIT_ASLR_THRESHOLD_BYTES) {
    g_oc_arena_failure_stage = "select:aslr-too-small";
    return 0;
  }
  if (aslr_size < large_search) {
    g_oc_arena_failure_stage = "select:aslr-too-small";
    return 0;
  }
  if (g_memory_backing_backend == NX_MEMORY_BACKEND_HEAP_ALIAS)
    return OC_DYNAMIC_ARENA_BYTES;

  u64 alias_size = 0;
  u64 alias_extra = 0;
  if (R_FAILED(svcGetInfo(&alias_size, InfoType_AliasRegionSize,
                          CUR_PROCESS_HANDLE, 0))) {
    g_oc_arena_failure_stage = "select:alias-query-failed";
    return 0;
  }
  if (R_FAILED(svcGetInfo(&alias_extra, InfoType_AliasRegionExtraSize,
                          CUR_PROCESS_HANDLE, 0)))
    alias_extra = 0;
  if (alias_extra >= alias_size) {
    g_oc_arena_failure_stage = "select:alias-extra-overflow";
    return 0;
  }
  const u64 usable = alias_size - alias_extra;
  if (usable < large_search) {
    g_oc_arena_failure_stage = "select:alias-too-small";
    return 0;
  }
  return OC_DYNAMIC_ARENA_BYTES;
}

size_t nx_dynamic_arena_target_bytes(void) {
  const size_t pages = __atomic_load_n(&oc_dynamic_pages, __ATOMIC_ACQUIRE);
  return pages ? pages * MMAP_PAGE : oc_select_dynamic_arena_bytes();
}

/* svcMapPhysicalMemory accepts destinations only inside Horizon's alias
 * region.  The heap-donor fallback uses svcMapProcessCodeMemory, whose
 * destination must be ordinary code/ASLR space. Select the matching virtual
 * region once, align the complete two-part layout, and keep its libnx
 * reservation for the process lifetime.
 *
 * HOS 18+ reports AliasRegionExtraSize as space appended to the public region
 * which is not part of the ordinary mappable alias interval.  Mirror libnx's
 * virtmemSetup() treatment by subtracting it from the end when available. */
static int oc_reserve_virtual_layout(OcVirtualLayout *layout,
                                     size_t layout_bytes) {
  if (!layout || !layout_bytes) return 0;
  memset(layout, 0, sizeof(*layout));

  if (g_memory_backing_backend == NX_MEMORY_BACKEND_HEAP_ALIAS) {
    if (!g_heap_donor_base ||
        g_heap_donor_capacity < OC_DYNAMIC_SEGMENT_BYTES ||
        layout_bytes > SIZE_MAX - OC_ALIAS_LAYOUT_ALIGNMENT)
      return 0;
    const size_t search_size =
      layout_bytes + OC_ALIAS_LAYOUT_ALIGNMENT;
    virtmemLock();
    void *raw = virtmemFindCodeMemory(search_size, 0);
    VirtmemReservation *reservation = raw
      ? virtmemAddReservation(raw, search_size) : NULL;
    uint8_t *selected = NULL;
    if (reservation &&
        (uintptr_t)raw <= UINTPTR_MAX - (OC_ALIAS_LAYOUT_ALIGNMENT - 1u)) {
      const uintptr_t aligned = ALIGN_MEM(
        (uintptr_t)raw, OC_ALIAS_LAYOUT_ALIGNMENT);
      if (aligned >= (uintptr_t)raw &&
          aligned - (uintptr_t)raw <= search_size &&
          layout_bytes <=
            search_size - (aligned - (uintptr_t)raw))
        selected = (uint8_t *)aligned;
    }
    if (!selected && reservation) {
      virtmemRemoveReservation(reservation);
      reservation = NULL;
    }
    virtmemUnlock();
    layout->base = selected;
    layout->reservation = reservation;
    return selected != NULL && reservation != NULL;
  }
  if (g_memory_backing_backend != NX_MEMORY_BACKEND_PHYSICAL)
    return 0;

  u64 alias_address = 0;
  u64 alias_size = 0;
  u64 alias_extra_size = 0;
  if (R_FAILED(svcGetInfo(&alias_address, InfoType_AliasRegionAddress,
                          CUR_PROCESS_HANDLE, 0)) ||
      R_FAILED(svcGetInfo(&alias_size, InfoType_AliasRegionSize,
                          CUR_PROCESS_HANDLE, 0)) ||
      !alias_address || alias_size < layout_bytes ||
      alias_address > UINTPTR_MAX || alias_size > UINTPTR_MAX - alias_address)
    return 0;
  if (R_SUCCEEDED(svcGetInfo(&alias_extra_size,
                             InfoType_AliasRegionExtraSize,
                             CUR_PROCESS_HANDLE, 0))) {
    if (alias_extra_size >= alias_size) return 0;
    alias_size -= alias_extra_size;
  }
  if (alias_size < layout_bytes) return 0;

  const uintptr_t region_begin = (uintptr_t)alias_address;
  const uintptr_t region_end = region_begin + (uintptr_t)alias_size;

  uint8_t *selected = NULL;
  VirtmemReservation *reservation = NULL;
  virtmemLock();
  for (uintptr_t at = region_begin; at < region_end; ) {
    MemoryInfo info;
    u32 page_info = 0;
    if (R_FAILED(svcQueryMemory(&info, &page_info, at)) ||
        info.size > UINTPTR_MAX - info.addr)
      break;
    const uintptr_t span_end = (uintptr_t)info.addr + (uintptr_t)info.size;
    if (span_end <= at) break;
    if (info.type == MemType_Unmapped) {
      const uintptr_t begin = (uintptr_t)info.addr < region_begin
        ? region_begin : (uintptr_t)info.addr;
      const uintptr_t end = span_end > region_end ? region_end : span_end;
      if (begin <= UINTPTR_MAX - (OC_ALIAS_LAYOUT_ALIGNMENT - 1u)) {
        uintptr_t candidate = ALIGN_MEM(begin, OC_ALIAS_LAYOUT_ALIGNMENT);
        while (candidate >= begin && candidate <= end &&
               layout_bytes <= end - candidate) {
          reservation = virtmemAddReservation(
            (void *)candidate, layout_bytes);
          if (reservation) {
            selected = (uint8_t *)candidate;
            break;
          }
          if (candidate > UINTPTR_MAX - OC_ALIAS_LAYOUT_ALIGNMENT) break;
          candidate += OC_ALIAS_LAYOUT_ALIGNMENT;
        }
        if (selected) break;
      }
    }
    at = span_end;
  }
  virtmemUnlock();

  layout->base = selected;
  layout->reservation = reservation;
  return selected != NULL && reservation != NULL;
}

static void oc_allocation_record_acquire_locked(void) {
  const uint64_t current = __atomic_add_fetch(
    &oc_allocation_slots_in_use, 1, __ATOMIC_RELAXED);
  oc_update_peak(&oc_peak_allocation_slots_in_use, current);
}

static void oc_allocation_record_release_locked(void) {
  const uint64_t current = __atomic_load_n(
    &oc_allocation_slots_in_use, __ATOMIC_RELAXED);
  if (current)
    __atomic_store_n(&oc_allocation_slots_in_use, current - 1u,
                     __ATOMIC_RELAXED);
}

static void oc_allocation_record_conflict_locked(void) {
  __atomic_add_fetch(&oc_allocation_slot_exhaustions, 1,
                     __ATOMIC_RELAXED);
}

static uint32_t oc_extent_node_take_locked(void) {
  const uint32_t node = oc_free_extent_node_head;
  if (!node || !oc_free_extent_node_count) return 0;
  oc_free_extent_node_head = oc_dynamic_extents[node].next;
  --oc_free_extent_node_count;
  memset(&oc_dynamic_extents[node], 0, sizeof(oc_dynamic_extents[node]));
  return node;
}

static void oc_extent_node_return_locked(uint32_t node) {
  if (!node || node > oc_dynamic_extent_capacity ||
      oc_free_extent_node_count >= oc_dynamic_extent_capacity)
    return;
  memset(&oc_dynamic_extents[node], 0, sizeof(oc_dynamic_extents[node]));
  oc_dynamic_extents[node].next = oc_free_extent_node_head;
  oc_free_extent_node_head = node;
  ++oc_free_extent_node_count;
}

static size_t oc_donor_heap_quantum_units(void) {
  return ((size_t)2 * 1024 * 1024) / OC_HEAP_DONOR_UNIT_BYTES;
}

static uint8_t *oc_donor_source_from_encoded(uint32_t encoded) {
  if (!encoded || !g_heap_donor_base) return NULL;
  const size_t first = (size_t)encoded - 1u;
  if (first >= oc_donor_unit_capacity) return NULL;
  return (uint8_t *)g_heap_donor_base +
    first * OC_HEAP_DONOR_UNIT_BYTES;
}

static int oc_donor_resize_locked(size_t new_active_units) {
  if (g_memory_backing_backend != NX_MEMORY_BACKEND_HEAP_ALIAS ||
      !g_kernel_heap_base || !g_heap_donor_base ||
      new_active_units > oc_donor_unit_capacity)
    return 0;
  const size_t quantum_units = oc_donor_heap_quantum_units();
  if (!quantum_units || new_active_units % quantum_units) return 0;
  if (new_active_units > SIZE_MAX / OC_HEAP_DONOR_UNIT_BYTES)
    return 0;
  const size_t new_active =
    new_active_units * OC_HEAP_DONOR_UNIT_BYTES;
  if (g_heap_donor_kernel_offset > SIZE_MAX - new_active)
    return 0;
  const size_t new_kernel_size =
    g_heap_donor_kernel_offset + new_active;
  const size_t old_active_units = oc_donor_active_units;
  const size_t old_kernel_size = g_kernel_heap_bytes;
  void *heap_base = NULL;
  const Result result = svcSetHeapSize(&heap_base, new_kernel_size);
  __atomic_store_n(&oc_donor_last_resize_result, (uint32_t)result,
                   __ATOMIC_RELAXED);
  if (new_active_units > old_active_units)
    __atomic_add_fetch(&oc_donor_grow_calls, 1, __ATOMIC_RELAXED);
  else if (new_active_units < old_active_units)
    __atomic_add_fetch(&oc_donor_shrink_calls, 1, __ATOMIC_RELAXED);
  if (R_FAILED(result) || heap_base != (void *)g_kernel_heap_base)
    return 0;
  if (new_active_units > old_active_units) {
    uint8_t *added = (uint8_t *)g_heap_donor_base +
      old_active_units * OC_HEAP_DONOR_UNIT_BYTES;
    const size_t added_bytes =
      (new_active_units - old_active_units) * OC_HEAP_DONOR_UNIT_BYTES;
    if (!oc_address_range_state(added, added_bytes,
                                Perm_Rw, MemType_Heap)) {
      void *rollback_base = NULL;
      const Result rollback = svcSetHeapSize(&rollback_base,
                                             old_kernel_size);
      __atomic_store_n(&oc_donor_last_resize_result, (uint32_t)rollback,
                       __ATOMIC_RELAXED);
      return 0;
    }
  }
  oc_donor_active_units = new_active_units;
  __atomic_store_n(&g_heap_donor_active_bytes, new_active,
                   __ATOMIC_RELEASE);
  __atomic_store_n(&g_kernel_heap_bytes, new_kernel_size,
                   __ATOMIC_RELEASE);
  return 1;
}

static int oc_donor_grow_locked(size_t minimum_extra_units) {
  if (oc_donor_active_units >= oc_donor_unit_capacity) return 0;
  const size_t grow_units =
    OC_HEAP_DONOR_GROW_BYTES / OC_HEAP_DONOR_UNIT_BYTES;
  const size_t quantum_units = oc_donor_heap_quantum_units();
  if (!grow_units || !quantum_units) return 0;
  size_t target = oc_donor_active_units;
  const size_t addition = minimum_extra_units > grow_units
    ? minimum_extra_units : grow_units;
  if (target > SIZE_MAX - addition)
    target = oc_donor_unit_capacity;
  else
    target += addition;
  if (target > oc_donor_unit_capacity)
    target = oc_donor_unit_capacity;
  target &= ~(quantum_units - 1u);
  if (target <= oc_donor_active_units) return 0;
  return oc_donor_resize_locked(target);
}

static void oc_donor_try_shrink_locked(void) {
  if (g_memory_backing_backend != NX_MEMORY_BACKEND_HEAP_ALIAS ||
      !oc_donor_units_used || !oc_donor_active_units)
    return;
  size_t highest = 0;
  for (size_t unit = oc_donor_active_units; unit > 0; --unit) {
    if (oc_donor_units_used[unit - 1u]) {
      highest = unit;
      break;
    }
  }
  const size_t quantum_units = oc_donor_heap_quantum_units();
  size_t floor_units =
    OC_HEAP_DONOR_INITIAL_BYTES / OC_HEAP_DONOR_UNIT_BYTES;
  if (floor_units > oc_donor_unit_capacity)
    floor_units = oc_donor_unit_capacity;
  size_t target = highest > floor_units ? highest : floor_units;
  if (target > SIZE_MAX - (quantum_units - 1u)) return;
  target = (target + quantum_units - 1u) & ~(quantum_units - 1u);
  if (target > oc_donor_unit_capacity)
    target = oc_donor_unit_capacity;
  const size_t shrink_units =
    OC_HEAP_DONOR_SHRINK_BYTES / OC_HEAP_DONOR_UNIT_BYTES;
  if (target >= oc_donor_active_units ||
      oc_donor_active_units - target < shrink_units)
    return;
  (void)oc_donor_resize_locked(target);
}

static size_t oc_donor_find_free_locked(size_t units,
                                        size_t alignment_units,
                                        int high) {
  if (!units || !alignment_units ||
      (alignment_units & (alignment_units - 1u)) ||
      units > oc_donor_active_units)
    return SIZE_MAX;
  if (!high) {
    size_t first = 0;
    while (first <= oc_donor_active_units - units) {
      first = (first + alignment_units - 1u) &
              ~(alignment_units - 1u);
      if (first > oc_donor_active_units - units) break;
      size_t used = 0;
      while (used < units && !oc_donor_units_used[first + used]) ++used;
      if (used == units) return first;
      first += used + 1u;
    }
    return SIZE_MAX;
  }

  size_t first = (oc_donor_active_units - units) &
                 ~(alignment_units - 1u);
  for (;;) {
    size_t used = 0;
    while (used < units && !oc_donor_units_used[first + used]) ++used;
    if (used == units) return first;
    if (first < alignment_units) break;
    first = (first - alignment_units) & ~(alignment_units - 1u);
  }
  return SIZE_MAX;
}

static uint32_t oc_donor_allocate_locked(size_t units,
                                         size_t alignment_units,
                                         int high) {
  if (g_memory_backing_backend != NX_MEMORY_BACKEND_HEAP_ALIAS ||
      !oc_donor_units_used || !units ||
      units > oc_donor_unit_capacity)
    return 0;
  for (;;) {
    const size_t first = oc_donor_find_free_locked(
      units, alignment_units, high);
    if (first != SIZE_MAX) {
      memset(oc_donor_units_used + first, 1, units);
      const uint64_t current = __atomic_add_fetch(
        &oc_donor_used_units, units, __ATOMIC_RELAXED);
      oc_update_peak(&oc_peak_donor_used_units, current);
      return (uint32_t)(first + 1u);
    }
    if (!oc_donor_grow_locked(units)) return 0;
  }
}

static void oc_donor_release_locked(uint32_t encoded, size_t units) {
  if (!encoded || !units || !oc_donor_units_used) return;
  const size_t first = (size_t)encoded - 1u;
  if (first >= oc_donor_active_units ||
      units > oc_donor_active_units - first)
    return;
  for (size_t unit = first; unit < first + units; ++unit)
    if (!oc_donor_units_used[unit])
      fatal_error("Heap donor ownership underflow at unit %u.",
                  (unsigned)unit);
  memset(oc_donor_units_used + first, 0, units);
  const uint64_t current = __atomic_load_n(
    &oc_donor_used_units, __ATOMIC_RELAXED);
  __atomic_store_n(&oc_donor_used_units,
                   current >= units ? current - units : 0,
                   __ATOMIC_RELAXED);
  oc_donor_try_shrink_locked();
}

static Result oc_backing_map_locked(void *destination, size_t size,
                                    size_t donor_alignment_units,
                                    int donor_high,
                                    uint32_t *source_out) {
  if (source_out) *source_out = 0;
  Result result = MAKERESULT(Module_Libnx, LibnxError_HeapAllocFailed);
  uint32_t encoded = 0;
  if (g_memory_backing_backend == NX_MEMORY_BACKEND_PHYSICAL) {
    result = svcMapPhysicalMemory(destination, size);
  } else if (g_memory_backing_backend == NX_MEMORY_BACKEND_HEAP_ALIAS &&
             size % OC_HEAP_DONOR_UNIT_BYTES == 0) {
    const size_t units = size / OC_HEAP_DONOR_UNIT_BYTES;
    encoded = oc_donor_allocate_locked(
      units, donor_alignment_units, donor_high);
    uint8_t *source = oc_donor_source_from_encoded(encoded);
    const Handle process = envGetOwnProcessHandle();
    if (source && process != INVALID_HANDLE &&
        oc_address_range_state(source, size, Perm_Rw, MemType_Heap)) {
      result = svcMapProcessCodeMemory(
        process, (u64)destination, (u64)source, size);
      if (R_SUCCEEDED(result)) {
        const Result permission = svcSetProcessMemoryPermission(
          process, (u64)destination, size, Perm_Rw);
        if (R_FAILED(permission) ||
            !oc_address_range_state(destination, size, Perm_Rw,
                                    MemType_ModuleCodeMutable)) {
          const Result undo = svcUnmapProcessCodeMemory(
            process, (u64)destination, (u64)source, size);
          if (R_FAILED(undo))
            fatal_error("Heap-donor code-alias rollback failed: map=0x%08x permission=0x%08x unmap=0x%08x.",
                        (unsigned)result, (unsigned)permission,
                        (unsigned)undo);
          result = R_FAILED(permission)
            ? permission
            : MAKERESULT(Module_Libnx, LibnxError_BadQueryMemory);
        }
      }
    }
    if (R_FAILED(result) && encoded)
      oc_donor_release_locked(encoded, units);
  }
  __atomic_add_fetch(&oc_map_call_count, 1, __ATOMIC_RELAXED);
  __atomic_store_n(&oc_last_map_result, (uint32_t)result,
                   __ATOMIC_RELAXED);
  if (R_SUCCEEDED(result) && source_out) *source_out = encoded;
  return result;
}

/* One-shot diagnostic for the first code-alias unmap failure.  Write the
 * destination's queried memory state straight to fd 2 (stderr.txt) so the data
 * survives without allocating or re-entering the broker lock. */
static void oc_backing_unmap_diag_once(void *destination, size_t size,
                                       void *source_address,
                                       Result unmap_result) {
  static _Atomic int once = 0;
  if (__atomic_exchange_n(&once, 1, __ATOMIC_ACQ_REL)) return;
  MemoryInfo mi;
  u32 pi = 0;
  Result q = svcQueryMemory(&mi, &pi, (u64)destination);
  char buf[256];
  int n = snprintf(buf, sizeof buf,
    "backing_unmap_fail dst=%p src=%p size=0x%zx unmap=0x%08x "
    "query=0x%08x dst[addr=0x%lx size=0x%lx type=%u perm=0x%x attr=0x%x ip=0x%x]\n",
    destination, source_address, size,
    (unsigned)unmap_result, (unsigned)q,
    (unsigned long)mi.addr, (unsigned long)mi.size,
    (unsigned)mi.type, (unsigned)mi.perm, (unsigned)mi.attr, (unsigned)pi);
  if (n > 0) (void)write(2, buf, (size_t)(n < (int)sizeof buf ? n : (int)sizeof buf - 1));
}

static Result oc_backing_unmap_locked(void *destination, size_t size,
                                      uint32_t source) {
  if (g_memory_backing_backend == NX_MEMORY_BACKEND_PHYSICAL)
    return svcUnmapPhysicalMemory(destination, size);
  if (g_memory_backing_backend != NX_MEMORY_BACKEND_HEAP_ALIAS ||
      !source || size % OC_HEAP_DONOR_UNIT_BYTES != 0)
    return MAKERESULT(Module_Libnx, LibnxError_BadInput);
  uint8_t *source_address = oc_donor_source_from_encoded(source);
  if (!source_address)
    return MAKERESULT(Module_Libnx, LibnxError_BadInput);
  const Handle process = envGetOwnProcessHandle();
  if (process == INVALID_HANDLE)
    return MAKERESULT(Module_Libnx, LibnxError_BadInput);
  /* svcSetProcessMemoryPermission(Perm_Rw) changes AliasCode into the writable
   * AliasCodeData state.  That state no longer carries FlagCode, so trying to
   * reset its permission (including Perm_None) returns InvalidMemoryState.
   * UnmapCodeMemory accepts AliasCodeData directly and restores the locked
   * source heap pages to ordinary RW memory; this is also libnx's JIT teardown
   * sequence. */
  const Result result = svcUnmapProcessCodeMemory(
    process, (u64)destination, (u64)source_address, size);
  if (R_FAILED(result)) {
    __atomic_add_fetch(&oc_backing_unmap_fail, 1, __ATOMIC_RELAXED);
    oc_backing_unmap_diag_once(destination, size, source_address, result);
    return result;
  }
  __atomic_add_fetch(&oc_backing_unmap_ok, 1, __ATOMIC_RELAXED);
  oc_donor_release_locked(source,
    size / OC_HEAP_DONOR_UNIT_BYTES);
  return result;
}

static uint32_t oc_dynamic_find_extent_locked(size_t need, size_t alignment,
                                               size_t *first_out) {
  uint32_t best = 0;
  size_t best_first = 0;
  size_t best_waste = SIZE_MAX;
  for (uint32_t node = oc_dynamic_extent_head; node;
       node = oc_dynamic_extents[node].next) {
    const OcDynamicExtent *extent = &oc_dynamic_extents[node];
    if (need > extent->pages) continue;
    const size_t latest_first =
      (size_t)extent->first_page + extent->pages - need;
    const uintptr_t latest = (uintptr_t)oc_dynamic_base +
                             latest_first * MMAP_PAGE;
    const uintptr_t aligned = latest & ~(uintptr_t)(alignment - 1u);
    if (aligned < (uintptr_t)oc_dynamic_base) continue;
    const size_t first = (size_t)(
      (aligned - (uintptr_t)oc_dynamic_base) / MMAP_PAGE);
    if (first < extent->first_page || first > latest_first) continue;
    const size_t waste = extent->pages - need;
    if (waste < best_waste || (waste == best_waste && first > best_first)) {
      best = node;
      best_first = first;
      best_waste = waste;
      if (!waste) break;
    }
  }
  if (best && first_out) *first_out = best_first;
  return best;
}

/* Sparse reservations consume complete, currently unmapped 8 MiB segments
 * from the low end of the same extent map. Dynamic allocations pack from the
 * high end above, so neither class strands a fixed-capacity sibling pool. */
static uint32_t oc_sparse_find_extent_locked(size_t need, size_t *first_out) {
  const size_t alignment_pages = MMAP_BIG_ALIGN / MMAP_PAGE;
  for (uint32_t node = oc_dynamic_extent_head; node;
       node = oc_dynamic_extents[node].next) {
    const OcDynamicExtent *extent = &oc_dynamic_extents[node];
    const size_t extent_first = extent->first_page;
    const size_t extent_end = extent_first + extent->pages;
    if (need > extent->pages ||
        extent_first > SIZE_MAX - (alignment_pages - 1u))
      continue;
    size_t first = (extent_first + alignment_pages - 1u) &
                   ~(alignment_pages - 1u);
    while (first <= extent_end && need <= extent_end - first) {
      const size_t first_segment = first / OC_DYNAMIC_SEGMENT_PAGES;
      const size_t last_segment =
        (first + need - 1u) / OC_DYNAMIC_SEGMENT_PAGES;
      size_t mapped_segment = oc_dynamic_segments;
      for (size_t segment = first_segment; segment <= last_segment; ++segment) {
        if (oc_dynamic_segment_mapped[segment]) {
          mapped_segment = segment;
          break;
        }
      }
      if (mapped_segment == oc_dynamic_segments &&
          oc_address_range_state(oc_dynamic_base + first * MMAP_PAGE,
                                 need * MMAP_PAGE, 0, MemType_Unmapped)) {
        if (first_out) *first_out = first;
        return node;
      }
      size_t next = mapped_segment == oc_dynamic_segments
        ? first + alignment_pages
        : (mapped_segment + 1u) * OC_DYNAMIC_SEGMENT_PAGES;
      if (next > SIZE_MAX - (alignment_pages - 1u)) break;
      first = (next + alignment_pages - 1u) & ~(alignment_pages - 1u);
    }
  }
  return 0;
}

static int oc_dynamic_commit_segments_locked(size_t first, size_t pages) {
  if (!pages || first >= oc_dynamic_pages ||
      pages > oc_dynamic_pages - first)
    return 0;
  const size_t first_segment = first / OC_DYNAMIC_SEGMENT_PAGES;
  const size_t last_segment =
    (first + pages - 1u) / OC_DYNAMIC_SEGMENT_PAGES;
  uint8_t newly_mapped[OC_DYNAMIC_MAX_SEGMENTS];
  memset(newly_mapped, 0, sizeof(newly_mapped));

  for (size_t segment = first_segment; segment <= last_segment; ++segment) {
    if (oc_dynamic_segment_mapped[segment]) continue;
    void *address = oc_dynamic_base +
      segment * OC_DYNAMIC_SEGMENT_BYTES;
    uint32_t source = 0;
    const Result result = oc_backing_map_locked(
      address, OC_DYNAMIC_SEGMENT_BYTES,
      OC_DONOR_UNITS_PER_SEGMENT, 1, &source);
    if (R_FAILED(result)) {
      for (size_t rollback = first_segment;
           rollback <= last_segment; ++rollback) {
        if (!newly_mapped[rollback]) continue;
        Result undo = oc_backing_unmap_locked(
          oc_dynamic_base + rollback * OC_DYNAMIC_SEGMENT_BYTES,
          OC_DYNAMIC_SEGMENT_BYTES,
          oc_dynamic_segment_sources[rollback]);
        if (R_FAILED(undo))
          fatal_error("Dynamic memory rollback failed: 0x%08x.",
                      (unsigned)undo);
        oc_dynamic_segment_mapped[rollback] = 0;
        oc_dynamic_segment_sources[rollback] = 0;
        const uint64_t mapped = __atomic_load_n(
          &oc_dynamic_mapped_segments, __ATOMIC_RELAXED);
        __atomic_store_n(&oc_dynamic_mapped_segments,
          mapped ? mapped - 1u : 0,
          __ATOMIC_RELAXED);
      }
      return 0;
    }
    oc_dynamic_segment_sources[segment] = source;
    oc_dynamic_segment_mapped[segment] = 1;
    newly_mapped[segment] = 1;
    const uint64_t mapped = __atomic_add_fetch(
      &oc_dynamic_mapped_segments, 1, __ATOMIC_RELAXED);
    oc_update_peak(&oc_peak_dynamic_mapped_segments, mapped);
  }

  const size_t end = first + pages;
  for (size_t segment = first_segment; segment <= last_segment; ++segment) {
    const size_t segment_first = segment * OC_DYNAMIC_SEGMENT_PAGES;
    const size_t segment_end = segment_first + OC_DYNAMIC_SEGMENT_PAGES;
    const size_t overlap_first = first > segment_first ? first : segment_first;
    const size_t overlap_end = end < segment_end ? end : segment_end;
    oc_dynamic_segment_live_pages[segment] +=
      (uint32_t)(overlap_end - overlap_first);
  }
  return 1;
}

static void oc_dynamic_release_segments_locked(size_t first, size_t pages) {
  const size_t end = first + pages;
  const size_t first_segment = first / OC_DYNAMIC_SEGMENT_PAGES;
  const size_t last_segment = (end - 1u) / OC_DYNAMIC_SEGMENT_PAGES;
  for (size_t segment = first_segment; segment <= last_segment; ++segment) {
    const size_t segment_first = segment * OC_DYNAMIC_SEGMENT_PAGES;
    const size_t segment_end = segment_first + OC_DYNAMIC_SEGMENT_PAGES;
    const size_t overlap_first = first > segment_first ? first : segment_first;
    const size_t overlap_end = end < segment_end ? end : segment_end;
    const uint32_t owned = (uint32_t)(overlap_end - overlap_first);
    if (oc_dynamic_segment_live_pages[segment] < owned)
      fatal_error("Dynamic segment ownership underflow.");
    oc_dynamic_segment_live_pages[segment] -= owned;
  }

  for (size_t segment = first_segment; segment <= last_segment; ++segment) {
    if (!oc_dynamic_segment_mapped[segment] ||
        oc_dynamic_segment_live_pages[segment])
      continue;
    Result result = oc_backing_unmap_locked(
      oc_dynamic_base + segment * OC_DYNAMIC_SEGMENT_BYTES,
      OC_DYNAMIC_SEGMENT_BYTES,
      oc_dynamic_segment_sources[segment]);
    if (R_FAILED(result)) {
      /* If Horizon cannot unmap this exact source/destination range, preserve
       * the live mapping and donor ownership.  The pages are already back in
       * the free extent map, so future allocations can reuse the backing
       * without creating an overlapping alias. */
      continue;
    }
    oc_dynamic_segment_mapped[segment] = 0;
    oc_dynamic_segment_sources[segment] = 0;
    const uint64_t mapped = __atomic_load_n(
      &oc_dynamic_mapped_segments, __ATOMIC_RELAXED);
    __atomic_store_n(&oc_dynamic_mapped_segments,
      mapped ? mapped - 1u : 0,
      __ATOMIC_RELAXED);
  }
}

static int oc_dynamic_consume_extent_locked(uint32_t node, size_t first,
                                            size_t pages) {
  OcDynamicExtent *extent = &oc_dynamic_extents[node];
  const size_t extent_first = extent->first_page;
  const size_t extent_end = extent_first + extent->pages;
  const size_t allocation_end = first + pages;
  const size_t prefix = first - extent_first;
  const size_t suffix = extent_end - allocation_end;
  if (prefix && suffix) {
    const uint32_t suffix_node = oc_extent_node_take_locked();
    if (!suffix_node) return 0;
    OcDynamicExtent *tail = &oc_dynamic_extents[suffix_node];
    tail->first_page = (uint32_t)allocation_end;
    tail->pages = (uint32_t)suffix;
    tail->previous = node;
    tail->next = extent->next;
    if (tail->next) oc_dynamic_extents[tail->next].previous = suffix_node;
    extent->next = suffix_node;
    extent->pages = (uint32_t)prefix;
  } else if (prefix) {
    extent->pages = (uint32_t)prefix;
  } else if (suffix) {
    extent->first_page = (uint32_t)allocation_end;
    extent->pages = (uint32_t)suffix;
  } else {
    if (extent->previous)
      oc_dynamic_extents[extent->previous].next = extent->next;
    else
      oc_dynamic_extent_head = extent->next;
    if (extent->next)
      oc_dynamic_extents[extent->next].previous = extent->previous;
    oc_extent_node_return_locked(node);
  }
  return 1;
}

static void oc_dynamic_return_extent_locked(size_t first, size_t pages) {
  uint32_t previous = 0;
  uint32_t next = oc_dynamic_extent_head;
  while (next && oc_dynamic_extents[next].first_page < first) {
    previous = next;
    next = oc_dynamic_extents[next].next;
  }
  uint32_t node = 0;
  if (previous &&
      (size_t)oc_dynamic_extents[previous].first_page +
        oc_dynamic_extents[previous].pages == first) {
    node = previous;
    oc_dynamic_extents[node].pages += (uint32_t)pages;
  } else {
    node = oc_extent_node_take_locked();
    if (!node) fatal_error("Dynamic free-extent metadata exhausted.");
    OcDynamicExtent *extent = &oc_dynamic_extents[node];
    extent->first_page = (uint32_t)first;
    extent->pages = (uint32_t)pages;
    extent->previous = previous;
    extent->next = next;
    if (previous) oc_dynamic_extents[previous].next = node;
    else oc_dynamic_extent_head = node;
    if (next) oc_dynamic_extents[next].previous = node;
  }
  next = oc_dynamic_extents[node].next;
  if (next &&
      (size_t)oc_dynamic_extents[node].first_page +
        oc_dynamic_extents[node].pages ==
          oc_dynamic_extents[next].first_page) {
    oc_dynamic_extents[node].pages += oc_dynamic_extents[next].pages;
    oc_dynamic_extents[node].next = oc_dynamic_extents[next].next;
    if (oc_dynamic_extents[next].next)
      oc_dynamic_extents[oc_dynamic_extents[next].next].previous = node;
    oc_extent_node_return_locked(next);
  }
}

static void *oc_pool_owned_alloc(size_t size, size_t alignment,
                                 OcPoolOwner owner) {
  if (!size || !alignment || (alignment & (alignment - 1u)) ||
      size > SIZE_MAX - (MMAP_PAGE - 1u)) {
    oc_pool_record_failure(owner);
    return NULL;
  }
  if (alignment < MMAP_PAGE) alignment = MMAP_PAGE;

  /* A donor-backed destination is MemType_MappedMemory and cannot itself be
   * borrowed by libnx for a caller-owned pthread stack.  Give that owner a
   * direct RW slice of the donor heap; threadClose restores it before release. */
  if (owner == OC_POOL_OWNER_THREAD &&
      g_memory_backing_backend == NX_MEMORY_BACKEND_HEAP_ALIAS) {
    if (size > SIZE_MAX - (OC_HEAP_DONOR_UNIT_BYTES - 1u) ||
        mutexIsLockedByCurrentThread(&g_mmap_lock)) {
      oc_pool_record_failure(owner);
      return NULL;
    }
    const size_t units =
      (size + OC_HEAP_DONOR_UNIT_BYTES - 1u) /
        OC_HEAP_DONOR_UNIT_BYTES;
    if (!units || units > UINT32_MAX) {
      oc_pool_record_failure(owner);
      return NULL;
    }
    mmap_broker_lock();
    const uint32_t source = oc_donor_allocate_locked(units, 1, 1);
    uint8_t *result = oc_donor_source_from_encoded(source);
    const size_t first_unit = source ? (size_t)source - 1u : SIZE_MAX;
    OcDonorAllocation *record = source && oc_donor_allocations &&
        first_unit < oc_donor_unit_capacity
      ? &oc_donor_allocations[first_unit] : NULL;
    if (!source || !result || !record ||
        !oc_address_range_state(result,
          units * OC_HEAP_DONOR_UNIT_BYTES, Perm_Rw, MemType_Heap)) {
      if (source) oc_donor_release_locked(source, units);
      mmap_broker_unlock();
      oc_pool_record_failure(owner);
      return NULL;
    }
    if (record->units) {
      oc_allocation_record_conflict_locked();
      oc_donor_release_locked(source, units);
      mmap_broker_unlock();
      oc_pool_record_failure(owner);
      return NULL;
    }
    record->requested = size;
    record->units = (uint32_t)units;
    oc_allocation_record_acquire_locked();
    const size_t pages =
      units * OC_HEAP_DONOR_UNIT_BYTES / MMAP_PAGE;
    const uint64_t current = __atomic_add_fetch(
      &oc_thread_pool_pages, pages, __ATOMIC_RELAXED);
    oc_update_peak(&oc_peak_thread_pool_pages, current);
    mmap_broker_unlock();
    return result;
  }

  const size_t pages = (size + MMAP_PAGE - 1u) / MMAP_PAGE;
  if (!pages || pages > oc_dynamic_pages ||
      mutexIsLockedByCurrentThread(&g_mmap_lock)) {
    oc_pool_record_failure(owner);
    return NULL;
  }

  mmap_broker_lock();
  size_t first = 0;
  const uint32_t extent =
    oc_dynamic_find_extent_locked(pages, alignment, &first);
  if (!extent) {
    mmap_broker_unlock();
    oc_pool_record_failure(owner);
    return NULL;
  }
  OcDynamicAllocation *record = &oc_dynamic_allocations[first];
  if (record->pages) {
    oc_allocation_record_conflict_locked();
    mmap_broker_unlock();
    oc_pool_record_failure(owner);
    return NULL;
  }
  if (!oc_dynamic_commit_segments_locked(first, pages)) {
    mmap_broker_unlock();
    oc_pool_record_failure(owner);
    return NULL;
  }
  if (!oc_dynamic_consume_extent_locked(extent, first, pages)) {
    /* Segment commit raises live-page counts only after every physical map
     * succeeds.  Roll the complete transaction back if free-extent metadata
     * cannot represent the aligned split. */
    oc_dynamic_release_segments_locked(first, pages);
    mmap_broker_unlock();
    oc_pool_record_failure(owner);
    return NULL;
  }

  void *result = oc_dynamic_base + first * MMAP_PAGE;
  record->pages = (uint32_t)pages;
  record->requested = size;
  record->owner = (uint32_t)owner;
  oc_allocation_record_acquire_locked();
  uint64_t *owner_pages = oc_pool_owner_pages(owner);
  uint64_t *owner_peak = oc_pool_owner_peak(owner);
  if (owner_pages && owner_peak) {
    const uint64_t current = __atomic_add_fetch(
      owner_pages, pages, __ATOMIC_RELAXED);
    oc_update_peak(owner_peak, current);
  }
  mmap_broker_unlock();
  return result;
}

static int oc_pool_owned_lookup_locked(
    const void *pointer, OcPoolAllocationView *view) {
  if (view) memset(view, 0, sizeof(*view));
  if (!pointer) return 0;
  const uintptr_t address = (uintptr_t)pointer;
  const uintptr_t dynamic_begin = (uintptr_t)oc_dynamic_base;
  const size_t dynamic_bytes = oc_dynamic_pages * MMAP_PAGE;
  if (dynamic_begin &&
      dynamic_bytes <= UINTPTR_MAX - dynamic_begin &&
      oc_dynamic_metadata_ready &&
      address >= dynamic_begin &&
      address < dynamic_begin + dynamic_bytes &&
      !((address - dynamic_begin) & (MMAP_PAGE - 1u))) {
    const size_t first = (address - dynamic_begin) / MMAP_PAGE;
    const OcDynamicAllocation *record = &oc_dynamic_allocations[first];
    const size_t usable = (size_t)record->pages * MMAP_PAGE;
    if (!record->pages || record->pages > oc_dynamic_pages - first ||
        !record->requested || record->requested > usable ||
        record->owner < OC_POOL_OWNER_GUEST ||
        record->owner > OC_POOL_OWNER_THREAD)
      return 0;
    if (view) {
      view->first = first;
      view->pages = record->pages;
      view->requested = record->requested;
      view->owner = (OcPoolOwner)record->owner;
    }
    return 1;
  }

  const uintptr_t donor_begin = (uintptr_t)g_heap_donor_base;
  if (!donor_begin || !oc_donor_allocations ||
      g_heap_donor_capacity > UINTPTR_MAX - donor_begin ||
      address < donor_begin ||
      address >= donor_begin + g_heap_donor_capacity ||
      ((address - donor_begin) & (OC_HEAP_DONOR_UNIT_BYTES - 1u)))
    return 0;
  const size_t first =
    (address - donor_begin) / OC_HEAP_DONOR_UNIT_BYTES;
  const OcDonorAllocation *record = &oc_donor_allocations[first];
  const size_t usable = (size_t)record->units * OC_HEAP_DONOR_UNIT_BYTES;
  if (!record->units || record->units > oc_donor_unit_capacity - first ||
      !record->requested || record->requested > usable)
    return 0;
  if (view) {
    view->first = first;
    view->pages = usable / MMAP_PAGE;
    view->donor_units = record->units;
    view->requested = record->requested;
    view->owner = OC_POOL_OWNER_THREAD;
    view->direct_donor = 1;
  }
  return 1;
}

static int oc_pool_owned_release(void *pointer, OcPoolOwner expected_owner) {
  if (!pointer || mutexIsLockedByCurrentThread(&g_mmap_lock)) return 0;
  mmap_broker_lock();
  OcPoolAllocationView allocation;
  if (!oc_pool_owned_lookup_locked(pointer, &allocation) ||
      (expected_owner != OC_POOL_OWNER_NONE &&
       allocation.owner != expected_owner)) {
    mmap_broker_unlock();
    return 0;
  }
  const OcPoolOwner owner = allocation.owner;
  const size_t first = allocation.first;
  const size_t pages = allocation.pages;
  /* A libnx caller-owned stack temporarily borrows its source pages.  Never
   * recycle or decommit them until pthread_join/threadClose restores RW heap. */
  const MemoryType mapped_type = allocation.direct_donor
    ? MemType_Heap : nx_memory_backing_mapped_type();
  if (!oc_address_range_state(pointer, pages * MMAP_PAGE,
                              Perm_Rw, mapped_type)) {
    mmap_broker_unlock();
    oc_pool_record_failure(owner);
    return 0;
  }

  const uint32_t donor_source = allocation.direct_donor
    ? (uint32_t)(first + 1u) : 0;
  const size_t donor_units = allocation.donor_units;
  if (allocation.direct_donor)
    memset(&oc_donor_allocations[first], 0,
           sizeof(oc_donor_allocations[first]));
  else
    memset(&oc_dynamic_allocations[first], 0,
           sizeof(oc_dynamic_allocations[first]));
  uint64_t *owner_pages = oc_pool_owner_pages(owner);
  if (owner_pages) {
    const uint64_t current = __atomic_load_n(owner_pages, __ATOMIC_RELAXED);
    __atomic_store_n(owner_pages, current >= pages ? current - pages : 0,
                     __ATOMIC_RELAXED);
  }
  oc_allocation_record_release_locked();
  if (allocation.direct_donor) {
    oc_donor_release_locked(donor_source, donor_units);
  } else {
    oc_dynamic_return_extent_locked(first, pages);
    oc_dynamic_release_segments_locked(first, pages);
  }
  mmap_broker_unlock();
  return 1;
}

static int oc_pool_owned_query(const void *pointer, size_t *requested_out,
                               size_t *usable_out,
                               OcPoolOwner expected_owner) {
  if (requested_out) *requested_out = 0;
  if (usable_out) *usable_out = 0;
  if (!pointer || mutexIsLockedByCurrentThread(&g_mmap_lock)) return 0;
  mmap_broker_lock();
  OcPoolAllocationView allocation;
  const int found = oc_pool_owned_lookup_locked(pointer, &allocation);
  const MemoryType mapped_type = found && allocation.direct_donor
    ? MemType_Heap : nx_memory_backing_mapped_type();
  if (!found ||
      (expected_owner != OC_POOL_OWNER_NONE &&
      allocation.owner != expected_owner) ||
      !oc_address_range_state(pointer,
        allocation.pages * MMAP_PAGE, Perm_Rw, mapped_type)) {
    mmap_broker_unlock();
    return 0;
  }
  if (requested_out) *requested_out = allocation.requested;
  if (usable_out) *usable_out = allocation.pages * MMAP_PAGE;
  mmap_broker_unlock();
  return 1;
}

static int oc_alias_layout_ready_locked(void) {
  return oc_alias_layout_reservation &&
    oc_base && oc_pages && oc_pages == oc_dynamic_pages &&
    oc_used && oc_owned && oc_committed && oc_sparse_start_pages &&
    oc_sparse_granule_sources &&
    oc_dynamic_base && oc_dynamic_segments && oc_sparse_granules &&
    oc_dynamic_extent_capacity && oc_alias_layout_bytes &&
    oc_dynamic_segment_mapped && oc_dynamic_segment_live_pages &&
    oc_dynamic_segment_sources &&
    oc_dynamic_metadata_ready &&
    (g_memory_backing_backend == NX_MEMORY_BACKEND_PHYSICAL ||
     (g_memory_backing_backend == NX_MEMORY_BACKEND_HEAP_ALIAS &&
      oc_donor_units_used && oc_donor_allocations &&
      oc_donor_unit_capacity && oc_donor_active_units)) &&
    (unity_slab_prepared_base || unity_slab_state != UNITY_SLAB_EMPTY);
}

int nx_alias_memory_arenas_prepare(void) {
  mmap_broker_lock();
  const int already_ready = oc_alias_layout_ready_locked();
  const int partial_layout = !already_ready &&
    (oc_alias_layout_reservation || oc_base || oc_dynamic_base ||
     unity_slab_prepared_base || unity_slab_state != UNITY_SLAB_EMPTY);
  mmap_broker_unlock();
  if (already_ready) return 1;
  if (partial_layout) {
    g_oc_arena_failure_stage = "prepare:partial-layout";
    return 0;
  }

  int published = 0;
  int ready = 0;

  const size_t arena_bytes = oc_select_dynamic_arena_bytes();
  if (!arena_bytes || arena_bytes % MMAP_PAGE ||
      arena_bytes > OC_DYNAMIC_ARENA_BYTES ||
      arena_bytes > SIZE_MAX - UNITY_SLAB_RESERVATION_BYTES) {
    if (!g_oc_arena_failure_stage[0] ||
        !strncmp(g_oc_arena_failure_stage, "not attempted", 13) ||
        !strncmp(g_oc_arena_failure_stage, "prepare:", 8))
      g_oc_arena_failure_stage = "prepare:arena-bytes-invalid";
    return 0;
  }
  const size_t arena_pages = arena_bytes / MMAP_PAGE;
  const size_t arena_segments = arena_bytes / OC_DYNAMIC_SEGMENT_BYTES;
  const size_t sparse_granules =
    arena_bytes / OC_SPARSE_COMMIT_GRANULE_BYTES;
  const size_t extent_capacity = (arena_pages + 1u) / 2u;
  const size_t layout_bytes = arena_bytes + UNITY_SLAB_RESERVATION_BYTES;
  if (!arena_pages || arena_pages > OC_DYNAMIC_MAX_PAGES ||
      !arena_segments || arena_segments > OC_DYNAMIC_MAX_SEGMENTS ||
      !sparse_granules || sparse_granules > OC_SPARSE_MAX_GRANULES ||
      !extent_capacity ||
      extent_capacity > OC_DYNAMIC_MAX_EXTENT_CAPACITY ||
      (g_memory_backing_backend == NX_MEMORY_BACKEND_HEAP_ALIAS &&
       arena_bytes <= g_heap_donor_capacity)) {
    g_oc_arena_failure_stage = "prepare:bounds-check";
    return 0;
  }

  /* These records bootstrap the fallback allocator itself, so they must come
   * only from newlib's fixed heap and must never recurse into the dynamic arena
   * that is still being initialized. */
  uint8_t *used = (uint8_t *)nx_primary_calloc(arena_pages, 1);
  uint8_t *owned = (uint8_t *)nx_primary_calloc(arena_pages, 1);
  uint8_t *committed = (uint8_t *)nx_primary_calloc(arena_pages, 1);
  uint32_t *sparse_start_pages = (uint32_t *)nx_primary_calloc(
    arena_pages, sizeof(*sparse_start_pages));
  uint32_t *sparse_sources = (uint32_t *)nx_primary_calloc(
    sparse_granules, sizeof(*sparse_sources));
  uint8_t *segment_mapped =
    (uint8_t *)nx_primary_calloc(arena_segments, 1);
  uint32_t *segment_live = (uint32_t *)nx_primary_calloc(
    arena_segments, sizeof(*segment_live));
  uint32_t *segment_sources = (uint32_t *)nx_primary_calloc(
    arena_segments, sizeof(*segment_sources));
  size_t donor_capacity = 0;
  size_t donor_active = 0;
  uint8_t *donor_units = NULL;
  OcDonorAllocation *donor_allocations = NULL;
  if (g_memory_backing_backend == NX_MEMORY_BACKEND_HEAP_ALIAS &&
      g_heap_donor_capacity % OC_HEAP_DONOR_UNIT_BYTES == 0 &&
      g_heap_donor_active_bytes % OC_HEAP_DONOR_UNIT_BYTES == 0) {
    donor_capacity = g_heap_donor_capacity / OC_HEAP_DONOR_UNIT_BYTES;
    donor_active = g_heap_donor_active_bytes / OC_HEAP_DONOR_UNIT_BYTES;
    if (donor_capacity && donor_capacity <= UINT32_MAX && donor_active &&
        donor_active <= donor_capacity) {
      donor_units = (uint8_t *)nx_primary_calloc(donor_capacity, 1);
      donor_allocations = (OcDonorAllocation *)nx_primary_calloc(
        donor_capacity, sizeof(*donor_allocations));
    }
  }

  if (!used || !owned || !committed || !sparse_start_pages ||
      !sparse_sources || !segment_mapped || !segment_live ||
      !segment_sources ||
      (g_memory_backing_backend == NX_MEMORY_BACKEND_HEAP_ALIAS &&
       (!donor_units || !donor_allocations))) {
    g_oc_arena_failure_stage = "prepare:metadata-alloc";
    goto fail_metadata;
  }

  OcVirtualLayout layout;
  if (!oc_reserve_virtual_layout(&layout, layout_bytes)) {
    g_oc_arena_failure_stage = "prepare:reserve-layout";
    goto fail_metadata;
  }
  if (!oc_address_range_state(layout.base, layout_bytes,
                              0, MemType_Unmapped)) {
    g_oc_arena_failure_stage = "prepare:range-not-unmapped";
    virtmemLock();
    virtmemRemoveReservation(layout.reservation);
    virtmemUnlock();
    goto fail_metadata;
  }

  uint8_t *sparse_base = layout.base;
  uint8_t *dynamic_base = sparse_base;
  uint8_t *slab_base = dynamic_base + arena_bytes;
  const int partition_geometry_valid =
    ((uintptr_t)sparse_base & (OC_ALIAS_LAYOUT_ALIGNMENT - 1u)) == 0 &&
    ((uintptr_t)dynamic_base & (OC_DYNAMIC_SEGMENT_BYTES - 1u)) == 0 &&
    ((uintptr_t)slab_base & (UNITY_SLAB_ALIGNMENT - 1u)) == 0 &&
    slab_base + UNITY_SLAB_RESERVATION_BYTES ==
      sparse_base + layout_bytes;
  if (!partition_geometry_valid) {
    g_oc_arena_failure_stage = "prepare:partition-geometry";
    virtmemLock();
    virtmemRemoveReservation(layout.reservation);
    virtmemUnlock();
    goto fail_metadata;
  }

  mmap_broker_lock();
  if (!oc_alias_layout_reservation && !oc_base && !oc_dynamic_base &&
      !unity_slab_prepared_base && unity_slab_state == UNITY_SLAB_EMPTY) {
    oc_used = used;
    oc_owned = owned;
    oc_committed = committed;
    oc_sparse_start_pages = sparse_start_pages;
    oc_sparse_granule_sources = sparse_sources;
    oc_pages = arena_pages;
    oc_base = sparse_base;

    oc_dynamic_pages = arena_pages;
    oc_dynamic_segments = arena_segments;
    oc_sparse_granules = sparse_granules;
    oc_dynamic_extent_capacity = extent_capacity;
    oc_alias_layout_bytes = layout_bytes;
    oc_dynamic_segment_mapped = segment_mapped;
    oc_dynamic_segment_live_pages = segment_live;
    oc_dynamic_segment_sources = segment_sources;
    memset(oc_dynamic_allocations, 0,
           arena_pages * sizeof(oc_dynamic_allocations[0]));
    memset(oc_dynamic_extents, 0,
           (extent_capacity + 1u) * sizeof(oc_dynamic_extents[0]));
    oc_dynamic_extents[1].first_page = 0;
    oc_dynamic_extents[1].pages = (uint32_t)arena_pages;
    for (uint32_t node = 2u;
         node <= (uint32_t)extent_capacity; ++node)
      oc_dynamic_extents[node].next =
        node < (uint32_t)extent_capacity ? node + 1u : 0;
    oc_dynamic_extent_head = 1;
    oc_free_extent_node_head = 2u;
    oc_free_extent_node_count = extent_capacity - 1u;
    oc_dynamic_metadata_ready = 1;
    __atomic_store_n(&oc_allocation_slots_in_use, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&oc_peak_allocation_slots_in_use, 0,
                     __ATOMIC_RELAXED);
    __atomic_store_n(&oc_allocation_slot_exhaustions, 0,
                     __ATOMIC_RELAXED);
    oc_dynamic_base = dynamic_base;

    oc_donor_units_used = donor_units;
    oc_donor_allocations = donor_allocations;
    oc_donor_unit_capacity = donor_capacity;
    oc_donor_active_units = donor_active;
    __atomic_store_n(&oc_donor_used_units, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&oc_peak_donor_used_units, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&oc_donor_grow_calls, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&oc_donor_shrink_calls, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&oc_donor_last_resize_result, 0, __ATOMIC_RELAXED);

    unity_slab_prepared_base = slab_base;
    memset(unity_slab_donor_sources, 0,
           sizeof(unity_slab_donor_sources));
    oc_alias_layout_reservation = layout.reservation;
    published = 1;
  }
  ready = oc_alias_layout_ready_locked();
  mmap_broker_unlock();
  if (published) return 1;

  g_oc_arena_failure_stage = "prepare:lost-race";
  virtmemLock();
  virtmemRemoveReservation(layout.reservation);
  virtmemUnlock();

fail_metadata:
  nx_primary_free(used);
  nx_primary_free(owned);
  nx_primary_free(committed);
  nx_primary_free(sparse_start_pages);
  nx_primary_free(sparse_sources);
  nx_primary_free(segment_mapped);
  nx_primary_free(segment_live);
  nx_primary_free(segment_sources);
  nx_primary_free(donor_units);
  nx_primary_free(donor_allocations);
  return ready;
}

int nx_dynamic_arena_prepare(void) {
  return nx_alias_memory_arenas_prepare();
}

int nx_sparse_pool_guest_arena_prepare(void) {
  return oc_dynamic_base != NULL && oc_alias_layout_reservation != NULL;
}

void *nx_sparse_pool_spill_alloc(size_t size) {
  return nx_sparse_pool_spill_alloc_aligned(size, MMAP_PAGE);
}

void *nx_sparse_pool_spill_alloc_aligned(size_t size, size_t alignment) {
  return oc_pool_owned_alloc(size, alignment, OC_POOL_OWNER_GUEST);
}

void *nx_sparse_pool_host_alloc_aligned(size_t size, size_t alignment) {
  return oc_pool_owned_alloc(size, alignment, OC_POOL_OWNER_HOST);
}

void *nx_sparse_pool_thread_alloc(size_t size) {
  return oc_pool_owned_alloc(size, MMAP_PAGE, OC_POOL_OWNER_THREAD);
}

int nx_sparse_pool_spill_release(void *pointer) {
  return oc_pool_owned_release(pointer, OC_POOL_OWNER_GUEST);
}

int nx_sparse_pool_thread_release(void *pointer) {
  return oc_pool_owned_release(pointer, OC_POOL_OWNER_THREAD);
}

int nx_sparse_pool_owned_release(void *pointer) {
  return oc_pool_owned_release(pointer, OC_POOL_OWNER_NONE);
}

int nx_sparse_pool_spill_query(const void *pointer, size_t *requested_out,
                               size_t *usable_out) {
  return oc_pool_owned_query(pointer, requested_out, usable_out,
                             OC_POOL_OWNER_GUEST);
}

int nx_sparse_pool_owned_query(const void *pointer, size_t *requested_out,
                               size_t *usable_out) {
  return oc_pool_owned_query(pointer, requested_out, usable_out,
                             OC_POOL_OWNER_NONE);
}

int nx_sparse_pool_contains_address(const void *pointer) {
  const uintptr_t address = (uintptr_t)pointer;
  if (!pointer) return 0;
  const uintptr_t donor_begin = (uintptr_t)__atomic_load_n(
    &g_heap_donor_base, __ATOMIC_ACQUIRE);
  const size_t donor_capacity = __atomic_load_n(
    &g_heap_donor_capacity, __ATOMIC_ACQUIRE);
  if (donor_begin && donor_capacity <= UINTPTR_MAX - donor_begin &&
      address >= donor_begin && address < donor_begin + donor_capacity)
    return 1;
  const uintptr_t dynamic_begin = (uintptr_t)__atomic_load_n(
    &oc_dynamic_base, __ATOMIC_ACQUIRE);
  const size_t dynamic_pages = __atomic_load_n(
    &oc_dynamic_pages, __ATOMIC_ACQUIRE);
  if (dynamic_begin &&
      dynamic_pages <= (UINTPTR_MAX - dynamic_begin) / MMAP_PAGE &&
      address >= dynamic_begin &&
      address < dynamic_begin + dynamic_pages * MMAP_PAGE)
    return 1;
  const uintptr_t virtual_begin = (uintptr_t)__atomic_load_n(
    &oc_base, __ATOMIC_ACQUIRE);
  const size_t virtual_pages = __atomic_load_n(
    &oc_pages, __ATOMIC_ACQUIRE);
  return virtual_begin &&
    virtual_pages <= (UINTPTR_MAX - virtual_begin) / MMAP_PAGE &&
    address >= virtual_begin &&
    address < virtual_begin + virtual_pages * MMAP_PAGE;
}

void nx_sparse_arena_get_diagnostics(NxSparseArenaDiagnostics *out) {
  if (!out) return;
  memset(out, 0, sizeof(*out));
  out->virtual_capacity_bytes =
    __atomic_load_n(&oc_pages, __ATOMIC_RELAXED) * MMAP_PAGE;
  out->pool_capacity_bytes =
    __atomic_load_n(&oc_dynamic_pages, __ATOMIC_RELAXED) * MMAP_PAGE;
  out->reserved_bytes =
    __atomic_load_n(&oc_reserved_pages, __ATOMIC_RELAXED) * MMAP_PAGE;
  out->peak_reserved_bytes =
    __atomic_load_n(&oc_peak_reserved_pages, __ATOMIC_RELAXED) * MMAP_PAGE;
  out->committed_bytes =
    __atomic_load_n(&oc_committed_pages, __ATOMIC_RELAXED) * MMAP_PAGE;
  out->peak_committed_bytes =
    __atomic_load_n(&oc_peak_committed_pages, __ATOMIC_RELAXED) * MMAP_PAGE;
  out->spill_limit_bytes = out->pool_capacity_bytes;
  out->spill_bytes =
    __atomic_load_n(&oc_spill_pages, __ATOMIC_RELAXED) * MMAP_PAGE;
  out->peak_spill_bytes =
    __atomic_load_n(&oc_peak_spill_pages, __ATOMIC_RELAXED) * MMAP_PAGE;
  out->host_spill_limit_bytes = out->pool_capacity_bytes;
  out->host_spill_bytes =
    __atomic_load_n(&oc_host_spill_pages, __ATOMIC_RELAXED) * MMAP_PAGE;
  out->peak_host_spill_bytes =
    __atomic_load_n(&oc_peak_host_spill_pages, __ATOMIC_RELAXED) * MMAP_PAGE;
  out->thread_pool_limit_bytes = out->pool_capacity_bytes;
  out->thread_pool_bytes =
    __atomic_load_n(&oc_thread_pool_pages, __ATOMIC_RELAXED) * MMAP_PAGE;
  out->peak_thread_pool_bytes =
    __atomic_load_n(&oc_peak_thread_pool_pages, __ATOMIC_RELAXED) * MMAP_PAGE;
  out->guest_allocation_failures =
    __atomic_load_n(&oc_guest_allocation_failures, __ATOMIC_RELAXED);
  out->host_allocation_failures =
    __atomic_load_n(&oc_host_allocation_failures, __ATOMIC_RELAXED);
  out->thread_allocation_failures =
    __atomic_load_n(&oc_thread_allocation_failures, __ATOMIC_RELAXED);
  out->map_call_count =
    __atomic_load_n(&oc_map_call_count, __ATOMIC_RELAXED);
  out->map_retry_count =
    __atomic_load_n(&oc_map_retry_count, __ATOMIC_RELAXED);
  out->last_map_result =
    __atomic_load_n(&oc_last_map_result, __ATOMIC_RELAXED);
  out->backing_unmap_ok =
    __atomic_load_n(&oc_backing_unmap_ok, __ATOMIC_RELAXED);
  out->backing_unmap_fail =
    __atomic_load_n(&oc_backing_unmap_fail, __ATOMIC_RELAXED);
  out->ownership_record_capacity =
    __atomic_load_n(&oc_dynamic_pages, __ATOMIC_RELAXED) +
      oc_donor_unit_capacity;
  out->ownership_records_in_use =
    __atomic_load_n(&oc_allocation_slots_in_use, __ATOMIC_RELAXED);
  out->peak_ownership_records_in_use =
    __atomic_load_n(&oc_peak_allocation_slots_in_use, __ATOMIC_RELAXED);
  out->ownership_record_exhaustions =
    __atomic_load_n(&oc_allocation_slot_exhaustions, __ATOMIC_RELAXED);
  const uint64_t mapped_segments = __atomic_load_n(
    &oc_dynamic_mapped_segments, __ATOMIC_RELAXED);
  out->dynamic_mapped_bytes =
    mapped_segments * OC_DYNAMIC_SEGMENT_BYTES;
  out->peak_dynamic_mapped_bytes = __atomic_load_n(
    &oc_peak_dynamic_mapped_segments, __ATOMIC_RELAXED) *
      OC_DYNAMIC_SEGMENT_BYTES;
  out->guest_backing_bytes = out->dynamic_mapped_bytes;

  u64 total = 0, used = 0;
  if (R_SUCCEEDED(svcGetInfo(&total, InfoType_TotalMemorySize,
                             CUR_PROCESS_HANDLE, 0)))
    out->system_total_memory_bytes = total;
  if (R_SUCCEEDED(svcGetInfo(&used, InfoType_UsedMemorySize,
                             CUR_PROCESS_HANDLE, 0)))
    out->system_used_memory_bytes = used;
  if (total > used) out->system_available_memory_bytes = total - used;
  u64 system_resource = 0;
  if (R_SUCCEEDED(svcGetInfo(&system_resource,
                             InfoType_SystemResourceSizeTotal,
                             CUR_PROCESS_HANDLE, 0)))
    out->system_resource_size_bytes = system_resource;
  out->backing_backend = (uint32_t)g_memory_backing_backend;
  if (g_memory_backing_backend == NX_MEMORY_BACKEND_HEAP_ALIAS) {
    out->donor_capacity_bytes = g_heap_donor_capacity;
    out->donor_active_bytes = __atomic_load_n(
      &g_heap_donor_active_bytes, __ATOMIC_ACQUIRE);
    out->donor_used_bytes = __atomic_load_n(
      &oc_donor_used_units, __ATOMIC_RELAXED) *
        OC_HEAP_DONOR_UNIT_BYTES;
    out->donor_peak_used_bytes = __atomic_load_n(
      &oc_peak_donor_used_units, __ATOMIC_RELAXED) *
        OC_HEAP_DONOR_UNIT_BYTES;
    out->donor_grow_calls = __atomic_load_n(
      &oc_donor_grow_calls, __ATOMIC_RELAXED);
    out->donor_shrink_calls = __atomic_load_n(
      &oc_donor_shrink_calls, __ATOMIC_RELAXED);
    out->donor_last_resize_result = __atomic_load_n(
      &oc_donor_last_resize_result, __ATOMIC_RELAXED);
  }

  if (oc_dynamic_metadata_ready &&
      !mutexIsLockedByCurrentThread(&g_mmap_lock)) {
    /* Heartbeat diagnostics must never block.  mmap_broker_lock() is a
     * spin-loop (mutexTryLock + svcSleepThread(0)); if the holder of
     * g_mmap_lock is wedged in a kernel SVC (the verification hang), the
     * heartbeat thread would spin here forever, truncating the run log
     * mid-heartbeat and hiding the very contention we need to see.  Use a
     * one-shot try-lock: if the broker is busy, skip the free-list scan
     * (pool_free_bytes stays 0) and let the caller record that the snapshot
     * was deferred. */
    nx_guest_gc_critical_enter();
    const int locked = mutexTryLock(&g_mmap_lock);
    if (locked) {
      uint64_t free_pages = 0;
      uint64_t largest_pages = 0;
      for (uint32_t node = oc_dynamic_extent_head; node;
           node = oc_dynamic_extents[node].next) {
        free_pages += oc_dynamic_extents[node].pages;
        if (oc_dynamic_extents[node].pages > largest_pages)
          largest_pages = oc_dynamic_extents[node].pages;
      }
      mutexUnlock(&g_mmap_lock);
      nx_guest_gc_critical_leave();
      out->pool_free_bytes = free_pages * MMAP_PAGE;
      out->pool_largest_free_bytes = largest_pages * MMAP_PAGE;
    } else {
      nx_guest_gc_critical_leave();
      out->pool_free_bytes = UINT64_MAX;  /* sentinel: broker busy */
      out->pool_largest_free_bytes = UINT64_MAX;
    }
  }
}

static int oc_contains(void *addr) {
  return oc_pages && (uint8_t *)addr >= oc_base &&
         (uint8_t *)addr < oc_base + oc_pages * MMAP_PAGE;
}

/* Reserve a 64 MiB-aligned, whole-segment sparse range from the low end of
 * the shared extent map. The exact rounded size is retained at its start page
 * so unmap can return the complete virtual extent without scanning neighbors. */
static void *oc_alloc_locked(size_t len) {
  if (!oc_pages || !oc_owned || !oc_sparse_start_pages) return NULL;
  if (len > SIZE_MAX - (MMAP_PAGE - 1u)) return NULL;
  size_t need = (len + MMAP_PAGE - 1u) / MMAP_PAGE;
  if (!need) need = 1;
  if (need > SIZE_MAX - (OC_DYNAMIC_SEGMENT_PAGES - 1u)) return NULL;
  need = (need + OC_DYNAMIC_SEGMENT_PAGES - 1u) &
         ~(OC_DYNAMIC_SEGMENT_PAGES - 1u);
  if (need > oc_pages) return NULL;

  size_t first = 0;
  const uint32_t extent = oc_sparse_find_extent_locked(need, &first);
  if (!extent || first >= oc_pages || need > oc_pages - first) return NULL;
  for (size_t page = first; page < first + need; ++page)
    if (oc_used[page] || oc_owned[page] || oc_committed[page]) return NULL;
  if (!oc_dynamic_consume_extent_locked(extent, first, need)) return NULL;

  memset(oc_used + first, 1, need);
  memset(oc_owned + first, 1, need);
  oc_sparse_start_pages[first] = (uint32_t)need;
  const uint64_t reserved = __atomic_add_fetch(
    &oc_reserved_pages, need, __ATOMIC_RELAXED);
  oc_update_peak(&oc_peak_reserved_pages, reserved);
  return oc_base + first * MMAP_PAGE;
}

static int oc_commit_locked(void *addr, size_t len) {
  if (!len || (uint8_t *)addr < oc_base ||
      len > SIZE_MAX - (MMAP_PAGE - 1u))
    return 0;
  const size_t first = ((uint8_t *)addr - oc_base) / MMAP_PAGE;
  const size_t cnt = (len + MMAP_PAGE - 1u) / MMAP_PAGE;
  if (first >= oc_pages || !cnt || cnt > oc_pages - first ||
      first % OC_SPARSE_GRANULE_PAGES ||
      cnt % OC_SPARSE_GRANULE_PAGES)
    return 0;
  for (size_t page = first; page < first + cnt; ++page)
    if (!oc_owned[page]) return 0;
  uint8_t newly_mapped[OC_SPARSE_MAX_GRANULES];
  memset(newly_mapped, 0, sizeof(newly_mapped));
  size_t newly = 0;
  const size_t first_granule = first / OC_SPARSE_GRANULE_PAGES;
  const size_t granule_count = cnt / OC_SPARSE_GRANULE_PAGES;
  for (size_t offset = 0; offset < granule_count; ++offset) {
    const size_t granule = first_granule + offset;
    const size_t page_first = granule * OC_SPARSE_GRANULE_PAGES;
    size_t live_pages = 0;
    for (size_t page = page_first;
         page < page_first + OC_SPARSE_GRANULE_PAGES; ++page)
      if (oc_committed[page]) ++live_pages;
    if (live_pages == OC_SPARSE_GRANULE_PAGES) continue;
    if (live_pages != 0) return 0;
    uint32_t source = 0;
    const Result result = oc_backing_map_locked(
      oc_base + page_first * MMAP_PAGE,
      OC_SPARSE_COMMIT_GRANULE_BYTES,
      OC_DONOR_UNITS_PER_GRANULE, 0, &source);
    if (R_FAILED(result)) goto rollback;
    memset(oc_base + page_first * MMAP_PAGE, 0,
           OC_SPARSE_COMMIT_GRANULE_BYTES);
    memset(oc_committed + page_first, 2, OC_SPARSE_GRANULE_PAGES);
    oc_sparse_granule_sources[granule] = source;
    newly_mapped[granule] = 1;
    newly += OC_SPARSE_GRANULE_PAGES;
  }
  if (!newly) return 1;
  for (size_t page = first; page < first + cnt; ++page)
    if (oc_committed[page] == 2) oc_committed[page] = 1;
  const uint64_t committed = __atomic_add_fetch(
    &oc_committed_pages, newly, __ATOMIC_RELAXED);
  oc_update_peak(&oc_peak_committed_pages, committed);
  return 1;

rollback:
  for (size_t granule = first_granule;
       granule < first_granule + granule_count; ++granule) {
    if (!newly_mapped[granule]) continue;
    const size_t page_first = granule * OC_SPARSE_GRANULE_PAGES;
    Result undo = oc_backing_unmap_locked(
      oc_base + page_first * MMAP_PAGE,
      OC_SPARSE_COMMIT_GRANULE_BYTES,
      oc_sparse_granule_sources[granule]);
    if (R_FAILED(undo))
      fatal_error("Sparse backing rollback failed: 0x%08x.",
                  (unsigned)undo);
    memset(oc_committed + page_first, 0, OC_SPARSE_GRANULE_PAGES);
    oc_sparse_granule_sources[granule] = 0;
  }
  return 0;
}

/* Unity reserves each large PROT_NONE region once, then commits it in tiny
 * mprotect slices.  Coalesce first touch to 1 MiB, clipped to pages owned by
 * this shim, so Horizon does not accumulate page-table work for 4 KiB calls.
 * Physical admission is decided solely by the process-wide resource limit. */
static int oc_commit_coarse_locked(void *addr, size_t len) {
  if (!len) return 1;
  if (!oc_base || !oc_owned || !oc_committed ||
      (uint8_t *)addr < oc_base ||
      ((uintptr_t)addr & (MMAP_PAGE - 1u)) != 0)
    return 0;
  const size_t first = ((uint8_t *)addr - oc_base) / MMAP_PAGE;
  if (first >= oc_pages || len > SIZE_MAX - (MMAP_PAGE - 1u)) return 0;
  const size_t cnt = (len + MMAP_PAGE - 1u) / MMAP_PAGE;
  if (!cnt || cnt > oc_pages - first) return 0;
  const size_t exact_end = first + cnt;
  for (size_t page = first; page < exact_end; ++page)
    if (!oc_owned[page]) return 0;

  const size_t granule_pages =
    OC_SPARSE_COMMIT_GRANULE_BYTES / MMAP_PAGE;
  const size_t aligned_first = first & ~(granule_pages - 1u);
  size_t aligned_end = exact_end;
  if (aligned_end <= SIZE_MAX - (granule_pages - 1u))
    aligned_end = (aligned_end + granule_pages - 1u) &
                  ~(granule_pages - 1u);
  if (aligned_end > oc_pages) aligned_end = oc_pages;

  size_t expanded_first = first;
  while (expanded_first > aligned_first && oc_owned[expanded_first - 1u])
    --expanded_first;
  size_t expanded_end = exact_end;
  while (expanded_end < aligned_end && oc_owned[expanded_end])
    ++expanded_end;

  size_t exact_new = 0;
  for (size_t page = first; page < exact_end; ++page)
    if (!oc_committed[page]) ++exact_new;
  size_t expanded_new = 0;
  for (size_t page = expanded_first; page < expanded_end; ++page)
    if (!oc_committed[page]) ++expanded_new;

  size_t commit_first = first;
  size_t commit_end = exact_end;
  if (expanded_new >= exact_new) {
    commit_first = expanded_first;
    commit_end = expanded_end;
  }
  return oc_commit_locked(oc_base + commit_first * MMAP_PAGE,
                          (commit_end - commit_first) * MMAP_PAGE);
}

static void oc_decommit_locked(void *addr, size_t len) {
  if (!len || (uint8_t *)addr < oc_base ||
      len > SIZE_MAX - (MMAP_PAGE - 1u))
    return;
  const size_t first = ((uint8_t *)addr - oc_base) / MMAP_PAGE;
  size_t cnt = (len + MMAP_PAGE - 1u) / MMAP_PAGE;
  if (first >= oc_pages) return;
  if (cnt > oc_pages - first) cnt = oc_pages - first;
  const size_t end = first + cnt;
  const size_t first_granule = first / OC_SPARSE_GRANULE_PAGES;
  const size_t last_granule = (end - 1u) / OC_SPARSE_GRANULE_PAGES;
  size_t unmapped = 0;
  for (size_t granule = first_granule;
       granule <= last_granule; ++granule) {
    const size_t granule_first = granule * OC_SPARSE_GRANULE_PAGES;
    const size_t granule_end = granule_first + OC_SPARSE_GRANULE_PAGES;
    if (!oc_committed[granule_first]) continue;
    const size_t part_first = first > granule_first ? first : granule_first;
    const size_t part_end = end < granule_end ? end : granule_end;
    if (part_first == granule_first && part_end == granule_end) {
      Result result = oc_backing_unmap_locked(
        oc_base + granule_first * MMAP_PAGE,
        OC_SPARSE_COMMIT_GRANULE_BYTES,
        oc_sparse_granule_sources[granule]);
      if (R_FAILED(result)) {
        /* A code alias can be unmapped only with the exact source/destination
         * range accepted by Horizon.  Keep rejected backing recorded, but
         * preserve anonymous MADV_DONTNEED/munmap semantics before recycling
         * the virtual extent. */
        memset(oc_base + granule_first * MMAP_PAGE, 0,
               OC_SPARSE_COMMIT_GRANULE_BYTES);
        continue;
      }
      memset(oc_committed + granule_first, 0,
             OC_SPARSE_GRANULE_PAGES);
      oc_sparse_granule_sources[granule] = 0;
      unmapped += OC_SPARSE_GRANULE_PAGES;
    } else if (part_first < part_end) {
      memset(oc_base + part_first * MMAP_PAGE, 0,
             (part_end - part_first) * MMAP_PAGE);
    }
  }
  if (!unmapped) return;
  const uint64_t committed = __atomic_load_n(
    &oc_committed_pages, __ATOMIC_RELAXED);
  __atomic_store_n(&oc_committed_pages,
                   committed >= unmapped ? committed - unmapped : 0,
                   __ATOMIC_RELAXED);
}

static void oc_free_locked(void *addr, size_t len) {
  if (!oc_owned || !oc_sparse_start_pages || (uint8_t *)addr < oc_base)
    return;
  const size_t first = ((uint8_t *)addr - oc_base) / MMAP_PAGE;
  if (first >= oc_pages) return;
  const size_t cnt = oc_sparse_start_pages[first];
  if (!cnt || cnt > oc_pages - first ||
      (len && len > cnt * MMAP_PAGE))
    return;
  oc_decommit_locked(addr, cnt * MMAP_PAGE);
  const uint64_t reserved = __atomic_load_n(
    &oc_reserved_pages, __ATOMIC_RELAXED);
  __atomic_store_n(&oc_reserved_pages,
                   reserved >= cnt ? reserved - cnt : 0,
                   __ATOMIC_RELAXED);
  memset(oc_used + first, 0, cnt);
  memset(oc_owned + first, 0, cnt);
  oc_sparse_start_pages[first] = 0;
  oc_dynamic_return_extent_locked(first, cnt);
}

static void mmap_arena_init_locked(void) {
  if (mmap_arena_initialized) return;
  mmap_arena_initialized = 1;
  /* The dynamic broker supersedes this permanently backed mmap partition.
   * Keep the arena optional so ordinary mappings fall through to newlib
   * first and the shared dynamic arena second. */
  if (!g_mmap_arena_base || !g_mmap_arena_size) return;
  uint8_t *base = (uint8_t *)g_mmap_arena_base;
  size_t usable = g_mmap_arena_size;
  size_t pages  = usable / MMAP_PAGE;
  uint8_t *used = (uint8_t *)calloc(pages, 1);
  if (!used) fatal_error("mmap bitmap alloc failed");
  mmap_usable = usable; mmap_pages = pages; mmap_used = used;
  mmap_arena  = base;
}

/* Return only complete Linux-style mappings.  Earlier code accepted a large
 * final aligned prefix and reported it as the requested length; a guest could
 * then legally touch beyond the reserved pages. */
static void *mmap_arena_alloc_locked(size_t len, size_t *got) {
  if (len > SIZE_MAX - (MMAP_PAGE - 1u)) {
    *got = 0;
    return NULL;
  }
  size_t need = (len + MMAP_PAGE - 1) / MMAP_PAGE;
  if (!need) need = 1;
  if (len >= MMAP_BIG_THRESH) {
    const size_t step = MMAP_BIG_ALIGN / MMAP_PAGE;   // 256MB in pages
    for (size_t i = 0; i + need <= mmap_pages; i += step) {
      size_t run = 0;
      while (run < need && !mmap_used[i + run]) run++;
      if (run == need) {
        for (size_t k = 0; k < need; k++) mmap_used[i + k] = 1;
        *got = need * MMAP_PAGE;
        return mmap_arena + i * MMAP_PAGE;
      }
    }
  } else {
    for (size_t i = 0; i + need <= mmap_pages; ) {
      size_t run = 0;
      while (run < need && !mmap_used[i + run]) run++;
      if (run == need) {
        for (size_t k = 0; k < need; k++) mmap_used[i + k] = 1;
        *got = need * MMAP_PAGE;
        return mmap_arena + i * MMAP_PAGE;
      }
      i += run + 1;
    }
  }
  *got = 0;
  return NULL;
}

static void mmap_arena_free_locked(void *addr, size_t len) {
  if (!mmap_arena || (uint8_t *)addr < mmap_arena) return;
  size_t off = (uint8_t *)addr - mmap_arena;
  if (off >= mmap_usable) return;
  size_t first = off / MMAP_PAGE;
  size_t cnt   = (len + MMAP_PAGE - 1) / MMAP_PAGE;
  for (size_t k = 0; k < cnt && first + k < mmap_pages; k++)
    mmap_used[first + k] = 0;
}

static void mmap_arena_free(void *addr, size_t len) {
  mmap_broker_lock();
  mmap_arena_free_locked(addr, len);
  mmap_broker_unlock();
}

/* Publish the exact client slab partition prepared by the atomic alias layout.
 * The master reservation remains process-owned; this function only transfers
 * the partition from bootstrap state to Unity's exact mmap state. */
int mmap_prepare_unity_slab_reservation(void **base_out, size_t *size_out) {
  if (base_out) *base_out = NULL;
  if (size_out) *size_out = 0;

  mmap_broker_lock();
  if (unity_slab_state != UNITY_SLAB_EMPTY) {
    void *base = unity_slab_reservation;
    const size_t size = unity_slab_reservation_size;
    mmap_broker_unlock();
    if (base_out) *base_out = base;
    if (size_out) *size_out = size;
    return base && size == UNITY_SLAB_RESERVATION_BYTES;
  }
  uint8_t *candidate = unity_slab_prepared_base;
  if (!candidate || !oc_alias_layout_reservation) {
    mmap_broker_unlock();
    return 0;
  }

  const uintptr_t raw = (uintptr_t)candidate;
  const uintptr_t aligned = raw <= UINTPTR_MAX - (UNITY_SLAB_ALIGNMENT - 1u)
    ? ALIGN_MEM(raw, UNITY_SLAB_ALIGNMENT) : 0;
  const size_t prefix = aligned >= raw ? (size_t)(aligned - raw) : SIZE_MAX;
  if (!aligned || prefix > UNITY_SLAB_RESERVATION_BYTES ||
      GENSHIN_UNITY_SLAB_USABLE_BYTES >
        UNITY_SLAB_RESERVATION_BYTES - prefix ||
      !oc_address_range_state(candidate, UNITY_SLAB_RESERVATION_BYTES,
                              0, MemType_Unmapped)) {
    mmap_broker_unlock();
    return 0;
  }

  memset(unity_slab_committed, 0, sizeof(unity_slab_committed));
  memset(unity_slab_donor_sources, 0,
         sizeof(unity_slab_donor_sources));
  unity_slab_reservation = candidate;
  unity_slab_reservation_size = UNITY_SLAB_RESERVATION_BYTES;
  unity_slab_aligned_base = (uint8_t *)aligned;
  unity_slab_prepared_base = NULL;
  unity_slab_state = UNITY_SLAB_RESERVED;
  void *base = unity_slab_reservation;
  const size_t size = unity_slab_reservation_size;
  mmap_broker_unlock();

  if (base_out) *base_out = base;
  if (size_out) *size_out = size;
  return base && size == UNITY_SLAB_RESERVATION_BYTES;
}

int mmap_validate_unity_slab_reservation(const void *base, size_t size) {
  mmap_broker_lock();
  int valid = unity_slab_state == UNITY_SLAB_CLAIMED &&
              base == unity_slab_reservation &&
              size == unity_slab_reservation_size &&
              size == UNITY_SLAB_RESERVATION_BYTES &&
              oc_alias_layout_reservation && unity_slab_aligned_base;
  for (size_t chunk = 0; valid && chunk < UNITY_SLAB_CHUNK_COUNT; ++chunk) {
    const void *address = unity_slab_aligned_base +
      chunk * UNITY_SLAB_CHUNK_BYTES;
    valid = oc_address_range_state(
      address, UNITY_SLAB_CHUNK_BYTES,
      unity_slab_committed[chunk] ? Perm_Rw : 0,
      unity_slab_committed[chunk]
        ? nx_memory_backing_mapped_type() : MemType_Unmapped);
  }
  mmap_broker_unlock();
  return valid;
}

void *mmap_commit_unity_slab_chunk(void *chunk) {
  const uintptr_t address = (uintptr_t)chunk;
  Result result = 0;

  mmap_broker_lock();
  const uintptr_t begin = (uintptr_t)unity_slab_aligned_base;
  const uintptr_t usable_size = (uintptr_t)GENSHIN_UNITY_SLAB_USABLE_BYTES;
  const int valid = unity_slab_state == UNITY_SLAB_CLAIMED && begin &&
    address >= begin && address - begin < usable_size &&
    !(address & (UNITY_SLAB_CHUNK_BYTES - 1u)) &&
    address - begin <= usable_size - UNITY_SLAB_CHUNK_BYTES;
  if (!valid) {
    mmap_broker_unlock();
    fatal_error("Unity selected an invalid on-demand slab chunk at %p.", chunk);
  }

  const size_t index = (address - begin) / UNITY_SLAB_CHUNK_BYTES;
  if (unity_slab_committed[index]) {
    mmap_broker_unlock();
    return chunk;
  }

  uint32_t source = 0;
  result = oc_backing_map_locked(
    chunk, UNITY_SLAB_CHUNK_BYTES,
    OC_DONOR_UNITS_PER_SEGMENT, 1, &source);
  unity_slab_diag_increment(&unity_slab_diagnostics.physical_commit_calls);
  unity_slab_diagnostics.last_map_result = (uint32_t)result;
  if (R_SUCCEEDED(result)) {
    /* Anonymous first-touch pages must be zero even when the loader supplied
     * a debug-filled heap earlier in the process lifetime. */
    memset(chunk, 0, UNITY_SLAB_CHUNK_BYTES);
    unity_slab_committed[index] = 1;
    unity_slab_donor_sources[index] = source;
    ++unity_slab_diagnostics.committed_chunks;
    if (unity_slab_diagnostics.committed_chunks >
        unity_slab_diagnostics.peak_committed_chunks)
      unity_slab_diagnostics.peak_committed_chunks =
        unity_slab_diagnostics.committed_chunks;
  }
  mmap_broker_unlock();

  if (R_FAILED(result))
    fatal_error("Unity slab physical commit failed for %p (0x%08x).",
                chunk, (unsigned)result);
  return chunk;
}

static Result mmap_decommit_unity_slab_chunk_locked(size_t index) {
  if (index >= UNITY_SLAB_CHUNK_COUNT || !unity_slab_committed[index])
    return 0;
  void *address = unity_slab_aligned_base +
    index * UNITY_SLAB_CHUNK_BYTES;
  const Result result = oc_backing_unmap_locked(
    address, UNITY_SLAB_CHUNK_BYTES,
    unity_slab_donor_sources[index]);
  unity_slab_diag_increment(&unity_slab_diagnostics.physical_decommit_calls);
  if (R_SUCCEEDED(result)) {
    unity_slab_committed[index] = 0;
    unity_slab_donor_sources[index] = 0;
    if (unity_slab_diagnostics.committed_chunks)
      --unity_slab_diagnostics.committed_chunks;
  }
  return result;
}

int mmap_get_unity_slab_diagnostics(UnitySlabMmapDiagnostics *out) {
  if (!out) return 0;
  mmap_broker_lock();
  *out = unity_slab_diagnostics;
  out->reservation_base = (uintptr_t)unity_slab_reservation;
  out->reservation_size = (uint64_t)unity_slab_reservation_size;
  out->reservation_state = (uint32_t)unity_slab_state;
  mmap_broker_unlock();
  return 1;
}

static void *mmap_claim_unity_slab_reservation(size_t length, int prot,
                                                 int flags, int fd,
                                                 long offset, void *address_hint,
                                                 uintptr_t caller) {
  const int exact_tuple =
    length == UNITY_SLAB_RESERVATION_BYTES &&
    prot == (BIONIC_PROT_READ | BIONIC_PROT_WRITE) &&
    flags == (BIONIC_MAP_PRIVATE | BIONIC_MAP_ANONYMOUS) &&
    fd == -1 && offset == 0;
  const int slab_sized = length >= GENSHIN_UNITY_SLAB_USABLE_BYTES;
  if (!slab_sized) return NULL;

  mmap_broker_lock();
  unity_slab_diag_increment(&unity_slab_diagnostics.large_calls);
  unity_slab_diagnostics.last_caller = caller;
  unity_slab_diagnostics.last_address_hint = (uintptr_t)address_hint;
  unity_slab_diagnostics.last_claim_result = 0;
  unity_slab_diagnostics.last_length = (uint64_t)length;
  unity_slab_diagnostics.last_offset = (int64_t)offset;
  unity_slab_diagnostics.last_prot = prot;
  unity_slab_diagnostics.last_flags = flags;
  unity_slab_diagnostics.last_fd = fd;
  unity_slab_diagnostics.last_decision = 0;
  void *result = NULL;
  if (exact_tuple) {
    unity_slab_diag_increment(&unity_slab_diagnostics.exact_tuple_calls);
    if (unity_slab_state == UNITY_SLAB_RESERVED && unity_slab_reservation) {
      unity_slab_state = UNITY_SLAB_CLAIMED;
      result = unity_slab_reservation;
      unity_slab_diag_increment(&unity_slab_diagnostics.successful_claims);
      unity_slab_diagnostics.last_decision = 2;
      unity_slab_diagnostics.last_claim_result = (uintptr_t)result;
    } else {
      unity_slab_diagnostics.last_decision = 1;
    }
  }
  mmap_broker_unlock();
  return result;
}

/* Heap-backed mappings tracked for munmap. */
#define MMAP_FALLBACK_MAX 4096
static struct { void *ptr; size_t len; } g_fb[MMAP_FALLBACK_MAX];
static int   g_fb_n = 0;
static Mutex g_fb_lock;

/* Metadata descriptors are duplicated by the Android client before mmap.
 * Reopen their tracked resolved path and use libnx's ordinary sequential read
 * path so fsdev owns both the duplicate lifetime and mapped-buffer fallback. */
static long metadata_read_complete(int fd, void *buffer, size_t count,
                                   long offset) {
  char *path = fd_path_snapshot(fd);
  if (!path) return nx_pread(fd, buffer, count, offset);
  int stable = open(path, O_RDONLY);
  const int open_errno = errno;
  free(path);
  if (stable < 0) {
    errno = open_errno;
    return -1;
  }
  if (lseek(stable, offset, SEEK_SET) < 0) {
    const int saved = errno;
    close(stable);
    errno = saved;
    return -1;
  }
  size_t done = 0;
  int failed = 0;
  while (done < count) {
    const long chunk = read(stable, (char *)buffer + done, count - done);
    if (chunk < 0) {
      failed = done == 0;
      break;
    }
    if (chunk == 0) break;
    done += (size_t)chunk;
  }
  const int saved = errno;
  close(stable);
  errno = saved;
  return failed ? -1 : (long)done;
}

static void *mmap_fallback(size_t length, int flags, int fd, long offset,
                           size_t required_file_bytes,
                           int metadata_mapping) {
  /* Preserve Unity's alignment requirement for large anonymous pools. */
  size_t align = (length >= MMAP_BIG_THRESH && (flags & BIONIC_MAP_ANONYMOUS))
                   ? MMAP_BIG_ALIGN : MMAP_PAGE;
  void *q = memalign(align, length);
  if (!q) { errno = ENOMEM; return NULL; }
  long got = 0;
  int read_errno = 0;
  if (flags & BIONIC_MAP_ANONYMOUS) {
    memset(q, 0, length);
  } else {
    const size_t read_length = required_file_bytes
                                 ? required_file_bytes : length;
    if (fd >= 0) {
      if (asset_pack_fd_is(fd)) {
        got = asset_pack_pread_fd(fd, q, read_length, offset);
        if (got < 0) got = 0;
      } else if (metadata_mapping) {
        got = metadata_read_complete(fd, q, read_length, offset);
        if (got < 0) { read_errno = errno; got = 0; }
      } else {
        while ((size_t)got < read_length) {
          long r = nx_pread(fd, (char *)q + got,
                            read_length - (size_t)got,
                            offset + got);
          if (r < 0) { read_errno = errno; break; }
          if (r == 0) break;
          got += r;
        }
      }
    }
    if (required_file_bytes && (size_t)got < required_file_bytes) {
      free(q);
      errno = read_errno ? read_errno : EIO;
      return NULL;
    }
    if ((size_t)got < length) memset((char *)q + got, 0, length - got);
  }
  mutexLock(&g_fb_lock);
  if (g_fb_n >= MMAP_FALLBACK_MAX) {
    mutexUnlock(&g_fb_lock);
    free(q);
    const int fallback_errno = errno ? errno : ENOMEM;
    errno = fallback_errno;
    return NULL;
  }
  g_fb[g_fb_n].ptr = q;
  g_fb[g_fb_n].len = length;
  g_fb_n++;
  mutexUnlock(&g_fb_lock);
  return q;
}

static int mmap_fallback_free(void *addr) {
  mutexLock(&g_fb_lock);
  for (int i = 0; i < g_fb_n; i++) {
    if (g_fb[i].ptr == addr) {
      free(addr);
      g_fb[i] = g_fb[--g_fb_n];
      mutexUnlock(&g_fb_lock);
      return 1;
    }
  }
  mutexUnlock(&g_fb_lock);
  return 0;
}

/* Writable MAP_SHARED mappings are copied into the wrapper's arena.  Retain a
   descriptor and explicitly write dirty ranges back on msync/munmap. */
#define MMAP_SHARED_MAX 512
typedef struct {
  int used;
  void *ptr;
  size_t len;
  int fd;
  long offset;
} SharedFileMap;
static SharedFileMap g_shared_maps[MMAP_SHARED_MAX];
static Mutex g_shared_map_lock;

static int mmap_shared_register(void *ptr, size_t len, int fd, long offset) {
  char *path = fd_path_snapshot(fd);
  int retained = path ? open(path, O_RDWR) : -1;
  if (retained < 0) retained = fcntl(fd, F_DUPFD_CLOEXEC, 0);
  if (retained < 0) { free(path); return 0; }
  if (path) {
    fd_ino_set(retained, path);
    nx_file_io_track_open(retained, path, 1);
    free(path);
  }
  mutexLock(&g_shared_map_lock);
  for (int i = 0; i < MMAP_SHARED_MAX; i++) if (!g_shared_maps[i].used) {
    g_shared_maps[i] = (SharedFileMap){ 1, ptr, len, retained, offset };
    mutexUnlock(&g_shared_map_lock);
    return 1;
  }
  mutexUnlock(&g_shared_map_lock);
  close_fake(retained);
  errno = ENOMEM;
  return 0;
}

static int mmap_shared_flush_one(const SharedFileMap *map, uintptr_t start,
                                 uintptr_t end, int synchronize) {
  uintptr_t map_start = (uintptr_t)map->ptr;
  uintptr_t map_end = map_start + map->len;
  if (start < map_start) start = map_start;
  if (end > map_end) end = map_end;
  if (start >= end) return 0;
  uint64_t file_offset = (uint64_t)map->offset + (start - map_start);
  if (file_offset > LONG_MAX) { errno = EOVERFLOW; return -1; }
  const uint8_t *bytes = (const uint8_t *)start;
  size_t left = end - start;
  int failure = 0;
  while (left) {
    const size_t chunk = left < UINT64_C(0x10000) ? left : UINT64_C(0x10000);
    long wrote = nx_pwrite(map->fd, bytes, chunk, (long)file_offset);
    if (wrote <= 0) {
      if (wrote == 0) errno = EIO;
      failure = errno;
      break;
    }
    bytes += (size_t)wrote;
    left -= (size_t)wrote;
    file_offset += (uint64_t)wrote;
  }
  if (!failure && synchronize && fsync(map->fd) < 0) failure = errno;
  if (failure) { errno = failure; return -1; }
  return 0;
}

int mmap_msync_fake(void *addr, size_t length, int flags) {
  const int known = 1 /*MS_ASYNC*/ | 2 /*MS_INVALIDATE*/ | 4 /*MS_SYNC*/;
  if (!addr || ((uintptr_t)addr & (MMAP_PAGE - 1u)) || (flags & ~known) ||
      (flags & 1 && flags & 4)) {
    errno = EINVAL;
    return -1;
  }
  uintptr_t start = (uintptr_t)addr;
  if (length > UINTPTR_MAX - start) { errno = EINVAL; return -1; }
  uintptr_t end = start + length;
  int result = 0;
  int failure = 0;
  mutexLock(&g_shared_map_lock);
  for (int i = 0; i < MMAP_SHARED_MAX; i++) if (g_shared_maps[i].used) {
    uintptr_t map_start = (uintptr_t)g_shared_maps[i].ptr;
    uintptr_t map_end = map_start + g_shared_maps[i].len;
    if (start < map_end && end > map_start &&
        mmap_shared_flush_one(&g_shared_maps[i], start, end, flags & 4) < 0) {
      result = -1;
      if (!failure) failure = errno;
    }
  }
  mutexUnlock(&g_shared_map_lock);
  if (failure) errno = failure;
  return result;
}

static int mmap_shared_retire(void *addr, size_t length, int *partial) {
  SharedFileMap map = {0};
  if (partial) *partial = 0;
  uintptr_t start = (uintptr_t)addr;
  if (length > SIZE_MAX - (MMAP_PAGE - 1u)) {
    if (partial) *partial = 1;
    errno = EINVAL;
    return -1;
  }
  size_t rounded_length = (length + MMAP_PAGE - 1u) & ~(MMAP_PAGE - 1u);
  if (rounded_length > UINTPTR_MAX - start) {
    if (partial) *partial = 1;
    errno = EINVAL;
    return -1;
  }
  mutexLock(&g_shared_map_lock);
  for (int i = 0; i < MMAP_SHARED_MAX; i++) if (g_shared_maps[i].used) {
    uintptr_t map_start = (uintptr_t)g_shared_maps[i].ptr;
    size_t map_length = (g_shared_maps[i].len + MMAP_PAGE - 1u) &
                        ~(MMAP_PAGE - 1u);
    uintptr_t end = start + rounded_length;
    uintptr_t map_end = map_start + map_length;
    if (start < map_end && end > map_start) {
      if (start != map_start || rounded_length != map_length) {
        mutexUnlock(&g_shared_map_lock);
        if (partial) *partial = 1;
        errno = EINVAL;
        return -1;
      }
      map = g_shared_maps[i];
      memset(&g_shared_maps[i], 0, sizeof(g_shared_maps[i]));
      break;
    }
  }
  mutexUnlock(&g_shared_map_lock);
  if (!map.used) return 0;
  int result = mmap_shared_flush_one(&map, (uintptr_t)map.ptr,
                                     (uintptr_t)map.ptr + map.len, 1);
  int saved = errno;
  close_fake(map.fd);
  errno = saved;
  return result;
}

/* Read-only map deduplication. */
#define MAPC_N 24
static struct { uint64_t ino; long off; size_t len; void *ptr; } g_mapc[MAPC_N];
static int g_mapc_n = 0;
static void *mapcache_get(uint64_t ino, long off, size_t len) {
  void *r = NULL;
  mutexLock(&g_fb_lock);
  for (int i = 0; i < g_mapc_n; i++)
    if (g_mapc[i].ino == ino && g_mapc[i].off == off && g_mapc[i].len == len) { r = g_mapc[i].ptr; break; }
  mutexUnlock(&g_fb_lock);
  return r;
}
static void *mapcache_put(uint64_t ino, long off, size_t len, void *ptr) {
  void *result = ptr;
  int discard_duplicate = 0;
  mutexLock(&g_fb_lock);
  /* A concurrent mapper can populate the same key between get and put.  Use
   * its process-lifetime pinned copy and retire this still-private fallback. */
  for (int i = 0; i < g_mapc_n; i++) {
    if (g_mapc[i].ino == ino && g_mapc[i].off == off &&
        g_mapc[i].len == len) {
      result = g_mapc[i].ptr;
      discard_duplicate = result != ptr;
      break;
    }
  }
  if (result == ptr && g_mapc_n < MAPC_N) {
    /* Pin only a mapping that actually obtained a cache slot.  If the cache
     * is full it must remain in g_fb so munmap can reclaim it. */
    for (int i = 0; i < g_fb_n; i++)
      if (g_fb[i].ptr == ptr) { g_fb[i] = g_fb[--g_fb_n]; break; }
    g_mapc[g_mapc_n].ino = ino;
    g_mapc[g_mapc_n].off = off;
    g_mapc[g_mapc_n].len = len;
    g_mapc[g_mapc_n].ptr = ptr;
    g_mapc_n++;
  }
  mutexUnlock(&g_fb_lock);
  if (discard_duplicate) (void)mmap_fallback_free(ptr);
  return result;
}

void *mmap_fake(void *addr, size_t length, int prot, int flags, int fd, long offset) {
  if (length == 0) length = 1;
  if (length > SIZE_MAX - (MMAP_PAGE - 1u)) {
    errno = EOVERFLOW;
    return (void *)-1;
  }

  /* The exact-image allocator calls through mmap@LIBC during first render.
   * Keep one bounded snapshot of slab-sized calls so a failed post-render invariant
   * can distinguish argument drift, an import-route miss, and lifecycle state
   * changes without logging from inside a 4 GiB clear. */
  const uintptr_t caller = (uintptr_t)__builtin_extract_return_addr(
    __builtin_return_address(0));
  void *unity_slab = mmap_claim_unity_slab_reservation(
    length, prot, flags, fd, offset, addr, caller);
  if (unity_slab) return unity_slab;

  const int file_backed = !(flags & BIONIC_MAP_ANONYMOUS);
  char metadata_name[32];
  const int metadata_mapping = file_backed &&
    fd_unity_metadata_name(fd, metadata_name, sizeof metadata_name);
  const int shared_write = file_backed && (flags & BIONIC_MAP_SHARED) &&
                           (prot & BIONIC_PROT_WRITE);
  size_t required_file_bytes = shared_write ? length : 0;
  if (file_backed && (fd < 0 || offset < 0)) {
    errno = fd < 0 ? EBADF : EINVAL;
    return (void *)-1;
  }
  if (file_backed && (uint64_t)length > (uint64_t)INT64_MAX - (uint64_t)offset) {
    errno = EOVERFLOW;
    return (void *)-1;
  }
  if (metadata_mapping) {
    uint64_t file_size = 0;
    int size_ok = asset_pack_fstat_fd(fd, &file_size, NULL, NULL);
    if (!size_ok) {
      struct stat metadata_stat;
      if (fstat(fd, &metadata_stat) == 0 && metadata_stat.st_size >= 0) {
        file_size = (uint64_t)metadata_stat.st_size;
        size_ok = 1;
      }
    }
    if (!size_ok || (uint64_t)offset >= file_size) {
      if (size_ok) errno = EINVAL;

      return (void *)-1;
    }
    const uint64_t available = file_size - (uint64_t)offset;
    required_file_bytes = available < (uint64_t)length
                            ? (size_t)available : length;
  }
  if (shared_write) {
    if (asset_pack_fd_is(fd)) { errno = EACCES; return (void *)-1; }
    int descriptor_flags = fcntl(fd, F_GETFL, 0);
    struct stat descriptor_stat;
    if (descriptor_flags < 0 || (descriptor_flags & O_ACCMODE) != O_RDWR ||
        fstat(fd, &descriptor_stat) < 0 || !S_ISREG(descriptor_stat.st_mode)) {
      if (descriptor_flags >= 0) errno = EACCES;
      return (void *)-1;
    }
    uint64_t file_size = descriptor_stat.st_size < 0 ? 0 :
                         (uint64_t)descriptor_stat.st_size;
    if ((uint64_t)offset > file_size ||
        (uint64_t)length > file_size - (uint64_t)offset) {
      errno = EINVAL;
      return (void *)-1;
    }
  }

  /* Commit large anonymous reservations lazily. */
  if (oc_pages && length >= MMAP_BIG_THRESH &&
      (flags & BIONIC_MAP_ANONYMOUS) && prot == BIONIC_PROT_NONE) {
    mmap_broker_lock();
    void *op = oc_alloc_locked(length);
    mmap_broker_unlock();
    if (op) return op;
  }

  size_t reserved = 0;
  mmap_broker_lock();
  mmap_arena_init_locked();
  void *p = mmap_arena_alloc_locked(length, &reserved);
  mmap_broker_unlock();
  /* A successful mmap always covers the complete requested byte range.  Keep
   * this defensive check even though the arena allocator now guarantees it. */
  if (p && reserved < length) {
    mmap_arena_free(p, reserved);
    p = NULL;
  }
  if (!p) {
    /* Spill exhausted arena mappings into the heap. */
    int ro_file = fd >= 0 && !(flags & BIONIC_MAP_ANONYMOUS) && !(prot & BIONIC_PROT_WRITE);
    uint64_t mino = 0;
    if (ro_file) {
      uint64_t packed_size;
      if (!asset_pack_fstat_fd(fd, &packed_size, &mino, NULL) && fd < FD_INO_MAX)
        mino = g_fd_ino[fd];
    }
    if (mino) {
      void *hit = mapcache_get(mino, offset, length);
      if (hit) return hit;
    }
    void *q = mmap_fallback(length, flags, fd, offset,
                            required_file_bytes, metadata_mapping);
    if (q) {
      if (mino) q = mapcache_put(mino, offset, length, q);
      if (shared_write && !mmap_shared_register(q, length, fd, offset)) {
        int saved = errno;
        (void)mmap_fallback_free(q);
        errno = saved;
        return (void *)-1;
      }
      return q;
    }
    errno = ENOMEM;
    return (void *)-1;
  }

  size_t fill = length < reserved ? length : reserved;

  long got = 0;
  if (flags & BIONIC_MAP_ANONYMOUS) {
    memset(p, 0, fill);   // anonymous memory must read back as zero
  } else {
    int read_errno = 0;
    const size_t read_length = required_file_bytes
                                 ? required_file_bytes : fill;
    if (fd >= 0) {
      if (asset_pack_fd_is(fd)) {
        got = asset_pack_pread_fd(fd, p, read_length, offset);
        if (got < 0) { read_errno = errno; got = 0; }
      } else if (metadata_mapping) {
        got = metadata_read_complete(fd, p, read_length, offset);
        if (got < 0) { read_errno = errno; got = 0; }
      } else {
        while ((size_t)got < read_length) {
          long r = nx_pread(fd, (char *)p + got,
                            read_length - (size_t)got,
                            offset + got);
          if (r < 0) { read_errno = errno; break; }
          if (r == 0) break;
          got += r;
        }
      }
    }
    if (required_file_bytes && (size_t)got < required_file_bytes) {
      mmap_arena_free(p, length);
      errno = read_errno ? read_errno : EIO;
      return (void *)-1;
    }
    if ((size_t)got < fill) memset((char *)p + got, 0, fill - (size_t)got);
  }
  if (shared_write && !mmap_shared_register(p, length, fd, offset)) {
    int saved = errno;
    mmap_arena_free(p, length);
    errno = saved;
    return (void *)-1;
  }
  return p;
}

int munmap_fake(void *addr, size_t length) {
  const uintptr_t unmap_begin = (uintptr_t)addr;
  if (!addr || (unmap_begin & (MMAP_PAGE - 1u)) || !length ||
      length > UINTPTR_MAX - unmap_begin) {
    errno = EINVAL;
    return -1;
  }
  /* Reject a split before touching shared-map writeback state, so an invalid
   * range spanning two mapping classes has no partial side effect. */
  int retired_slab = 0;
  mmap_broker_lock();
  if (unity_slab_reservation && unity_slab_reservation_size) {
    const uintptr_t slab_begin = (uintptr_t)unity_slab_reservation;
    const uintptr_t slab_end = slab_begin + unity_slab_reservation_size;
    const uintptr_t unmap_end = unmap_begin + length;
    const int intersects = unmap_begin < slab_end && slab_begin < unmap_end;
    const int exact = unmap_begin == slab_begin && unmap_end == slab_end;
    if (intersects && !exact) {
      mmap_broker_unlock();
      errno = EINVAL;
      return -1;
    }
    if (exact) {
      for (size_t chunk = 0; chunk < UNITY_SLAB_CHUNK_COUNT; ++chunk) {
        const Result result = mmap_decommit_unity_slab_chunk_locked(chunk);
        if (R_FAILED(result)) {
          /* Code-alias decommit may be rejected by the kernel for the heap-donor
           * alias path (see oc_backing_unmap_locked).  Leave the chunk marked
           * committed and continue: its backing stays valid, and a future
           * remap of the same reservation will find it already committed. */
          continue;
        }
      }
      unity_slab_diag_increment(&unity_slab_diagnostics.exact_unmaps);
      retired_slab = 1;
      unity_slab_state = UNITY_SLAB_RETIRED;
      unity_slab_reservation = NULL;
      unity_slab_reservation_size = 0;
      unity_slab_aligned_base = NULL;
    }
  }
  mmap_broker_unlock();
  /* The slab's physical chunks are gone, but the one master reservation also
   * protects the live sparse and dynamic siblings and must remain registered. */
  if (retired_slab) return 0;
  int partial = 0;
  int shared_result = mmap_shared_retire(addr, length, &partial);
  if (partial) return -1; /* keep both mapping and writeback record intact. */
  if (mmap_fallback_free(addr)) return shared_result; // newlib fallback allocation
  if (oc_contains(addr)) {                   // sparse alias partition
    mmap_broker_lock();
    oc_free_locked(addr, length);
    mmap_broker_unlock();
    return shared_result;
  }
  mmap_arena_free(addr, length);            // unreserve address space
  return shared_result;
}

int mprotect_fake(void *addr, size_t len, int prot) {
  if (oc_contains(addr)) {
    if (prot != BIONIC_PROT_NONE) {
      mmap_broker_lock();
      const int committed = oc_commit_coarse_locked(addr, len);
      mmap_broker_unlock();
      if (!committed) {
        errno = ENOMEM;
        return -1;
      }
    }
  }
  return 0;
}

void *mremap_fake(void *old_addr, size_t old_size, size_t new_size, int flags, ...) {
  /* Linux MREMAP_FIXED cannot be honored because the Switch arena chooses its
   * own sparse address.  The SDKs we load only require MAYMOVE growth. */
  if (!old_addr || old_addr == (void *)-1 || !old_size || !new_size || (flags & 2)) {
    errno = EINVAL;
    return (void *)-1;
  }
  if (new_size <= old_size) return old_addr;
  void *replacement = mmap_fake(NULL, new_size, BIONIC_PROT_READ | BIONIC_PROT_WRITE,
                                BIONIC_MAP_PRIVATE | BIONIC_MAP_ANONYMOUS, -1, 0);
  if (replacement == (void *)-1) return replacement;
  memcpy(replacement, old_addr, old_size);
  munmap_fake(old_addr, old_size);
  return replacement;
}
int madvise_fake(void *addr, size_t len, int advice) {
  /* Linux/Android anonymous MAP_PRIVATE memory reads back as zero after the
   * kernel accepts MADV_DONTNEED.  Fully covered Unity chunks can therefore
   * return their physical pages; partial chunks stay mapped and only the
   * advised byte range is cleared. */
  const int discard = advice == 4 /* MADV_DONTNEED */ ||
                      advice == 8 /* MADV_FREE */;
  if (!discard || !len) return 0;
  const uintptr_t start = (uintptr_t)addr;
  if (!addr || (start & (MMAP_PAGE - 1u)) ||
      len > SIZE_MAX - (MMAP_PAGE - 1u)) {
    errno = EINVAL;
    return -1;
  }
  const size_t rounded = (len + MMAP_PAGE - 1u) & ~(MMAP_PAGE - 1u);
  if (rounded > UINTPTR_MAX - start) {
    errno = EINVAL;
    return -1;
  }
  const uintptr_t end = start + rounded;

  mmap_broker_lock();
  if (unity_slab_reservation && unity_slab_reservation_size) {
    const uintptr_t slab_begin = (uintptr_t)unity_slab_reservation;
    const uintptr_t slab_end = slab_begin + unity_slab_reservation_size;
    const int intersects = start < slab_end && slab_begin < end;
    const int contained = start >= slab_begin && end <= slab_end;
    if (contained) {
      const uintptr_t usable_begin = (uintptr_t)unity_slab_aligned_base;
      const uintptr_t usable_end = usable_begin +
        (uintptr_t)GENSHIN_UNITY_SLAB_USABLE_BYTES;
      const uintptr_t clear_begin = start > usable_begin ? start : usable_begin;
      const uintptr_t clear_end = end < usable_end ? end : usable_end;
      if (clear_begin < clear_end) {
        const size_t first_chunk =
          (clear_begin - usable_begin) / UNITY_SLAB_CHUNK_BYTES;
        const size_t last_chunk =
          (clear_end - 1u - usable_begin) / UNITY_SLAB_CHUNK_BYTES;
        for (size_t chunk = first_chunk; chunk <= last_chunk; ++chunk) {
          if (!unity_slab_committed[chunk]) continue;
          const uintptr_t chunk_begin = usable_begin +
            chunk * UNITY_SLAB_CHUNK_BYTES;
          const uintptr_t chunk_end = chunk_begin + UNITY_SLAB_CHUNK_BYTES;
          const uintptr_t part_begin =
            clear_begin > chunk_begin ? clear_begin : chunk_begin;
          const uintptr_t part_end = clear_end < chunk_end ? clear_end : chunk_end;
          if (part_begin == chunk_begin && part_end == chunk_end) {
            const Result result =
              mmap_decommit_unity_slab_chunk_locked(chunk);
            if (R_FAILED(result)) {
              mmap_broker_unlock();
              fatal_error("Unity slab discard failed at chunk %u (0x%08x).",
                          (unsigned)chunk, (unsigned)result);
            }
          } else {
            memset((void *)part_begin, 0, (size_t)(part_end - part_begin));
          }
        }
      }
      unity_slab_diag_increment(&unity_slab_diagnostics.discard_calls);
      unity_slab_diag_add(&unity_slab_diagnostics.discarded_bytes,
                          (uint64_t)rounded);
      unity_slab_diagnostics.last_discard_address = start;
      unity_slab_diagnostics.last_discard_length = (uint64_t)rounded;
      unity_slab_diagnostics.last_discard_advice = advice;
      unity_slab_diagnostics.last_discard_decision = 1;
      mmap_broker_unlock();
      return 0;
    }
    if (intersects) {
      mmap_broker_unlock();
      errno = EINVAL;
      return -1;
    }
  }
  if (oc_contains(addr)) oc_decommit_locked(addr, rounded);
  mmap_broker_unlock();
  return 0;
}

char *realpath_fake(const char *path, char *resolved) {
  if (!path) return NULL;          /* POSIX: realpath(NULL,..) is an error, not a crash */
  if (!resolved) resolved = malloc(0x1000);
  strcpy(resolved, path);
  return resolved;
}
/* The guest passes Linux/Bionic errno numbers to strerror, while newlib uses
 * different values for many pthread and socket errors.  Translate the values
 * that can escape this wrapper before consulting the host message table. */
#ifndef ESOCKTNOSUPPORT
#define ESOCKTNOSUPPORT 124
#endif
static int bionic_errno_to_native(int error) {
  switch (error) {
    case 35: return EDEADLK;
    case 36: return ENAMETOOLONG;
    case 37: return ENOLCK;
    case 38: return ENOSYS;
    case 39: return ENOTEMPTY;
    case 40: return ELOOP;
    case 75: return EOVERFLOW;
    case 88: return ENOTSOCK;
    case 89: return EDESTADDRREQ;
    case 90: return EMSGSIZE;
    case 91: return EPROTOTYPE;
    case 92: return ENOPROTOOPT;
    case 93: return EPROTONOSUPPORT;
    case 94: return ESOCKTNOSUPPORT;
    case 95: return EOPNOTSUPP;
    case 97: return EAFNOSUPPORT;
    case 98: return EADDRINUSE;
    case 99: return EADDRNOTAVAIL;
    case 100: return ENETDOWN;
    case 101: return ENETUNREACH;
    case 102: return ENETRESET;
    case 103: return ECONNABORTED;
    case 104: return ECONNRESET;
    case 105: return ENOBUFS;
    case 106: return EISCONN;
    case 107: return ENOTCONN;
    case 110: return ETIMEDOUT;
    case 111: return ECONNREFUSED;
    case 112: return EHOSTDOWN;
    case 113: return EHOSTUNREACH;
    case 114: return EALREADY;
    case 115: return EINPROGRESS;
    case 130: return EOWNERDEAD;
    case 131: return ENOTRECOVERABLE;
    default: return error;
  }
}
char *strerror_fake(int error) {
  return (char *)strerror(bionic_errno_to_native(error));
}
int strerror_r_fake(int error, char *buffer, size_t length) {
  if (!buffer || !length) return BIONIC_EINVAL;
  snprintf(buffer, length, "%s", strerror_fake(error));
  return 0;
}
typedef struct {
  uint64_t f_type;
  uint64_t f_bsize;
  uint64_t f_blocks;
  uint64_t f_bfree;
  uint64_t f_bavail;
  uint64_t f_files;
  uint64_t f_ffree;
  int32_t f_fsid[2];
  uint64_t f_namelen;
  uint64_t f_frsize;
  uint64_t f_flags;
  uint64_t f_spare[4];
} BionicStatFs;
_Static_assert(sizeof(BionicStatFs) == 0x78,
               "Android arm64 statfs ABI size");

int statfs_fake(const char *path, void *buf) {
  if (!path || !buf) { errno = EFAULT; return -1; }
  char translated[1024];
  const char *native_path = dev_abs(path, translated, sizeof(translated));
  struct statvfs native;
  if (statvfs(native_path, &native) != 0) return -1;

  BionicStatFs result;
  memset(&result, 0, sizeof(result));
  result.f_type = UINT64_C(0x4d44); /* MSDOS_SUPER_MAGIC: SD-card storage. */
  result.f_bsize = native.f_bsize;
  result.f_blocks = native.f_blocks;
  result.f_bfree = native.f_bfree;
  result.f_bavail = native.f_bavail;
  result.f_files = native.f_files;
  result.f_ffree = native.f_ffree;
  result.f_namelen = native.f_namemax;
  result.f_frsize = native.f_frsize ? native.f_frsize : native.f_bsize;
#ifdef ST_RDONLY
  if (native.f_flag & ST_RDONLY) result.f_flags |= 1u;
#endif
#ifdef ST_NOSUID
  if (native.f_flag & ST_NOSUID) result.f_flags |= 2u;
#endif
  memcpy(buf, &result, sizeof(result));
  return 0;
}

/* Synthetic memory and CPU information used by Unity sizing logic. */
static const char *synthetic_text(const char *text, size_t *size_out) {
  if (size_out) *size_out = text ? strlen(text) : 0;
  return text;
}

static const char *synthetic_proc(const char *path, size_t *size_out) {
  static const char cmdline_plain[] = SS_PACKAGE;
  static const char cmdline_vulkan[] =
    SS_PACKAGE "\0-force-vulkan\0-force-device-index\0" "0";
  if (size_out) *size_out = 0;
  if (!path) return NULL;
  if (!strcmp(path, "/proc/self/cmdline") || !strcmp(path, "/proc/curproc/cmdline")) {
    if (config.force_vulkan) {
      if (size_out) *size_out = sizeof cmdline_vulkan;
      return cmdline_vulkan;
    }
    if (size_out) *size_out = sizeof cmdline_plain;
    return cmdline_plain;
  }
  if (!strcmp(path, "/proc/meminfo"))
    return synthetic_text(
      "MemTotal:       3145728 kB\n"
      "MemFree:        2097152 kB\n"
      "MemAvailable:   2097152 kB\n"
      "Buffers:              0 kB\n"
      "Cached:               0 kB\n"
      "SwapTotal:            0 kB\n"
      "SwapFree:             0 kB\n", size_out);
  if (!strcmp(path, "/proc/cpuinfo"))
    return synthetic_text(
      "processor\t: 0\nprocessor\t: 1\nprocessor\t: 2\n"
      "Features\t: fp asimd aes pmull sha1 sha2 crc32\n"
      "CPU implementer\t: 0x41\nCPU architecture: 8\nCPU variant\t: 0x1\n"
      "CPU part\t: 0xd07\nCPU revision\t: 1\n", size_out);
  if (strstr(path, "cpu_capacity"))
    return synthetic_text("1024\n", size_out);
  if (strstr(path, "cpuinfo_max_freq") || strstr(path, "scaling_max_freq"))
    return synthetic_text("1785000\n", size_out);
  if (strstr(path, "cpuinfo_min_freq") || strstr(path, "scaling_min_freq"))
    return synthetic_text("1020000\n", size_out);
  if (strstr(path, "/cpu/possible") || strstr(path, "/cpu/present") || strstr(path, "/cpu/online"))
    return synthetic_text("0-2\n", size_out);
  if (!strncmp(path, "/proc/", 6) || !strncmp(path, "/sys/", 5))
    return synthetic_text("", size_out); // empty for the rest
  return NULL;
}

#define PACK_FILE_SLOTS 64
static struct { FILE *file; void *data; int fd; } g_pack_files[PACK_FILE_SLOTS];
static Mutex g_pack_file_lock;

static int packed_file_add(FILE *file, void *data, int fd) {
  mutexLock(&g_pack_file_lock);
  for (int i = 0; i < PACK_FILE_SLOTS; i++) {
    if (!g_pack_files[i].file) {
      g_pack_files[i].file = file;
      g_pack_files[i].data = data;
      g_pack_files[i].fd = fd;
      mutexUnlock(&g_pack_file_lock);
      return 1;
    }
  }
  mutexUnlock(&g_pack_file_lock);
  return 0;
}

static FILE *packed_fopen(const char *path) {
  void *data = NULL;
  size_t size = 0;
  if (!asset_pack_read_all_path(path, &data, &size)) return NULL;
  FILE *file = fmemopen(data, size ? size : 1, "r");
  int fd = file ? asset_pack_open_path(path) : -1;
  if (!file || !packed_file_add(file, data, fd)) {
    if (fd >= 0) asset_pack_close_fd(fd);
    if (file) fclose(file);
    free(data);
    errno = EMFILE;
    return NULL;
  }
  return file;
}

/* This exact client fork does not consume Unity's usual -force-vulkan
 * command-line switch.  Its renderer selector instead fopen()s a private
 * sentinel appended to the writable application path and logs
 * "FOUND _FORCE_VULKAN_, force vulkan" when the open succeeds.  Expose the
 * sentinel virtually so force_vulkan remains configuration-controlled and no
 * marker has to be written outside the staged game tree. */
static int force_vulkan_sentinel_path(const char *path) {
  static const char suffix[] = "/_FORCE_VULKAN_";
  if (!config.force_vulkan || !path) return 0;
  const size_t path_length = strlen(path);
  const size_t suffix_length = sizeof(suffix) - 1;
  return path_length >= suffix_length &&
         !memcmp(path + path_length - suffix_length, suffix, suffix_length);
}

static FILE *force_vulkan_sentinel_fopen(const char *mode) {
  if (!mode || !strchr(mode, 'r') || strpbrk(mode, "wa+")) {
    errno = EACCES;
    return NULL;
  }
  unsigned char *data = (unsigned char *)malloc(1);
  if (!data) return NULL;
  *data = 1;
  FILE *file = fmemopen(data, 1, "r");
  if (!file || !packed_file_add(file, data, -1)) {
    if (file) fclose(file);
    free(data);
    return NULL;
  }
  return file;
}

static FILE *fdopen_fake_backend(int fd, const char *mode) {
  if (!asset_pack_fd_is(fd)) {
    if (ra_flush_detach(fd) < 0) return NULL;
    return fdopen(fd, mode);
  }
  if (!mode || strpbrk(mode, "wa+")) { errno = EINVAL; return NULL; }
  uint64_t size;
  long position = asset_pack_lseek_fd(fd, 0, SEEK_CUR);
  if (!asset_pack_fstat_fd(fd, &size, NULL, NULL) || size > SIZE_MAX || position < 0) return NULL;
  void *data = malloc(size ? (size_t)size : 1);
  if (!data || (size && asset_pack_pread_fd(fd, data, (size_t)size, 0) != (long)size)) {
    free(data);
    return NULL;
  }
  FILE *file = fmemopen(data, size ? (size_t)size : 1, "r");
  if (!file || fseek(file, position, SEEK_SET) != 0 || !packed_file_add(file, data, fd)) {
    if (file) fclose(file);
    free(data);
    errno = EMFILE;
    return NULL;
  }
  return file;
}

FILE *fdopen_fake(int fd, const char *mode) {
  uint32_t stripe;
  if (!nx_fd_route_source_lock(fd, &stripe)) return NULL;
  FILE *result = fdopen_fake_backend(fd, mode);
  int saved = errno;
  nx_fd_route_source_unlock(stripe);
  errno = saved;
  return result;
}

static int packed_fileno(FILE *file) {
  int fd = -1;
  mutexLock(&g_pack_file_lock);
  for (int i = 0; i < PACK_FILE_SLOTS; i++)
    if (g_pack_files[i].file == file) { fd = g_pack_files[i].fd; break; }
  mutexUnlock(&g_pack_file_lock);
  return fd;
}

static int packed_fclose(FILE *file) {
  void *data = NULL;
  int fd = -1;
  mutexLock(&g_pack_file_lock);
  for (int i = 0; i < PACK_FILE_SLOTS; i++) {
    if (g_pack_files[i].file == file) {
      data = g_pack_files[i].data;
      fd = g_pack_files[i].fd;
      g_pack_files[i].file = NULL;
      g_pack_files[i].data = NULL;
      g_pack_files[i].fd = -1;
      break;
    }
  }
  mutexUnlock(&g_pack_file_lock);
  if (!data) return 0;
  int result = fclose(file);
  if (fd >= 0) close_fake(fd);
  free(data);
  return result == 0 ? 1 : -1;
}

FILE *fopen_fake(const char *path, const char *mode) {
  if (force_vulkan_sentinel_path(path))
    return force_vulkan_sentinel_fopen(mode);
  size_t synth_size = 0;
  const char *synth = synthetic_proc(path, &synth_size);
  if (synth) {
    size_t n = synth_size ? synth_size : strlen(synth);
    void *data = malloc(n ? n : 1);
    if (data && n) memcpy(data, synth, n);
    FILE *file = data ? fmemopen(data, n ? n : 1, "r") : NULL;
    if (!file || !packed_file_add(file, data, -1)) {
      if (file) fclose(file);
      free(data);
      return NULL;
    }
    return file;
  }
  const int writing = strpbrk(mode, "wa+") != NULL;
  if (!writing && strchr(mode, 'r')) {
    FILE *packed = packed_fopen(path);
    if (packed) return packed;
  }
  char _nb[600]; path = dev_abs(path, _nb, sizeof _nb);
  FILE *f = fopen(path, mode);
  if (!f && writing) {            // save file: create the subdir and retry
    mkdir_parents(path);
    f = fopen(path, mode);
  }
  if (!f && !writing && strchr(mode, 'r')) {
    char alt[512];
    if (assets_suffix_fallback(path, alt, sizeof(alt))) {
      f = fopen(alt, mode);
    }
  }
  if (!f)
    return NULL;
  return f;
}

/* Android LP64's public FILE ABI is 152 bytes.  __sF is exactly three
 * consecutive FILE objects, so stdout/stderr pointer arithmetic uses 152. */
_Alignas(8) uint8_t fake_sF[3][152]; // referenced by imports.c (__sF / std{in,out,err})
_Static_assert(sizeof(fake_sF[0]) == 152, "Android LP64 FILE size changed");

static int fake_file_index(const void *f) {
  const uintptr_t address = (uintptr_t)f;
  const uintptr_t base = (uintptr_t)&fake_sF[0][0];
  if (address < base || address >= base + sizeof(fake_sF)) return -1;
  const uintptr_t offset = address - base;
  if (offset % sizeof(fake_sF[0]) != 0) return -1;
  const size_t index = offset / sizeof(fake_sF[0]);
  return index < 3 ? (int)index : -1;
}

static int is_fake_file(const void *f) { return fake_file_index(f) >= 0; }

size_t fwrite_fake(const void *ptr, size_t size, size_t n, FILE *f) {
  if (is_fake_file(f)) return n;
  return fwrite(ptr, size, n, f);
}
size_t fread_fake(void *ptr, size_t size, size_t n, FILE *f) {
  if (is_fake_file(f)) return 0;
  return fread(ptr, size, n, f);
}
int fputc_fake(int c, FILE *f) { if (is_fake_file(f)) return c; return fputc(c, f); }
int fgetc_fake(FILE *f) { if (is_fake_file(f)) return EOF; return fgetc(f); }
int fputs_fake(const char *s, FILE *f) { if (is_fake_file(f)) return 0; return fputs(s, f); }
int fflush_fake(FILE *f) { if (is_fake_file(f) || f == NULL) return 0; return fflush(f); }
int fclose_fake(FILE *f) {
  if (is_fake_file(f)) return 0;
  int packed = packed_fclose(f);
  return packed ? packed < 0 ? -1 : 0 : fclose(f);
}
int ferror_fake(FILE *f) { if (is_fake_file(f)) return 0; return ferror(f); }
int feof_fake(FILE *f) { if (is_fake_file(f)) return 1; return feof(f); }
int fileno_fake(FILE *f) {
  int fake_index = fake_file_index(f);
  if (fake_index >= 0) return fake_index;
  int packed = packed_fileno(f);
  return packed >= 0 ? packed : fileno(f);
}
void clearerr_fake(FILE *f) { if (!is_fake_file(f)) clearerr(f); }
int fscanf_fake(FILE *f, const char *fmt, ...) {
  if (is_fake_file(f)) return EOF;
  va_list va;
  va_start(va, fmt);
  int result = vfscanf(f, fmt, va);
  va_end(va);
  return result;
}
void rewind_fake(FILE *f) { if (!is_fake_file(f)) rewind(f); }
FILE *freopen_fake(const char *path, const char *mode, FILE *f) {
  /* The fake standard streams deliberately remain silent/empty.  Pretending
   * to redirect one keeps its Android FILE address stable without exposing it
   * to newlib, whose FILE object has a different representation. */
  if (is_fake_file(f)) return f;
  return freopen(path, mode, f);
}
int ungetc_fake(int c, FILE *f) {
  if (is_fake_file(f)) return EOF;
  return ungetc(c, f);
}
int setvbuf_fake(FILE *f, char *buffer, int mode, size_t size) {
  if (is_fake_file(f)) return 0;
  return setvbuf(f, buffer, mode, size);
}
void setbuf_fake(FILE *f, char *buffer) {
  if (!is_fake_file(f)) setbuf(f, buffer);
}
wint_t fputwc_fake(wchar_t wc, FILE *f) {
  if (is_fake_file(f)) return (wint_t)wc;
  return fputwc(wc, f);
}
wint_t getwc_fake(FILE *f) {
  if (is_fake_file(f)) return WEOF;
  return fgetwc(f);
}
wint_t ungetwc_fake(wint_t wc, FILE *f) {
  if (is_fake_file(f)) return WEOF;
  return ungetwc(wc, f);
}
int fseek_fake(FILE *f, long off, int whence) { if (is_fake_file(f)) return -1; return fseek(f, off, whence); }
long ftell_fake(FILE *f) { if (is_fake_file(f)) return -1; return ftell(f); }
char *fgets_fake(char *s, int n, FILE *f) { if (is_fake_file(f)) return NULL; return fgets(s, n, f); }

int fprintf_fake(FILE *f, const char *fmt, ...) {
  if (is_fake_file(f)) return 0;
  va_list va; va_start(va, fmt);
  int ret = vfprintf(f, fmt, va);
  va_end(va);
  return ret;
}
int vfprintf_fake(FILE *f, const char *fmt, va_list va) {
  if (is_fake_file(f)) return 0;
  return vfprintf(f, fmt, va);
}
int vfscanf_fake(FILE *f, const char *fmt, va_list va) {
  if (is_fake_file(f)) return EOF;
  return vfscanf(f, fmt, va);
}

long read_fake(int fd, void *buf, size_t count) {
  if (!buf && count) { errno = EFAULT; return -1; }
  if (count > (size_t)LONG_MAX) { errno = EINVAL; return -1; }
  for (;;) {
    NxFdRouteTicket ticket;
    if (!nx_fd_route_snapshot(fd, &ticket)) return -1;
    long cached_result = 0;
    const int cached =
      ra_read_if_attached(fd, &ticket, buf, count, &cached_result);
    if (cached > 0)
      return cached_result;
    if (cached < 0) continue;

    return nx_read(fd, buf, count);
  }
}
long write_fake(int fd, const void *buf, size_t count) {
  if (!buf && count) { errno = EFAULT; return -1; }
  if (count > (size_t)LONG_MAX) { errno = EINVAL; return -1; }
  return nx_write(fd, buf, count);
}
static void network_track_close(int fd);

int close_fake_backend(int fd) {
  const int is_epoll = nx_epoll_is_fd(fd);
  const int is_asset = asset_pack_fd_is(fd);
  const int is_fake = fakefd_is_fake(fd);
  if (!is_epoll && !is_asset &&
      ((is_fake && !fakefd_is_live(fd)) ||
       (!is_fake && fcntl(fd, F_GETFD, 0) < 0))) {
    errno = EBADF;
    return -1;
  }
  if (!is_epoll && !is_asset && !is_fake)
    nx_file_io_finalize_fd(fd);
  /* Invalidate numeric registrations before the descriptor can be recycled
   * by another thread.  A failed close leaves a conservative unregistration
   * instead of allowing a stale callback to alias a future file object. */
  nx_epoll_forget_fd(fd);
  android_native_looper_forget_fd(fd);
  network_track_close(fd);
  int result;
  if (is_epoll) {
    result = nx_epoll_close(fd) ? 0 : -1;
  } else if (is_asset) {
    fd_ino_clear(fd);
    result = asset_pack_close_fd(fd);
  } else {
    ra_detach(fd);
    fd_ino_clear(fd);
    result = is_fake ? fakefd_close(fd) : close(fd);
  }
  return result;
}
int close_fake(int fd) {
  NxFdRoutePair guard;
  if (!nx_fd_route_replace_begin(fd, &guard)) return -1;
  int result = close_fake_backend(fd);
  int saved = errno;
  nx_fd_route_replace_end(&guard);
  errno = saved;
  return result;
}
int pipe_fake(int fds[2]) { return fakefd_pipe(fds); }

/* Bionic socket ABI to libnx BSD socket bridge. */
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <poll.h>
#include <fcntl.h>
#include <sys/select.h>
#include <sys/time.h>
int g_net_on = 1;

/* Transport observability is counter-only: never retain hosts, URLs, packet
 * contents, credentials, cookies, or tokens.  Relaxed atomics keep the shim
 * safe on curl/resolver workers without putting a mutex in hot send/recv
 * paths. */
static NetworkTransportDiagnostics g_network_transport_diagnostics;
/* Horizon's BSD service on the tested hardware rejects AF_INET6 sockets with
 * EAFNOSUPPORT.  Learn that once, then stop returning unusable IPv6 candidates
 * to Android's happy-eyeballs/update logic. */
static int32_t g_ipv6_socket_state;

/* Keep payload-free per-socket progress for hardware diagnosis.  Aggregate
 * receive progress cannot reveal one timed-out block while the other three
 * downloader sockets are still moving.  Horizon descriptors stay small in
 * this process; out-of-range descriptors retain the aggregate counters. */
#define NETWORK_TRACKED_FD_LIMIT 1024
#define NETWORK_BULK_MIN_BYTES   (UINT64_C(64) << 10)
#define NETWORK_STALL_MIN_BYTES  NETWORK_BULK_MIN_BYTES
#define NETWORK_STALL_MIN_MS     UINT64_C(10000)
#define NETWORK_READINESS_PROBE_MIN_MS UINT64_C(500)
#define NETWORK_BULK_READINESS_PROBE_INTERVAL_MS UINT64_C(250)
#define NETWORK_TLS_READINESS_PROBE_INTERVAL_MS UINT64_C(1000)
typedef struct {
  Mutex owner_lock;
  uint32_t active;
  uint32_t stream;
  uint32_t datagram;
  uint32_t receive_window_state; /* 0=pending, 1=setting, 2=set, 3=failed */
  uint32_t udp_receive_window_state; /* 0=pending, 1=setting, 2=set, 3=failed */
  uint32_t recv_inflight;
  uint32_t readiness_probe_state; /* 0=none/consumed, 1=valid, 2=failed */
  uint32_t udp_send_route_state; /* 0=unknown, 1=retain name, 2=connected peer */
  uint32_t udp_send_errors;
  uint32_t udp_receive_errors;
  int32_t udp_last_send_error;
  int32_t udp_last_receive_error;
  uint64_t generation;
  uint64_t description_id; /* shared by dup/F_DUPFD aliases */
  uint64_t connected_tick_ns;
  uint64_t received_bytes;
  uint64_t last_receive_tick_ns;
  uint64_t last_readiness_probe_tick_ns;
  uint64_t readiness_queued_bytes;
  uint64_t last_recv_enter_tick_ns;
  uint64_t last_poll_tick_ns;
  uint64_t udp_send_calls;
  uint64_t udp_sent_bytes;
  uint64_t udp_receive_calls;
  uint64_t udp_received_bytes;
  uintptr_t last_recv_thread;
  uintptr_t last_poll_thread;
} NetworkSocketProgress;
static NetworkSocketProgress g_network_socket_progress[NETWORK_TRACKED_FD_LIMIT];
static uint64_t g_network_socket_generation;
static uint32_t g_network_long_stream_receive_window;
static uint32_t g_network_datagram_receive_window;

static int network_socket_is_datagram(int fd) {
  if (fd < 0 || fd >= NETWORK_TRACKED_FD_LIMIT) return 0;
  NetworkSocketProgress *progress = &g_network_socket_progress[fd];
  return __atomic_load_n(&progress->active, __ATOMIC_ACQUIRE) &&
         __atomic_load_n(&progress->datagram, __ATOMIC_RELAXED);
}

static int network_sockaddr_endpoint_equal(const unsigned char *left,
                                            unsigned left_length,
                                            const unsigned char *right,
                                            unsigned right_length) {
  if (!left || !right || left_length < 2 || right_length < 2 ||
      left[1] != right[1])
    return 0;
  if (left[1] == AF_INET && left_length >= sizeof(struct sockaddr_in) &&
      right_length >= sizeof(struct sockaddr_in)) {
    struct sockaddr_in a;
    struct sockaddr_in b;
    memcpy(&a, left, sizeof a);
    memcpy(&b, right, sizeof b);
    return a.sin_port == b.sin_port &&
           a.sin_addr.s_addr == b.sin_addr.s_addr;
  }
  if (left[1] == AF_INET6 && left_length >= sizeof(struct sockaddr_in6) &&
      right_length >= sizeof(struct sockaddr_in6)) {
    struct sockaddr_in6 a;
    struct sockaddr_in6 b;
    memcpy(&a, left, sizeof a);
    memcpy(&b, right, sizeof b);
    return a.sin6_port == b.sin6_port &&
           a.sin6_scope_id == b.sin6_scope_id &&
           !memcmp(&a.sin6_addr, &b.sin6_addr, sizeof a.sin6_addr);
  }
  return left_length == right_length &&
         !memcmp(left, right, left_length);
}

/* Linux accepts a redundant destination on a connected UDP send.  Horizon's
 * BSD service returns EISCONN instead.  Resolve the supplied native address
 * against getpeername once per socket and omit it only when it names the exact
 * connected peer; unconnected or alternate-destination datagrams retain their
 * original sendto contract. */
static int network_udp_destination_is_connected_peer(
    int fd, const unsigned char *destination, unsigned destination_length) {
  if (!destination || !network_socket_is_datagram(fd)) return 0;
  NetworkSocketProgress *progress = &g_network_socket_progress[fd];
  uint32_t route_state = __atomic_load_n(&progress->udp_send_route_state,
                                          __ATOMIC_ACQUIRE);
  if (route_state) return route_state == 2;

  unsigned char peer[128];
  socklen_t peer_length = sizeof peer;
  const int saved = errno;
  const int peer_result = getpeername(fd, (struct sockaddr *)peer,
                                      &peer_length);
  const uint32_t resolved = peer_result == 0 &&
                            network_sockaddr_endpoint_equal(
                              destination, destination_length, peer,
                              (unsigned)peer_length)
    ? 2u : 1u;
  uint32_t expected = 0;
  if (!__atomic_compare_exchange_n(&progress->udp_send_route_state,
                                    &expected, resolved, false,
                                    __ATOMIC_RELEASE, __ATOMIC_ACQUIRE))
    route_state = expected;
  else
    route_state = resolved;

  errno = saved;
  return route_state == 2;
}

static void network_maybe_promote_datagram_window(int fd,
                                                  NetworkSocketProgress *progress,
                                                  uint64_t received_bytes);

static void network_udp_record_send(int fd, long result) {
  if (!network_socket_is_datagram(fd)) return;
  NetworkSocketProgress *progress = &g_network_socket_progress[fd];
  __atomic_fetch_add(&progress->udp_send_calls, 1, __ATOMIC_RELAXED);
  if (result < 0) {
    const int error = errno;
    __atomic_fetch_add(&progress->udp_send_errors, 1, __ATOMIC_RELAXED);
    __atomic_store_n(&progress->udp_last_send_error, error,
                     __ATOMIC_RELAXED);

    return;
  }
  __atomic_fetch_add(&progress->udp_sent_bytes, (uint64_t)result,
                     __ATOMIC_RELAXED);
  __atomic_store_n(&progress->udp_last_send_error, 0, __ATOMIC_RELAXED);
}

static void network_udp_record_receive(int fd, long result) {
  if (!network_socket_is_datagram(fd)) return;
  NetworkSocketProgress *progress = &g_network_socket_progress[fd];
  __atomic_fetch_add(&progress->udp_receive_calls, 1, __ATOMIC_RELAXED);
  if (result < 0) {
    const int error = errno;
    __atomic_fetch_add(&progress->udp_receive_errors, 1, __ATOMIC_RELAXED);
    __atomic_store_n(&progress->udp_last_receive_error, error,
                     __ATOMIC_RELAXED);

    return;
  }
  /* The KCP receive queue drains in bursts; the default Horizon datagram
   * buffer is small enough that a burst plus any il2cpp GC pause overflows
   * it, and the resulting silent loss collapses KCP into retransmit
   * backoff.  Promote the datagram receive window once bulk traffic is
   * confirmed, mirroring the stream promotion below. */
  const uint64_t received_total = __atomic_add_fetch(
    &progress->udp_received_bytes, (uint64_t)result, __ATOMIC_RELAXED);
  __atomic_store_n(&progress->last_receive_tick_ns,
                   armTicksToNs(armGetSystemTick()), __ATOMIC_RELAXED);
  __atomic_store_n(&progress->udp_last_receive_error, 0, __ATOMIC_RELAXED);
  network_maybe_promote_datagram_window(fd, progress, received_total);
}

static void network_track_open(int fd, int native_type) {
  if (fd < 0 || fd >= NETWORK_TRACKED_FD_LIMIT) return;
  NetworkSocketProgress *progress = &g_network_socket_progress[fd];
  mutexLock(&progress->owner_lock);
  __atomic_store_n(&progress->active, 0, __ATOMIC_RELEASE);
  __atomic_store_n(&progress->stream, native_type == SOCK_STREAM,
                   __ATOMIC_RELAXED);
  __atomic_store_n(&progress->datagram, native_type == SOCK_DGRAM,
                   __ATOMIC_RELAXED);
  __atomic_store_n(&progress->receive_window_state, 0, __ATOMIC_RELAXED);
  __atomic_store_n(&progress->udp_receive_window_state, 0, __ATOMIC_RELAXED);
  __atomic_store_n(&progress->recv_inflight, 0, __ATOMIC_RELAXED);
  __atomic_store_n(&progress->readiness_probe_state, 0, __ATOMIC_RELAXED);
  __atomic_store_n(&progress->udp_send_route_state, 0, __ATOMIC_RELAXED);
  __atomic_store_n(&progress->udp_send_errors, 0, __ATOMIC_RELAXED);
  __atomic_store_n(&progress->udp_receive_errors, 0, __ATOMIC_RELAXED);
  __atomic_store_n(&progress->udp_last_send_error, 0, __ATOMIC_RELAXED);
  __atomic_store_n(&progress->udp_last_receive_error, 0, __ATOMIC_RELAXED);
  const uint64_t generation = __atomic_add_fetch(
    &g_network_socket_generation, 1, __ATOMIC_RELAXED);
  __atomic_store_n(&progress->generation, generation, __ATOMIC_RELAXED);
  __atomic_store_n(&progress->description_id, generation,
                   __ATOMIC_RELAXED);
  __atomic_store_n(&progress->connected_tick_ns, 0, __ATOMIC_RELAXED);
  __atomic_store_n(&progress->received_bytes, 0, __ATOMIC_RELAXED);
  __atomic_store_n(&progress->last_receive_tick_ns, 0, __ATOMIC_RELAXED);
  __atomic_store_n(&progress->last_readiness_probe_tick_ns, 0,
                   __ATOMIC_RELAXED);
  __atomic_store_n(&progress->readiness_queued_bytes, 0, __ATOMIC_RELAXED);
  __atomic_store_n(&progress->last_recv_enter_tick_ns, 0, __ATOMIC_RELAXED);
  __atomic_store_n(&progress->last_poll_tick_ns, 0, __ATOMIC_RELAXED);
  __atomic_store_n(&progress->udp_send_calls, 0, __ATOMIC_RELAXED);
  __atomic_store_n(&progress->udp_sent_bytes, 0, __ATOMIC_RELAXED);
  __atomic_store_n(&progress->udp_receive_calls, 0, __ATOMIC_RELAXED);
  __atomic_store_n(&progress->udp_received_bytes, 0, __ATOMIC_RELAXED);
  __atomic_store_n(&progress->last_recv_thread, 0, __ATOMIC_RELAXED);
  __atomic_store_n(&progress->last_poll_thread, 0, __ATOMIC_RELAXED);
  __atomic_store_n(&progress->active, 1, __ATOMIC_RELEASE);
  mutexUnlock(&progress->owner_lock);
}

static void network_track_connect(int fd) {
  if (fd < 0 || fd >= NETWORK_TRACKED_FD_LIMIT) return;
  NetworkSocketProgress *progress = &g_network_socket_progress[fd];
  if (!__atomic_load_n(&progress->active, __ATOMIC_ACQUIRE) ||
      !__atomic_load_n(&progress->stream, __ATOMIC_RELAXED))
    return;
  __atomic_store_n(&progress->connected_tick_ns,
                   armTicksToNs(armGetSystemTick()), __ATOMIC_RELAXED);
}

static void network_track_close(int fd) {
  if (fd < 0 || fd >= NETWORK_TRACKED_FD_LIMIT) return;
  NetworkSocketProgress *progress = &g_network_socket_progress[fd];
  int trace_datagram = 0;
  uint64_t send_calls = 0;
  uint64_t sent_bytes = 0;
  uint64_t receive_calls = 0;
  uint64_t received_bytes = 0;
  uint32_t send_errors = 0;
  uint32_t receive_errors = 0;
  int32_t last_send_error = 0;
  int32_t last_receive_error = 0;
  mutexLock(&progress->owner_lock);
  trace_datagram =
    __atomic_load_n(&progress->active, __ATOMIC_ACQUIRE) &&
    __atomic_load_n(&progress->datagram, __ATOMIC_RELAXED);
  if (trace_datagram) {
    send_calls = __atomic_load_n(&progress->udp_send_calls,
                                  __ATOMIC_RELAXED);
    sent_bytes = __atomic_load_n(&progress->udp_sent_bytes,
                                  __ATOMIC_RELAXED);
    receive_calls = __atomic_load_n(&progress->udp_receive_calls,
                                     __ATOMIC_RELAXED);
    received_bytes = __atomic_load_n(&progress->udp_received_bytes,
                                      __ATOMIC_RELAXED);
    send_errors = __atomic_load_n(&progress->udp_send_errors,
                                   __ATOMIC_RELAXED);
    receive_errors = __atomic_load_n(&progress->udp_receive_errors,
                                      __ATOMIC_RELAXED);
    last_send_error = __atomic_load_n(&progress->udp_last_send_error,
                                       __ATOMIC_RELAXED);
    last_receive_error = __atomic_load_n(
      &progress->udp_last_receive_error, __ATOMIC_RELAXED);
  }
  __atomic_store_n(&progress->active, 0, __ATOMIC_RELEASE);
  mutexUnlock(&progress->owner_lock);
  if (trace_datagram) {
    char message[256];
    snprintf(message, sizeof message,
             "event=close fd=%d tx_calls=%llu tx_bytes=%llu tx_errors=%u "
             "tx_errno=%d rx_calls=%llu rx_bytes=%llu rx_errors=%u "
             "rx_errno=%d",
             fd, (unsigned long long)send_calls,
             (unsigned long long)sent_bytes, send_errors, last_send_error,
             (unsigned long long)receive_calls,
             (unsigned long long)received_bytes, receive_errors,
             last_receive_error);
  }
}

/* dup/F_DUPFD create a second descriptor for the same socket description.
 * Curl is allowed to receive through one alias and poll another, so a numeric
 * descriptor-only tracker must copy the source's readiness baseline to the
 * new alias.  Give the alias its own generation: closing either descriptor
 * must not invalidate an in-flight operation on the other one. */
void network_track_duplicate(int source, int target) {
  if (target < 0 || target >= NETWORK_TRACKED_FD_LIMIT || source == target)
    return;

  uint32_t source_active = 0;
  uint32_t source_stream = 0;
  uint32_t source_datagram = 0;
  uint32_t source_window_state = 0;
  uint64_t source_description = 0;
  uint64_t source_connected = 0;
  uint64_t source_received = 0;
  uint64_t source_last_receive = 0;
  if (source >= 0 && source < NETWORK_TRACKED_FD_LIMIT) {
    NetworkSocketProgress *from = &g_network_socket_progress[source];
    mutexLock(&from->owner_lock);
    source_active = __atomic_load_n(&from->active, __ATOMIC_ACQUIRE);
    source_stream = __atomic_load_n(&from->stream, __ATOMIC_RELAXED);
    source_datagram = __atomic_load_n(&from->datagram, __ATOMIC_RELAXED);
    source_window_state = __atomic_load_n(&from->receive_window_state,
                                           __ATOMIC_RELAXED);
    source_description = __atomic_load_n(&from->description_id,
                                          __ATOMIC_RELAXED);
    source_connected = __atomic_load_n(&from->connected_tick_ns,
                                        __ATOMIC_RELAXED);
    source_received = __atomic_load_n(&from->received_bytes,
                                       __ATOMIC_RELAXED);
    source_last_receive = __atomic_load_n(&from->last_receive_tick_ns,
                                           __ATOMIC_RELAXED);
    mutexUnlock(&from->owner_lock);
  }

  NetworkSocketProgress *to = &g_network_socket_progress[target];
  mutexLock(&to->owner_lock);
  __atomic_store_n(&to->active, 0, __ATOMIC_RELEASE);
  __atomic_store_n(&to->stream, source_stream, __ATOMIC_RELAXED);
  __atomic_store_n(&to->datagram, source_datagram, __ATOMIC_RELAXED);
  __atomic_store_n(&to->receive_window_state, source_window_state,
                   __ATOMIC_RELAXED);
  __atomic_store_n(&to->recv_inflight, 0, __ATOMIC_RELAXED);
  __atomic_store_n(&to->readiness_probe_state, 0, __ATOMIC_RELAXED);
  __atomic_store_n(&to->udp_send_route_state, 0, __ATOMIC_RELAXED);
  __atomic_store_n(&to->udp_send_errors, 0, __ATOMIC_RELAXED);
  __atomic_store_n(&to->udp_receive_errors, 0, __ATOMIC_RELAXED);
  __atomic_store_n(&to->udp_last_send_error, 0, __ATOMIC_RELAXED);
  __atomic_store_n(&to->udp_last_receive_error, 0, __ATOMIC_RELAXED);
  __atomic_store_n(&to->generation,
                   __atomic_add_fetch(&g_network_socket_generation, 1,
                                      __ATOMIC_RELAXED),
                   __ATOMIC_RELAXED);
  __atomic_store_n(&to->description_id,
                   source_active && (source_stream || source_datagram)
                     ? source_description : 0,
                   __ATOMIC_RELAXED);
  __atomic_store_n(&to->connected_tick_ns, source_connected,
                   __ATOMIC_RELAXED);
  __atomic_store_n(&to->received_bytes, source_received, __ATOMIC_RELAXED);
  __atomic_store_n(&to->last_receive_tick_ns, source_last_receive,
                   __ATOMIC_RELAXED);
  __atomic_store_n(&to->last_readiness_probe_tick_ns, 0,
                   __ATOMIC_RELAXED);
  __atomic_store_n(&to->readiness_queued_bytes, 0, __ATOMIC_RELAXED);
  __atomic_store_n(&to->last_recv_enter_tick_ns, 0, __ATOMIC_RELAXED);
  __atomic_store_n(&to->last_poll_tick_ns, 0, __ATOMIC_RELAXED);
  __atomic_store_n(&to->udp_send_calls, 0, __ATOMIC_RELAXED);
  __atomic_store_n(&to->udp_sent_bytes, 0, __ATOMIC_RELAXED);
  __atomic_store_n(&to->udp_receive_calls, 0, __ATOMIC_RELAXED);
  __atomic_store_n(&to->udp_received_bytes, 0, __ATOMIC_RELAXED);
  __atomic_store_n(&to->last_recv_thread, 0, __ATOMIC_RELAXED);
  __atomic_store_n(&to->last_poll_thread, 0, __ATOMIC_RELAXED);
  __atomic_store_n(&to->active,
                   source_active && (source_stream || source_datagram),
                   __ATOMIC_RELEASE);
  mutexUnlock(&to->owner_lock);
}

/* A duplicated descriptor names the same socket description, but TLS stacks
 * may consume through one alias and wait through another.  Keep receive hot
 * paths O(1); the bounded poll path can cheaply find the newest progress from
 * at most 1024 descriptor slots.  Taking maxima also avoids double-counting
 * the baseline copied when an alias is created after traffic has started. */
static int network_track_receive_snapshot(int fd, uint64_t *bytes_out,
                                          uint64_t *last_receive_out) {
  if (fd < 0 || fd >= NETWORK_TRACKED_FD_LIMIT) return 0;
  NetworkSocketProgress *progress = &g_network_socket_progress[fd];
  if (!__atomic_load_n(&progress->active, __ATOMIC_ACQUIRE) ||
      !__atomic_load_n(&progress->stream, __ATOMIC_RELAXED))
    return 0;
  const uint64_t description = __atomic_load_n(
    &progress->description_id, __ATOMIC_RELAXED);
  if (!description) return 0;

  uint64_t bytes = __atomic_load_n(&progress->received_bytes,
                                    __ATOMIC_RELAXED);
  uint64_t last = __atomic_load_n(&progress->last_receive_tick_ns,
                                   __ATOMIC_RELAXED);
  for (unsigned alias_fd = 0; alias_fd < NETWORK_TRACKED_FD_LIMIT;
       ++alias_fd) {
    if ((int)alias_fd == fd) continue;
    NetworkSocketProgress *alias = &g_network_socket_progress[alias_fd];
    if (!__atomic_load_n(&alias->active, __ATOMIC_ACQUIRE) ||
        !__atomic_load_n(&alias->stream, __ATOMIC_RELAXED) ||
        __atomic_load_n(&alias->description_id, __ATOMIC_RELAXED) !=
          description)
      continue;
    const uint64_t alias_bytes = __atomic_load_n(&alias->received_bytes,
                                                  __ATOMIC_RELAXED);
    const uint64_t alias_last = __atomic_load_n(
      &alias->last_receive_tick_ns, __ATOMIC_RELAXED);
    if (alias_bytes > bytes) bytes = alias_bytes;
    if (alias_last > last) last = alias_last;
  }
  if (bytes_out) *bytes_out = bytes;
  if (last_receive_out) *last_receive_out = last;
  return bytes != 0 && last != 0;
}

/* Preserve the managed worker identity around the only calls which can consume
 * socket payload.  The generation token prevents a late return from touching
 * a descriptor which close/socket has already recycled. */
static uint64_t network_track_recv_enter(int fd) {
  if (fd < 0 || fd >= NETWORK_TRACKED_FD_LIMIT) return 0;
  NetworkSocketProgress *progress = &g_network_socket_progress[fd];
  mutexLock(&progress->owner_lock);
  if (!__atomic_load_n(&progress->active, __ATOMIC_ACQUIRE) ||
      (!__atomic_load_n(&progress->stream, __ATOMIC_RELAXED) &&
       !__atomic_load_n(&progress->datagram, __ATOMIC_RELAXED))) {
    mutexUnlock(&progress->owner_lock);
    return 0;
  }
  const uint64_t generation =
    __atomic_load_n(&progress->generation, __ATOMIC_RELAXED);
  __atomic_store_n(&progress->last_recv_thread,
                   (uintptr_t)pthread_self(), __ATOMIC_RELAXED);
  __atomic_store_n(&progress->last_recv_enter_tick_ns,
                   armTicksToNs(armGetSystemTick()), __ATOMIC_RELAXED);
  __atomic_fetch_add(&progress->recv_inflight, 1, __ATOMIC_RELAXED);
  mutexUnlock(&progress->owner_lock);
  return generation;
}

static void network_track_recv_leave(int fd, uint64_t generation) {
  if (!generation || fd < 0 || fd >= NETWORK_TRACKED_FD_LIMIT) return;
  NetworkSocketProgress *progress = &g_network_socket_progress[fd];
  mutexLock(&progress->owner_lock);
  if (generation != __atomic_load_n(&progress->generation,
                                     __ATOMIC_RELAXED)) {
    mutexUnlock(&progress->owner_lock);
    return;
  }
  const uint32_t inflight = __atomic_load_n(&progress->recv_inflight,
                                            __ATOMIC_RELAXED);
  if (inflight)
    __atomic_store_n(&progress->recv_inflight, inflight - 1,
                     __ATOMIC_RELAXED);
  mutexUnlock(&progress->owner_lock);
}

#define NET_DIAG_ADD(field, value) \
  __atomic_fetch_add(&g_network_transport_diagnostics.field, (value), \
                     __ATOMIC_RELAXED)
#define NET_DIAG_STORE(field, value) \
  __atomic_store_n(&g_network_transport_diagnostics.field, (value), \
                   __ATOMIC_RELAXED)

static int n2b_errno(int error);

void network_configure_long_stream_receive_window(uint32_t initial_size,
                                                  uint32_t maximum_size) {
  /* Use only the growth already reserved by socketInitialize.  Keeping the
   * initial libnx window on short login/dispatch requests preserves the
   * hardware-proven control path.  The update manifest itself is 233352 bytes,
   * so 1 MiB was too late: promote only after a stream has delivered 64 KiB,
   * above every observed control response but before the manifest's tail. */
  const uint32_t target = maximum_size > initial_size &&
                          maximum_size <= (uint32_t)INT_MAX
    ? maximum_size : 0;
  __atomic_store_n(&g_network_long_stream_receive_window, target,
                   __ATOMIC_RELAXED);
  NET_DIAG_STORE(long_stream_receive_window_target, target);
}

void network_configure_datagram_receive_window(uint32_t initial_size,
                                               uint32_t maximum_size) {
  /* Horizon's BSD config has no udp max field, so the datagram receive
   * queue cannot grow past socketInitialize's reservation at runtime.
   * main.c passes the TCP max as the ceiling: when the enlarged
   * udp_rx_buf_size reservation was accepted the two match and the
   * runtime promotion stays disabled (target 0); when the reservation
   * fell back to the small default the promotion at least tries and the
   * telemetry records the effective window the service granted. */
  const uint32_t target = maximum_size > initial_size &&
                          maximum_size <= (uint32_t)INT_MAX
    ? maximum_size : 0;
  __atomic_store_n(&g_network_datagram_receive_window, target,
                   __ATOMIC_RELAXED);
  NET_DIAG_STORE(datagram_receive_window_target, target);
}

static void network_maybe_promote_receive_window(
    int fd, NetworkSocketProgress *progress, uint64_t received_bytes) {
  if (!progress || received_bytes < NETWORK_BULK_MIN_BYTES) return;
  const uint32_t target = __atomic_load_n(
    &g_network_long_stream_receive_window, __ATOMIC_RELAXED);
  if (!target) return;
  uint32_t expected = 0;
  if (!__atomic_compare_exchange_n(&progress->receive_window_state,
                                    &expected, 1, false,
                                    __ATOMIC_ACQ_REL, __ATOMIC_RELAXED))
    return;

  NET_DIAG_ADD(long_stream_window_attempts, 1);
  const int saved_errno = errno;
  const int requested = (int)target;
  const int result = setsockopt(fd, SOL_SOCKET, SO_RCVBUF,
                                &requested, sizeof requested);
  const int option_error = result < 0 ? errno : 0;
  int effective = 0;
  socklen_t effective_size = sizeof effective;
  if (result == 0 &&
      getsockopt(fd, SOL_SOCKET, SO_RCVBUF,
                 &effective, &effective_size) < 0)
    effective = requested;
  errno = saved_errno;

  if (result == 0) {
    NET_DIAG_ADD(long_stream_window_successes, 1);
    NET_DIAG_STORE(last_long_stream_window_error, 0);
    NET_DIAG_STORE(last_long_stream_window_effective,
                   effective > 0 ? (uint64_t)effective : target);
    __atomic_store_n(&progress->receive_window_state, 2, __ATOMIC_RELEASE);
  } else {
    NET_DIAG_ADD(long_stream_window_failures, 1);
    NET_DIAG_STORE(last_long_stream_window_error, n2b_errno(option_error));
    __atomic_store_n(&progress->receive_window_state, 3, __ATOMIC_RELEASE);
  }
}

static void network_maybe_promote_datagram_window(
    int fd, NetworkSocketProgress *progress, uint64_t received_bytes) {
  if (!progress || received_bytes < NETWORK_BULK_MIN_BYTES) return;
  const uint32_t target = __atomic_load_n(
    &g_network_datagram_receive_window, __ATOMIC_RELAXED);
  if (!target) return;
  uint32_t expected = 0;
  if (!__atomic_compare_exchange_n(&progress->udp_receive_window_state,
                                    &expected, 1, false,
                                    __ATOMIC_ACQ_REL, __ATOMIC_RELAXED))
    return;

  NET_DIAG_ADD(datagram_window_attempts, 1);
  const int saved_errno = errno;
  const int requested = (int)target;
  const int result = setsockopt(fd, SOL_SOCKET, SO_RCVBUF,
                                &requested, sizeof requested);
  const int option_error = result < 0 ? errno : 0;
  int effective = 0;
  socklen_t effective_size = sizeof effective;
  if (result == 0 &&
      getsockopt(fd, SOL_SOCKET, SO_RCVBUF,
                 &effective, &effective_size) < 0)
    effective = requested;
  errno = saved_errno;

  if (result == 0) {
    NET_DIAG_ADD(datagram_window_successes, 1);
    NET_DIAG_STORE(last_datagram_window_error, 0);
    NET_DIAG_STORE(last_datagram_window_effective,
                   effective > 0 ? (uint64_t)effective : target);
    __atomic_store_n(&progress->udp_receive_window_state, 2,
                     __ATOMIC_RELEASE);
  } else {
    NET_DIAG_ADD(datagram_window_failures, 1);
    NET_DIAG_STORE(last_datagram_window_error, n2b_errno(option_error));
    __atomic_store_n(&progress->udp_receive_window_state, 3,
                     __ATOMIC_RELEASE);
  }
}

int network_get_transport_progress_fast(NetworkTransportProgress *out) {
  if (!out) return 0;
  memset(out, 0, sizeof(*out));
  out->recv_calls = __atomic_load_n(
    &g_network_transport_diagnostics.recv_calls, __ATOMIC_RELAXED);
  out->received_bytes = __atomic_load_n(
    &g_network_transport_diagnostics.received_bytes, __ATOMIC_RELAXED);
  out->recv_failures = __atomic_load_n(
    &g_network_transport_diagnostics.recv_failures, __ATOMIC_RELAXED);
  out->last_receive_tick_ns = __atomic_load_n(
    &g_network_transport_diagnostics.last_receive_tick_ns,
    __ATOMIC_RELAXED);
  out->poll_wait_calls = __atomic_load_n(
    &g_network_transport_diagnostics.poll_wait_calls, __ATOMIC_RELAXED);
  out->poll_wait_failures = __atomic_load_n(
    &g_network_transport_diagnostics.poll_wait_failures, __ATOMIC_RELAXED);

  const uint64_t now_ns = armTicksToNs(armGetSystemTick());
  for (unsigned fd = 0; fd < NETWORK_TRACKED_FD_LIMIT; ++fd) {
    NetworkSocketProgress *progress = &g_network_socket_progress[fd];
    if (!__atomic_load_n(&progress->active, __ATOMIC_ACQUIRE) ||
        !__atomic_load_n(&progress->stream, __ATOMIC_RELAXED))
      continue;
    ++out->tracked_stream_sockets;
    const uint64_t bytes = __atomic_load_n(
      &progress->received_bytes, __ATOMIC_RELAXED);
    const uint64_t last = __atomic_load_n(
      &progress->last_receive_tick_ns, __ATOMIC_RELAXED);
    if (!bytes || !last) continue;
    ++out->receiving_stream_sockets;
    const uint64_t idle_ms = now_ns >= last
      ? (now_ns - last) / UINT64_C(1000000) : 0;
    if (bytes >= NETWORK_STALL_MIN_BYTES &&
        idle_ms >= NETWORK_STALL_MIN_MS)
      ++out->stalled_stream_sockets;
    if (bytes >= NETWORK_STALL_MIN_BYTES &&
        idle_ms > out->longest_stream_idle_ms)
      out->longest_stream_idle_ms = idle_ms;
  }
  return 1;
}

int network_get_transport_diagnostics(NetworkTransportDiagnostics *out) {
  if (!out) return 0;
#define NET_DIAG_LOAD(field) \
  out->field = __atomic_load_n(&g_network_transport_diagnostics.field, \
                               __ATOMIC_RELAXED)
  NET_DIAG_LOAD(dns_calls);
  NET_DIAG_LOAD(dns_successes);
  NET_DIAG_LOAD(dns_failures);
  NET_DIAG_LOAD(last_dns_error);
  NET_DIAG_LOAD(resolver_pool_workers);
  NET_DIAG_LOAD(resolver_jobs_queued);
  NET_DIAG_LOAD(resolver_jobs_completed);
  NET_DIAG_LOAD(resolver_jobs_abandoned);
  NET_DIAG_LOAD(resolver_jobs_coalesced);
  NET_DIAG_LOAD(resolver_cache_hits);
  NET_DIAG_LOAD(resolver_cache_stores);
  NET_DIAG_LOAD(resolver_pool_failures);
  NET_DIAG_LOAD(resolver_default_deadlines);
  NET_DIAG_LOAD(reverse_numeric_results);
  NET_DIAG_LOAD(ipv6_results_filtered);
  NET_DIAG_LOAD(socket_calls);
  NET_DIAG_LOAD(socket_successes);
  NET_DIAG_LOAD(socket_failures);
  NET_DIAG_LOAD(last_socket_error);
  NET_DIAG_LOAD(connect_calls);
  NET_DIAG_LOAD(connect_immediate_successes);
  NET_DIAG_LOAD(connect_in_progress);
  NET_DIAG_LOAD(connect_failures);
  NET_DIAG_LOAD(last_connect_error);
  NET_DIAG_LOAD(connect_error_checks);
  NET_DIAG_LOAD(connect_error_clear);
  NET_DIAG_LOAD(connect_error_failures);
  NET_DIAG_LOAD(last_connect_completion_error);
  NET_DIAG_LOAD(send_calls);
  NET_DIAG_LOAD(sent_bytes);
  NET_DIAG_LOAD(send_would_block);
  NET_DIAG_LOAD(send_failures);
  NET_DIAG_LOAD(last_send_error);
  NET_DIAG_LOAD(recv_calls);
  NET_DIAG_LOAD(received_bytes);
  NET_DIAG_LOAD(recv_eof);
  NET_DIAG_LOAD(recv_would_block);
  NET_DIAG_LOAD(recv_failures);
  NET_DIAG_LOAD(last_recv_error);
  NET_DIAG_LOAD(last_receive_tick_ns);
  NET_DIAG_LOAD(long_stream_receive_window_target);
  NET_DIAG_LOAD(long_stream_window_attempts);
  NET_DIAG_LOAD(long_stream_window_successes);
  NET_DIAG_LOAD(long_stream_window_failures);
  NET_DIAG_LOAD(last_long_stream_window_error);
  NET_DIAG_LOAD(last_long_stream_window_effective);
  NET_DIAG_LOAD(datagram_receive_window_target);
  NET_DIAG_LOAD(datagram_window_attempts);
  NET_DIAG_LOAD(datagram_window_successes);
  NET_DIAG_LOAD(datagram_window_failures);
  NET_DIAG_LOAD(last_datagram_window_error);
  NET_DIAG_LOAD(last_datagram_window_effective);
  NET_DIAG_LOAD(poll_readiness_probes);
  NET_DIAG_LOAD(poll_readiness_hits);
  NET_DIAG_LOAD(poll_readiness_probe_failures);
  NET_DIAG_LOAD(poll_wait_calls);
  NET_DIAG_LOAD(poll_wait_failures);
  NET_DIAG_LOAD(poll_stale_snapshot_recoveries);
  NET_DIAG_LOAD(poll_invalid_fd_recoveries);
  NET_DIAG_LOAD(poll_inactive_fd_recoveries);
  NET_DIAG_LOAD(poll_reused_fd_recoveries);
  NET_DIAG_LOAD(poll_ebadf_fallback_recoveries);
  NET_DIAG_LOAD(poll_ebadf_retries);
  NET_DIAG_LOAD(poll_route_snapshot_retries);
  NET_DIAG_LOAD(poll_probe_other_errors);
  NET_DIAG_LOAD(last_poll_wait_error);
#undef NET_DIAG_LOAD

  out->tracked_stream_sockets = 0;
  out->receiving_stream_sockets = 0;
  out->stalled_stream_sockets = 0;
  out->largest_stream_received_bytes = 0;
  out->largest_stream_idle_ms = 0;
  out->longest_stream_idle_ms = 0;
  out->stalled_stream_queue_probes = 0;
  out->stalled_stream_queue_probe_failures = 0;
  out->stalled_streams_with_queued_data = 0;
  out->stalled_stream_queued_bytes = 0;
  out->largest_stalled_stream_queued_bytes = 0;
  out->stalled_queued_fd = -1;
  out->stalled_queued_recv_inflight = 0;
  out->stalled_queued_generation = 0;
  out->stalled_queued_socket_bytes = 0;
  out->stalled_queued_last_recv_enter_tick_ns = 0;
  out->stalled_queued_last_poll_tick_ns = 0;
  out->stalled_queued_recv_thread = 0;
  out->stalled_queued_poll_thread = 0;
  out->udp_recv_calls = 0;
  out->udp_received_bytes = 0;
  out->udp_send_calls = 0;
  out->udp_sent_bytes = 0;
  out->udp_receive_errors = 0;
  out->tracked_datagram_sockets = 0;
  out->largest_datagram_received_bytes = 0;
  out->last_udp_receive_tick_ns = 0;
  const uint64_t now_ns = armTicksToNs(armGetSystemTick());
  for (unsigned fd = 0; fd < NETWORK_TRACKED_FD_LIMIT; ++fd) {
    NetworkSocketProgress *progress = &g_network_socket_progress[fd];
    if (!__atomic_load_n(&progress->active, __ATOMIC_ACQUIRE) ||
        !__atomic_load_n(&progress->stream, __ATOMIC_RELAXED))
      continue;
    ++out->tracked_stream_sockets;
    const uint64_t bytes =
      __atomic_load_n(&progress->received_bytes, __ATOMIC_RELAXED);
    const uint64_t last =
      __atomic_load_n(&progress->last_receive_tick_ns, __ATOMIC_RELAXED);
    if (!bytes || !last) continue;
    ++out->receiving_stream_sockets;
    const uint64_t idle_ms = now_ns >= last ? (now_ns - last) / 1000000u : 0;
    const int stalled = bytes >= NETWORK_STALL_MIN_BYTES &&
                        idle_ms >= NETWORK_STALL_MIN_MS;
    if (stalled)
      ++out->stalled_stream_sockets;
    if (bytes >= NETWORK_STALL_MIN_BYTES &&
        idle_ms > out->longest_stream_idle_ms)
      out->longest_stream_idle_ms = idle_ms;
    if (bytes > out->largest_stream_received_bytes) {
      out->largest_stream_received_bytes = bytes;
      out->largest_stream_idle_ms = idle_ms;
    }
    if (stalled) {
      /* Never issue BSD IPC from the Unity frame thread: a live FIONREAD
       * query serializes behind a blocking receive and freezes update checks.
       * The bounded poll recovery path already probes FIONREAD on its network
       * worker, so consume only that generation-owned cached result here. */
      const uint64_t generation =
        __atomic_load_n(&progress->generation, __ATOMIC_RELAXED);
      const uint32_t probe_state = __atomic_load_n(
        &progress->readiness_probe_state, __ATOMIC_ACQUIRE);
      const uint64_t queued_bytes = __atomic_load_n(
        &progress->readiness_queued_bytes, __ATOMIC_RELAXED);
      if (!__atomic_load_n(&progress->active, __ATOMIC_ACQUIRE) ||
          generation != __atomic_load_n(&progress->generation,
                                         __ATOMIC_RELAXED))
        continue;
      if (probe_state) {
        ++out->stalled_stream_queue_probes;
        if (probe_state == 2) {
          ++out->stalled_stream_queue_probe_failures;
        } else if (queued_bytes > 0) {
          ++out->stalled_streams_with_queued_data;
          out->stalled_stream_queued_bytes += queued_bytes;
          if (queued_bytes > out->largest_stalled_stream_queued_bytes) {
            out->largest_stalled_stream_queued_bytes = queued_bytes;
            out->stalled_queued_fd = (int32_t)fd;
            out->stalled_queued_recv_inflight =
              __atomic_load_n(&progress->recv_inflight, __ATOMIC_RELAXED);
            out->stalled_queued_generation = generation;
            out->stalled_queued_socket_bytes = bytes;
            out->stalled_queued_last_recv_enter_tick_ns = __atomic_load_n(
              &progress->last_recv_enter_tick_ns, __ATOMIC_RELAXED);
            out->stalled_queued_last_poll_tick_ns = __atomic_load_n(
              &progress->last_poll_tick_ns, __ATOMIC_RELAXED);
            out->stalled_queued_recv_thread = __atomic_load_n(
              &progress->last_recv_thread, __ATOMIC_RELAXED);
            out->stalled_queued_poll_thread = __atomic_load_n(
              &progress->last_poll_thread, __ATOMIC_RELAXED);
          }
        }
      }
    }
  }

  /* Aggregate UDP/KCP sockets separately.  Genshin's bulk resource download
   * runs over KCP (reliable UDP via recvmsg/recvfrom), so without this pass
   * the download throughput is invisible — the TCP counters above freeze at
   * the control-traffic total while gigabytes flow here. */
  for (unsigned fd = 0; fd < NETWORK_TRACKED_FD_LIMIT; ++fd) {
    NetworkSocketProgress *progress = &g_network_socket_progress[fd];
    if (!__atomic_load_n(&progress->active, __ATOMIC_ACQUIRE) ||
        !__atomic_load_n(&progress->datagram, __ATOMIC_RELAXED))
      continue;
    ++out->tracked_datagram_sockets;
    out->udp_recv_calls += __atomic_load_n(&progress->udp_receive_calls,
                                           __ATOMIC_RELAXED);
    out->udp_received_bytes += __atomic_load_n(&progress->udp_received_bytes,
                                               __ATOMIC_RELAXED);
    out->udp_send_calls += __atomic_load_n(&progress->udp_send_calls,
                                           __ATOMIC_RELAXED);
    out->udp_sent_bytes += __atomic_load_n(&progress->udp_sent_bytes,
                                           __ATOMIC_RELAXED);
    out->udp_receive_errors += __atomic_load_n(&progress->udp_receive_errors,
                                               __ATOMIC_RELAXED);
    const uint64_t dbytes = __atomic_load_n(&progress->udp_received_bytes,
                                           __ATOMIC_RELAXED);
    if (dbytes > out->largest_datagram_received_bytes)
      out->largest_datagram_received_bytes = dbytes;
    const uint64_t dlast = __atomic_load_n(&progress->last_receive_tick_ns,
                                           __ATOMIC_RELAXED);
    if (dlast > out->last_udp_receive_tick_ns)
      out->last_udp_receive_tick_ns = dlast;
  }

  NxEpollDiagnostics epoll;
  nx_epoll_get_diagnostics(out->stalled_queued_fd, &epoll);
  out->epoll_wait_calls = epoll.wait_calls;
  out->epoll_delivered_events = epoll.delivered_events;
  out->epoll_wait_timeouts = epoll.wait_timeouts;
  out->epoll_wait_failures = epoll.wait_failures;
  out->epoll_stale_snapshot_retries = epoll.stale_snapshot_retries;
  out->last_epoll_wait_error = epoll.last_wait_error;
  out->epoll_live_sets = epoll.live_sets;
  out->epoll_registered_items = epoll.registered_items;
  out->epoll_disabled_items = epoll.disabled_items;
  out->stalled_queued_epoll_registrations = epoll.probe_registrations;
  out->stalled_queued_epoll_disabled_registrations =
    epoll.probe_disabled_registrations;
  return 1;
}

/* Newlib hides these socket errno names unless its Linux extensions are
 * enabled, but the libnx socket service can still return their ABI values. */
#ifndef ESHUTDOWN
#define ESHUTDOWN 110
#endif
#ifndef ESOCKTNOSUPPORT
#define ESOCKTNOSUPPORT 124
#endif

static int n2b_errno(int e) {
  switch (e) {
    case EAGAIN: return 11; case EINPROGRESS: return 115; case EALREADY: return 114;
    case EISCONN: return 106; case ENOTCONN: return 107; case ECONNREFUSED: return 111;
    case ECONNRESET: return 104; case ECONNABORTED: return 103; case ETIMEDOUT: return 110;
    case ENETUNREACH: return 101; case EHOSTUNREACH: return 113; case ENETDOWN: return 100;
    case EADDRINUSE: return 98; case EADDRNOTAVAIL: return 99; case EINTR: return 4;
    case EPIPE: return 32; case EBADF: return 9; case EINVAL: return 22; case EACCES: return 13;
    case EDESTADDRREQ: return 89; case EPROTOTYPE: return 91;
    case ESOCKTNOSUPPORT: return 94; case EAFNOSUPPORT: return 97;
    case ENETRESET: return 102; case ESHUTDOWN: return 108;
    case ETOOMANYREFS: return 109; case EHOSTDOWN: return 112;
    case EMFILE: return 24; case ENFILE: return 23;
    case EMSGSIZE: return 90; case ENOBUFS: return 105; case ENOMEM: return 12;
    case ENOTSOCK: return 88; case ENOPROTOOPT: return 92;
    case EPROTONOSUPPORT: return 93; case EOPNOTSUPP: return 95;
    default: return e;
  }
}
#define NET_FAIL() do { errno = n2b_errno(errno); } while (0)

/* Keep one aggregate for every bionic socket I/O entry point.  Unity and its
 * bundled HTTP stacks are free to switch between send/recv, sendto/recvfrom,
 * and sendmsg/recvmsg, so every variant has to feed the same counters. */
static void network_record_send_result(long result) {
  NET_DIAG_ADD(send_calls, 1);
  if (result < 0) {
    NET_DIAG_STORE(last_send_error, errno);
    if (errno == 11) NET_DIAG_ADD(send_would_block, 1);
    else NET_DIAG_ADD(send_failures, 1);
  } else {
    NET_DIAG_ADD(sent_bytes, (uint64_t)result);
    NET_DIAG_STORE(last_send_error, 0);
  }
}

static void network_record_recv_result(int fd, long result) {
  NET_DIAG_ADD(recv_calls, 1);
  if (result < 0) {
    NET_DIAG_STORE(last_recv_error, errno);
    if (errno == 11) NET_DIAG_ADD(recv_would_block, 1);
    else NET_DIAG_ADD(recv_failures, 1);
  } else if (result == 0) {
    NET_DIAG_ADD(recv_eof, 1);
    NET_DIAG_STORE(last_recv_error, 0);
  } else {
    const uint64_t now_ns = armTicksToNs(armGetSystemTick());
    NET_DIAG_ADD(received_bytes, (uint64_t)result);
    NET_DIAG_STORE(last_receive_tick_ns, now_ns);
    NET_DIAG_STORE(last_recv_error, 0);
    if (fd >= 0 && fd < NETWORK_TRACKED_FD_LIMIT) {
      NetworkSocketProgress *progress = &g_network_socket_progress[fd];
      if (__atomic_load_n(&progress->active, __ATOMIC_ACQUIRE) &&
          __atomic_load_n(&progress->stream, __ATOMIC_RELAXED)) {
        /* Positive consumption invalidates any older FIONREAD queue sample. */
        __atomic_store_n(&progress->readiness_queued_bytes, 0,
                         __ATOMIC_RELAXED);
        __atomic_store_n(&progress->readiness_probe_state, 0,
                         __ATOMIC_RELEASE);
        const uint64_t received_bytes = __atomic_add_fetch(
          &progress->received_bytes, (uint64_t)result, __ATOMIC_RELAXED);
        __atomic_store_n(&progress->last_receive_tick_ns, now_ns,
                         __ATOMIC_RELAXED);
        network_maybe_promote_receive_window(fd, progress, received_bytes);
      }
    }
  }
}
static int af_b2n(int f) { return f == 10 ? AF_INET6 : f; }
static int af_n2b(int f) { return f == AF_INET6 ? 10 : f; }
static int getaddrinfo_with_deadline(const char *node, const char *service,
                                     const struct addrinfo *hints,
                                     uint64_t deadline_ns,
                                     struct addrinfo **result_out);
static void resolver_addrinfo_free(struct addrinfo *head);
static uint64_t resolver_effective_deadline(uint64_t requested_deadline_ns);

/* arm64 Bionic and BSD/newlib use the same hostent pointer layout, but their
 * AF_INET6 constants differ.  Publish bounded thread-local storage so legacy
 * callers never retain resolver-owned pointers. */
typedef struct {
  char *h_name;
  char **h_aliases;
  int h_addrtype;
  int h_length;
  char **h_addr_list;
} BionicHostEnt;
_Static_assert(offsetof(BionicHostEnt, h_name) == 0,
               "arm64 bionic hostent name offset");
_Static_assert(offsetof(BionicHostEnt, h_aliases) == 8,
               "arm64 bionic hostent aliases offset");
_Static_assert(offsetof(BionicHostEnt, h_addrtype) == 16,
               "arm64 bionic hostent family offset");
_Static_assert(offsetof(BionicHostEnt, h_length) == 20,
               "arm64 bionic hostent length offset");
_Static_assert(offsetof(BionicHostEnt, h_addr_list) == 24,
               "arm64 bionic hostent address-list offset");
_Static_assert(sizeof(BionicHostEnt) == 32,
               "arm64 bionic hostent size");

#define LEGACY_HOSTENT_TEXT_CAP       8192
#define LEGACY_HOSTENT_ALIAS_CAP      32
#define LEGACY_HOSTENT_ADDRESS_CAP    64
#define LEGACY_HOSTENT_ADDRESS_BYTES  16
#define LEGACY_HOST_QUERY_CAP         256

typedef struct {
  BionicHostEnt view;
  char *aliases[LEGACY_HOSTENT_ALIAS_CAP + 1];
  char *addresses[LEGACY_HOSTENT_ADDRESS_CAP + 1];
  unsigned char address_bytes[LEGACY_HOSTENT_ADDRESS_CAP]
                             [LEGACY_HOSTENT_ADDRESS_BYTES];
  char text[LEGACY_HOSTENT_TEXT_CAP];
  size_t text_used;
} BionicHostEntStorage;

_Static_assert(offsetof(BionicHostEntStorage, view) == 0,
               "guest hostent starts its thread-local storage");
static _Thread_local BionicHostEntStorage g_bionic_hostent_storage;

static void hostent_storage_reset(BionicHostEntStorage *storage) {
  memset(storage, 0, sizeof(*storage));
  storage->view.h_aliases = storage->aliases;
  storage->view.h_addr_list = storage->addresses;
}

static int legacy_copy_query_name(const char *source,
                                  char destination[LEGACY_HOST_QUERY_CAP]) {
  if (!source) { h_errno = NO_RECOVERY; errno = 22; return -1; }
  const uintptr_t base = (uintptr_t)source;
  for (size_t i = 0; i < LEGACY_HOST_QUERY_CAP; ++i) {
    if (i > UINTPTR_MAX - base || !nx_addr_readable(base + i, 1)) {
      h_errno = NO_RECOVERY;
      errno = 14;                    /* Bionic EFAULT. */
      return -1;
    }
    destination[i] = source[i];
    if (!destination[i]) {
      if (!i) { h_errno = NO_RECOVERY; errno = 22; return -1; }
      return 0;
    }
  }
  h_errno = NO_RECOVERY;
  errno = 34;                        /* Bionic ERANGE; never truncate names. */
  return -1;
}

static int hostent_copy_text(BionicHostEntStorage *storage,
                             const char *source, char **destination) {
  if (!source) {
    *destination = NULL;
    return 0;
  }
  const size_t available = sizeof(storage->text) - storage->text_used;
  const size_t length = strnlen(source, available);
  if (length == available) return -1;
  char *copy = &storage->text[storage->text_used];
  memcpy(copy, source, length + 1);
  storage->text_used += length + 1;
  *destination = copy;
  return 0;
}

static int hostent_from_addrinfo(BionicHostEntStorage *storage,
                                 const char *query,
                                 const struct addrinfo *native) {
  const char *canonical = query;
  size_t address_count = 0;
  for (const struct addrinfo *p = native; p; p = p->ai_next) {
    if (p->ai_canonname && *p->ai_canonname) canonical = p->ai_canonname;
    if (p->ai_family != AF_INET || !p->ai_addr ||
        p->ai_addrlen < sizeof(struct sockaddr_in))
      continue;
    if (address_count == LEGACY_HOSTENT_ADDRESS_CAP) return -1;
    const struct sockaddr_in *address =
      (const struct sockaddr_in *)p->ai_addr;
    memcpy(storage->address_bytes[address_count], &address->sin_addr,
           sizeof address->sin_addr);
    storage->addresses[address_count] =
      (char *)storage->address_bytes[address_count];
    ++address_count;
  }
  if (!address_count ||
      hostent_copy_text(storage, canonical, &storage->view.h_name) != 0)
    return -1;
  storage->addresses[address_count] = NULL;
  storage->aliases[0] = NULL;
  storage->view.h_addrtype = AF_INET;
  storage->view.h_length = sizeof(struct in_addr);
  return 0;
}

static void legacy_host_lookup_failure(int resolver_error) {
  if (resolver_error == EAI_AGAIN) {
    h_errno = TRY_AGAIN;
    errno = 11; /* Bionic EAGAIN. */
  } else if (resolver_error == EAI_NONAME) {
    h_errno = HOST_NOT_FOUND;
    errno = 2; /* Bionic ENOENT. */
  } else if (resolver_error == EAI_MEMORY) {
    h_errno = NO_RECOVERY;
    errno = 12; /* Bionic ENOMEM. */
  } else {
    h_errno = NO_RECOVERY;
    errno = 5; /* Bionic EIO. */
  }
}

void *gethostbyname_fake(const char *name) {
  NET_DIAG_ADD(dns_calls, 1);
  BionicHostEntStorage *storage = &g_bionic_hostent_storage;
  hostent_storage_reset(storage);
  char query[LEGACY_HOST_QUERY_CAP];
  if (legacy_copy_query_name(name, query) != 0) {
    NET_DIAG_ADD(dns_failures, 1);
    return NULL;
  }
  if (!g_net_on) {
    h_errno = TRY_AGAIN;
    errno = 100;
    NET_DIAG_ADD(dns_failures, 1);
    NET_DIAG_STORE(last_dns_error, EAI_AGAIN);
    return NULL;
  }
  const int caller_errno = errno;
  struct addrinfo hints = {0};
  hints.ai_family = AF_INET; /* POSIX gethostbyname is IPv4-only. */
  hints.ai_flags = AI_CANONNAME;
  struct addrinfo *native = NULL;
  const uint64_t deadline_ns = resolver_effective_deadline(0);
  const int result = getaddrinfo_with_deadline(query, NULL, &hints,
                                               deadline_ns, &native);
  if (result != 0 || hostent_from_addrinfo(storage, query, native) != 0) {
    if (native) resolver_addrinfo_free(native);
    hostent_storage_reset(storage);
    legacy_host_lookup_failure(result ? result : EAI_FAIL);
    NET_DIAG_ADD(dns_failures, 1);
    NET_DIAG_STORE(last_dns_error, result ? result : EAI_FAIL);
    return NULL;
  }
  resolver_addrinfo_free(native);
  h_errno = NETDB_SUCCESS;
  errno = caller_errno;
  NET_DIAG_ADD(dns_successes, 1);
  NET_DIAG_STORE(last_dns_error, 0);
  return &storage->view;
}

static void *hostent_from_numeric_address(BionicHostEntStorage *storage,
                                          const void *address,
                                          unsigned length,
                                          int native_family,
                                          int caller_errno) {
  char numeric[INET6_ADDRSTRLEN];
  if (!inet_ntop(native_family, address, numeric, sizeof numeric)) {
    h_errno = NO_RECOVERY;
    errno = n2b_errno(errno);
    return NULL;
  }
  if (hostent_copy_text(storage, numeric, &storage->view.h_name) != 0) {
    hostent_storage_reset(storage);
    h_errno = NO_RECOVERY;
    errno = 34; /* Bionic ERANGE. */
    return NULL;
  }
  memcpy(storage->address_bytes[0], address, length);
  storage->addresses[0] = (char *)storage->address_bytes[0];
  storage->addresses[1] = NULL;
  storage->aliases[0] = NULL;
  storage->view.h_addrtype = af_n2b(native_family);
  storage->view.h_length = (int)length;
  h_errno = NETDB_SUCCESS;
  errno = caller_errno;
  NET_DIAG_ADD(reverse_numeric_results, 1);
  return &storage->view;
}

void *gethostbyaddr_fake(const void *address, unsigned length, int family) {
  BionicHostEntStorage *storage = &g_bionic_hostent_storage;
  hostent_storage_reset(storage);
  const int native_family = af_b2n(family);
  const unsigned required_length = native_family == AF_INET
    ? (unsigned)sizeof(struct in_addr)
    : native_family == AF_INET6 ? (unsigned)sizeof(struct in6_addr) : 0;
  if (!address) { h_errno = NO_RECOVERY; errno = 22; return NULL; }
  if (!required_length) { h_errno = NO_RECOVERY; errno = 97; return NULL; }
  if (length != required_length) {
    h_errno = NO_RECOVERY; errno = 22; return NULL;
  }
  if (!nx_addr_readable((uintptr_t)address, length)) {
    h_errno = NO_RECOVERY; errno = 14; return NULL;
  }
  unsigned char query[LEGACY_HOSTENT_ADDRESS_BYTES];
  memcpy(query, address, length);
  if (!g_net_on) { h_errno = TRY_AGAIN; errno = 100; return NULL; }
  const int caller_errno = errno;
  /* The exact client invokes reverse DNS only while constructing optional MTR
   * telemetry after a CDN timeout.  Newlib holds the process-global resolver
   * lock for the whole PTR query, blocking every concurrent forward lookup and
   * freezing the update UI.  Android callers accept a numeric canonical name
   * here, so preserve the address/family hostent contract without the
   * nonessential blocking lookup. */
  return hostent_from_numeric_address(storage, query, length, native_family,
                                       caller_errno);
}
static unsigned sa_b2n(const void *bsa, unsigned blen, unsigned char out[128]) {
  if (!bsa || blen < 2) return 0;
  unsigned n = blen > 128 ? 128 : blen;
  memcpy(out, bsa, n);
  const unsigned char *b = (const unsigned char *)bsa;
  out[0] = (unsigned char)n;                       /* BSD sin_len */
  out[1] = (unsigned char)af_b2n(b[0] | (b[1] << 8)); /* BSD u8 family */
  return n;
}
static void sa_n2b(const unsigned char *nsa, unsigned n, void *bout, unsigned *boutlen) {
  const unsigned required = n > 128 ? 128 : n;
  if (!bout || !boutlen) { if (boutlen) *boutlen = required; return; }
  const unsigned capacity = *boutlen;
  const unsigned c = capacity < required ? capacity : required;
  if (c) memcpy(bout, nsa, c);
  if (c >= 2) { int fam = af_n2b(nsa[1]);
                ((unsigned char *)bout)[0] = (unsigned char)(fam & 0xFF);
                ((unsigned char *)bout)[1] = (unsigned char)((fam >> 8) & 0xFF); }
  *boutlen = required;
}
#define B_SOCK_NONBLOCK 0x800
#define B_SOCK_CLOEXEC  0x80000
#define B_MSG_OOB        0x0001
#define B_MSG_PEEK       0x0002
#define B_MSG_DONTROUTE  0x0004
#define B_MSG_CTRUNC     0x0008
#define B_MSG_TRUNC      0x0020
#define B_MSG_ERRQUEUE   0x2000
#define B_MSG_NOSIGNAL  0x4000
#define B_MSG_DONTWAIT  0x40
#define B_MSG_EOR        0x0080
#define B_MSG_WAITALL    0x0100
static int msg_b2n(int f) {
  int o = 0;
  if (f & B_MSG_OOB)       o |= MSG_OOB;
  if (f & B_MSG_PEEK)      o |= MSG_PEEK;
  if (f & B_MSG_DONTROUTE) o |= MSG_DONTROUTE;
  if (f & B_MSG_CTRUNC)    o |= MSG_CTRUNC;
  if (f & B_MSG_TRUNC)     o |= MSG_TRUNC;
  if (f & B_MSG_EOR)       o |= MSG_EOR;
  if (f & B_MSG_WAITALL)   o |= MSG_WAITALL;
  if (f & B_MSG_NOSIGNAL) o |= MSG_NOSIGNAL;
  if (f & B_MSG_DONTWAIT) o |= MSG_DONTWAIT;
  return o;
}
static int msg_receive_b2n(int f) {
  /* Linux tolerates output/send-only bits in recvmsg's input flags.  Horizon
   * passes this value to its BSD receive service, where MSG_NOSIGNAL in
   * particular is not a receive operation.  Keep only meaningful receive
   * inputs; MSG_ERRQUEUE is rejected by recvmsg_fake before this point. */
  return msg_b2n(f & ~(B_MSG_DONTROUTE | B_MSG_CTRUNC | B_MSG_EOR |
                       B_MSG_NOSIGNAL));
}
static int msg_datagram_send_b2n(int f) {
  /* Datagram writes never generate SIGPIPE, so Android's MSG_NOSIGNAL has no
   * observable UDP effect.  Omitting it also keeps Horizon's direct sendto
   * request limited to flags which are meaningful for a datagram. */
  return msg_b2n(f & ~B_MSG_NOSIGNAL);
}
static int msg_n2b(int f) {
  int o = 0;
  if (f & MSG_OOB)       o |= B_MSG_OOB;
  if (f & MSG_PEEK)      o |= B_MSG_PEEK;
  if (f & MSG_DONTROUTE) o |= B_MSG_DONTROUTE;
  if (f & MSG_CTRUNC)    o |= B_MSG_CTRUNC;
  if (f & MSG_TRUNC)     o |= B_MSG_TRUNC;
  if (f & MSG_EOR)       o |= B_MSG_EOR;
  if (f & MSG_WAITALL)   o |= B_MSG_WAITALL;
  if (f & MSG_NOSIGNAL)  o |= B_MSG_NOSIGNAL;
  if (f & MSG_DONTWAIT)  o |= B_MSG_DONTWAIT;
  return o;
}
/* Socket-option numbers are not a shared Linux/FreeBSD ABI.  Never forward
 * an unknown number: several Linux values name an unrelated libnx option. */
static int opt_b2n(int *level, int *name) {   /* 0 mapped, -1 unsupported */
  if (*level == 1) { *level = SOL_SOCKET;
    switch (*name) {
      case 2:  *name = SO_REUSEADDR; return 0; case 3:  *name = SO_TYPE;      return 0;
      case 4:  *name = SO_ERROR;     return 0; case 6:  *name = SO_BROADCAST; return 0;
      case 7:  *name = SO_SNDBUF;    return 0; case 8:  *name = SO_RCVBUF;    return 0;
      case 9:  *name = SO_KEEPALIVE; return 0; case 13: *name = SO_LINGER;    return 0;
      case 20: *name = SO_RCVTIMEO;  return 0; case 21: *name = SO_SNDTIMEO;  return 0;
      case 5:  *name = SO_DONTROUTE; return 0; case 10: *name = SO_OOBINLINE; return 0;
      case 15: *name = SO_REUSEPORT; return 0; case 18: *name = SO_RCVLOWAT;  return 0;
      case 19: *name = SO_SNDLOWAT;  return 0; case 29: *name = SO_TIMESTAMP; return 0;
      case 30: *name = SO_ACCEPTCONN; return 0; case 38: *name = SO_PROTOCOL; return 0;
      default: errno = ENOPROTOOPT; return -1;
    }
  }
  if (*level == 6) { *level = IPPROTO_TCP;
    switch (*name) {
      case 1: *name = TCP_NODELAY;  return 0;
      case 2: *name = TCP_MAXSEG;   return 0;
      case 4: *name = TCP_KEEPIDLE; return 0;
      case 5: *name = TCP_KEEPINTVL; return 0;
      case 6: *name = TCP_KEEPCNT;  return 0;
      /* Linux TCP_FASTOPEN_CONNECT has no exact FreeBSD equivalent.  The
       * libnx TCP_FASTOPEN option is the closest supported socket contract. */
      case 30: *name = TCP_FASTOPEN; return 0;
      default: errno = ENOPROTOOPT; return -1;
    }
  }
  if (*level == 0) { *level = IPPROTO_IP;
    switch (*name) {
      case 1:  *name = IP_TOS; return 0;
      case 2:  *name = IP_TTL; return 0;
      case 3:  *name = IP_HDRINCL; return 0;
      case 4:  *name = IP_OPTIONS; return 0;
      case 6:  *name = IP_RECVOPTS; return 0;
      case 7:  *name = IP_RECVRETOPTS; return 0;
      /* Linux IP_MTU_DISCOVER is normalized to the FreeBSD boolean by the
       * set/get wrappers below. */
      case 10: *name = IP_DONTFRAG; return 0;
      case 12: *name = IP_RECVTTL; return 0;
      case 13: *name = IP_RECVTOS; return 0;
      case 21: *name = IP_MINTTL; return 0;
      case 32: *name = IP_MULTICAST_IF; return 0;
      case 33: *name = IP_MULTICAST_TTL; return 0;
      case 34: *name = IP_MULTICAST_LOOP; return 0;
      case 35: *name = IP_ADD_MEMBERSHIP; return 0;
      case 36: *name = IP_DROP_MEMBERSHIP; return 0;
      /* IP_RECVERR/error queues, IP_PKTINFO, and
       * IP_BIND_ADDRESS_NO_PORT have no ABI-compatible libnx contract. */
      default: errno = ENOPROTOOPT; return -1;
    }
  }
  if (*level == 41) { *level = IPPROTO_IPV6;
    switch (*name) {
      case 16: *name = IPV6_UNICAST_HOPS; return 0;
      case 17: *name = IPV6_MULTICAST_IF; return 0;
      case 18: *name = IPV6_MULTICAST_HOPS; return 0;
      case 19: *name = IPV6_MULTICAST_LOOP; return 0;
      case 20: *name = IPV6_JOIN_GROUP; return 0;
      case 21: *name = IPV6_LEAVE_GROUP; return 0;
      case 26: *name = IPV6_V6ONLY; return 0;
      /* libnx exposes no compatible Linux error queue, flowinfo-send,
       * RFC2292 packet-info, or IPv6 MTU-discovery option. */
      default: errno = ENOPROTOOPT; return -1;
    }
  }
  errno = ENOPROTOOPT;
  return -1;
}

/* Linux/bionic keeps the two count fields pointer-sized on arm64.  libnx's
 * FreeBSD ABI instead uses int and socklen_t, moving msg_flags four bytes
 * earlier, so this structure must never be cast to the host msghdr. */
typedef struct {
  void *msg_name;
  uint32_t msg_namelen;
  uint32_t name_padding;
  struct iovec *msg_iov;
  uint64_t msg_iovlen;
  void *msg_control;
  uint64_t msg_controllen;
  int32_t msg_flags;
  uint32_t flags_padding;
} GuestMsghdr;
_Static_assert(offsetof(GuestMsghdr, msg_namelen) == 8,
               "arm64 bionic msghdr name length offset");
_Static_assert(offsetof(GuestMsghdr, msg_iov) == 16,
               "arm64 bionic msghdr iovec offset");
_Static_assert(offsetof(GuestMsghdr, msg_iovlen) == 24,
               "arm64 bionic msghdr iovec count offset");
_Static_assert(offsetof(GuestMsghdr, msg_control) == 32,
               "arm64 bionic msghdr control offset");
_Static_assert(offsetof(GuestMsghdr, msg_controllen) == 40,
               "arm64 bionic msghdr control length offset");
_Static_assert(offsetof(GuestMsghdr, msg_flags) == 48,
               "arm64 bionic msghdr flags offset");
_Static_assert(sizeof(GuestMsghdr) == 56, "arm64 bionic msghdr size");

/* cmsghdr is another subtle ABI split.  Bionic uses a pointer-sized
 * cmsg_len, placing the level/type words at offsets 8/12.  libnx uses a
 * 32-bit socklen_t and places them at 4/8.  Both ABIs align ancillary data
 * to eight bytes and begin the payload at offset 16, so payloads can be
 * copied byte-for-byte once the header metadata has been rewritten. */
typedef struct {
  uint64_t cmsg_len;
  int32_t cmsg_level;
  int32_t cmsg_type;
} GuestCmsghdr;
_Static_assert(offsetof(GuestCmsghdr, cmsg_level) == 8,
               "arm64 bionic cmsghdr level offset");
_Static_assert(offsetof(GuestCmsghdr, cmsg_type) == 12,
               "arm64 bionic cmsghdr type offset");
_Static_assert(sizeof(GuestCmsghdr) == 16, "arm64 bionic cmsghdr size");
_Static_assert(sizeof(struct cmsghdr) == 12, "libnx cmsghdr size");
_Static_assert(CMSG_LEN(0) == 16, "libnx ancillary payload offset");

#define B_SOL_SOCKET       1
#define B_SCM_RIGHTS       1
#define B_SCM_CREDENTIALS  2
#define B_SCM_TIMESTAMP    29
#define B_SCM_TIMESTAMPNS  35

static int cmsg_align8(size_t value, size_t *aligned) {
  if (value > SIZE_MAX - 7u) {
    errno = EINVAL;
    return -1;
  }
  *aligned = (value + 7u) & ~(size_t)7u;
  return 0;
}

/* Ancillary levels and types are protocol ABIs too.  Only translate payloads
 * whose representation is known to match; Linux and FreeBSD reuse many of
 * the remaining numeric values for unrelated control messages. */
static int cmsg_meta_b2n(int bionic_level, int bionic_type,
                         int *native_level, int *native_type) {
  if (bionic_level == B_SOL_SOCKET) {
    *native_level = SOL_SOCKET;
    switch (bionic_type) {
      case B_SCM_RIGHTS:
        *native_type = SCM_RIGHTS;
        return 0;
      case B_SCM_TIMESTAMP:
        *native_type = SCM_TIMESTAMP;
        return 0;
#ifdef SCM_REALTIME
      case B_SCM_TIMESTAMPNS:
        *native_type = SCM_REALTIME;
        return 0;
#endif
      /* Linux ucred and FreeBSD cmsgcred are not layout compatible. */
      case B_SCM_CREDENTIALS:
      default:
        errno = EOPNOTSUPP;
        return -1;
    }
  }

  if (bionic_level == 0 /* IPPROTO_IP */) {
    *native_level = IPPROTO_IP;
    switch (bionic_type) {
      case 1: *native_type = IP_TOS; return 0;      /* Linux IP_TOS */
      case 2: *native_type = IP_TTL; return 0;      /* Linux IP_TTL */
      case 4: *native_type = IP_OPTIONS; return 0;  /* Linux IP_OPTIONS */
      case 7: *native_type = IP_RETOPTS; return 0;  /* Linux IP_RETOPTS */
      default: break;
    }
  }

  /* libnx exposes no compatible IPV6_PKTINFO/HOPLIMIT or Linux error-queue
   * payload contract. */
  errno = EOPNOTSUPP;
  return -1;
}

/* Return 1 when a native socket-level control message has no safe bionic
 * representation.  recvmsg reports it as truncated instead of exposing a
 * header with misleading Linux metadata. */
static int cmsg_meta_n2b(int native_level, int native_type,
                         int *bionic_level, int *bionic_type) {
  if (native_level == SOL_SOCKET) {
    *bionic_level = B_SOL_SOCKET;
    switch (native_type) {
      case SCM_RIGHTS:
        *bionic_type = B_SCM_RIGHTS;
        return 0;
      case SCM_TIMESTAMP:
        *bionic_type = B_SCM_TIMESTAMP;
        return 0;
#ifdef SCM_REALTIME
      case SCM_REALTIME:
        *bionic_type = B_SCM_TIMESTAMPNS;
        return 0;
#endif
      default:
        return 1;
    }
  }

  if (native_level == IPPROTO_IP) {
    *bionic_level = 0;
    switch (native_type) {
      case IP_TOS:
      case IP_RECVTOS:
        *bionic_type = 1; /* Linux IP_TOS */
        return 0;
      case IP_TTL:
        *bionic_type = 2; /* Linux IP_TTL */
        return 0;
      /* FreeBSD IP_RECVTTL carries a byte while Linux IP_TTL ancillary data
       * carries an int.  Do not expose a header whose payload ABI is wrong. */
      case IP_RECVTTL:
        return 1;
      case IP_OPTIONS:
        *bionic_type = 4; /* Linux IP_OPTIONS */
        return 0;
      case IP_RETOPTS:
        *bionic_type = 7; /* Linux IP_RETOPTS */
        return 0;
      default:
        return 1;
    }
  }

  return 1;
}

static void *cmsg_control_b2n(const void *guest_control, size_t guest_len) {
  if (!guest_len) return NULL;
  if (!guest_control) {
    errno = EFAULT;
    return NULL;
  }

  unsigned char *native_control = malloc(guest_len);
  if (!native_control) {
    errno = ENOMEM;
    return NULL;
  }
  memcpy(native_control, guest_control, guest_len);

  size_t offset = 0;
  while (offset < guest_len) {
    const size_t remaining = guest_len - offset;
    if (remaining < sizeof(GuestCmsghdr)) goto malformed;

    GuestCmsghdr guest_header;
    memcpy(&guest_header, (const unsigned char *)guest_control + offset,
           sizeof guest_header);
    if (guest_header.cmsg_len < sizeof(GuestCmsghdr) ||
        guest_header.cmsg_len > remaining ||
        guest_header.cmsg_len > UINT_MAX)
      goto malformed;

    int native_level, native_type;
    if (cmsg_meta_b2n(guest_header.cmsg_level, guest_header.cmsg_type,
                      &native_level, &native_type) < 0) {
      free(native_control);
      return NULL;
    }

    struct cmsghdr native_header = {
      .cmsg_len = (socklen_t)guest_header.cmsg_len,
      .cmsg_level = native_level,
      .cmsg_type = native_type,
    };
    memcpy(native_control + offset, &native_header, sizeof native_header);
    memset(native_control + offset + sizeof native_header, 0,
           sizeof(GuestCmsghdr) - sizeof native_header);

    size_t next;
    if (cmsg_align8((size_t)guest_header.cmsg_len, &next) < 0) {
      free(native_control);
      return NULL;
    }
    if (next > remaining) break; /* The final cmsg need not include padding. */
    offset += next;
  }
  return native_control;

malformed:
  free(native_control);
  errno = EINVAL;
  return NULL;
}

static size_t cmsg_control_n2b(const void *native_control, size_t native_len,
                               void *guest_control, size_t guest_capacity,
                               int *truncated) {
  size_t input_offset = 0;
  size_t output_offset = 0;

  if (native_len > guest_capacity) {
    native_len = guest_capacity;
    *truncated = 1;
  }
  while (input_offset < native_len) {
    const size_t remaining = native_len - input_offset;
    if (remaining < CMSG_LEN(0)) {
      *truncated = 1;
      break;
    }

    struct cmsghdr native_header;
    memcpy(&native_header,
           (const unsigned char *)native_control + input_offset,
           sizeof native_header);
    const size_t cmsg_len = native_header.cmsg_len;
    if (cmsg_len < CMSG_LEN(0) || cmsg_len > remaining) {
      *truncated = 1;
      break;
    }

    size_t aligned_len;
    if (cmsg_align8(cmsg_len, &aligned_len) < 0) {
      *truncated = 1;
      break;
    }
    const size_t span = aligned_len <= remaining ? aligned_len : cmsg_len;

    int bionic_level, bionic_type;
    if (cmsg_meta_n2b(native_header.cmsg_level, native_header.cmsg_type,
                      &bionic_level, &bionic_type) != 0) {
      *truncated = 1;
    } else if (span > guest_capacity - output_offset) {
      *truncated = 1;
      break;
    } else {
      memcpy((unsigned char *)guest_control + output_offset,
             (const unsigned char *)native_control + input_offset, span);
      const GuestCmsghdr guest_header = {
        .cmsg_len = cmsg_len,
        .cmsg_level = bionic_level,
        .cmsg_type = bionic_type,
      };
      memcpy((unsigned char *)guest_control + output_offset, &guest_header,
             sizeof guest_header);
      output_offset += span;
    }

    if (aligned_len > remaining) break;
    input_offset += aligned_len;
  }
  return output_offset;
}

#define BIONIC_FD_SETSIZE 1024
#define POLL_FAKE_SLICE_MS 10
#define POLL_FAKE_STACK_FDS 64
#define POLL_FAKE_EBADF_RETRIES 4
#define B_POLLIN      0x0001u
#define B_POLLPRI     0x0002u
#define B_POLLOUT     0x0004u
#define B_POLLERR     0x0008u
#define B_POLLHUP     0x0010u
#define B_POLLNVAL    0x0020u
#define B_POLLRDNORM  0x0040u
#define B_POLLRDBAND  0x0080u
#define B_POLLWRNORM  0x0100u
#define B_POLLWRBAND  0x0200u
#define B_POLLRDHUP   0x2000u
typedef struct { int32_t fd; int16_t events, revents; } BionicPollFd;
typedef struct { uint64_t bits[BIONIC_FD_SETSIZE / 64]; } BionicFdSet;
typedef struct { int64_t seconds, microseconds; } BionicTimeval;
_Static_assert(sizeof(BionicPollFd) == 8, "arm64 bionic pollfd ABI");
_Static_assert(sizeof(struct pollfd) == 8, "libnx pollfd ABI");
_Static_assert(POLLIN == 0x001 && POLLPRI == 0x002 && POLLOUT == 0x004 &&
               POLLERR == 0x008 && POLLHUP == 0x010 && POLLNVAL == 0x020,
               "common bionic/libnx poll event ABI");
_Static_assert(POLLWRNORM == POLLOUT && POLLWRBAND == 0x0100 &&
               POLLINIGNEOF == 0x2000,
               "libnx poll values requiring bionic translation");
_Static_assert(sizeof(BionicFdSet) == 128, "arm64 bionic fd_set ABI");
_Static_assert(sizeof(BionicTimeval) == 16, "arm64 bionic timeval ABI");

static int bionic_fd_isset(const BionicFdSet *set, unsigned bit) {
  return set && bit < BIONIC_FD_SETSIZE &&
         ((set->bits[bit / 64u] >> (bit % 64u)) & 1u);
}

static void bionic_fd_set(BionicFdSet *set, unsigned bit) {
  if (set && bit < BIONIC_FD_SETSIZE)
    set->bits[bit / 64u] |= UINT64_C(1) << (bit % 64u);
}

static int16_t poll_events_b2n(int16_t guest_events) {
  const uint16_t bionic = (uint16_t)guest_events;
  int native = 0;
  if (bionic & B_POLLIN)     native |= POLLIN;
  if (bionic & B_POLLPRI)    native |= POLLPRI;
  if (bionic & B_POLLOUT)    native |= POLLOUT;
  if (bionic & B_POLLRDNORM) native |= POLLRDNORM;
  if (bionic & B_POLLRDBAND) native |= POLLRDBAND;
  if (bionic & B_POLLWRNORM) native |= POLLOUT;
  if (bionic & B_POLLWRBAND) native |= POLLWRBAND;
  /* FreeBSD 0x2000 is POLLINIGNEOF, not Linux POLLRDHUP.  Half-close is
   * probed separately without consuming input. */
  return (int16_t)native;
}

static int16_t poll_revents_n2b(int16_t native_revents,
                                int16_t guest_events) {
  const uint16_t native = (uint16_t)native_revents;
  const uint16_t requested = (uint16_t)guest_events;
  uint16_t bionic = 0;

  if (native & (POLLIN | POLLRDNORM))
    bionic |= requested & (B_POLLIN | B_POLLRDNORM);
  if (native & (POLLPRI | POLLRDBAND))
    bionic |= requested & (B_POLLPRI | B_POLLRDBAND);
  if (native & (POLLOUT | POLLWRNORM))
    bionic |= requested & (B_POLLOUT | B_POLLWRNORM);
  if (native & POLLWRBAND)
    bionic |= requested & B_POLLWRBAND;

  /* Linux requires these result bits to be reported even when absent from
   * events.  A native HUP also satisfies a requested half-close watch. */
  if (native & POLLERR)  bionic |= B_POLLERR;
  if (native & POLLHUP) {
    bionic |= B_POLLHUP;
    bionic |= requested & B_POLLRDHUP;
  }
  if (native & POLLNVAL) bionic |= B_POLLNVAL;
  return (int16_t)bionic;
}

static int poll_stream_rdhup(int fd) {
  unsigned char byte;
  const int saved = errno;
  int type = 0;
  socklen_t type_len = sizeof type;
  int half_closed = 0;
  if (getsockopt(fd, SOL_SOCKET, SO_TYPE, &type, &type_len) == 0 &&
      type == SOCK_STREAM)
    half_closed = recv(fd, &byte, sizeof byte,
                       MSG_PEEK | MSG_DONTWAIT) == 0;
  errno = saved;
  return half_closed;
}

static int network_poll_guard_interval_ms(const BionicPollFd *fds,
                                          unsigned long nfds) {
  int interval_ms = 0;
  for (unsigned long i = 0; i < nfds; ++i) {
    const int fd = fds[i].fd;
    if (fd < 0 || fd >= NETWORK_TRACKED_FD_LIMIT ||
        !((uint16_t)fds[i].events & (B_POLLIN | B_POLLRDNORM)))
      continue;
    uint64_t received = 0;
    if (!network_track_receive_snapshot(fd, &received, NULL))
      continue;
    const int candidate = received >= NETWORK_BULK_MIN_BYTES
      ? (int)NETWORK_BULK_READINESS_PROBE_INTERVAL_MS
      : (int)NETWORK_TLS_READINESS_PROBE_INTERVAL_MS;
    if (!interval_ms || candidate < interval_ms) interval_ms = candidate;
  }
  return interval_ms;
}

static void network_track_poll_waiters(const BionicPollFd *fds,
                                       unsigned long nfds) {
  const uintptr_t thread = (uintptr_t)pthread_self();
  const uint64_t now_ns = armTicksToNs(armGetSystemTick());
  for (unsigned long i = 0; i < nfds; ++i) {
    const int fd = fds[i].fd;
    if (fd < 0 || fd >= NETWORK_TRACKED_FD_LIMIT ||
        !((uint16_t)fds[i].events & (B_POLLIN | B_POLLRDNORM)))
      continue;
    NetworkSocketProgress *progress = &g_network_socket_progress[fd];
    mutexLock(&progress->owner_lock);
    uint64_t received = 0;
    if (!network_track_receive_snapshot(fd, &received, NULL)) {
      mutexUnlock(&progress->owner_lock);
      continue;
    }
    __atomic_store_n(&progress->last_poll_thread, thread, __ATOMIC_RELAXED);
    __atomic_store_n(&progress->last_poll_tick_ns, now_ns,
                     __ATOMIC_RELAXED);
    mutexUnlock(&progress->owner_lock);
  }
}

/* Horizon has missed a later read/FIN notification after an earlier successful
 * receive.  This first appeared at the tail of a 233352-byte manifest, and the
 * latest trace reproduced it during replacement TLS handshakes after exactly
 * 5279 bytes per connection.  Guard every stream which has proved it can
 * receive, but probe short TLS/control streams only once per second; sustained
 * bodies keep the 250-ms cadence.  FIONREAD is exactly Linux POLLIN semantics,
 * so this repairs readiness without consuming or inspecting payload data. */
static int16_t network_poll_recover_readiness(int fd, int16_t guest_events,
                                               int16_t native_revents) {
  if (fd < 0 || fd >= NETWORK_TRACKED_FD_LIMIT ||
      !((uint16_t)guest_events & (B_POLLIN | B_POLLRDNORM)) ||
      ((uint16_t)native_revents &
       (POLLIN | POLLRDNORM | POLLERR | POLLHUP | POLLNVAL)))
    return native_revents;

  NetworkSocketProgress *progress = &g_network_socket_progress[fd];
  uint64_t received = 0;
  uint64_t last_receive_ns = 0;
  if (!network_track_receive_snapshot(fd, &received, &last_receive_ns))
    return native_revents;
  const uint64_t probe_interval_ms = received >= NETWORK_BULK_MIN_BYTES
    ? NETWORK_BULK_READINESS_PROBE_INTERVAL_MS
    : NETWORK_TLS_READINESS_PROBE_INTERVAL_MS;

  const uint64_t now_ns = armTicksToNs(armGetSystemTick());
  if (!last_receive_ns || now_ns < last_receive_ns ||
      (now_ns - last_receive_ns) / UINT64_C(1000000) <
        NETWORK_READINESS_PROBE_MIN_MS)
    return native_revents;

  uint64_t previous_probe = __atomic_load_n(
    &progress->last_readiness_probe_tick_ns, __ATOMIC_RELAXED);
  if (previous_probe && now_ns >= previous_probe &&
      (now_ns - previous_probe) / UINT64_C(1000000) <
        probe_interval_ms)
    return native_revents;
  if (!__atomic_compare_exchange_n(&progress->last_readiness_probe_tick_ns,
                                    &previous_probe, now_ns, false,
                                    __ATOMIC_ACQ_REL, __ATOMIC_RELAXED))
    return native_revents;

  const uint64_t generation =
    __atomic_load_n(&progress->generation, __ATOMIC_RELAXED);
  int queued = 0;
  const int saved_errno = errno;
  const int probe_result = ioctl(fd, FIONREAD, &queued);
  const int probe_errno = errno;
  errno = saved_errno;
  if (!__atomic_load_n(&progress->active, __ATOMIC_ACQUIRE) ||
      generation != __atomic_load_n(&progress->generation,
                                     __ATOMIC_RELAXED))
    return native_revents;

  NET_DIAG_ADD(poll_readiness_probes, 1);
  if (probe_result < 0 || queued < 0) {
    (void)probe_errno;
    __atomic_store_n(&progress->readiness_queued_bytes, 0,
                     __ATOMIC_RELAXED);
    __atomic_store_n(&progress->readiness_probe_state, 2,
                     __ATOMIC_RELEASE);
    NET_DIAG_ADD(poll_readiness_probe_failures, 1);
    return native_revents;
  }
  __atomic_store_n(&progress->readiness_queued_bytes, (uint64_t)queued,
                   __ATOMIC_RELAXED);
  __atomic_store_n(&progress->readiness_probe_state, 1,
                   __ATOMIC_RELEASE);
  if (queued > 0) {
    NET_DIAG_ADD(poll_readiness_hits, 1);
    return (int16_t)((uint16_t)native_revents | POLLIN);
  }

  /* FIONREAD is zero both for a live idle stream and for EOF.  A non-consuming
   * peek distinguishes them if the native poll also missed the final FIN. */
  unsigned char byte;
  const int peek_saved_errno = errno;
  const long peek = recv(fd, &byte, sizeof byte, MSG_PEEK | MSG_DONTWAIT);
  const int peek_errno = errno;
  errno = peek_saved_errno;
  if (peek == 0) {
    NET_DIAG_ADD(poll_readiness_hits, 1);
    return (int16_t)((uint16_t)native_revents | POLLIN | POLLHUP);
  }
  if (peek < 0 && peek_errno != EAGAIN && peek_errno != EWOULDBLOCK) {
    NET_DIAG_ADD(poll_readiness_hits, 1);
    return (int16_t)((uint16_t)native_revents | POLLERR);
  }
  return native_revents;
}

/* Linux poll reports POLLNVAL for every invalid positive descriptor, including
 * one closed before entry; it does not fail every unrelated descriptor in the
 * batch.  Horizon can instead return batch EBADF.  Capture exact tracked-socket
 * generations before the wait, then validate individual entries only on that
 * exceptional result.  Keep the generation after close: an inactive old socket
 * is precisely the identity the cold recovery scan must still recognize. */
static uint64_t network_poll_socket_generation(int fd) {
  if (fd < 0 || fd >= NETWORK_TRACKED_FD_LIMIT) return 0;
  NetworkSocketProgress *progress = &g_network_socket_progress[fd];
  return __atomic_load_n(&progress->generation, __ATOMIC_RELAXED);
}

static int network_poll_mark_invalid_entries(
    struct pollfd *native, const uint64_t *generations,
    unsigned long nfds) {
  int invalid = 0;
  for (unsigned long i = 0; i < nfds; ++i) {
    const int fd = native[i].fd;
    if (fd < 0) continue;

    int descriptor_status = -1;
    int descriptor_error = EBADF;
    int route_stable = 0;
    for (unsigned attempt = 0; attempt < 2; ++attempt) {
      /* close_fake publishes a route replacement around tracker invalidation
       * and native close.  Wait for that replacement to settle before asking
       * libnx about this descriptor.  F_GETFD is not implemented by libnx and
       * returns a positive ENOTSUP value; F_GETFL is its supported validity
       * probe and reaches _socketGetFd. */
      NxFdRouteTicket settled_route;
      if (!nx_fd_route_snapshot(fd, &settled_route)) break;
      const int saved_errno = errno;
      errno = 0;
      descriptor_status = fcntl(fd, F_GETFL, 0);
      descriptor_error = errno;
      errno = saved_errno;
      if (nx_fd_route_validate(&settled_route)) {
        route_stable = 1;
        break;
      }
      NET_DIAG_ADD(poll_route_snapshot_retries, 1);
    }
    if (!route_stable) continue;

    if (descriptor_status < 0) {
      if (descriptor_error == EBADF) {
        native[i].revents = POLLNVAL;
        NET_DIAG_ADD(poll_invalid_fd_recoveries, 1);
        ++invalid;
      } else {
        /* A native file can reuse a numeric slot formerly owned by a socket.
         * libnx reports that as a non-socket error.  Do not apply the retained
         * socket generation to a different descriptor class. */
        NET_DIAG_ADD(poll_probe_other_errors, 1);
      }
      continue;
    }

    const uint64_t generation = generations ? generations[i] : 0;
    if (!generation || fd >= NETWORK_TRACKED_FD_LIMIT) continue;
    NetworkSocketProgress *progress = &g_network_socket_progress[fd];
    if (!__atomic_load_n(&progress->active, __ATOMIC_ACQUIRE)) {
      native[i].revents = POLLNVAL;
      NET_DIAG_ADD(poll_inactive_fd_recoveries, 1);
      ++invalid;
    } else if (generation != __atomic_load_n(&progress->generation,
                                              __ATOMIC_RELAXED)) {
      native[i].revents = POLLNVAL;
      NET_DIAG_ADD(poll_reused_fd_recoveries, 1);
      ++invalid;
    }
  }
  return invalid;
}

/* A concurrent close/reuse can keep moving between a failed batch wait and its
 * cold probes.  Linux never exposes batch EBADF from poll, so after bounded
 * exact retries conservatively invalidate the remaining native batch.  This
 * wakes the Android owner to rebuild its descriptor set instead of terminating
 * the shared curl/TLS worker.  The counter makes this last resort explicit. */
static int network_poll_mark_all_native_invalid(struct pollfd *native,
                                                unsigned long nfds) {
  int invalid = 0;
  for (unsigned long i = 0; i < nfds; ++i) {
    if (native[i].fd < 0) continue;
    native[i].revents = POLLNVAL;
    ++invalid;
  }
  return invalid;
}

/* poll(2) cannot hand the high synthetic descriptor namespace to BSD.  Keep
 * native entries in a shadow array and rescan fake pipes between short native
 * waits.  No readiness probe consumes pipe data. */
int poll_fake(void *fds_ptr, unsigned long nfds, int timeout) {
  BionicPollFd *fds = (BionicPollFd *)fds_ptr;
  if (nfds > UINT_MAX) { errno = EINVAL; return -1; }
  if (nfds && !fds) { errno = EFAULT; return -1; }
  if (nfds > SIZE_MAX / sizeof(struct pollfd)) { errno = EINVAL; return -1; }

  struct pollfd stack_native[POLL_FAKE_STACK_FDS];
  uint64_t stack_generations[POLL_FAKE_STACK_FDS];
  struct pollfd *heap_native = nfds > POLL_FAKE_STACK_FDS
    ? malloc((size_t)nfds * sizeof(*heap_native)) : NULL;
  uint64_t *heap_generations = nfds > POLL_FAKE_STACK_FDS
    ? malloc((size_t)nfds * sizeof(*heap_generations)) : NULL;
  if (nfds > POLL_FAKE_STACK_FDS &&
      (!heap_native || !heap_generations)) {
    free(heap_native);
    free(heap_generations);
    errno = ENOMEM;
    return -1;
  }
  struct pollfd *native = nfds ? (heap_native ? heap_native : stack_native) : NULL;
  uint64_t *generations = nfds
    ? (heap_generations ? heap_generations : stack_generations) : NULL;
  int has_native = 0;
  int has_fake = 0;
  for (unsigned long i = 0; i < nfds; i++) {
    native[i].fd = fds[i].fd;
    native[i].events = fds[i].events;
    native[i].events = poll_events_b2n(native[i].events);
    native[i].revents = 0;
    fds[i].revents = 0;
    if (fds[i].fd >= 0 && fakefd_is_fake(fds[i].fd)) {
      native[i].fd = -1;
      has_fake = 1;
    } else if (fds[i].fd >= 0) {
      has_native = 1;
    }
  }

  const uint64_t start_ns = armTicksToNs(armGetSystemTick());
  const uint64_t timeout_ns = timeout < 0 ? UINT64_MAX
    : (uint64_t)(unsigned)timeout * UINT64_C(1000000);
  /* Once a stream has received data, bound its native-only read wait so a
   * level-ready byte or FIN cannot be lost for the complete guest timeout.
   * Keep TLS/control probes at 1 Hz and active body probes at 4 Hz rather than
   * restoring the retired 100 BSD poll IPCs/second loop. */
  const int guard_interval_ms = has_native && !has_fake
    ? network_poll_guard_interval_ms(fds, nfds) : 0;
  const int guarded_native_wait = guard_interval_ms > 0;
  int native_wait_ms = has_native && !has_fake
    ? guarded_native_wait && (timeout < 0 ||
                              timeout > guard_interval_ms)
        ? guard_interval_ms : timeout
    : 0;
  unsigned transient_ebadf_retries = 0;
  for (;;) {
    if (has_native) {
      for (unsigned long i = 0; i < nfds; i++) {
        native[i].revents = 0;
        generations[i] = network_poll_socket_generation(native[i].fd);
      }
      network_track_poll_waiters(fds, nfds);
      NET_DIAG_ADD(poll_wait_calls, 1);
      int native_result = poll(native, (nfds_t)nfds, native_wait_ms);
      if (native_result < 0) {
        const int poll_error = errno;
        const int stale = poll_error == EBADF
          ? network_poll_mark_invalid_entries(native, generations, nfds) : 0;
        if (stale) {
          /* Wake the Android poll owner with Linux's per-entry close result.
           * Its next iteration rebuilds the live descriptor set instead of
           * terminating the shared downloader/TLS worker. */
          NET_DIAG_ADD(poll_stale_snapshot_recoveries, 1);
          NET_DIAG_STORE(last_poll_wait_error, 0);
          errno = 0;
        } else if (poll_error == EBADF &&
                   transient_ebadf_retries < POLL_FAKE_EBADF_RETRIES) {
          /* close_fake invalidates tracking immediately before native close.
           * If the cold scan lands inside that interval, repeat after the
           * in-progress close can finish instead of leaking Horizon EBADF to
          * an Android poll loop which can never legally receive it. */
          ++transient_ebadf_retries;
          NET_DIAG_ADD(poll_ebadf_retries, 1);
          continue;
        } else if (poll_error == EBADF) {
          const int fallback =
            network_poll_mark_all_native_invalid(native, nfds);
          if (fallback) {
            NET_DIAG_ADD(poll_stale_snapshot_recoveries, 1);
            NET_DIAG_ADD(poll_ebadf_fallback_recoveries, 1);
            NET_DIAG_STORE(last_poll_wait_error, 0);
            errno = 0;
          } else {
            NET_DIAG_ADD(poll_wait_failures, 1);
            NET_DIAG_STORE(last_poll_wait_error, n2b_errno(poll_error));
            free(heap_generations);
            free(heap_native);
            errno = n2b_errno(poll_error);
            return -1;
          }
        } else {
          NET_DIAG_ADD(poll_wait_failures, 1);
          NET_DIAG_STORE(last_poll_wait_error, n2b_errno(poll_error));
          free(heap_generations);
          free(heap_native);
          errno = n2b_errno(poll_error);
          return -1;
        }
      } else {
        transient_ebadf_retries = 0;
      }
    }

    int ready_count = 0;
    for (unsigned long i = 0; i < nfds; i++) {
      short native_ready = fakefd_is_fake(fds[i].fd)
        ? fakefd_poll_revents(fds[i].fd, native[i].events)
        : native[i].revents;
      if (!fakefd_is_fake(fds[i].fd))
        native_ready = network_poll_recover_readiness(
          fds[i].fd, fds[i].events, native_ready);
      short ready = poll_revents_n2b(native_ready, fds[i].events);
      if (!fakefd_is_fake(fds[i].fd) && fds[i].fd >= 0 &&
          ((uint16_t)fds[i].events & B_POLLRDHUP) &&
          !((uint16_t)ready & B_POLLRDHUP) && poll_stream_rdhup(fds[i].fd))
        ready = (int16_t)((uint16_t)ready | B_POLLRDHUP);
      fds[i].revents = ready;
      if (ready) ready_count++;
    }
    if (ready_count || timeout == 0) {
      free(heap_generations);
      free(heap_native);
      return ready_count;
    }
    if (has_native && !has_fake && !guarded_native_wait) {
      free(heap_generations);
      free(heap_native);
      return 0;
    }

    uint64_t elapsed_ns = armTicksToNs(armGetSystemTick()) - start_ns;
    if (timeout_ns != UINT64_MAX && elapsed_ns >= timeout_ns) {
      free(heap_generations);
      free(heap_native);
      return 0;
    }
    uint64_t slice_ns = (guarded_native_wait
      ? (uint64_t)guard_interval_ms
      : UINT64_C(POLL_FAKE_SLICE_MS)) * UINT64_C(1000000);
    uint64_t remaining_ns = timeout_ns == UINT64_MAX
      ? slice_ns : timeout_ns - elapsed_ns;
    if (remaining_ns < slice_ns) slice_ns = remaining_ns;
    int slice_ms = (int)((slice_ns + 999999u) / 1000000u);
    if (slice_ms < 1) slice_ms = 1;
    if (has_native) {
      native_wait_ms = slice_ms;
    } else {
      fakefd_wait((unsigned long long)slice_ns);
      native_wait_ms = 0;
    }
  }
}

int select_fake(int n, void *read_ptr, void *write_ptr, void *except_ptr,
                void *timeout_ptr) {
  BionicFdSet *read_set = (BionicFdSet *)read_ptr;
  BionicFdSet *write_set = (BionicFdSet *)write_ptr;
  BionicFdSet *except_set = (BionicFdSet *)except_ptr;
  BionicTimeval *guest_timeout = (BionicTimeval *)timeout_ptr;
  if (n < 0) { errno = EINVAL; return -1; }

  uint64_t requested_ns = UINT64_MAX;
  int timeout_ms = -1;
  if (guest_timeout) {
    if (guest_timeout->seconds < 0 || guest_timeout->microseconds < 0 ||
        guest_timeout->microseconds >= 1000000) {
      errno = EINVAL;
      return -1;
    }
    uint64_t seconds = (uint64_t)guest_timeout->seconds;
    uint64_t micros = (uint64_t)guest_timeout->microseconds;
    requested_ns = seconds > (UINT64_MAX - micros * 1000u) / 1000000000u
      ? UINT64_MAX : seconds * UINT64_C(1000000000) + micros * 1000u;
    uint64_t milliseconds = requested_ns == UINT64_MAX ? (uint64_t)INT_MAX
      : requested_ns / 1000000u + (requested_ns % 1000000u != 0);
    timeout_ms = milliseconds > (uint64_t)INT_MAX ? INT_MAX : (int)milliseconds;
  }

  const unsigned limit = n > BIONIC_FD_SETSIZE ? BIONIC_FD_SETSIZE : (unsigned)n;
  BionicPollFd polls[BIONIC_FD_SETSIZE];
  uint16_t bits[BIONIC_FD_SETSIZE];
  uint8_t interests[BIONIC_FD_SETSIZE];
  unsigned count = 0;
  int has_fake_alias = 0;
  for (unsigned bit = 0; bit < limit; bit++) {
    uint8_t interest = (bionic_fd_isset(read_set, bit) ? 1u : 0u) |
                       (bionic_fd_isset(write_set, bit) ? 2u : 0u) |
                       (bionic_fd_isset(except_set, bit) ? 4u : 0u);
    if (!interest) continue;
    int fake_fd = fakefd_from_select_bit((int)bit);
    if (fake_fd >= 0) has_fake_alias = 1;
    polls[count].fd = fake_fd >= 0 ? fake_fd : (int)bit;
    polls[count].events = (interest & 1u ? B_POLLIN : 0) |
                          (interest & 2u ? B_POLLOUT : 0) |
                          (interest & 4u ? B_POLLPRI : 0);
    polls[count].revents = 0;
    bits[count] = (uint16_t)bit;
    interests[count] = interest;
    count++;
  }
  /* n can exceed fd_set's ABI width only for our high fake descriptors,
   * whose fortified FD_* wrappers place them in the reserved alias bits. */
  if (n > BIONIC_FD_SETSIZE && !has_fake_alias) {
    errno = EINVAL;
    return -1;
  }

  const uint64_t start_ns = armTicksToNs(armGetSystemTick());
  int result = poll_fake(polls, count, timeout_ms);
  if (guest_timeout && requested_ns != UINT64_MAX) {
    uint64_t elapsed_ns = armTicksToNs(armGetSystemTick()) - start_ns;
    uint64_t remaining_ns = elapsed_ns < requested_ns ? requested_ns - elapsed_ns : 0;
    guest_timeout->seconds = (int64_t)(remaining_ns / 1000000000u);
    guest_timeout->microseconds = (int64_t)((remaining_ns % 1000000000u) / 1000u);
  }
  if (result < 0) return -1;

  for (unsigned i = 0; i < count; i++) {
    if ((uint16_t)polls[i].revents & B_POLLNVAL) { errno = EBADF; return -1; }
  }
  if (read_set) memset(read_set, 0, sizeof(*read_set));
  if (write_set) memset(write_set, 0, sizeof(*write_set));
  if (except_set) memset(except_set, 0, sizeof(*except_set));
  int selected = 0;
  for (unsigned i = 0; i < count; i++) {
    const uint16_t ready = (uint16_t)polls[i].revents;
    int descriptor_ready = 0;
    if ((interests[i] & 1u) &&
        (ready & (B_POLLIN | B_POLLRDNORM | B_POLLERR | B_POLLHUP |
                  B_POLLRDHUP))) {
      bionic_fd_set(read_set, bits[i]);
      descriptor_ready = 1;
    }
    if ((interests[i] & 2u) &&
        (ready & (B_POLLOUT | B_POLLWRNORM | B_POLLERR))) {
      bionic_fd_set(write_set, bits[i]);
      descriptor_ready = 1;
    }
    if ((interests[i] & 4u) && (ready & B_POLLPRI)) {
      bionic_fd_set(except_set, bits[i]);
      descriptor_ready = 1;
    }
    selected += descriptor_ready;
  }
  return selected;
}
int socket_fake(int d, int t, int p) {
  /* Android permits creating sockets while the route is temporarily down.
   * Returning EAFNOSUPPORT here is a permanent capability failure and causes
   * curl/SDK feature probes to disable networking instead of reconnecting.
   * Route-dependent operations (connect, DNS, send) report the transient
   * host error when they are actually attempted. */
  NET_DIAG_ADD(socket_calls, 1);
  int nb = (t & B_SOCK_NONBLOCK) != 0;
  const int native_domain = af_b2n(d);
  const int native_type = t & ~(B_SOCK_NONBLOCK | B_SOCK_CLOEXEC);
  int fd = socket(native_domain, native_type, p);
  if (fd < 0) {
    const int native_error = errno;
    if (native_domain == AF_INET6 &&
        (native_error == EAFNOSUPPORT || native_error == EPROTONOSUPPORT ||
         native_error == ESOCKTNOSUPPORT))
      __atomic_store_n(&g_ipv6_socket_state, -1, __ATOMIC_RELAXED);
    NET_FAIL();
    NET_DIAG_ADD(socket_failures, 1);
    NET_DIAG_STORE(last_socket_error, errno);
    return -1;
  }
  if (native_domain == AF_INET6)
    __atomic_store_n(&g_ipv6_socket_state, 1, __ATOMIC_RELAXED);
  if (nb) {
    const int fl = fcntl(fd, F_GETFL, 0);
    if (fl < 0 || fcntl(fd, F_SETFL, fl | O_NONBLOCK) < 0) {
      const int saved = n2b_errno(errno);
      close(fd);
      errno = saved;
      NET_DIAG_ADD(socket_failures, 1);
      NET_DIAG_STORE(last_socket_error, errno);
      return -1;
    }
  }
  network_track_open(fd, native_type);
  NET_DIAG_ADD(socket_successes, 1);
  return fd;
}
int connect_fake(int s, const void *a, unsigned l) {
  NET_DIAG_ADD(connect_calls, 1);
  network_track_connect(s);
  unsigned char nsa[128]; unsigned n = sa_b2n(a, l, nsa);
  int r = connect(s, (const struct sockaddr *)nsa, n);
  if (r < 0) {
    NET_FAIL();
    NET_DIAG_STORE(last_connect_error, errno);
    if (errno == 115 || errno == 114)
      NET_DIAG_ADD(connect_in_progress, 1);
    else
      NET_DIAG_ADD(connect_failures, 1);
  } else {
    NET_DIAG_ADD(connect_immediate_successes, 1);
    NET_DIAG_STORE(last_connect_error, 0);
  }
  return r;
}
long recvmsg_fake(int s, void *msg, int flags) {
  if (!msg) { errno = EFAULT; return -1; }
  /* Linux error queues have no libnx/FreeBSD socket equivalent.  In
   * particular, treating MTR's MSG_ERRQUEUE|MSG_DONTWAIT (0x2040) as an
   * ordinary receive can consume live game traffic. */
  if ((uint32_t)flags & B_MSG_ERRQUEUE) {
    errno = EOPNOTSUPP;
    NET_FAIL();
    return -1;
  }
  GuestMsghdr *guest = (GuestMsghdr *)msg;
  if (guest->msg_iovlen > INT_MAX || guest->msg_controllen > UINT_MAX) {
    errno = EINVAL;
    return -1;
  }
  const size_t guest_control_capacity = (size_t)guest->msg_controllen;
  if (guest_control_capacity && !guest->msg_control) {
    errno = EFAULT;
    return -1;
  }
  /* Genshin's bundled KCP transport uses exactly one iovec and no ancillary
   * data.  libnx implements recvmsg through recvmmsg, including a page-sized
   * serialization allocation on every nonblocking probe.  recvfrom is the
   * same operation for this datagram shape, avoids that hot allocation, and
   * does not pass Android's send-only MSG_NOSIGNAL to Horizon's receive IPC. */
  if (network_socket_is_datagram(s) && guest->msg_iovlen == 1 &&
      guest_control_capacity == 0 && guest->msg_iov) {
    const struct iovec *iov = &guest->msg_iov[0];
    unsigned char native_name[128];
    unsigned native_name_len = sizeof native_name;
    void *guest_name = guest->msg_name;
    const unsigned guest_name_len = guest->msg_namelen;
    const uint64_t receive_generation = network_track_recv_enter(s);
    long r = recvfrom(s, iov->iov_base, iov->iov_len,
                      msg_receive_b2n(flags),
                      guest_name ? (struct sockaddr *)native_name : NULL,
                      guest_name ? &native_name_len : NULL);
    const int native_error = errno;
    network_track_recv_leave(s, receive_generation);
    errno = native_error;
    if (r < 0) NET_FAIL();
    const int mapped_error = r < 0 ? errno : 0;
    if (r >= 0) {
      guest->msg_controllen = 0;
      guest->msg_flags = 0;
      if (guest_name) {
        unsigned capacity = guest_name_len;
        sa_n2b(native_name, native_name_len, guest_name, &capacity);
        guest->msg_namelen = capacity;
      }
    }
    network_udp_record_receive(s, r);
    network_record_recv_result(s, r);
    if (r < 0) errno = mapped_error;
    return r;
  }
  void *native_control = NULL;
  if (guest_control_capacity) {
    native_control = calloc(1, guest_control_capacity);
    if (!native_control) { errno = ENOMEM; return -1; }
  }
  struct msghdr native = {
    .msg_name = guest->msg_name,
    .msg_namelen = guest->msg_namelen,
    .msg_iov = guest->msg_iov,
    .msg_iovlen = (int)guest->msg_iovlen,
    .msg_control = native_control,
    .msg_controllen = (socklen_t)guest_control_capacity,
    .msg_flags = 0,
  };
  unsigned char native_name[128];
  void *guest_name = guest->msg_name;
  unsigned guest_name_len = guest->msg_namelen;
  if (guest_name) { native.msg_name = native_name; native.msg_namelen = sizeof native_name; }
  const uint64_t receive_generation = network_track_recv_enter(s);
  long r = recvmsg(s, &native, msg_receive_b2n(flags));
  network_track_recv_leave(s, receive_generation);
  if (r < 0) {
    const int saved = errno;
    free(native_control);
    errno = saved;
    NET_FAIL();
    network_udp_record_receive(s, r);
    network_record_recv_result(s, r);
    return r;
  }
  int control_truncated = (native.msg_flags & MSG_CTRUNC) != 0;
  guest->msg_controllen = cmsg_control_n2b(
    native_control, native.msg_controllen, guest->msg_control,
    guest_control_capacity, &control_truncated);
  free(native_control);
  guest->msg_flags = msg_n2b(native.msg_flags) |
                     (control_truncated ? B_MSG_CTRUNC : 0);
  if (guest_name) {
    unsigned cap = guest_name_len;
    sa_n2b(native_name, native.msg_namelen, guest_name, &cap);
    guest->msg_namelen = cap;
  }
  network_udp_record_receive(s, r);
  network_record_recv_result(s, r);
  return r;
}
long sendmsg_fake(int s, const void *msg, int flags) {
  if (!msg) { errno = EFAULT; return -1; }
  const GuestMsghdr *guest = (const GuestMsghdr *)msg;
  if (guest->msg_iovlen > INT_MAX || guest->msg_controllen > UINT_MAX) {
    errno = EINVAL;
    return -1;
  }
  const size_t guest_control_len = (size_t)guest->msg_controllen;
  /* The matching KCP send is also a single datagram without control data.
   * Use libnx's direct BSD IPC instead of sendmmsg's per-call marshalling. */
  if (network_socket_is_datagram(s) && guest->msg_iovlen == 1 &&
      guest_control_len == 0 && guest->msg_iov) {
    const struct iovec *iov = &guest->msg_iov[0];
    unsigned char native_name[128];
    const unsigned native_name_len = guest->msg_name
      ? sa_b2n(guest->msg_name, guest->msg_namelen, native_name) : 0;
    const int connected_peer = guest->msg_name &&
      network_udp_destination_is_connected_peer(s, native_name,
                                                 native_name_len);
    long r = sendto(s, iov->iov_base, iov->iov_len,
                    msg_datagram_send_b2n(flags),
                    guest->msg_name && !connected_peer
                      ? (const struct sockaddr *)native_name : NULL,
                    connected_peer ? 0 : native_name_len);
    if (r < 0) NET_FAIL();
    network_udp_record_send(s, r);
    network_record_send_result(r);
    return r;
  }
  void *native_control = cmsg_control_b2n(guest->msg_control,
                                          guest_control_len);
  if (guest_control_len && !native_control) {
    NET_FAIL();
    return -1;
  }
  struct msghdr native = {
    .msg_name = guest->msg_name,
    .msg_namelen = guest->msg_namelen,
    .msg_iov = guest->msg_iov,
    .msg_iovlen = (int)guest->msg_iovlen,
    .msg_control = native_control,
    .msg_controllen = (socklen_t)guest_control_len,
    .msg_flags = msg_b2n(guest->msg_flags),
  };
  unsigned char native_name[128];
  if (guest->msg_name) {
    native.msg_namelen = sa_b2n(guest->msg_name, guest->msg_namelen, native_name);
    native.msg_name = native_name;
  }
  long r = sendmsg(s, &native, msg_b2n(flags));
  const int saved = errno;
  free(native_control);
  errno = saved;
  if (r < 0) NET_FAIL();
  network_udp_record_send(s, r);
  network_record_send_result(r);
  return r;
}
int inet_pton_shim(int af, const char *src, void *dst) { return inet_pton(af_b2n(af), src, dst); }
const char *inet_ntop_shim(int af, const void *src, char *dst, unsigned size) { return inet_ntop(af_b2n(af), src, dst, size); }
int bind_fake(int s, const void *a, unsigned l) {
  unsigned char nsa[128]; unsigned n = sa_b2n(a, l, nsa);
  int r = bind(s, (const struct sockaddr *)nsa, n); if (r < 0) NET_FAIL(); return r;
}
int listen_fake(int s, int b) { int r = listen(s, b); if (r < 0) NET_FAIL(); return r; }
int accept_fake(int s, void *a, void *l) {
  unsigned char nsa[128]; unsigned nl = sizeof nsa;
  int r = accept(s, a ? (struct sockaddr *)nsa : NULL, a ? &nl : NULL);
  if (r < 0) { NET_FAIL(); return -1; }
  if (a && l) sa_n2b(nsa, nl, a, (unsigned *)l);
  /* accept returns a new connected endpoint.  Track it independently so a
   * close/reuse race in poll retains the same identity guarantees as socket. */
  network_track_open(r, SOCK_STREAM);
  return r;
}
long send_fake(int s, const void *b, size_t l, int f) {
  long r = send(s, b, l, msg_b2n(f));
  if (r < 0) NET_FAIL();
  network_udp_record_send(s, r);
  network_record_send_result(r);
  return r;
}
long sendto_fake(int s, const void *b, size_t l, int f, const void *a, unsigned al) {
  unsigned char nsa[128];
  unsigned nl = a ? sa_b2n(a, al, nsa) : 0;
  long r = sendto(s, b, l, msg_b2n(f), a ? (const struct sockaddr *)nsa : NULL, nl);
  if (r < 0) NET_FAIL();
  network_udp_record_send(s, r);
  network_record_send_result(r);
  return r;
}
long recv_fake(int s, void *b, size_t l, int f) {
  const uint64_t receive_generation = network_track_recv_enter(s);
  long r = recv(s, b, l, msg_receive_b2n(f));
  network_track_recv_leave(s, receive_generation);
  if (r < 0) NET_FAIL();
  network_udp_record_receive(s, r);
  network_record_recv_result(s, r);
  return r;
}
long recvfrom_fake(int s, void *b, size_t l, int f, void *a, void *al) {
  unsigned char nsa[128]; unsigned nl = sizeof nsa;
  const uint64_t receive_generation = network_track_recv_enter(s);
  long r = recvfrom(s, b, l, msg_receive_b2n(f), a ? (struct sockaddr *)nsa : NULL, a ? &nl : NULL);
  network_track_recv_leave(s, receive_generation);
  if (r < 0) {
    NET_FAIL();
    network_udp_record_receive(s, r);
    network_record_recv_result(s, r);
    return -1;
  }
  if (a && al) sa_n2b(nsa, nl, a, (unsigned *)al);
  network_udp_record_receive(s, r);
  network_record_recv_result(s, r);
  return r;
}
int shutdown_fake(int s, int how) { int r = shutdown(s, how); if (r < 0) NET_FAIL(); return r; }
int socketpair_fake(int domain, int type, int protocol, int pair[2]) {
  if (!pair) { errno = EFAULT; return -1; }
  const int nonblocking = (type & B_SOCK_NONBLOCK) != 0;
  const int native_type = type & ~(B_SOCK_NONBLOCK | B_SOCK_CLOEXEC);
  int r = socketpair(af_b2n(domain),
                     native_type,
                     protocol, pair);
  if (r < 0) NET_FAIL();
  if (r == 0 && nonblocking) {
    for (unsigned i = 0; i < 2; ++i) {
      const int flags = fcntl(pair[i], F_GETFL, 0);
      if (flags < 0 || fcntl(pair[i], F_SETFL, flags | O_NONBLOCK) < 0) {
        const int saved = n2b_errno(errno);
        close(pair[0]);
        close(pair[1]);
        pair[0] = pair[1] = -1;
        errno = saved;
        return -1;
      }
    }
  }
  if (r == 0) {
    network_track_open(pair[0], native_type);
    network_track_open(pair[1], native_type);
  }
  return r;
}
int setsockopt_fake(int s, int lv, int n, const void *v, unsigned l) {
  int L = lv, N = n;
  if (opt_b2n(&L, &N) < 0) { NET_FAIL(); return -1; }
  int dontfrag;
  if (lv == 0 && n == 10) {
    if (!v || l < sizeof(int)) { errno = EINVAL; return -1; }
    const int linux_pmtudisc = *(const int *)v;
    if (linux_pmtudisc != 0 && linux_pmtudisc != 2) {
      errno = 92; /* bionic ENOPROTOOPT */
      return -1;
    }
    dontfrag = linux_pmtudisc == 2;
    v = &dontfrag;
    l = sizeof dontfrag;
  }
  /* Floor-clamp SO_RCVBUF so the guest cannot shrink a receive window below
   * the promoted target.  The one-shot promotions in
   * network_maybe_promote_receive_window / _datagram_window never re-fire,
   * so a later setsockopt(SO_RCVBUF, small) from the game would collapse the
   * window and cap bulk-download throughput (observed ~350 kbit/s).  The
   * clamp is socket-type aware: streams use the long-stream target,
   * datagrams (KCP) use the datagram target.  When the configured target is
   * 0 (no promotion configured / control traffic), this is a no-op and the
   * guest's value passes through unchanged. */
  int rcvbuf_floor_value = 0;
  if (L == SOL_SOCKET && N == SO_RCVBUF && v && l >= sizeof(int)) {
    const int is_datagram = network_socket_is_datagram(s);
    const uint32_t target = __atomic_load_n(
      is_datagram ? &g_network_datagram_receive_window
                  : &g_network_long_stream_receive_window,
      __ATOMIC_RELAXED);
    if (target > 0 && target <= (uint32_t)INT_MAX) {
      int requested = *(const int *)v;
      if ((uint32_t)requested < target) {
        rcvbuf_floor_value = (int)target;
        v = &rcvbuf_floor_value;
        l = sizeof rcvbuf_floor_value;
        if (is_datagram) NET_DIAG_ADD(datagram_window_attempts, 1);
        else NET_DIAG_ADD(long_stream_window_attempts, 1);
      }
    }
  }
  int r = setsockopt(s, L, N, v, l); if (r < 0) NET_FAIL(); return r;
}
int getsockopt_fake(int s, int lv, int n, void *v, void *l) {
  int L = lv, N = n;
  if (opt_b2n(&L, &N) < 0) { NET_FAIL(); return -1; }
  int r = getsockopt(s, L, N, v, (unsigned *)l); if (r < 0) { NET_FAIL(); return -1; }
  if (L == SOL_SOCKET && N == SO_ERROR && v && l && *(unsigned *)l >= 4) {
    *(int *)v = n2b_errno(*(int *)v);
    NET_DIAG_ADD(connect_error_checks, 1);
    NET_DIAG_STORE(last_connect_completion_error, *(int *)v);
    if (*(int *)v == 0) NET_DIAG_ADD(connect_error_clear, 1);
    else NET_DIAG_ADD(connect_error_failures, 1);
  }
  if (lv == 0 && n == 10 && v && l && *(unsigned *)l >= sizeof(int))
    *(int *)v = *(int *)v ? 2 : 0; /* Linux IP_PMTUDISC_DO / DONT. */
  return r;
}
int getsockname_fake(int s, void *a, void *l) {
  unsigned char nsa[128]; unsigned nl = sizeof nsa;
  int r = getsockname(s, (struct sockaddr *)nsa, &nl); if (r < 0) { NET_FAIL(); return -1; }
  sa_n2b(nsa, nl, a, (unsigned *)l); return 0;
}
int getpeername_fake(int s, void *a, void *l) {
  unsigned char nsa[128]; unsigned nl = sizeof nsa;
  int r = getpeername(s, (struct sockaddr *)nsa, &nl); if (r < 0) { NET_FAIL(); return -1; }
  sa_n2b(nsa, nl, a, (unsigned *)l); return 0;
}
/* arm64 Bionic follows the BSD addrinfo field order: ai_canonname precedes
 * ai_addr.  Keep a separate guest type even though libnx currently has the
 * same order; making the offsets explicit prevents a host-header change from
 * silently swapping the two guest pointers. */
struct b_addrinfo {
  int ai_flags, ai_family, ai_socktype, ai_protocol;
  unsigned ai_addrlen;
  char *ai_canonname;
  void *ai_addr;
  struct b_addrinfo *ai_next;
};
_Static_assert(offsetof(struct b_addrinfo, ai_addrlen) == 16,
               "arm64 bionic addrinfo length offset");
_Static_assert(offsetof(struct b_addrinfo, ai_canonname) == 24,
               "arm64 bionic addrinfo canonical-name offset");
_Static_assert(offsetof(struct b_addrinfo, ai_addr) == 32,
               "arm64 bionic addrinfo address offset");
_Static_assert(offsetof(struct b_addrinfo, ai_next) == 40,
               "arm64 bionic addrinfo next offset");
_Static_assert(sizeof(struct b_addrinfo) == 48,
               "arm64 bionic addrinfo size");

#define B_AI_PASSIVE      0x0001u
#define B_AI_CANONNAME    0x0002u
#define B_AI_NUMERICHOST  0x0004u
#define B_AI_NUMERICSERV  0x0008u
#define B_AI_ALL          0x0100u
#define B_AI_ADDRCONFIG   0x0400u
#define B_AI_V4MAPPED     0x0800u
#define B_AI_MASK (B_AI_PASSIVE | B_AI_CANONNAME | B_AI_NUMERICHOST | \
                   B_AI_V4MAPPED | B_AI_ALL | B_AI_ADDRCONFIG | \
                   B_AI_NUMERICSERV)

#define B_NI_NOFQDN       0x01u
#define B_NI_NUMERICHOST  0x02u
#define B_NI_NAMEREQD     0x04u
#define B_NI_NUMERICSERV  0x08u
#define B_NI_DGRAM        0x10u

static int ai_flags_b2n(int bionic_flags, int *native_flags) {
  const uint32_t flags = (uint32_t)bionic_flags;
  if (flags & ~B_AI_MASK) return -1;
  int translated = 0;
  if (flags & B_AI_PASSIVE)     translated |= AI_PASSIVE;
  if (flags & B_AI_CANONNAME)   translated |= AI_CANONNAME;
  if (flags & B_AI_NUMERICHOST) translated |= AI_NUMERICHOST;
  if (flags & B_AI_V4MAPPED)    translated |= AI_V4MAPPED;
  if (flags & B_AI_ALL)         translated |= AI_ALL;
  if (flags & B_AI_ADDRCONFIG)  translated |= AI_ADDRCONFIG;
  if (flags & B_AI_NUMERICSERV) translated |= AI_NUMERICSERV;
  *native_flags = translated;
  return 0;
}

static int ai_flags_n2b(int native_flags, int *bionic_flags) {
  const int known = AI_PASSIVE | AI_CANONNAME | AI_NUMERICHOST |
                    AI_V4MAPPED | AI_ALL | AI_ADDRCONFIG | AI_NUMERICSERV;
  if (native_flags & ~known) return -1;
  int translated = 0;
  if (native_flags & AI_PASSIVE)     translated |= B_AI_PASSIVE;
  if (native_flags & AI_CANONNAME)   translated |= B_AI_CANONNAME;
  if (native_flags & AI_NUMERICHOST) translated |= B_AI_NUMERICHOST;
  if (native_flags & AI_V4MAPPED)    translated |= B_AI_V4MAPPED;
  if (native_flags & AI_ALL)         translated |= B_AI_ALL;
  if (native_flags & AI_ADDRCONFIG)  translated |= B_AI_ADDRCONFIG;
  if (native_flags & AI_NUMERICSERV) translated |= B_AI_NUMERICSERV;
  *bionic_flags = translated;
  return 0;
}

/* curl's synchronous resolver fallback arms SIGALRM around getaddrinfo().
 * Horizon cannot asynchronously enter a guest POSIX signal handler, so retain
 * alarm's per-thread deadline and enforce it at the blocking resolver boundary
 * instead.  This preserves the timeout without jumping through guest frames. */
static _Thread_local uint64_t g_resolver_alarm_deadline_ns;

unsigned alarm_fake(unsigned seconds) {
  const uint64_t now = armTicksToNs(armGetSystemTick());
  unsigned previous = 0;
  if (g_resolver_alarm_deadline_ns > now) {
    const uint64_t remaining = g_resolver_alarm_deadline_ns - now;
    const uint64_t rounded = (remaining + UINT64_C(999999999)) /
                             UINT64_C(1000000000);
    previous = rounded > UINT_MAX ? UINT_MAX : (unsigned)rounded;
  }
  if (!seconds) {
    g_resolver_alarm_deadline_ns = 0;
  } else {
    const uint64_t delay = (uint64_t)seconds * UINT64_C(1000000000);
    g_resolver_alarm_deadline_ns = now > UINT64_MAX - delay ? UINT64_MAX :
                                                                   now + delay;
  }
  return previous;
}

typedef struct ResolverJob {
  Mutex lock;
  CondVar cond;
  char *node;
  char *service;
  struct addrinfo hints;
  int have_hints;
  struct addrinfo *result;
  int rc;
  int done;
  int abandoned;
  struct ResolverJob *next;
  struct ResolverJob *inflight_next;
} ResolverJob;

#define MAX_PENDING_RESOLVERS 8
#define RESOLVER_POOL_WORKERS 4
#define RESOLVER_DEFAULT_TIMEOUT_NS UINT64_C(5000000000)
#define RESOLVER_CACHE_ENTRIES 16
#define RESOLVER_CACHE_TTL_NS UINT64_C(60000000000)
#define RESOLVER_NEGATIVE_CACHE_TTL_NS UINT64_C(1000000000)
static int g_pending_resolvers;
static Mutex g_resolver_queue_lock;
static CondVar g_resolver_queue_cond;
static ResolverJob *g_resolver_queue_head;
static ResolverJob *g_resolver_queue_tail;
static ResolverJob *g_resolver_inflight_head;
enum {
  RESOLVER_POOL_STOPPED = 0,
  RESOLVER_POOL_STARTING,
  RESOLVER_POOL_READY,
};
static unsigned g_resolver_pool_state;

typedef struct {
  char *node;
  char *service;
  struct addrinfo hints;
  int have_hints;
  int rc;
  struct addrinfo *result;
  uint64_t expires_ns;
  uint64_t serial;
} ResolverCacheEntry;

static ResolverCacheEntry g_resolver_cache[RESOLVER_CACHE_ENTRIES];
static uint64_t g_resolver_cache_serial;

static void resolver_addrinfo_free(struct addrinfo *head) {
  while (head) {
    struct addrinfo *next = head->ai_next;
    free(head->ai_addr);
    free(head->ai_canonname);
    free(head);
    head = next;
  }
}

static struct addrinfo *resolver_addrinfo_clone(const struct addrinfo *source) {
  struct addrinfo *head = NULL;
  struct addrinfo *tail = NULL;
  for (const struct addrinfo *item = source; item; item = item->ai_next) {
    struct addrinfo *copy = calloc(1, sizeof *copy);
    if (!copy) goto fail;
    copy->ai_flags = item->ai_flags;
    copy->ai_family = item->ai_family;
    copy->ai_socktype = item->ai_socktype;
    copy->ai_protocol = item->ai_protocol;
    copy->ai_addrlen = item->ai_addrlen;
    if (item->ai_addr && item->ai_addrlen) {
      copy->ai_addr = malloc(item->ai_addrlen);
      if (!copy->ai_addr) { free(copy); goto fail; }
      memcpy(copy->ai_addr, item->ai_addr, item->ai_addrlen);
    }
    if (item->ai_canonname) {
      copy->ai_canonname = strdup(item->ai_canonname);
      if (!copy->ai_canonname) {
        free(copy->ai_addr);
        free(copy);
        goto fail;
      }
    }
    if (tail) tail->ai_next = copy;
    else head = copy;
    tail = copy;
  }
  return head;

fail:
  resolver_addrinfo_free(head);
  return NULL;
}

static int resolver_optional_string_equal(const char *left,
                                          const char *right) {
  return left == right || (left && right && strcmp(left, right) == 0);
}

static int resolver_hints_equal(const struct addrinfo *left, int have_left,
                                const struct addrinfo *right,
                                int have_right) {
  if (have_left != have_right) return 0;
  if (!have_left) return 1;
  return left->ai_flags == right->ai_flags &&
         left->ai_family == right->ai_family &&
         left->ai_socktype == right->ai_socktype &&
         left->ai_protocol == right->ai_protocol;
}

static int resolver_query_equal(const char *left_node,
                                const char *left_service,
                                const struct addrinfo *left_hints,
                                int left_have_hints,
                                const char *right_node,
                                const char *right_service,
                                const struct addrinfo *right_hints,
                                int right_have_hints) {
  return resolver_optional_string_equal(left_node, right_node) &&
         resolver_optional_string_equal(left_service, right_service) &&
         resolver_hints_equal(left_hints, left_have_hints, right_hints,
                              right_have_hints);
}

static void resolver_cache_entry_clear_locked(ResolverCacheEntry *entry) {
  free(entry->node);
  free(entry->service);
  resolver_addrinfo_free(entry->result);
  memset(entry, 0, sizeof *entry);
}

static int resolver_cache_lookup_locked(const char *node, const char *service,
                                        const struct addrinfo *hints,
                                        int have_hints, uint64_t now_ns,
                                        struct addrinfo **result_out,
                                        int *rc_out) {
  for (unsigned i = 0; i < RESOLVER_CACHE_ENTRIES; ++i) {
    ResolverCacheEntry *entry = &g_resolver_cache[i];
    if (!entry->expires_ns) continue;
    if (entry->expires_ns <= now_ns) {
      resolver_cache_entry_clear_locked(entry);
      continue;
    }
    if (!resolver_query_equal(node, service, hints, have_hints,
                              entry->node, entry->service, &entry->hints,
                              entry->have_hints))
      continue;
    struct addrinfo *copy = entry->rc == 0
      ? resolver_addrinfo_clone(entry->result) : NULL;
    *rc_out = entry->rc == 0 && !copy ? EAI_MEMORY : entry->rc;
    *result_out = copy;
    entry->serial = ++g_resolver_cache_serial;
    NET_DIAG_ADD(resolver_cache_hits, 1);
    return 1;
  }
  return 0;
}

static int resolver_cache_lookup(const char *node, const char *service,
                                 const struct addrinfo *hints,
                                 int have_hints,
                                 struct addrinfo **result_out, int *rc_out) {
  mutexLock(&g_resolver_queue_lock);
  const int found = resolver_cache_lookup_locked(
    node, service, hints, have_hints, armTicksToNs(armGetSystemTick()),
    result_out, rc_out);
  mutexUnlock(&g_resolver_queue_lock);
  return found;
}

static void resolver_cache_store(const char *node, const char *service,
                                 const struct addrinfo *hints,
                                 int have_hints, int rc,
                                 const struct addrinfo *result) {
  char *node_copy = node ? strdup(node) : NULL;
  char *service_copy = service ? strdup(service) : NULL;
  struct addrinfo *result_copy = rc == 0
    ? resolver_addrinfo_clone(result) : NULL;
  if ((node && !node_copy) || (service && !service_copy) ||
      (rc == 0 && !result_copy)) {
    free(node_copy);
    free(service_copy);
    resolver_addrinfo_free(result_copy);
    return;
  }

  const uint64_t now_ns = armTicksToNs(armGetSystemTick());
  mutexLock(&g_resolver_queue_lock);
  ResolverCacheEntry *target = NULL;
  for (unsigned i = 0; i < RESOLVER_CACHE_ENTRIES; ++i) {
    ResolverCacheEntry *entry = &g_resolver_cache[i];
    if (!entry->expires_ns || entry->expires_ns <= now_ns ||
        resolver_query_equal(node, service, hints, have_hints,
                             entry->node, entry->service, &entry->hints,
                             entry->have_hints)) {
      target = entry;
      break;
    }
    if (!target || entry->serial < target->serial) target = entry;
  }
  if (target) {
    resolver_cache_entry_clear_locked(target);
    target->node = node_copy;
    target->service = service_copy;
    if (have_hints) target->hints = *hints;
    target->have_hints = have_hints;
    target->rc = rc;
    target->result = result_copy;
    const uint64_t ttl = rc == 0 ? RESOLVER_CACHE_TTL_NS
                                 : RESOLVER_NEGATIVE_CACHE_TTL_NS;
    target->expires_ns = now_ns > UINT64_MAX - ttl ? UINT64_MAX
                                                    : now_ns + ttl;
    target->serial = ++g_resolver_cache_serial;
    node_copy = NULL;
    service_copy = NULL;
    result_copy = NULL;
    NET_DIAG_ADD(resolver_cache_stores, 1);
  }
  mutexUnlock(&g_resolver_queue_lock);
  free(node_copy);
  free(service_copy);
  resolver_addrinfo_free(result_copy);
}

static ResolverJob *resolver_find_inflight_locked(
    const char *node, const char *service, const struct addrinfo *hints,
    int have_hints) {
  for (ResolverJob *job = g_resolver_inflight_head; job;
       job = job->inflight_next)
    if (resolver_query_equal(node, service, hints, have_hints,
                             job->node, job->service, &job->hints,
                             job->have_hints))
      return job;
  return NULL;
}

static void resolver_remove_inflight_locked(ResolverJob *job) {
  ResolverJob **link = &g_resolver_inflight_head;
  while (*link && *link != job) link = &(*link)->inflight_next;
  if (*link == job) *link = job->inflight_next;
  job->inflight_next = NULL;
}

static void resolver_job_destroy(ResolverJob *job, int release_result) {
  if (!job) return;
  if (release_result && job->result) resolver_addrinfo_free(job->result);
  free(job->node);
  free(job->service);
  __atomic_sub_fetch(&g_pending_resolvers, 1, __ATOMIC_ACQ_REL);
  free(job);
}

static void *resolver_worker(void *opaque) {
  (void)opaque;
  for (;;) {
    mutexLock(&g_resolver_queue_lock);
    while (!g_resolver_queue_head)
      condvarWait(&g_resolver_queue_cond, &g_resolver_queue_lock);
    ResolverJob *job = g_resolver_queue_head;
    g_resolver_queue_head = job->next;
    if (!g_resolver_queue_head) g_resolver_queue_tail = NULL;
    job->next = NULL;
    mutexUnlock(&g_resolver_queue_lock);

    struct addrinfo *native_result = NULL;
    int rc = getaddrinfo(job->node, job->service,
                         job->have_hints ? &job->hints : NULL,
                         &native_result);
    struct addrinfo *result = NULL;
    if (rc == 0) {
      result = resolver_addrinfo_clone(native_result);
      if (!result) rc = EAI_MEMORY;
    }
    if (native_result) freeaddrinfo(native_result);
    resolver_cache_store(job->node, job->service,
                         job->have_hints ? &job->hints : NULL,
                         job->have_hints, rc, result);
    mutexLock(&g_resolver_queue_lock);
    resolver_remove_inflight_locked(job);
    condvarWakeAll(&g_resolver_queue_cond);
    mutexUnlock(&g_resolver_queue_lock);
    mutexLock(&job->lock);
    job->result = result;
    job->rc = rc;
    job->done = 1;
    const int abandoned = job->abandoned;
    condvarWakeAll(&job->cond);
    mutexUnlock(&job->lock);
    NET_DIAG_ADD(resolver_jobs_completed, 1);
    if (abandoned) resolver_job_destroy(job, 1);
  }
  return NULL;
}

static int ensure_resolver_pool(void) {
  mutexLock(&g_resolver_queue_lock);
  while (g_resolver_pool_state == RESOLVER_POOL_STARTING)
    condvarWait(&g_resolver_queue_cond, &g_resolver_queue_lock);
  if (g_resolver_pool_state == RESOLVER_POOL_READY) {
    mutexUnlock(&g_resolver_queue_lock);
    return 1;
  }
  g_resolver_pool_state = RESOLVER_POOL_STARTING;
  mutexUnlock(&g_resolver_queue_lock);

  unsigned created = 0;
  for (; created < RESOLVER_POOL_WORKERS; ++created) {
    pthread_t worker;
    if (pthread_create(&worker, NULL, resolver_worker, NULL) != 0) {
      NET_DIAG_ADD(resolver_pool_failures, 1);
      break;
    }
  }

  mutexLock(&g_resolver_queue_lock);
  g_resolver_pool_state = created ? RESOLVER_POOL_READY : RESOLVER_POOL_STOPPED;
  NET_DIAG_STORE(resolver_pool_workers, created);
  condvarWakeAll(&g_resolver_queue_cond);
  mutexUnlock(&g_resolver_queue_lock);
  return created != 0;
}

static int enqueue_resolver_job(ResolverJob *job) {
  if (!job || !ensure_resolver_pool()) return 0;
  mutexLock(&g_resolver_queue_lock);
  if (resolver_find_inflight_locked(job->node, job->service,
                                    job->have_hints ? &job->hints : NULL,
                                    job->have_hints)) {
    mutexUnlock(&g_resolver_queue_lock);
    return 2;
  }
  job->next = NULL;
  job->inflight_next = g_resolver_inflight_head;
  g_resolver_inflight_head = job;
  if (g_resolver_queue_tail)
    g_resolver_queue_tail->next = job;
  else
    g_resolver_queue_head = job;
  g_resolver_queue_tail = job;
  condvarWakeOne(&g_resolver_queue_cond);
  mutexUnlock(&g_resolver_queue_lock);
  NET_DIAG_ADD(resolver_jobs_queued, 1);
  return 1;
}

static int resolver_wait_for_coalesced_query(
    const char *node, const char *service, const struct addrinfo *hints,
    int have_hints, uint64_t deadline_ns, struct addrinfo **result_out) {
  int rc = EAI_AGAIN;
  mutexLock(&g_resolver_queue_lock);
  for (;;) {
    const uint64_t now_ns = armTicksToNs(armGetSystemTick());
    if (resolver_cache_lookup_locked(node, service, hints, have_hints,
                                     now_ns, result_out, &rc)) {
      mutexUnlock(&g_resolver_queue_lock);
      return rc;
    }
    if (!resolver_find_inflight_locked(node, service, hints, have_hints) ||
        now_ns >= deadline_ns) {
      mutexUnlock(&g_resolver_queue_lock);
      return EAI_AGAIN;
    }
    (void)condvarWaitTimeout(&g_resolver_queue_cond, &g_resolver_queue_lock,
                             deadline_ns - now_ns);
  }
}

static int getaddrinfo_with_deadline(const char *node, const char *service,
                                     const struct addrinfo *hints,
                                     uint64_t deadline_ns,
                                     struct addrinfo **result_out) {
  *result_out = NULL;
  const uint64_t now = armTicksToNs(armGetSystemTick());
  if (deadline_ns <= now) return EAI_AGAIN;
  int cached_rc = EAI_AGAIN;
  if (resolver_cache_lookup(node, service, hints, hints != NULL,
                            result_out, &cached_rc))
    return cached_rc;
  if (__atomic_fetch_add(&g_pending_resolvers, 1, __ATOMIC_ACQ_REL) >=
      MAX_PENDING_RESOLVERS) {
    __atomic_sub_fetch(&g_pending_resolvers, 1, __ATOMIC_ACQ_REL);
    return EAI_AGAIN;
  }

  ResolverJob *job = calloc(1, sizeof *job);
  if (!job) {
    __atomic_sub_fetch(&g_pending_resolvers, 1, __ATOMIC_ACQ_REL);
    return EAI_MEMORY;
  }
  if (node && !(job->node = strdup(node))) {
    resolver_job_destroy(job, 0);
    return EAI_MEMORY;
  }
  if (service && !(job->service = strdup(service))) {
    resolver_job_destroy(job, 0);
    return EAI_MEMORY;
  }
  if (hints) {
    job->hints = *hints;
    job->have_hints = 1;
  }

  /* libnx stores PTHREAD_CREATE_DETACHED in the attribute but its newlib
   * adapter never consumes that field.  A thread-per-query design therefore
   * leaks every completed resolver's native stack and handle.  Queue copied
   * jobs onto a fixed persistent pool instead. */
  const int enqueue_result = enqueue_resolver_job(job);
  if (!enqueue_result) {
    resolver_job_destroy(job, 0);
    return EAI_AGAIN;
  }
  if (enqueue_result == 2) {
    NET_DIAG_ADD(resolver_jobs_coalesced, 1);
    resolver_job_destroy(job, 0);
    return resolver_wait_for_coalesced_query(
      node, service, hints, hints != NULL, deadline_ns, result_out);
  }

  mutexLock(&job->lock);
  while (!job->done) {
    const uint64_t current = armTicksToNs(armGetSystemTick());
    if (current >= deadline_ns) break;
    (void)condvarWaitTimeout(&job->cond, &job->lock, deadline_ns - current);
  }
  if (!job->done) {
    job->abandoned = 1;
    NET_DIAG_ADD(resolver_jobs_abandoned, 1);
    mutexUnlock(&job->lock);
    return EAI_AGAIN;
  }
  const int rc = job->rc;
  *result_out = job->result;
  job->result = NULL;
  mutexUnlock(&job->lock);
  resolver_job_destroy(job, 0);
  return rc;
}

static uint64_t resolver_effective_deadline(uint64_t requested_deadline_ns) {
  const uint64_t now = armTicksToNs(armGetSystemTick());
  const uint64_t default_deadline =
    now > UINT64_MAX - RESOLVER_DEFAULT_TIMEOUT_NS
      ? UINT64_MAX : now + RESOLVER_DEFAULT_TIMEOUT_NS;
  if (requested_deadline_ns && requested_deadline_ns < default_deadline)
    return requested_deadline_ns;
  NET_DIAG_ADD(resolver_default_deadlines, 1);
  return default_deadline;
}

int getaddrinfo_fake(const char *node, const char *svc, const void *hints, void **res) {
  NET_DIAG_ADD(dns_calls, 1);
  if (!res) {
    NET_DIAG_ADD(dns_failures, 1);
    NET_DIAG_STORE(last_dns_error, EAI_FAIL);
    return EAI_FAIL;
  }
  *res = NULL;
  if (!g_net_on) {
    NET_DIAG_ADD(dns_failures, 1);
    NET_DIAG_STORE(last_dns_error, EAI_AGAIN);
    return EAI_AGAIN;
  }
  struct addrinfo nh, *nhp = NULL; const struct b_addrinfo *bh = (const struct b_addrinfo *)hints;
  if (bh) {
    memset(&nh, 0, sizeof nh);
    if (ai_flags_b2n(bh->ai_flags, &nh.ai_flags) < 0) {
      NET_DIAG_ADD(dns_failures, 1);
      NET_DIAG_STORE(last_dns_error, EAI_BADFLAGS);
      return EAI_BADFLAGS;
    }
    nh.ai_family = af_b2n(bh->ai_family);
    nh.ai_socktype = bh->ai_socktype;
    nh.ai_protocol = bh->ai_protocol;
    nhp = &nh;
  }
  /* Once Horizon has proved AF_INET6 unavailable, asking the native resolver
   * for AAAA candidates can only add latency and resolver-lock contention.
   * Preserve explicit family requests, but narrow ordinary AF_UNSPEC queries
   * before they enter the bounded worker pool. */
  if (__atomic_load_n(&g_ipv6_socket_state, __ATOMIC_RELAXED) < 0 &&
      (!nhp || nh.ai_family == AF_UNSPEC)) {
    if (!nhp) memset(&nh, 0, sizeof nh);
    nh.ai_family = AF_INET;
    nhp = &nh;
  }
  struct addrinfo *nres = NULL;
  const uint64_t deadline_ns =
    resolver_effective_deadline(g_resolver_alarm_deadline_ns);
  int rc = getaddrinfo_with_deadline(node, svc, nhp, deadline_ns, &nres);
  if (rc != 0) {
    NET_DIAG_ADD(dns_failures, 1);
    NET_DIAG_STORE(last_dns_error, rc);
    return rc;
  }
  struct b_addrinfo *head = NULL, *tail = NULL;
  int allocation_failed = 0;
  int flags_invalid = 0;
  for (struct addrinfo *p = nres; p; p = p->ai_next) {
    if (p->ai_family == AF_INET6 &&
        __atomic_load_n(&g_ipv6_socket_state, __ATOMIC_RELAXED) < 0) {
      NET_DIAG_ADD(ipv6_results_filtered, 1);
      continue;
    }
    int bionic_flags;
    if (ai_flags_n2b(p->ai_flags, &bionic_flags) < 0) {
      flags_invalid = 1;
      break;
    }
    struct b_addrinfo *b = (struct b_addrinfo *)calloc(1, sizeof *b);
    if (!b) { allocation_failed = 1; break; }
    b->ai_flags = bionic_flags; b->ai_family = af_n2b(p->ai_family);
    b->ai_socktype = p->ai_socktype; b->ai_protocol = p->ai_protocol;
    if (p->ai_addr && p->ai_addrlen) { unsigned char *ba = (unsigned char *)calloc(1, p->ai_addrlen); unsigned bl = p->ai_addrlen;
      if (!ba) { free(b); allocation_failed = 1; break; }
      sa_n2b((const unsigned char *)p->ai_addr, p->ai_addrlen, ba, &bl); b->ai_addr = ba; b->ai_addrlen = bl; }
    if (p->ai_canonname) {
      b->ai_canonname = strdup(p->ai_canonname);
      if (!b->ai_canonname) { free(b->ai_addr); free(b); allocation_failed = 1; break; }
    }
    if (!head) head = b; else tail->ai_next = b; tail = b;
  }
  resolver_addrinfo_free(nres);
  if (allocation_failed) {
    freeaddrinfo_fake(head);
    NET_DIAG_ADD(dns_failures, 1);
    NET_DIAG_STORE(last_dns_error, EAI_MEMORY);
    return EAI_MEMORY;
  }
  if (flags_invalid) {
    freeaddrinfo_fake(head);
    NET_DIAG_ADD(dns_failures, 1);
    NET_DIAG_STORE(last_dns_error, EAI_BADFLAGS);
    return EAI_BADFLAGS;
  }
  *res = head;
  if (!head) {
    NET_DIAG_ADD(dns_failures, 1);
    NET_DIAG_STORE(last_dns_error, EAI_AGAIN);
    return EAI_AGAIN;
  }
  NET_DIAG_ADD(dns_successes, 1);
  NET_DIAG_STORE(last_dns_error, 0);
  return 0;
}
void freeaddrinfo_fake(void *res) {
  struct b_addrinfo *p = (struct b_addrinfo *)res;
  while (p) { struct b_addrinfo *n = p->ai_next; free(p->ai_addr); free(p->ai_canonname); free(p); p = n; }
}

static int file_status_n2b(int native_flags) {
  int guest_flags = native_flags & O_ACCMODE;
  if (native_flags & O_APPEND) guest_flags |= LINUX_O_APPEND;
  if (native_flags & O_NONBLOCK) guest_flags |= LINUX_O_NONBLOCK;
  if (native_flags & O_SYNC) guest_flags |= LINUX_O_SYNC;
#ifdef O_DIRECT
  if (native_flags & O_DIRECT) guest_flags |= LINUX_O_DIRECT;
#endif
#ifdef FASYNC
  if (native_flags & FASYNC) guest_flags |= LINUX_O_ASYNC;
#endif
  return guest_flags;
}

static int file_status_b2n(int current, int guest_flags) {
  int mutable = O_APPEND | O_NONBLOCK;
#ifdef O_DIRECT
  mutable |= O_DIRECT;
#endif
#ifdef FASYNC
  mutable |= FASYNC;
#endif
  int native_flags = current & ~mutable;
  if (guest_flags & LINUX_O_APPEND) native_flags |= O_APPEND;
  if (guest_flags & LINUX_O_NONBLOCK) native_flags |= O_NONBLOCK;
#ifdef O_DIRECT
  if (guest_flags & LINUX_O_DIRECT) native_flags |= O_DIRECT;
#endif
#ifdef FASYNC
  if (guest_flags & LINUX_O_ASYNC) native_flags |= FASYNC;
#endif
  return native_flags;
}

/* Translate bionic descriptor commands and status-flag bit assignments. */
static int fcntl_shim_backend(int fd, int cmd, intptr_t arg) {
  if (fakefd_is_fake(fd)) return fakefd_fcntl(fd, cmd, arg);
  if (asset_pack_fd_is(fd)) {
    if (cmd == 0 /*F_DUPFD*/ || cmd == 1030 /*F_DUPFD_CLOEXEC*/) {
      if (arg < 0 || arg > INT_MAX) { errno = EINVAL; return -1; }
      int r = asset_pack_dup_fd_min(fd, (int)arg, cmd == 1030);
      if (r < 0) NET_FAIL();
      else fd_metadata_copy(fd, r);
      return r;
    }
    if (cmd == 3 /*F_GETFL*/) return O_RDONLY;
    if (cmd == 4 /*F_SETFL*/) return 0;
    if (cmd == 1 /*F_GETFD*/) {
      int r = fcntl(fd, F_GETFD, 0);
      if (r < 0) NET_FAIL();
      return r;
    }
    if (cmd == 2 /*F_SETFD*/) {
      int r = fcntl(fd, F_SETFD, ((int)arg & 1) ? FD_CLOEXEC : 0);
      if (r < 0) NET_FAIL();
      return r;
    }
    errno = EINVAL;
    return -1;
  }
  if (cmd == 3 /*F_GETFL*/) {
    int fl = fcntl(fd, F_GETFL, 0); if (fl < 0) { NET_FAIL(); return -1; }
    return file_status_n2b(fl);
  }
  if (cmd == 4 /*F_SETFL*/) {
    int current = fcntl(fd, F_GETFL, 0);
    if (current < 0) { NET_FAIL(); return -1; }
    int r = fcntl(fd, F_SETFL, file_status_b2n(current, (int)arg));
    if (r < 0) NET_FAIL();
    return r;
  }
  if (cmd == 1 /*F_GETFD*/) {
    int r = fcntl(fd, F_GETFD, 0); if (r < 0) NET_FAIL(); return r;
  }
  if (cmd == 2 /*F_SETFD*/) {
    int r = fcntl(fd, F_SETFD, ((int)arg & 1) ? FD_CLOEXEC : 0);
    if (r < 0) NET_FAIL();
    return r;
  }
  if (cmd == 1030 /* Bionic F_DUPFD_CLOEXEC */) {
    if (arg < 0 || arg > INT_MAX) { errno = EINVAL; return -1; }
    if (ra_flush_detach(fd) < 0) return -1;
    int r = fcntl(fd, F_DUPFD_CLOEXEC, (int)arg);
    if (r < 0) NET_FAIL();
    else {
      fd_metadata_copy(fd, r);
      network_track_duplicate(fd, r);
    }
    return r;
  }
  /* The remaining Bionic commands do not share newlib's command numbering.
     Failing closed is safer than accidentally treating a record-lock request as
     ownership control.  Add an explicit ABI translation before enabling one. */
  if (cmd >= 5) { errno = 38; return -1; } /* Bionic ENOSYS. */
  if (cmd == 0 /*F_DUPFD*/) {
    if (arg < 0 || arg > INT_MAX) { errno = EINVAL; return -1; }
    if (ra_flush_detach(fd) < 0) return -1;
  }
  int r = fcntl(fd, cmd, (int)arg);
  if (r < 0) NET_FAIL();
  else if (cmd == 0 /*F_DUPFD*/) {
    fd_metadata_copy(fd, r);
    network_track_duplicate(fd, r);
  }
  return r;
}
int fcntl_shim(int fd, int cmd, intptr_t arg) {
  uint32_t stripe;
  if (!nx_fd_route_source_lock(fd, &stripe)) return -1;
  int result = fcntl_shim_backend(fd, cmd, arg);
  int saved = errno;
  nx_fd_route_source_unlock(stripe);
  errno = saved;
  return result;
}
int getnameinfo_fake(const void *a, unsigned al, char *h, unsigned hl, char *s, unsigned sl, int f) {
  const uint32_t flags = (uint32_t)f;
  const uint32_t known = B_NI_NOFQDN | B_NI_NUMERICHOST | B_NI_NAMEREQD |
                         B_NI_NUMERICSERV | B_NI_DGRAM;
  if (flags & ~known) return EAI_BADFLAGS;
  /* Reverse PTR resolution is not required for game traffic and can serialize
   * every guest DNS user behind newlib's legacy resolver for over a minute.
   * Honor explicit numeric requests and make ordinary presentation numeric;
   * an explicit name-required request must fail rather than freeze the game. */
  if (h && (flags & B_NI_NAMEREQD)) return EAI_NONAME;
  int native_flags = 0;
  if (flags & B_NI_NOFQDN)      native_flags |= NI_NOFQDN;
  if (h) {
    native_flags |= NI_NUMERICHOST;
    if (!(flags & B_NI_NUMERICHOST))
      NET_DIAG_ADD(reverse_numeric_results, 1);
  }
  if (flags & B_NI_NUMERICSERV) native_flags |= NI_NUMERICSERV;
  if (flags & B_NI_DGRAM)       native_flags |= NI_DGRAM;
  unsigned char nsa[128]; unsigned nl = sa_b2n(a, al, nsa);
  return getnameinfo((const struct sockaddr *)nsa, nl, h, hl, s, sl,
                     native_flags);
}
int ioctl_fake(int fd, unsigned long request, ...) {
  va_list va;
  va_start(va, request);
  void *arg = va_arg(va, void *);
  va_end(va);
  if (fakefd_is_fake(fd)) return fakefd_ioctl(fd, request, arg);
  switch (request) {
    case 0x5421ul: { /* Linux/Android FIONBIO */
      int result = ioctl(fd, FIONBIO, arg);
      if (result < 0) NET_FAIL();

      return result;
    }
    case 0x541bul: /* Linux/Android FIONREAD */
      {
        int result = ioctl(fd, FIONREAD, arg);
        if (result < 0) NET_FAIL();
        return result;
      }
    default:
      errno = ENOTTY;
      return -1;
  }
}
int gethostname_fake(char *name, size_t len) { if (name && len) snprintf(name, len, "switch"); return 0; }
unsigned if_nametoindex_fake(const char *n) {
  if (!n) return 0;
  return (!strcmp(n, "wlan0") || !strcmp(n, "eth0") || !strcmp(n, "lo")) ? 1u : 0u;
}
int getpid_fake(void) { return 1; }
int kill_fake(int pid, int sig) {
  if (sig < 0 || sig >= 65) { errno = EINVAL; return -1; }
  if (pid != getpid_fake()) { errno = ESRCH; return -1; }
  if (sig == 0) return 0;
  switch (sig) {
    case 2:  /* SIGINT */
    case 3:  /* SIGQUIT */
    case 4:  /* SIGILL */
    case 6:  /* SIGABRT */
    case 7:  /* SIGBUS */
    case 8:  /* SIGFPE */
    case 9:  /* SIGKILL */
    case 11: /* SIGSEGV */
    case 15: /* SIGTERM */
      abort();
    default:
      errno = 38; /* Bionic ENOSYS: signal delivery is unavailable. */
      return -1;
  }
}
int sched_yield_fake(void) { svcSleepThread(0); return 0; }
static int64_t days_from_civil(int64_t year, unsigned month, unsigned day) {
  year -= month <= 2;
  const int64_t era = (year >= 0 ? year : year - 399) / 400;
  const unsigned yoe = (unsigned)(year - era * 400);
  const unsigned adjusted_month = month > 2 ? month - 3u : month + 9u;
  const unsigned doy = (153u * adjusted_month + 2u) / 5u + day - 1u;
  const unsigned doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;
  return era * 146097 + (int64_t)doe - 719468;
}
time_t timegm_fake(struct tm *value) {
  if (!value) { errno = EINVAL; return (time_t)-1; }
  int64_t year = (int64_t)value->tm_year + 1900;
  int month_index = value->tm_mon;
  year += month_index / 12;
  month_index %= 12;
  if (month_index < 0) { month_index += 12; --year; }
  const int64_t days = days_from_civil(year, (unsigned)month_index + 1u, 1u) +
                       (int64_t)value->tm_mday - 1;
  const int64_t seconds = days * 86400 + (int64_t)value->tm_hour * 3600 +
                          (int64_t)value->tm_min * 60 + value->tm_sec;
  return (time_t)seconds;
}
/* Bionic passwd layout. */
struct bionic_passwd {
  char *pw_name;     /* 0x00 */
  char *pw_passwd;   /* 0x08 */
  uint32_t pw_uid;   /* 0x10 */
  uint32_t pw_gid;   /* 0x14 */
  char *pw_gecos;    /* 0x18 */
  char *pw_dir;      /* 0x20 */
  char *pw_shell;    /* 0x28 */
};
void *getpwuid_fake(int uid) {
  (void)uid;
  static struct bionic_passwd pw;
  static char nm[] = "switch", dir[] = GAME_HOME, sh[] = "/bin/sh", empty[] = "";
  pw.pw_name = nm; pw.pw_passwd = empty; pw.pw_uid = 0; pw.pw_gid = 0;
  pw.pw_gecos = empty; pw.pw_dir = dir; pw.pw_shell = sh;
  return &pw;
}
int getpwuid_r_fake(int uid, void *pwd_out, char *buffer, size_t buffer_len,
                    void **result_out) {
  struct bionic_passwd *pwd = (struct bionic_passwd *)pwd_out;
  static const char name[] = "switch";
  static const char dir[] = GAME_HOME;
  static const char shell[] = "/bin/sh";
  const size_t name_n = sizeof name, empty_n = 1, dir_n = sizeof dir, shell_n = sizeof shell;
  const size_t need = name_n + empty_n + empty_n + dir_n + shell_n;
  if (result_out) *result_out = NULL;
  if (!pwd || !buffer) return EINVAL;
  if (buffer_len < need) return ERANGE;
  char *p = buffer;
  memcpy(p, name, name_n); pwd->pw_name = p; p += name_n;
  *p = '\0'; pwd->pw_passwd = p++;
  *p = '\0'; pwd->pw_gecos = p++;
  memcpy(p, dir, dir_n); pwd->pw_dir = p; p += dir_n;
  memcpy(p, shell, shell_n); pwd->pw_shell = p;
  pwd->pw_uid = (uint32_t)uid;
  pwd->pw_gid = 0;
  if (result_out) *result_out = pwd;
  return 0;
}

const char *managed_path(const char *p) {
  if (!p) return p;
  const char *c = strchr(p, ':');
  return (c && c[1] == '/') ? c + 1 : p;     // "sdmc:/switch/.." -> "/switch/.."
}
char *getenv_fake(const char *name) {
  if (name) {
    if (!strcmp(name, "HOME"))   return (char *)managed_path(GAME_HOME);
    if (!strcmp(name, "TMPDIR")) return (char *)managed_path(GAME_HOME);
    if (!strcmp(name, "SSL_CERT_FILE") || !strcmp(name, "CURL_CA_BUNDLE"))
      return (char *)CA_BUNDLE_PATH;
    if (!strcmp(name, "SSL_CERT_DIR")) return (char *)GAME_HOME "/certs";
  }
  return getenv(name);
}
/* Managed paths use Unix roots without devoptab prefixes. */
char *getcwd_fake(char *buf, size_t size) {
  char *r = getcwd(buf, size);
  if (!r) return r;
  const char *c = strchr(r, ':');
  if (c && c[1] == '/') memmove(r, c + 1, strlen(c + 1) + 1);  // drop "sdmc:"
  return r;
}

void *dlopen_fake(const char *name, int flags) {
  return plugin_loader_dlopen(name, flags);
}
int dlclose_fake(void *handle) { return plugin_loader_dlclose(handle); }
const char *dlerror_fake(void) { return plugin_loader_dlerror(); }
void *dlsym_fake(void *handle, const char *symbol) {
  return plugin_loader_dlsym(handle, symbol);
}

typedef struct { RwLock lock; } FakeRwLock;
static Mutex rwlock_storage_lock;

static FakeRwLock *load_rwlock_storage(const void *storage) {
  FakeRwLock *lock;
  memcpy(&lock, storage, sizeof(lock));
  return lock;
}

static void store_rwlock_storage(void *storage, FakeRwLock *lock) {
  memcpy(storage, &lock, sizeof(lock));
}

/* Bionic statically initializes rwlocks to zero.  Materialize the libnx
 * object once.  Bionic only promises 32-bit alignment for pthread_rwlock_t,
 * so serialize and memcpy the pointer instead of issuing an unaligned 64-bit
 * atomic operation. */
static int materialize_rwlock(void **storage, FakeRwLock **result) {
  if (!storage || !result) return EINVAL;
  mutexLock(&rwlock_storage_lock);
  FakeRwLock *lock = load_rwlock_storage(storage);
  if (!lock) {
    lock = calloc(1, sizeof(*lock));
    if (!lock) {
      mutexUnlock(&rwlock_storage_lock);
      return ENOMEM;
    }
    rwlockInit(&lock->lock);
    store_rwlock_storage(storage, lock);
  }
  mutexUnlock(&rwlock_storage_lock);
  *result = lock;
  return 0;
}

int pthread_rwlock_init_fake(void **rw, const void *attr) {
  (void)attr;
  if (!rw) return EINVAL;
  FakeRwLock *lock = calloc(1, sizeof(*lock));
  if (!lock) return ENOMEM;
  rwlockInit(&lock->lock);
  /* Reinitializing a live POSIX rwlock is undefined.  Do not inspect the
   * guest's pre-init bytes: heap-allocated bionic objects need not be zeroed. */
  mutexLock(&rwlock_storage_lock);
  store_rwlock_storage(rw, lock);
  mutexUnlock(&rwlock_storage_lock);
  return 0;
}

int pthread_rwlock_destroy_fake(void **rw) {
  if (!rw) return EINVAL;
  mutexLock(&rwlock_storage_lock);
  FakeRwLock *lock = load_rwlock_storage(rw);
  store_rwlock_storage(rw, NULL);
  mutexUnlock(&rwlock_storage_lock);
  free(lock);
  return 0;
}

int pthread_rwlock_rdlock_fake(void **rw) {
  FakeRwLock *lock;
  const int result = materialize_rwlock(rw, &lock);
  if (result) return result;
  rwlockReadLock(&lock->lock);
  return 0;
}

int pthread_rwlock_wrlock_fake(void **rw) {
  FakeRwLock *lock;
  const int result = materialize_rwlock(rw, &lock);
  if (result) return result;
  rwlockWriteLock(&lock->lock);
  return 0;
}

int pthread_rwlock_unlock_fake(void **rw) {
  if (!rw) return EINVAL;
  mutexLock(&rwlock_storage_lock);
  FakeRwLock *l = load_rwlock_storage(rw);
  mutexUnlock(&rwlock_storage_lock);
  if (!l) return EINVAL;
  if (rwlockIsWriteLockHeldByCurrentThread(&l->lock)) rwlockWriteUnlock(&l->lock);
  else rwlockReadUnlock(&l->lock);
  return 0;
}

/* arm64 Bionic sem_t is a 16-byte inline object, not a pointer.  Its first
 * word stores a signed 31-bit value shifted left by one plus the shared bit;
 * -1 denotes contention.  A process-wide condition variable is sufficient
 * here because every waiter rechecks its own count under the same lock. */
typedef struct {
  uint32_t count;
  int32_t reserved[3];
} BionicSem;
_Static_assert(sizeof(BionicSem)==16,"arm64 Bionic sem_t must be 16 bytes");
_Static_assert(offsetof(BionicSem,count)==0,"Bionic sem_t count offset");
#define BIONIC_SEM_SHARED     1u
#define BIONIC_SEM_VALUE_MASK UINT32_C(0xfffffffe)
#define BIONIC_SEM_VALUE_MAX UINT32_C(0x3fffffff)
static Mutex g_sem_lock;
static CondVar g_sem_cond;

static int bionic_sem_value(const BionicSem *sem){
  return (int32_t)sem->count>>1;
}
static uint32_t bionic_sem_bits(int value){
  return ((uint32_t)value<<1)&BIONIC_SEM_VALUE_MASK;
}
static void bionic_sem_store(BionicSem *sem,int value){
  sem->count=bionic_sem_bits(value)|(sem->count&BIONIC_SEM_SHARED);
}

int sem_init_fake(void *storage,int pshared,unsigned int value){
  if(!storage||value>BIONIC_SEM_VALUE_MAX){errno=EINVAL;return -1;}
  BionicSem *sem=storage;
  mutexLock(&g_sem_lock);
  sem->count=bionic_sem_bits((int)value)|
             (pshared?BIONIC_SEM_SHARED:0);
  memset(sem->reserved,0,sizeof(sem->reserved));
  mutexUnlock(&g_sem_lock);
  return 0;
}

int sem_destroy_fake(void *storage){
  if(!storage){errno=EINVAL;return -1;}
  /* Bionic sem_destroy is intentionally a no-op for a valid inline object. */
  return 0;
}

int sem_post_fake(void *storage){
  if(!storage){errno=EINVAL;return -1;}
  BionicSem *sem=storage;
  mutexLock(&g_sem_lock);
  int value=bionic_sem_value(sem);
  if(value==BIONIC_SEM_VALUE_MAX){mutexUnlock(&g_sem_lock);errno=BIONIC_EOVERFLOW;return -1;}
  bionic_sem_store(sem,value<0?1:value+1);
  if(value<0)condvarWakeAll(&g_sem_cond);
  mutexUnlock(&g_sem_lock);
  return 0;
}

int sem_wait_fake(void *storage){
  if(!storage){errno=EINVAL;return -1;}
  BionicSem *sem=storage;
  mutexLock(&g_sem_lock);
  for(;;){
    int value=bionic_sem_value(sem);
    if(value>0){bionic_sem_store(sem,value-1);mutexUnlock(&g_sem_lock);return 0;}
    if(value==0)bionic_sem_store(sem,-1);
    condvarWait(&g_sem_cond,&g_sem_lock);
  }
}

int sem_trywait_fake(void *storage){
  if(!storage){errno=EINVAL;return -1;}
  BionicSem *sem=storage;
  mutexLock(&g_sem_lock);
  int value=bionic_sem_value(sem);
  if(value<=0){mutexUnlock(&g_sem_lock);errno=EAGAIN;return -1;}
  bionic_sem_store(sem,value-1);
  mutexUnlock(&g_sem_lock);
  return 0;
}

int sem_getvalue_fake(void *storage,int *value_out){
  if(!storage||!value_out){errno=EINVAL;return -1;}
  mutexLock(&g_sem_lock);
  int value=bionic_sem_value((const BionicSem*)storage);
  *value_out=value<0?0:value;
  mutexUnlock(&g_sem_lock);
  return 0;
}

static int sem_realtime_remaining(const struct timespec *absolute,
                                  uint64_t *remaining_ns){
  if(!absolute||absolute->tv_sec<0||absolute->tv_nsec<0||
     absolute->tv_nsec>=1000000000L){errno=EINVAL;return -1;}
  struct timespec now;
  if(clock_gettime(CLOCK_REALTIME,&now)!=0)return -1;
  if(now.tv_sec>absolute->tv_sec||
     (now.tv_sec==absolute->tv_sec&&now.tv_nsec>=absolute->tv_nsec)){
    *remaining_ns=0;return 0;
  }
  uint64_t seconds=(uint64_t)(absolute->tv_sec-now.tv_sec);
  int64_t nanoseconds=(int64_t)absolute->tv_nsec-(int64_t)now.tv_nsec;
  if(nanoseconds<0){seconds--;nanoseconds+=1000000000L;}
  if(seconds>UINT64_MAX/UINT64_C(1000000000)){
    *remaining_ns=UINT64_MAX;
  }else{
    uint64_t base=seconds*UINT64_C(1000000000);
    *remaining_ns=(uint64_t)nanoseconds>UINT64_MAX-base
      ?UINT64_MAX:base+(uint64_t)nanoseconds;
  }
  return 1;
}

int sem_timedwait_fake(void *storage,const struct timespec *absolute){
  if(!storage){errno=EINVAL;return -1;}
  BionicSem *sem=storage;
  mutexLock(&g_sem_lock);
  /* POSIX/Bionic requires an immediately available token to win even when the
   * supplied absolute timeout would otherwise be invalid. */
  int value=bionic_sem_value(sem);
  if(value>0){bionic_sem_store(sem,value-1);
    mutexUnlock(&g_sem_lock);return 0;}
  for(;;){
    value=bionic_sem_value(sem);
    if(value>0){bionic_sem_store(sem,value-1);
      mutexUnlock(&g_sem_lock);return 0;}
    if(value==0)bionic_sem_store(sem,-1);
    uint64_t remaining=0;
    int state=sem_realtime_remaining(absolute,&remaining);
    if(state<=0){mutexUnlock(&g_sem_lock);if(state==0)errno=BIONIC_ETIMEDOUT;return -1;}
    const uint64_t slice=remaining<UINT64_C(1000000000)
      ?remaining:UINT64_C(1000000000);
    (void)condvarWaitTimeout(&g_sem_cond,&g_sem_lock,slice);
  }
}

/* Genshin merges a customized Unity 2017 IL2CPP runtime into libyuanshen.
 * These offsets were derived from the exact supported 7.0.1 image rather than
 * copied from the Subway/Unity-2022 wrapper.  Its signal-30 handler publishes
 * ucontext+0xb0 and SP into the per-thread record, increments suspend_ack,
 * waits for resume_flag, clears the context pointer, then increments
 * resume_ack.  Horizon has no POSIX signal delivery, so reproduce that
 * bookkeeping around libnx's pause/context/resume operations. */
uintptr_t g_il2cpp_base = 0;
size_t g_il2cpp_size = 0;
volatile uint32_t g_gc_bridge_suspends;
volatile uint32_t g_gc_bridge_resumes;
volatile uint32_t g_gc_bridge_failures;
volatile uint32_t g_gc_bridge_capture_retries;
volatile uint32_t g_gc_bridge_host_captures;
volatile uint32_t g_gc_bridge_pause_failures;
volatile uint32_t g_gc_bridge_dump_failures;
volatile uint32_t g_gc_bridge_snapshot_cycles;
volatile uint32_t g_gc_bridge_snapshot_failures;
volatile uint32_t g_gc_bridge_snapshot_threads;
volatile uint32_t g_gc_bridge_active_targets;
volatile uint32_t g_gc_bridge_worker_dependency_releases;
volatile uint32_t g_gc_bridge_worker_release_failures;
volatile uint32_t g_gc_bridge_signal_mask_deferrals;
volatile uint32_t g_gc_bridge_deferred_deliveries;
volatile uint32_t g_gc_bridge_mutex_dependency_releases;
volatile uint32_t g_gc_bridge_mutex_release_failures;

#define GI_GC_SIGNAL_RVA       UINT64_C(0x14df3af0)
#define GI_GC_SUSPEND_ACK_RVA  UINT64_C(0x14df3af8)
#define GI_GC_RESUME_ACK_RVA   UINT64_C(0x14df3afc)
#define GI_GC_RESUME_FLAG_RVA  UINT64_C(0x14df3b00)
#define GI_GC_THREADS_BEGIN_RVA UINT64_C(0x14df3b10)
#define GI_GC_THREADS_END_RVA   UINT64_C(0x14df3b18)
/* Pinned 7.0.1 GC service handshake. */
#define GI_GC_WORKER_ENTRY_RVA UINT64_C(0x044c8994)
#define GI_GC_REQUEST_RVA      UINT64_C(0x14ff4508)
#define GI_GC_COMPLETION_RVA   UINT64_C(0x14ff450c)
#define GI_GC_RECORD_CONTEXT_OFF 8u
#define GI_GC_RECORD_SP_OFF      32u
#define GI_GC_RECORD_ACTIVE_OFF  72u
#define GI_GC_MAX_THREAD_RECORDS 4096
#define GI_GC_MAX_SUSPENSIONS    256
#define GI_GC_CAPTURE_ATTEMPTS   64u
#define GI_GC_CAPTURE_RETRY_NS   UINT64_C(100000)
#define GI_GC_MUTEX_HANDOFF_ATTEMPTS 5000u
#define GI_GC_MUTEX_HANDOFF_RETRY_NS UINT64_C(100000)
#define GI_GC_SLOT_SIGNAL_PENDING 7
#define GI_GC_SLOT_DEFERRED_CAPTURE 8
#define GI_GC_SLOT_MUTEX_HANDOFF 9
#define GI_GC_MCONTEXT_OFF       0xb0u
#define GI_GC_MCONTEXT_BYTES     0x1120u
#define GI_GC_UCONTEXT_BYTES     (GI_GC_MCONTEXT_OFF + GI_GC_MCONTEXT_BYTES)
#define GI_GC_UC_GPRS_OFF        0xb8u
#define GI_GC_UC_SP_OFF          0x1b0u
#define GI_GC_UC_PC_OFF          0x1b8u
#define GI_GC_UC_PSTATE_OFF      0x1c0u

/* Analysis of the 6.7.0 client's conservative stack scanner established that
 * it queues exactly 0x1120 bytes starting at record->context.  That is Android arm64's
 * complete mcontext_t (including its 4 KiB reserved extension area), not only
 * the integer-register prefix.  Keeping a short 512-byte ucontext made every
 * collection scan about 4 KiB beyond each slot into the next suspension and
 * retain arbitrary wrapper pointers as managed roots. */
_Static_assert(GI_GC_UCONTEXT_BYTES == 0x11d0u,
               "pinned Android arm64 ucontext ABI");
_Static_assert(GI_GC_UC_PSTATE_OFF + sizeof(uint64_t) <=
                 GI_GC_UCONTEXT_BYTES,
               "GC register image stays inside ucontext");

_Static_assert(offsetof(ThreadContext, cpu_gprs) == 0x000,
               "libnx ThreadContext GPR ABI");
_Static_assert(offsetof(ThreadContext, fp) == 0x0e8,
               "libnx ThreadContext FP ABI");
_Static_assert(offsetof(ThreadContext, lr) == 0x0f0,
               "libnx ThreadContext LR ABI");
_Static_assert(offsetof(ThreadContext, sp) == 0x0f8,
               "libnx ThreadContext SP ABI");
_Static_assert(offsetof(ThreadContext, pc) == 0x100,
               "libnx ThreadContext PC ABI");
_Static_assert(offsetof(ThreadContext, psr) == 0x108,
               "libnx ThreadContext PSTATE ABI");
_Static_assert(offsetof(ThreadContext, tpidr) == 0x318,
               "libnx ThreadContext TPIDR_EL0 ABI");
_Static_assert(sizeof(ThreadContext) == 0x320,
               "libnx ThreadContext size ABI");

typedef struct {
  int active;
  int needs_resume;
  int gc_worker;
  int pending_delivery;
  pthread_t pthread;
  NxGuestThreadRef *thread_ref;
  Thread *thread;
  Handle handle;
  uintptr_t expected_thread_pointer;
  uint64_t signal_bit;
  void *guest_record;
  ThreadContext context;
  uint8_t ucontext[GI_GC_UCONTEXT_BYTES] __attribute__((aligned(16)));
} GcSuspension;

typedef struct {
  NxGuestThreadRef *thread_ref;
  pthread_t pthread;
  Thread *thread;
  Handle handle;
  uintptr_t guest_thread_pointer;
  uintptr_t guest_entry;
} GcThreadLease;

static Mutex gc_suspend_lock;
static GcSuspension gc_suspensions[GI_GC_MAX_SUSPENSIONS];
static GcThreadLease gc_cycle_threads[GI_GC_MAX_SUSPENSIONS];
static size_t gc_cycle_thread_count;
static uint64_t gc_cycle_generation;
static uint64_t gc_cycle_started_tick_ns;
static pthread_t gc_cycle_collector_pthread;
static Thread *gc_cycle_collector_thread;
static uintptr_t gc_cycle_collector_guest_thread_pointer;
static pthread_t gc_cycle_worker_pthread;
static Thread *gc_cycle_worker_thread;
static uintptr_t gc_cycle_worker_guest_thread_pointer;
static int gc_cycle_active;
static int gc_monitor_started;

/* Diagnostic thread snapshots use the same pause primitive as the emulated
 * signal-30 collector.  Serialize the short pause/dump/resume interval with
 * collection setup so neither path can accidentally resume the other's
 * target.  A live collection wins and diagnostics simply defer. */
int nx_gc_context_capture_begin(void) {
  /* A watchdog must never become another casualty of the condition it is
   * reporting.  If collection setup owns this mutex, defer this sample and
   * try again at the next threshold instead of waiting behind a paused
   * target. */
  if (!mutexTryLock(&gc_suspend_lock)) return 0;
  if (gc_cycle_active) {
    mutexUnlock(&gc_suspend_lock);
    return 0;
  }
  return 1;
}

void nx_gc_context_capture_end(void) {
  mutexUnlock(&gc_suspend_lock);
}

/* Called with gc_suspend_lock held and before the first target is paused.
 * Holding one strong reference to every live guest thread for the complete
 * collection cycle removes all registry-lock acquisition from the interval
 * in which arbitrary targets are stopped. */
static int gc_begin_cycle_locked(void) {
  if (gc_cycle_active) return 0;

  NxGuestThreadRef *refs[GI_GC_MAX_SUSPENSIONS];
  size_t count = 0;
  const int result = nx_guest_thread_snapshot(
    refs, GI_GC_MAX_SUSPENSIONS, &count);
  if (result != 0) {
    __atomic_add_fetch(&g_gc_bridge_snapshot_failures, 1, __ATOMIC_RELAXED);
    return result;
  }

  const pthread_t collector = pthread_self();
  memset(gc_cycle_threads, 0, sizeof(gc_cycle_threads));
  gc_cycle_collector_pthread = collector;
  gc_cycle_collector_thread = NULL;
  gc_cycle_collector_guest_thread_pointer = 0;
  gc_cycle_worker_pthread = (pthread_t)0;
  gc_cycle_worker_thread = NULL;
  gc_cycle_worker_guest_thread_pointer = 0;
  for (size_t i = 0; i < count; ++i) {
    gc_cycle_threads[i].thread_ref = refs[i];
    gc_cycle_threads[i].pthread = nx_guest_thread_pthread(refs[i]);
    gc_cycle_threads[i].thread = nx_guest_thread_native(refs[i]);
    gc_cycle_threads[i].handle = nx_guest_thread_handle(refs[i]);
    gc_cycle_threads[i].guest_thread_pointer =
      nx_guest_thread_pointer(refs[i]);
    gc_cycle_threads[i].guest_entry = nx_guest_thread_entry(refs[i]);
    if (gc_cycle_threads[i].pthread == collector) {
      gc_cycle_collector_thread = gc_cycle_threads[i].thread;
      gc_cycle_collector_guest_thread_pointer =
        gc_cycle_threads[i].guest_thread_pointer;
    }
    if (g_il2cpp_base && gc_cycle_threads[i].guest_entry ==
          g_il2cpp_base + GI_GC_WORKER_ENTRY_RVA) {
      gc_cycle_worker_pthread = gc_cycle_threads[i].pthread;
      gc_cycle_worker_thread = gc_cycle_threads[i].thread;
      gc_cycle_worker_guest_thread_pointer =
        gc_cycle_threads[i].guest_thread_pointer;
    }
  }
  gc_cycle_thread_count = count;
  gc_cycle_active = 1;
  ++gc_cycle_generation;
  if (!gc_cycle_generation) ++gc_cycle_generation;
  gc_cycle_started_tick_ns = armTicksToNs(armGetSystemTick());
  __atomic_add_fetch(&g_gc_bridge_snapshot_cycles, 1, __ATOMIC_RELAXED);
  __atomic_add_fetch(&g_gc_bridge_snapshot_threads, (uint32_t)count,
                     __ATOMIC_RELAXED);
  return 0;
}

/* Detach a cycle only after every target slot is clear.  The returned strong
 * references must be released after dropping gc_suspend_lock, and callers
 * guarantee that every successfully paused target has already resumed. */
static size_t gc_detach_idle_cycle_locked(
    NxGuestThreadRef **release_refs, size_t capacity) {
  if (!gc_cycle_active) return 0;
  for (unsigned i = 0; i < GI_GC_MAX_SUSPENSIONS; ++i)
    if (gc_suspensions[i].active) return 0;

  size_t count = gc_cycle_thread_count;
  if (count > capacity) count = capacity;
  for (size_t i = 0; i < count; ++i)
    release_refs[i] = gc_cycle_threads[i].thread_ref;
  memset(gc_cycle_threads, 0, sizeof(gc_cycle_threads));
  gc_cycle_thread_count = 0;
  gc_cycle_started_tick_ns = 0;
  gc_cycle_collector_pthread = (pthread_t)0;
  gc_cycle_collector_thread = NULL;
  gc_cycle_collector_guest_thread_pointer = 0;
  gc_cycle_worker_pthread = (pthread_t)0;
  gc_cycle_worker_thread = NULL;
  gc_cycle_worker_guest_thread_pointer = 0;
  gc_cycle_active = 0;
  return count;
}

static void gc_release_cycle_refs(NxGuestThreadRef **refs, size_t count) {
  for (size_t i = 0; i < count; ++i)
    nx_guest_thread_release(refs[i]);
}

/* Called with gc_suspend_lock held. */
static GcThreadLease *gc_find_cycle_thread_locked(pthread_t target) {
  if (!gc_cycle_active) return NULL;
  for (size_t i = 0; i < gc_cycle_thread_count; ++i)
    if (gc_cycle_threads[i].pthread == target) return &gc_cycle_threads[i];
  return NULL;
}

static int mapped_range(const void *address, size_t size, unsigned permissions) {
  if (!address || !size) return 0;
  const uintptr_t start = (uintptr_t)address;
  if (start > UINTPTR_MAX - size) return 0;
  MemoryInfo info;
  u32 page_info;
  if (R_FAILED(svcQueryMemory(&info, &page_info, start))) return 0;
  return start >= info.addr && start + size <= info.addr + info.size &&
         (info.perm & permissions) == permissions;
}

static int image_range(uintptr_t rva, size_t size) {
  return g_il2cpp_base && rva <= g_il2cpp_size &&
         size <= g_il2cpp_size - (size_t)rva;
}

static int gc_pc_in_guest_image(uintptr_t pc) {
  return g_il2cpp_base && pc >= g_il2cpp_base &&
         pc - g_il2cpp_base < g_il2cpp_size;
}

/* The GC controller can synchronously request a root-scan job after it has
 * stopped every registered thread.  On Android the runtime's private service
 * thread remains able to consume that job.  A blanket Horizon threadPause,
 * however, can hold the sole producer of completion while the controller is
 * blocked on completion -- the hardware-proven generation-31 deadlock.
 *
 * Keep ordinary stop-the-world semantics.  Only when the controller itself
 * performs the pinned completion futex wait do we resume the exact service
 * entry.  Its already-published ucontext remains a stable conservative
 * snapshot, and gc_release_slot later performs the signal-handler bookkeeping
 * without issuing a second kernel resume. */
static void gc_maybe_release_worker_dependency(
    volatile int32_t *address, int expected) {
  if (!g_il2cpp_base || expected != 0 ||
      !image_range(GI_GC_REQUEST_RVA, sizeof(uint32_t)) ||
      !image_range(GI_GC_COMPLETION_RVA, sizeof(uint32_t)) ||
      (uintptr_t)address != g_il2cpp_base + GI_GC_COMPLETION_RVA)
    return;

  GcSuspension *worker_slot = NULL;
  Thread *worker_thread = NULL;
  pthread_t worker_pthread = (pthread_t)0;
  uintptr_t captured_pc = 0;
  uint64_t generation = 0;
  mutexLock(&gc_suspend_lock);
  if (gc_cycle_active &&
      gc_cycle_collector_pthread == pthread_self()) {
    for (unsigned i = 0; i < GI_GC_MAX_SUSPENSIONS; ++i) {
      GcSuspension *candidate = &gc_suspensions[i];
      if (candidate->active != 2 || !candidate->gc_worker ||
          !candidate->needs_resume || !candidate->thread)
        continue;
      candidate->active = 6; /* dependency release in progress */
      worker_slot = candidate;
      worker_thread = candidate->thread;
      worker_pthread = candidate->pthread;
      captured_pc = (uintptr_t)candidate->context.pc.x;
      generation = gc_cycle_generation;
      break;
    }
  }
  mutexUnlock(&gc_suspend_lock);
  if (!worker_slot) return;

  const uint32_t request = __atomic_load_n(
    (uint32_t *)(g_il2cpp_base + GI_GC_REQUEST_RVA), __ATOMIC_ACQUIRE);
  const uint32_t completion = __atomic_load_n(
    (uint32_t *)(g_il2cpp_base + GI_GC_COMPLETION_RVA), __ATOMIC_ACQUIRE);
  const Result resume_result = threadResume(worker_thread);
  mutexLock(&gc_suspend_lock);
  if (worker_slot->active == 6) {
    if (R_SUCCEEDED(resume_result)) worker_slot->needs_resume = 0;
    worker_slot->active = 2;
  }
  mutexUnlock(&gc_suspend_lock);

  if (R_SUCCEEDED(resume_result))
    __atomic_add_fetch(&g_gc_bridge_worker_dependency_releases, 1,
                       __ATOMIC_RELAXED);
  else
    __atomic_add_fetch(&g_gc_bridge_worker_release_failures, 1,
                       __ATOMIC_RELAXED);

  const int guest_pc = gc_pc_in_guest_image(captured_pc);
  const uintptr_t pc_value = guest_pc
    ? captured_pc - g_il2cpp_base : captured_pc;
  char message[384];
  snprintf(message, sizeof(message),
           "worker_dependency_release generation=%llu pthread=0x%llx request=%u completion=%u captured_pc=%c0x%llx resume=0x%x",
           (unsigned long long)generation,
           (unsigned long long)(uintptr_t)worker_pthread,
           request, completion, guest_pc ? 'g' : 'h',
           (unsigned long long)pc_value, (unsigned)resume_result);
}

static int gc_layout_available(void) {
  return image_range(GI_GC_SIGNAL_RVA, sizeof(uint32_t)) &&
         image_range(GI_GC_SUSPEND_ACK_RVA, sizeof(uint32_t)) &&
         image_range(GI_GC_RESUME_ACK_RVA, sizeof(uint32_t)) &&
         image_range(GI_GC_RESUME_FLAG_RVA, sizeof(uint32_t)) &&
         image_range(GI_GC_THREADS_BEGIN_RVA, sizeof(uintptr_t)) &&
         image_range(GI_GC_THREADS_END_RVA, sizeof(uintptr_t)) &&
         image_range(GI_GC_WORKER_ENTRY_RVA, sizeof(uint32_t)) &&
         image_range(GI_GC_REQUEST_RVA, sizeof(uint32_t)) &&
         image_range(GI_GC_COMPLETION_RVA, sizeof(uint32_t));
}

static void gc_futex_wake(volatile uint32_t *address) {
  (void)futex_impl((volatile int32_t *)address, FUTEX_WAKE, INT_MAX, NULL,
                   FUTEX_BITSET_MATCH_ANY);
}

static void *gc_find_thread_record(pthread_t target) {
  if (!gc_layout_available()) return NULL;

  const uintptr_t begin =
    *(uintptr_t *)(g_il2cpp_base + GI_GC_THREADS_BEGIN_RVA);
  const uintptr_t end =
    *(uintptr_t *)(g_il2cpp_base + GI_GC_THREADS_END_RVA);
  if (!begin || !end || end < begin) return NULL;
  const uintptr_t bytes = end - begin;
  if (!bytes || bytes > GI_GC_MAX_THREAD_RECORDS * sizeof(uintptr_t) ||
      bytes % sizeof(uintptr_t) ||
      !mapped_range((const void *)begin, (size_t)bytes, Perm_R))
    return NULL;

  const size_t count = (size_t)(bytes / sizeof(uintptr_t));
  for (size_t i = 0; i < count; ++i) {
    uintptr_t record_address;
    memcpy(&record_address, (const void *)(begin + i * sizeof(uintptr_t)),
           sizeof(record_address));
    uint8_t *record = (uint8_t *)record_address;
    if (!mapped_range(record, GI_GC_RECORD_ACTIVE_OFF + sizeof(uint32_t), Perm_Rw))
      continue;
    pthread_t recorded;
    memcpy(&recorded, record, sizeof(recorded));
    uint32_t active;
    memcpy(&active, record + GI_GC_RECORD_ACTIVE_OFF, sizeof(active));
    if (recorded == target && active) return record;
  }
  return NULL;
}

static void gc_build_ucontext(GcSuspension *slot) {
  memset(slot->ucontext, 0, sizeof(slot->ucontext));
  for (unsigned i = 0; i < 29; ++i)
    memcpy(slot->ucontext + GI_GC_UC_GPRS_OFF + i * sizeof(uint64_t),
           &slot->context.cpu_gprs[i].x, sizeof(uint64_t));
  memcpy(slot->ucontext + GI_GC_UC_GPRS_OFF + 29 * sizeof(uint64_t),
         &slot->context.fp, sizeof(uint64_t));
  memcpy(slot->ucontext + GI_GC_UC_GPRS_OFF + 30 * sizeof(uint64_t),
         &slot->context.lr, sizeof(uint64_t));
  memcpy(slot->ucontext + GI_GC_UC_SP_OFF, &slot->context.sp, sizeof(uint64_t));
  memcpy(slot->ucontext + GI_GC_UC_PC_OFF, &slot->context.pc.x, sizeof(uint64_t));
  uint64_t pstate = slot->context.psr;
  memcpy(slot->ucontext + GI_GC_UC_PSTATE_OFF, &pstate, sizeof(uint64_t));
}

typedef enum {
  GI_GC_CAPTURE_COMMITTED = 0,
  GI_GC_CAPTURE_DEFERRED,
  GI_GC_CAPTURE_RETRY,
  GI_GC_CAPTURE_RESUME_PENDING,
} GcCaptureResult;

static int gc_signal_blocked(const GcSuspension *slot) {
  return slot->thread_ref &&
    (nx_guest_thread_gc_critical(slot->thread_ref) ||
     (slot->signal_bit &&
      (nx_guest_thread_signal_mask(slot->thread_ref) & slot->signal_bit) != 0));
}

/* Deliver one emulated signal-30 stop.  Android defers pthread-directed
 * signals while the target masks them; Unity uses that property around its
 * own GC locks.  A raw Horizon threadPause ignored the mask and could freeze a
 * lock owner while the private root scanner waited for the same mutex. */
static GcCaptureResult gc_capture_slot_once(GcSuspension *slot) {
  if (gc_signal_blocked(slot)) return GI_GC_CAPTURE_DEFERRED;

  const Result pause_result = threadPause(slot->thread);
  if (R_FAILED(pause_result)) {
    __atomic_add_fetch(&g_gc_bridge_pause_failures, 1, __ATOMIC_RELAXED);
    return GI_GC_CAPTURE_RETRY;
  }
  slot->needs_resume = 1;

  const Result dump_result = threadDumpContext(&slot->context, slot->thread);
  if (R_FAILED(dump_result))
    __atomic_add_fetch(&g_gc_bridge_dump_failures, 1, __ATOMIC_RELAXED);

  int deferred = 0;
  int accepted = 0;
  if (R_SUCCEEDED(dump_result)) {
    /* Close the race between the lock-free precheck and threadPause.  Once
     * paused, only this thread could change its mask, so this second load is a
     * stable delivery decision. */
    deferred = gc_signal_blocked(slot);
    const int different_tpidr = slot->expected_thread_pointer &&
      slot->context.tpidr != slot->expected_thread_pointer;
    const uintptr_t captured_pc = (uintptr_t)slot->context.pc.x;
    if (!deferred &&
        (!different_tpidr || !gc_pc_in_guest_image(captured_pc))) {
      if (different_tpidr)
        __atomic_add_fetch(&g_gc_bridge_host_captures, 1,
                           __ATOMIC_RELAXED);
      accepted = 1;
    }
  }

  if (!accepted) {
    const Result resume_result = threadResume(slot->thread);
    if (R_FAILED(resume_result)) {
      /* The monitor must first restore the target and then retry the pending
       * signal; pthread_kill has not published suspend_ack for this slot. */
      mutexLock(&gc_suspend_lock);
      slot->active = 4;
      mutexUnlock(&gc_suspend_lock);
      __atomic_add_fetch(&g_gc_bridge_failures, 1, __ATOMIC_RELAXED);
      return GI_GC_CAPTURE_RESUME_PENDING;
    }
    slot->needs_resume = 0;
    return deferred ? GI_GC_CAPTURE_DEFERRED : GI_GC_CAPTURE_RETRY;
  }

  gc_build_ucontext(slot);
  uint8_t *record = slot->guest_record;
  __atomic_store_n((uintptr_t *)(record + GI_GC_RECORD_CONTEXT_OFF),
                   (uintptr_t)(slot->ucontext + GI_GC_MCONTEXT_OFF),
                   __ATOMIC_RELEASE);
  __atomic_store_n((uintptr_t *)(record + GI_GC_RECORD_SP_OFF),
                   (uintptr_t)slot->context.sp, __ATOMIC_RELEASE);

  /* Publish monitor ownership before the acknowledgement can wake Unity's
   * collector and let it scan record->context. */
  mutexLock(&gc_suspend_lock);
  slot->pending_delivery = 0;
  slot->active = 2;
  mutexUnlock(&gc_suspend_lock);

  volatile uint32_t *ack =
    (volatile uint32_t *)(g_il2cpp_base + GI_GC_SUSPEND_ACK_RVA);
  const uint32_t old = __atomic_fetch_add(ack, 1, __ATOMIC_RELEASE);
  if (old == UINT32_MAX) gc_futex_wake(ack);
  __atomic_add_fetch(&g_gc_bridge_suspends, 1, __ATOMIC_RELAXED);
  return GI_GC_CAPTURE_COMMITTED;
}

static int gc_release_slot(GcSuspension *slot) {
  uint8_t *record = slot->guest_record;
  if (record && mapped_range(record, GI_GC_RECORD_CONTEXT_OFF + sizeof(uintptr_t), Perm_Rw))
    __atomic_store_n((uintptr_t *)(record + GI_GC_RECORD_CONTEXT_OFF), 0,
                     __ATOMIC_RELEASE);

  if (slot->needs_resume) {
    if (!slot->thread || R_FAILED(threadResume(slot->thread))) {
      /* Never acknowledge a target that is still paused.  resume_flag remains
       * set, so leave the slot retryable for the monitor's next pass. */
      mutexLock(&gc_suspend_lock);
      if (slot->active == 3) slot->active = 2;
      mutexUnlock(&gc_suspend_lock);
      return 0;
    }
    slot->needs_resume = 0;
  }

  __atomic_add_fetch(&g_gc_bridge_resumes, 1, __ATOMIC_RELAXED);
  /* The guest can start another stop as soon as it consumes resume_ack.  Make
   * the slot and, for the final target, the complete cycle snapshot reusable
   * before publishing that acknowledgement.  No registry reference is
   * released until every target is running again. */
  NxGuestThreadRef *release_refs[GI_GC_MAX_SUSPENSIONS];
  mutexLock(&gc_suspend_lock);
  memset(slot, 0, sizeof(*slot));
  __atomic_sub_fetch(&g_gc_bridge_active_targets, 1, __ATOMIC_RELAXED);
  const size_t release_count = gc_detach_idle_cycle_locked(
    release_refs, GI_GC_MAX_SUSPENSIONS);
  mutexUnlock(&gc_suspend_lock);
  gc_release_cycle_refs(release_refs, release_count);

  if (gc_layout_available()) {
    volatile uint32_t *ack =
      (volatile uint32_t *)(g_il2cpp_base + GI_GC_RESUME_ACK_RVA);
    const uint32_t old = __atomic_fetch_add(ack, 1, __ATOMIC_RELEASE);
    if (old == UINT32_MAX) gc_futex_wake(ack);
  }
  return 1;
}

/* A capture failure after threadPause must never strand the mutator.  These
 * slots have not published suspend_ack, so resume and retire them without
 * touching either of the guest collector counters. */
static int gc_abort_slot(GcSuspension *slot) {
  if (slot->needs_resume) {
    if (!slot->thread || R_FAILED(threadResume(slot->thread))) {
      mutexLock(&gc_suspend_lock);
      if (slot->active == 5) slot->active = 4;
      mutexUnlock(&gc_suspend_lock);
      return 0;
    }
    slot->needs_resume = 0;
  }
  if (slot->pending_delivery) {
    /* This was a rejected, never-acknowledged capture whose first resume
     * transiently failed.  Keep the pthread_kill request pending instead of
     * dropping it and leaving Unity's collector short one suspend_ack. */
    mutexLock(&gc_suspend_lock);
    if (slot->active == 5) slot->active = GI_GC_SLOT_SIGNAL_PENDING;
    mutexUnlock(&gc_suspend_lock);
    return 1;
  }
  NxGuestThreadRef *release_refs[GI_GC_MAX_SUSPENSIONS];
  mutexLock(&gc_suspend_lock);
  memset(slot, 0, sizeof(*slot));
  __atomic_sub_fetch(&g_gc_bridge_active_targets, 1, __ATOMIC_RELAXED);
  const size_t release_count = gc_detach_idle_cycle_locked(
    release_refs, GI_GC_MAX_SUSPENSIONS);
  mutexUnlock(&gc_suspend_lock);
  gc_release_cycle_refs(release_refs, release_count);
  return 1;
}

static void *gc_resume_monitor(void *opaque) {
  (void)opaque;
  volatile uint32_t *resume =
    (volatile uint32_t *)(g_il2cpp_base + GI_GC_RESUME_FLAG_RVA);
  for (;;) {
    GcSuspension *aborted = NULL;
    mutexLock(&gc_suspend_lock);
    for (unsigned i = 0; i < GI_GC_MAX_SUSPENSIONS; ++i) {
      if (gc_suspensions[i].active == 4) {
        gc_suspensions[i].active = 5;
        aborted = &gc_suspensions[i];
        break;
      }
    }
    mutexUnlock(&gc_suspend_lock);
    if (aborted) {
      (void)gc_abort_slot(aborted);
      svcSleepThread(UINT64_C(1000000));
      continue;
    }

    /* pthread_kill returns successfully when a target blocks the signal; the
     * signal remains pending.  Deliver one newly unmasked stop per monitor
     * pass, matching that POSIX behavior without ever pausing a protected GC
     * lock owner. */
    GcSuspension *pending = NULL;
    mutexLock(&gc_suspend_lock);
    for (unsigned i = 0; i < GI_GC_MAX_SUSPENSIONS; ++i) {
      if (gc_suspensions[i].active == GI_GC_SLOT_SIGNAL_PENDING &&
          !gc_signal_blocked(&gc_suspensions[i])) {
        gc_suspensions[i].active = GI_GC_SLOT_DEFERRED_CAPTURE;
        pending = &gc_suspensions[i];
        break;
      }
    }
    mutexUnlock(&gc_suspend_lock);
    if (pending) {
      const GcCaptureResult capture_result = gc_capture_slot_once(pending);
      if (capture_result == GI_GC_CAPTURE_COMMITTED) {
        __atomic_add_fetch(&g_gc_bridge_deferred_deliveries, 1,
                           __ATOMIC_RELAXED);
      } else if (capture_result != GI_GC_CAPTURE_RESUME_PENDING) {
        mutexLock(&gc_suspend_lock);
        if (pending->active == GI_GC_SLOT_DEFERRED_CAPTURE)
          pending->active = GI_GC_SLOT_SIGNAL_PENDING;
        mutexUnlock(&gc_suspend_lock);
        if (capture_result == GI_GC_CAPTURE_DEFERRED)
          __atomic_add_fetch(&g_gc_bridge_signal_mask_deferrals, 1,
                             __ATOMIC_RELAXED);
        else
          __atomic_add_fetch(&g_gc_bridge_capture_retries, 1,
                             __ATOMIC_RELAXED);
      }
      svcSleepThread(UINT64_C(1000000));
      continue;
    }
    if (__atomic_load_n(resume, __ATOMIC_ACQUIRE)) {
      for (;;) {
        GcSuspension *ready = NULL;
        mutexLock(&gc_suspend_lock);
        for (unsigned i = 0; i < GI_GC_MAX_SUSPENSIONS; ++i) {
          if (gc_suspensions[i].active == 2) {
            gc_suspensions[i].active = 3;
            ready = &gc_suspensions[i];
            break;
          }
        }
        mutexUnlock(&gc_suspend_lock);
        if (!ready) break;
        if (!gc_release_slot(ready)) break;
      }
    }

    svcSleepThread(UINT64_C(1000000));
  }
  return NULL;
}

/* Called with gc_suspend_lock held. */
static int gc_ensure_monitor(void) {
  if (gc_monitor_started) return 0;
  pthread_t monitor;
  pthread_attr_t attr;
  int result = pthread_attr_init(&attr);
  if (!result) {
    result = pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    if (!result) result = pthread_create(&monitor, &attr, gc_resume_monitor, NULL);
    pthread_attr_destroy(&attr);
  }
  if (!result) gc_monitor_started = 1;
  return result;
}

int pthread_kill_gc(pthread_t t, int sig) {
  if (!t) return ESRCH;
  if (sig == 0) {
    NxGuestThreadRef *probe = nx_guest_thread_acquire(t);
    if (!probe) return ESRCH;
    const int live = nx_guest_thread_is_live(probe) &&
                     nx_guest_thread_native(probe) &&
                     nx_guest_thread_handle(probe) != INVALID_HANDLE;
    nx_guest_thread_release(probe);
    return live ? 0 : ESRCH;
  }
  if (!gc_layout_available()) return EINVAL;

  const uint32_t gc_signal =
    __atomic_load_n((uint32_t *)(g_il2cpp_base + GI_GC_SIGNAL_RVA),
                    __ATOMIC_ACQUIRE);
  if ((uint32_t)sig != gc_signal || !gc_signal) {
    /* Signal 10 is used only by the optional multi-thread stack-capture
     * reporter in this client.  Preserve its non-fatal behavior until that
     * diagnostic handler is ported; signal 30 below is the collector gate. */
    return 0;
  }

  void *record = gc_find_thread_record(t);
  if (!record) {
    __atomic_add_fetch(&g_gc_bridge_failures, 1, __ATOMIC_RELAXED);
    return ESRCH;
  }

  NxGuestThreadRef *release_refs[GI_GC_MAX_SUSPENSIONS];
  size_t release_count = 0;
  mutexLock(&gc_suspend_lock);
  GcSuspension *slot = NULL;
  for (unsigned i = 0; i < GI_GC_MAX_SUSPENSIONS; ++i) {
    if (gc_suspensions[i].active && gc_suspensions[i].pthread == t) {
      mutexUnlock(&gc_suspend_lock);
      return 0;
    }
    if (!slot && !gc_suspensions[i].active) slot = &gc_suspensions[i];
  }
  if (!slot) {
    mutexUnlock(&gc_suspend_lock);
    __atomic_add_fetch(&g_gc_bridge_failures, 1, __ATOMIC_RELAXED);
    return EAGAIN;
  }
  const int monitor_result = gc_ensure_monitor();
  if (monitor_result) {
    mutexUnlock(&gc_suspend_lock);
    __atomic_add_fetch(&g_gc_bridge_failures, 1, __ATOMIC_RELAXED);
    return monitor_result;
  }
  const int snapshot_result = gc_begin_cycle_locked();
  if (snapshot_result) {
    mutexUnlock(&gc_suspend_lock);
    __atomic_add_fetch(&g_gc_bridge_failures, 1, __ATOMIC_RELAXED);
    return snapshot_result;
  }
  GcThreadLease *lease = gc_find_cycle_thread_locked(t);
  if (!lease || !lease->thread || lease->handle == INVALID_HANDLE ||
      lease->pthread == pthread_self()) {
    const int error = lease && lease->pthread == pthread_self()
      ? EDEADLK : ESRCH;
    release_count = gc_detach_idle_cycle_locked(
      release_refs, GI_GC_MAX_SUSPENSIONS);
    mutexUnlock(&gc_suspend_lock);
    gc_release_cycle_refs(release_refs, release_count);
    __atomic_add_fetch(&g_gc_bridge_failures, 1, __ATOMIC_RELAXED);
    return error;
  }
  memset(slot, 0, sizeof(*slot));
  slot->active = 1;
  slot->pending_delivery = 1;
  slot->gc_worker = image_range(GI_GC_WORKER_ENTRY_RVA, sizeof(uint32_t)) &&
    lease->guest_entry == g_il2cpp_base + GI_GC_WORKER_ENTRY_RVA;
  slot->pthread = t;
  slot->thread_ref = lease->thread_ref;
  slot->thread = lease->thread;
  slot->handle = lease->handle;
  slot->expected_thread_pointer = lease->guest_thread_pointer;
  slot->signal_bit = (uint32_t)sig <= 64u
    ? UINT64_C(1) << ((uint32_t)sig - 1u) : 0;
  slot->guest_record = record;
  const int initially_blocked = gc_signal_blocked(slot);
  if (initially_blocked) slot->active = GI_GC_SLOT_SIGNAL_PENDING;
  __atomic_add_fetch(&g_gc_bridge_active_targets, 1, __ATOMIC_RELAXED);
  mutexUnlock(&gc_suspend_lock);

  if (initially_blocked) {
    __atomic_add_fetch(&g_gc_bridge_signal_mask_deferrals, 1,
                       __ATOMIC_RELAXED);
    return 0;
  }

  for (unsigned attempt = 0; attempt < GI_GC_CAPTURE_ATTEMPTS; ++attempt) {
    const GcCaptureResult capture_result = gc_capture_slot_once(slot);
    if (capture_result == GI_GC_CAPTURE_COMMITTED) return 0;
    if (capture_result == GI_GC_CAPTURE_DEFERRED) {
      mutexLock(&gc_suspend_lock);
      if (slot->active == 1) slot->active = GI_GC_SLOT_SIGNAL_PENDING;
      mutexUnlock(&gc_suspend_lock);
      __atomic_add_fetch(&g_gc_bridge_signal_mask_deferrals, 1,
                         __ATOMIC_RELAXED);
      return 0;
    }
    if (capture_result == GI_GC_CAPTURE_RESUME_PENDING) return 0;

    if (attempt + 1 < GI_GC_CAPTURE_ATTEMPTS) {
      __atomic_add_fetch(&g_gc_bridge_capture_retries, 1, __ATOMIC_RELAXED);
      svcSleepThread(GI_GC_CAPTURE_RETRY_NS);
    }
  }

  mutexLock(&gc_suspend_lock);
  memset(slot, 0, sizeof(*slot));
  __atomic_sub_fetch(&g_gc_bridge_active_targets, 1, __ATOMIC_RELAXED);
  release_count = gc_detach_idle_cycle_locked(
    release_refs, GI_GC_MAX_SUSPENSIONS);
  mutexUnlock(&gc_suspend_lock);
  gc_release_cycle_refs(release_refs, release_count);
  __atomic_add_fetch(&g_gc_bridge_failures, 1, __ATOMIC_RELAXED);
  return EAGAIN;
}
