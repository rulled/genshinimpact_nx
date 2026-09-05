/* Additional Bionic/Linux ABI required by Genshin's merged Unity player. */

#include <stdlib.h>
#include <malloc.h>
#include <string.h>
#include <errno.h>
#include <limits.h>
#include <setjmp.h>
#include <poll.h>
#include <pthread.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/iosupport.h>
#include <sys/socket.h>
#include <switch.h>

#include "config.h"
#include "genshin_compat.h"
#include "libc_shim.h"
#include "asset_pack.h"
#include "android_native_unity.h"
#include "android_log_sink.h"

uintptr_t nx_stack_chk_guard = (uintptr_t)0x9e3779b97f4a7c15ULL;
void *nx_stdin_ptr  = &fake_sF[0][0];
void *nx_stdout_ptr = &fake_sF[1][0];
void *nx_stderr_ptr = &fake_sF[2][0];
static char nx_home_env[] = "HOME=/switch/genshinimpact_nx";
static char nx_tmp_env[]  = "TMPDIR=/switch/genshinimpact_nx";
static char *nx_env_values[] = { nx_home_env, nx_tmp_env, NULL };
char **nx_environ_ptr = nx_env_values;

static void fortify_fail(void) {
  abort();
}

void nx_assert2(const char *file, int line, const char *function, const char *expr) {
  (void)file; (void)line; (void)function; (void)expr;
  abort();
}

typedef struct NxCxaThreadDestructor {
  void (*destructor)(void *);
  void *object;
  void *dso;
  struct NxCxaThreadDestructor *next;
} NxCxaThreadDestructor;

static pthread_key_t nx_cxa_thread_key;
static pthread_once_t nx_cxa_thread_once = PTHREAD_ONCE_INIT;
static int nx_cxa_thread_key_state;

/* Publish the remaining list before each callback.  A callback which creates
 * another thread_local object then prepends its destructor and it is observed
 * on the next iteration, exactly like Bionic's thread_local_dtors stack. */
static void nx_drain_cxa_thread_destructors(NxCxaThreadDestructor *head) {
  while (head) {
    NxCxaThreadDestructor *current = head;
    head = current->next;
    (void)pthread_setspecific(nx_cxa_thread_key, head);
    void (*destructor)(void *) = current->destructor;
    void *object = current->object;
    free(current);
    destructor(object);
    head = pthread_getspecific(nx_cxa_thread_key);
  }
  (void)pthread_setspecific(nx_cxa_thread_key, NULL);
}

static void nx_cxa_thread_key_destructor(void *head) {
  nx_drain_cxa_thread_destructors(head);
}

static void nx_init_cxa_thread_key(void) {
  const int result = pthread_key_create(&nx_cxa_thread_key,
                                        nx_cxa_thread_key_destructor);
  __atomic_store_n(&nx_cxa_thread_key_state, result ? -result : 1,
                   __ATOMIC_RELEASE);
}

int nx_cxa_thread_atexit(void (*destructor)(void *), void *object, void *dso) {
  if (!destructor) { errno = EINVAL; return -1; }
  const int once_result = pthread_once(&nx_cxa_thread_once,
                                       nx_init_cxa_thread_key);
  if (once_result || __atomic_load_n(&nx_cxa_thread_key_state,
                                     __ATOMIC_ACQUIRE) != 1) {
    errno = EAGAIN;
    return -1;
  }

  NxCxaThreadDestructor *entry = malloc(sizeof(*entry));
  if (!entry) { errno = ENOMEM; return -1; }
  entry->destructor = destructor;
  entry->object = object;
  entry->dso = dso; /* Plug-ins are resident, so no loader pin is required. */
  entry->next = pthread_getspecific(nx_cxa_thread_key);
  const int set_result = pthread_setspecific(nx_cxa_thread_key, entry);
  if (set_result) {
    free(entry);
    errno = set_result;
    return -1;
  }
  return 0;
}

void nx_run_cxa_thread_destructors(void) {
  if (__atomic_load_n(&nx_cxa_thread_key_state, __ATOMIC_ACQUIRE) != 1)
    return;
  NxCxaThreadDestructor *head = pthread_getspecific(nx_cxa_thread_key);
  if (head) nx_drain_cxa_thread_destructors(head);
}
char *nx_fgets_chk(char *dst, size_t dst_size, int count, FILE *file) {
  if (count < 0 || (size_t)count > dst_size) fortify_fail();
  return fgets_fake(dst, count, file);
}
void *nx_memcpy_chk(void *dst, const void *src, size_t count, size_t dst_size) {
  if (count > dst_size) fortify_fail();
  return memcpy(dst, src, count);
}
void *nx_memset_chk(void *dst, int value, size_t count, size_t dst_size) {
  if (count > dst_size) fortify_fail();
  return memset(dst, value, count);
}
int nx_open_2(const char *path, int flags) { return open_fake(path, flags); }
long nx_read_chk(int fd, void *dst, size_t count, size_t dst_size) {
  if (count > dst_size) fortify_fail();
  return read_fake(fd, dst, count);
}
int nx_register_atfork(void (*prepare)(void), void (*parent)(void),
                       void (*child)(void), void *dso) {
  (void)prepare; (void)parent; (void)child; (void)dso; return 0;
}
char *nx_strcat_chk(char *dst, const char *src, size_t dst_size) {
  if (strlen(dst) + strlen(src) + 1 > dst_size) fortify_fail();
  return strcat(dst, src);
}
char *nx_strchr_chk(const char *str, int ch, size_t str_size) {
  if (strnlen(str, str_size) == str_size) fortify_fail();
  return strchr(str, ch);
}
char *nx_strcpy_chk(char *dst, const char *src, size_t dst_size) {
  if (strlen(src) + 1 > dst_size) fortify_fail();
  return strcpy(dst, src);
}
char *nx_strncpy_chk2(char *dst, const char *src, size_t count,
                      size_t dst_size, size_t src_size) {
  if (count > dst_size || strnlen(src, src_size) == src_size) fortify_fail();
  return strncpy(dst, src, count);
}
char *nx_strrchr_chk(const char *str, int ch, size_t str_size) {
  if (strnlen(str, str_size) == str_size) fortify_fail();
  return strrchr(str, ch);
}
char *nx_strncat_chk(char *dst, const char *src, size_t count, size_t dst_size) {
  const size_t dst_length = strnlen(dst, dst_size);
  const size_t src_length = strnlen(src, count);
  if (dst_length == dst_size || dst_length + src_length + 1 > dst_size) fortify_fail();
  memcpy(dst + dst_length, src, src_length);
  dst[dst_length + src_length] = '\0';
  return dst;
}
int nx_vsprintf_chk(char *dst, int flags, size_t dst_size, const char *fmt, va_list ap) {
  (void)flags;
  return vsnprintf(dst, dst_size, fmt, ap);
}
void nx_fd_clr_chk(int fd, void *set, size_t set_size) {
  fd = fakefd_select_bit(fd);
  if (!set || fd < 0 || (size_t)fd / 8u >= set_size) fortify_fail();
  ((unsigned long *)set)[(unsigned)fd / (8u * sizeof(unsigned long))] &=
    ~(1ul << ((unsigned)fd % (8u * sizeof(unsigned long))));
}
char *nx_gnu_strerror_r(int error, char *buffer, size_t size) {
  if (!buffer || !size) return strerror_fake(error);
  snprintf(buffer, size, "%s", strerror_fake(error));
  return buffer;
}
unsigned nx_umask_chk(unsigned mask) { (void)mask; return 0; }
int nx_android_log_buf_write(int buffer_id, int priority, const char *tag, const char *text) {
  return android_log_sink_write(buffer_id, priority, tag, text);
}
void nx_android_log_assert(const char *condition, const char *tag, const char *format, ...) {
  if (format) {
    va_list args;
    va_start(args, format);
    android_log_sink_vprint(4 /* LOG_ID_CRASH */, 7 /* ANDROID_LOG_FATAL */,
                            tag, format, args);
    va_end(args);
  } else if (condition) {
    char message[512];
    snprintf(message, sizeof(message), "Assertion failed: %s", condition);
    android_log_sink_write(4, 7, tag, message);
  } else {
    android_log_sink_write(4, 7, tag, "Assertion failed");
  }
  abort();
}

typedef struct {
  void *name; uint32_t name_length; uint32_t pad;
  void *vectors; size_t vector_count;
  void *control; size_t control_length;
  int flags;
} BionicMessageHeader;
typedef struct { size_t length; int level; int type; } BionicControlHeader;
void *nx_cmsg_nxthdr(const void *message_ptr, const void *control_ptr) {
  const BionicMessageHeader *message = message_ptr;
  if (!message || !control_ptr || !message->control) return NULL;
  const uintptr_t begin = (uintptr_t)message->control;
  if (message->control_length > UINTPTR_MAX - begin) return NULL;
  const uintptr_t end = begin + message->control_length;
  const uintptr_t current = (uintptr_t)control_ptr;
  if (current < begin || current > end ||
      end - current < sizeof(BionicControlHeader)) return NULL;

  BionicControlHeader control;
  memcpy(&control, control_ptr, sizeof(control));
  if (control.length < sizeof(control) || control.length > end - current ||
      control.length > UINTPTR_MAX - current - (sizeof(size_t) - 1u))
    return NULL;
  const uintptr_t next = (current + control.length + sizeof(size_t) - 1u) &
                         ~(uintptr_t)(sizeof(size_t) - 1u);
  return next <= end && end - next >= sizeof(control) ? (void *)next : NULL;
}

void nx_arc4random_buf(void *buffer, size_t size) { randomGet(buffer, size); }
uint32_t nx_arc4random(void) { uint32_t value; randomGet(&value, sizeof value); return value; }
int nx_getentropy(void *buffer, size_t size) {
  if (size > 256) { errno = EIO; return -1; }
  randomGet(buffer, size); return 0;
}

/* Small poll-backed epoll implementation which preserves the packed bionic
 * event/data ABI.  Waits resnapshot at bounded intervals so concurrent ctl()
 * and close() operations become visible without a kernel eventfd. */
#define NX_EPOLL_BASE 0x60000000
#define NX_EPOLL_SETS 8
#define NX_EPOLL_ITEMS 64
#define NX_EPOLL_WAIT_SLICE_MS 10
#define B_EPOLLIN       UINT32_C(0x00000001)
#define B_EPOLLPRI      UINT32_C(0x00000002)
#define B_EPOLLOUT      UINT32_C(0x00000004)
#define B_EPOLLERR      UINT32_C(0x00000008)
#define B_EPOLLHUP      UINT32_C(0x00000010)
#define B_EPOLLRDNORM   UINT32_C(0x00000040)
#define B_EPOLLRDBAND   UINT32_C(0x00000080)
#define B_EPOLLWRNORM   UINT32_C(0x00000100)
#define B_EPOLLWRBAND   UINT32_C(0x00000200)
#define B_EPOLLRDHUP    UINT32_C(0x00002000)
#define B_EPOLLWAKEUP   UINT32_C(0x20000000)
#define B_EPOLLONESHOT  UINT32_C(0x40000000)
#define B_EPOLLET       UINT32_C(0x80000000)
#define B_EPOLL_ALLOWED (B_EPOLLIN | B_EPOLLPRI | B_EPOLLOUT | B_EPOLLERR | \
                         B_EPOLLHUP | B_EPOLLRDNORM | B_EPOLLRDBAND | \
                         B_EPOLLWRNORM | B_EPOLLWRBAND | B_EPOLLRDHUP | \
                         B_EPOLLWAKEUP | B_EPOLLONESHOT | B_EPOLLET)
typedef struct {
  int fd;
  uint32_t events;
  uint64_t data;
  uint32_t generation;
  int used, disabled;
} NxEpollItem;
typedef struct {
  int used;
  uint32_t generation;
  NxEpollItem items[NX_EPOLL_ITEMS];
} NxEpollSet;
typedef struct __attribute__((packed)) { uint32_t events; uint64_t data; } BionicEpollEvent;
static NxEpollSet nx_epolls[NX_EPOLL_SETS];
static Mutex nx_epoll_mutex;
static uint32_t nx_epoll_generation;
static NxEpollDiagnostics nx_epoll_diagnostics;

#define EPOLL_DIAG_ADD(field, value) \
  __atomic_fetch_add(&nx_epoll_diagnostics.field, (value), __ATOMIC_RELAXED)
#define EPOLL_DIAG_STORE(field, value) \
  __atomic_store_n(&nx_epoll_diagnostics.field, (value), __ATOMIC_RELAXED)

static int epoll_wait_fail(int error) {
  EPOLL_DIAG_ADD(wait_failures, 1);
  EPOLL_DIAG_STORE(last_wait_error, error);
  errno = error;
  return -1;
}

static int epoll_wait_timeout(void) {
  EPOLL_DIAG_ADD(wait_timeouts, 1);
  EPOLL_DIAG_STORE(last_wait_error, 0);
  return 0;
}

static int epoll_wait_ready(int count) {
  EPOLL_DIAG_ADD(delivered_events, (uint64_t)count);
  EPOLL_DIAG_STORE(last_wait_error, 0);
  return count;
}

static uint32_t epoll_next_generation_locked(void) {
  if (++nx_epoll_generation == 0) ++nx_epoll_generation;
  return nx_epoll_generation;
}
static NxEpollSet *epoll_set_locked(int fd) {
  int index = fd - NX_EPOLL_BASE;
  return (index >= 0 && index < NX_EPOLL_SETS && nx_epolls[index].used)
    ? &nx_epolls[index] : NULL;
}
int nx_epoll_is_fd(int fd) {
  mutexLock(&nx_epoll_mutex);
  const int result = epoll_set_locked(fd) != NULL;
  mutexUnlock(&nx_epoll_mutex);
  return result;
}
int nx_epoll_create(int size) {
  if (size <= 0) { errno = EINVAL; return -1; }
  mutexLock(&nx_epoll_mutex);
  for (int i = 0; i < NX_EPOLL_SETS; i++) if (!nx_epolls[i].used) {
    memset(&nx_epolls[i], 0, sizeof nx_epolls[i]);
    nx_epolls[i].used = 1;
    nx_epolls[i].generation = epoll_next_generation_locked();
    mutexUnlock(&nx_epoll_mutex); return NX_EPOLL_BASE + i;
  }
  mutexUnlock(&nx_epoll_mutex); errno = EMFILE; return -1;
}
int nx_epoll_create1(int flags) {
  if (flags & ~0x80000 /* EPOLL_CLOEXEC */) { errno = EINVAL; return -1; }
  return nx_epoll_create(1);
}
int nx_epoll_close(int fd) {
  mutexLock(&nx_epoll_mutex); NxEpollSet *set = epoll_set_locked(fd);
  if (!set) { mutexUnlock(&nx_epoll_mutex); return 0; }
  memset(set, 0, sizeof *set); mutexUnlock(&nx_epoll_mutex); return 1;
}
void nx_epoll_forget_fd(int fd) {
  mutexLock(&nx_epoll_mutex);
  for (int set_index = 0; set_index < NX_EPOLL_SETS; ++set_index) {
    NxEpollSet *set = &nx_epolls[set_index];
    if (!set->used) continue;
    for (int item_index = 0; item_index < NX_EPOLL_ITEMS; ++item_index)
      if (set->items[item_index].used && set->items[item_index].fd == fd)
        memset(&set->items[item_index], 0, sizeof set->items[item_index]);
  }
  mutexUnlock(&nx_epoll_mutex);
}

void nx_epoll_get_diagnostics(int probe_fd, NxEpollDiagnostics *out) {
  if (!out) return;
  memset(out, 0, sizeof(*out));
  out->wait_calls = __atomic_load_n(
    &nx_epoll_diagnostics.wait_calls, __ATOMIC_RELAXED);
  out->delivered_events = __atomic_load_n(
    &nx_epoll_diagnostics.delivered_events, __ATOMIC_RELAXED);
  out->wait_timeouts = __atomic_load_n(
    &nx_epoll_diagnostics.wait_timeouts, __ATOMIC_RELAXED);
  out->wait_failures = __atomic_load_n(
    &nx_epoll_diagnostics.wait_failures, __ATOMIC_RELAXED);
  out->stale_snapshot_retries = __atomic_load_n(
    &nx_epoll_diagnostics.stale_snapshot_retries, __ATOMIC_RELAXED);
  out->last_wait_error = __atomic_load_n(
    &nx_epoll_diagnostics.last_wait_error, __ATOMIC_RELAXED);

  /* Telemetry runs from Unity's frame thread.  Missing one registry snapshot
   * is preferable to waiting behind a guest epoll mutation or poll teardown. */
  if (!mutexTryLock(&nx_epoll_mutex)) return;
  for (int set_index = 0; set_index < NX_EPOLL_SETS; ++set_index) {
    const NxEpollSet *set = &nx_epolls[set_index];
    if (!set->used) continue;
    ++out->live_sets;
    for (int item_index = 0; item_index < NX_EPOLL_ITEMS; ++item_index) {
      const NxEpollItem *item = &set->items[item_index];
      if (!item->used) continue;
      ++out->registered_items;
      if (item->disabled) ++out->disabled_items;
      if (probe_fd >= 0 && item->fd == probe_fd) {
        ++out->probe_registrations;
        if (item->disabled) ++out->probe_disabled_registrations;
      }
    }
  }
  mutexUnlock(&nx_epoll_mutex);
}

static int epoll_target_valid(int fd) {
  if (fd < 0 || nx_epoll_is_fd(fd)) return 0; /* nested epoll is unsupported */
  struct pollfd probe = { .fd = fd, .events = 0, .revents = 0 };
  const int result = poll_fake(&probe, 1, 0);
  return result >= 0 && !(probe.revents & 0x20 /* bionic POLLNVAL */);
}
int nx_epoll_ctl(int epfd, int op, int fd, const void *event_ptr) {
  if (epfd == fd) { errno = EINVAL; return -1; }
  if (!epoll_target_valid(fd)) { errno = EBADF; return -1; }
  BionicEpollEvent event = {0};
  if (op != 2 /* EPOLL_CTL_DEL */) {
    if (!event_ptr) { errno = EFAULT; return -1; }
    memcpy(&event, event_ptr, sizeof event);
    if (event.events & ~B_EPOLL_ALLOWED) { errno = EINVAL; return -1; }
  }

  mutexLock(&nx_epoll_mutex); NxEpollSet *set = epoll_set_locked(epfd);
  if (!set) { mutexUnlock(&nx_epoll_mutex); errno = EBADF; return -1; }
  int found = -1, free_slot = -1;
  for (int i = 0; i < NX_EPOLL_ITEMS; i++) {
    if (set->items[i].used && set->items[i].fd == fd) found = i;
    if (!set->items[i].used && free_slot < 0) free_slot = i;
  }
  if (op == 2 /* EPOLL_CTL_DEL */) {
    if (found < 0) { mutexUnlock(&nx_epoll_mutex); errno = ENOENT; return -1; }
    memset(&set->items[found], 0, sizeof set->items[found]);
    mutexUnlock(&nx_epoll_mutex); return 0;
  }
  int slot = found;
  if (op == 1 /* ADD */) {
    if (found >= 0) { mutexUnlock(&nx_epoll_mutex); errno = EEXIST; return -1; }
    slot = free_slot;
  } else if (op == 3 /* MOD */) {
    if (found < 0) { mutexUnlock(&nx_epoll_mutex); errno = ENOENT; return -1; }
  } else { mutexUnlock(&nx_epoll_mutex); errno = EINVAL; return -1; }
  if (slot < 0) { mutexUnlock(&nx_epoll_mutex); errno = ENOSPC; return -1; }
  set->items[slot] = (NxEpollItem){
    .fd = fd, .events = event.events, .data = event.data,
    .generation = epoll_next_generation_locked(), .used = 1, .disabled = 0
  };
  mutexUnlock(&nx_epoll_mutex); return 0;
}

typedef struct {
  NxEpollItem item;
  int slot;
} NxEpollSnapshot;

static short epoll_poll_events(uint32_t events) {
  short result = 0;
  if (events & B_EPOLLIN) result |= 0x001;
  if (events & B_EPOLLPRI) result |= 0x002;
  if (events & B_EPOLLOUT) result |= 0x004;
  if (events & B_EPOLLRDNORM) result |= 0x040;
  if (events & B_EPOLLRDBAND) result |= 0x080;
  if (events & B_EPOLLWRNORM) result |= 0x100;
  if (events & B_EPOLLWRBAND) result |= 0x200;
  if (events & B_EPOLLRDHUP) result |= 0x2000;
  return result;
}

static uint32_t epoll_ready_events(uint32_t requested, short revents) {
  const uint16_t ready = (uint16_t)revents;
  uint32_t result = 0;
  if (ready & (0x001 | 0x040)) result |= requested & (B_EPOLLIN | B_EPOLLRDNORM);
  if (ready & (0x002 | 0x080)) result |= requested & (B_EPOLLPRI | B_EPOLLRDBAND);
  if (ready & (0x004 | 0x100)) result |= requested & (B_EPOLLOUT | B_EPOLLWRNORM);
  if (ready & 0x200) result |= requested & B_EPOLLWRBAND;
  if (ready & 0x008) result |= B_EPOLLERR;
  if (ready & 0x010) result |= B_EPOLLHUP;
  if (ready & 0x2000) result |= requested & B_EPOLLRDHUP;
  return result;
}

int nx_epoll_wait(int epfd, void *events_ptr, int max_events, int timeout_ms) {
  EPOLL_DIAG_ADD(wait_calls, 1);
  if (!events_ptr || max_events <= 0) return epoll_wait_fail(EINVAL);
  const uint64_t start_ns = armTicksToNs(armGetSystemTick());
  const uint64_t timeout_ns = timeout_ms < 0 ? UINT64_MAX :
    (uint64_t)(unsigned)timeout_ms * UINT64_C(1000000);

  for (;;) {
    struct pollfd polls[NX_EPOLL_ITEMS];
    NxEpollSnapshot snapshot[NX_EPOLL_ITEMS];
    int count = 0;
    uint32_t set_generation;

    mutexLock(&nx_epoll_mutex);
    NxEpollSet *set = epoll_set_locked(epfd);
    if (!set) {
      mutexUnlock(&nx_epoll_mutex);
      return epoll_wait_fail(EBADF);
    }
    set_generation = set->generation;
    for (int slot = 0; slot < NX_EPOLL_ITEMS; ++slot) {
      if (!set->items[slot].used || set->items[slot].disabled) continue;
      snapshot[count].item = set->items[slot];
      snapshot[count].slot = slot;
      polls[count].fd = set->items[slot].fd;
      polls[count].events = epoll_poll_events(set->items[slot].events);
      polls[count].revents = 0;
      ++count;
    }
    mutexUnlock(&nx_epoll_mutex);

    int slice_ms = NX_EPOLL_WAIT_SLICE_MS;
    if (timeout_ms == 0) {
      slice_ms = 0;
    } else if (timeout_ns != UINT64_MAX) {
      const uint64_t elapsed_ns = armTicksToNs(armGetSystemTick()) - start_ns;
      if (elapsed_ns >= timeout_ns) return epoll_wait_timeout();
      uint64_t remaining_ms = (timeout_ns - elapsed_ns + 999999u) / 1000000u;
      if (remaining_ms < (uint64_t)slice_ms) slice_ms = (int)remaining_ms;
      if (slice_ms < 1) slice_ms = 1;
    }

    const int result = poll_fake(polls, (unsigned long)count, slice_ms);
    if (result < 0) {
      const int poll_error = errno;
      if (poll_error == EBADF) {
        /* close_fake() removes every registration before the numeric BSD fd
         * can be closed or reused.  Horizon may nevertheless return EBADF for
         * the whole poll batch when that close races an already-copied
         * snapshot.  Treat it as a ctl/close notification only when the live
         * registry proves that at least one copied item changed; otherwise a
         * real bad descriptor must still be reported to Bionic. */
        int snapshot_changed = 0;
        mutexLock(&nx_epoll_mutex);
        set = epoll_set_locked(epfd);
        if (!set || set->generation != set_generation) {
          mutexUnlock(&nx_epoll_mutex);
          return epoll_wait_fail(EBADF);
        }
        for (int i = 0; i < count; ++i) {
          const NxEpollItem *current = &set->items[snapshot[i].slot];
          if (!current->used ||
              current->generation != snapshot[i].item.generation ||
              current->fd != snapshot[i].item.fd) {
            snapshot_changed = 1;
            break;
          }
        }
        mutexUnlock(&nx_epoll_mutex);
        if (snapshot_changed) {
          EPOLL_DIAG_ADD(stale_snapshot_retries, 1);
          EPOLL_DIAG_STORE(last_wait_error, 0);
          continue;
        }
      }
      return epoll_wait_fail(poll_error);
    }

    int out = 0;
    mutexLock(&nx_epoll_mutex);
    set = epoll_set_locked(epfd);
    if (!set || set->generation != set_generation) {
      mutexUnlock(&nx_epoll_mutex);
      return epoll_wait_fail(EBADF);
    }
    for (int i = 0; i < count && out < max_events; ++i) {
      if (!polls[i].revents) continue;
      NxEpollItem *current = &set->items[snapshot[i].slot];
      if (!current->used || current->generation != snapshot[i].item.generation)
        continue;
      if ((uint16_t)polls[i].revents & 0x20 /* POLLNVAL */) {
        memset(current, 0, sizeof(*current));
        continue;
      }
      const uint32_t ready = epoll_ready_events(current->events,
                                                 polls[i].revents);
      if (!ready) continue;
      const BionicEpollEvent event = { .events = ready, .data = current->data };
      memcpy((char *)events_ptr + (size_t)out * sizeof event, &event,
             sizeof event);
      if (current->events & B_EPOLLONESHOT) current->disabled = 1;
      ++out;
    }
    mutexUnlock(&nx_epoll_mutex);
    if (out) return epoll_wait_ready(out);
    if (timeout_ms == 0) return epoll_wait_timeout();
    if (timeout_ns != UINT64_MAX &&
        armTicksToNs(armGetSystemTick()) - start_ns >= timeout_ns)
      return epoll_wait_timeout();
    /* EPOLLET is conservatively serviced as level-triggered.  Consumers that
     * drain descriptors (the exact client's socket loops) observe equivalent
     * progress. */
  }
}

static int unsupported_fd(void) { errno = 38; return -1; } /* Bionic ENOSYS. */
int nx_inotify_init1(int flags) { (void)flags; return unsupported_fd(); }
int nx_inotify_add_watch(int fd, const char *path, uint32_t mask) {
  (void)fd; (void)path; (void)mask; return unsupported_fd();
}
int nx_signalfd(int fd, const void *mask, int flags) {
  (void)fd; (void)mask; (void)flags; return unsupported_fd();
}
int nx_eventfd(unsigned initial_value, int flags) {
  return fakefd_eventfd(initial_value, flags);
}
int nx_fork(void) { return unsupported_fd(); }
int nx_execl(const char *path, const char *arg, ...) {
  (void)path; (void)arg; return unsupported_fd();
}
int nx_execv(const char *path, char *const argv[]) {
  (void)path; (void)argv; return unsupported_fd();
}
int nx_execve(const char *path, char *const argv[], char *const envp[]) {
  (void)path; (void)argv; (void)envp; return unsupported_fd();
}
int nx_clone(int (*entry)(void *), void *stack, int flags, void *argument, ...) {
  (void)entry; (void)stack; (void)flags; (void)argument; return unsupported_fd();
}
int nx_waitpid(int pid, int *status, int options) {
  (void)pid; (void)status; (void)options; return unsupported_fd();
}
int nx_getppid(void) { return 0; }
int nx_getrlimit(int resource, void *limit) {
  (void)resource; if (!limit) { errno = EFAULT; return -1; }
  ((uint64_t *)limit)[0] = ((uint64_t *)limit)[1] = UINT64_MAX; return 0;
}
int nx_getrusage(int who, void *usage) {
  (void)who; if (!usage) { errno = EFAULT; return -1; }
  memset(usage, 0, 144); return 0;
}

/* Linux/Bionic's arm64 struct sysinfo.  Keep the layout explicit: newlib does
 * not provide this Linux UAPI type and its host-long padding must not leak into
 * the guest ABI.  mem_unit=1 makes all RAM fields byte counts. */
typedef struct {
  int64_t uptime;
  uint64_t loads[3];
  uint64_t totalram;
  uint64_t freeram;
  uint64_t sharedram;
  uint64_t bufferram;
  uint64_t totalswap;
  uint64_t freeswap;
  uint16_t procs;
  uint16_t pad;
  uint32_t align_pad;
  uint64_t totalhigh;
  uint64_t freehigh;
  uint32_t mem_unit;
  uint32_t tail_pad;
} BionicSysinfo;
_Static_assert(sizeof(BionicSysinfo) == 112, "arm64 bionic sysinfo size");
_Static_assert(offsetof(BionicSysinfo, totalram) == 32, "arm64 bionic totalram offset");
_Static_assert(offsetof(BionicSysinfo, freeram) == 40, "arm64 bionic freeram offset");
_Static_assert(offsetof(BionicSysinfo, procs) == 80, "arm64 bionic procs offset");
_Static_assert(offsetof(BionicSysinfo, totalhigh) == 88, "arm64 bionic totalhigh offset");
_Static_assert(offsetof(BionicSysinfo, mem_unit) == 104, "arm64 bionic mem_unit offset");

#define NX_GUEST_TOTAL_RAM (UINT64_C(512) * 1024 * 1024)
int nx_sysinfo(void *info) {
  if (!info) { errno = EFAULT; return -1; }
  BionicSysinfo result = {0};
  result.uptime = (int64_t)(armGetSystemTick() / armGetSystemTickFreq());
  result.totalram = NX_GUEST_TOTAL_RAM;
  result.freeram = NX_GUEST_TOTAL_RAM / 2;
  result.procs = 1;
  result.mem_unit = 1;

  /* sysconf advertises a bounded 512-MiB guest budget to Unity.  Reflect the
   * same total here and cap the live process headroom to that budget. */
  uint64_t host_total = 0, host_used = 0;
  const Result total_result = svcGetInfo(&host_total, InfoType_TotalMemorySize,
                                         CUR_PROCESS_HANDLE, 0);
  const Result used_result = svcGetInfo(&host_used, InfoType_UsedMemorySize,
                                        CUR_PROCESS_HANDLE, 0);
  if (R_SUCCEEDED(total_result) && R_SUCCEEDED(used_result) &&
      host_used <= host_total) {
    const uint64_t host_free = host_total - host_used;
    result.freeram = host_free < result.totalram ? host_free : result.totalram;
  }
  memcpy(info, &result, sizeof(result));
  return 0;
}
long nx_pathconf(const char *path, int name) { (void)path; (void)name; return 255; }
int nx_mincore(void *address, size_t length, unsigned char *vector) {
  (void)address; if (!vector) { errno = EFAULT; return -1; }
  memset(vector, 1, (length + 0xfff) >> 12); return 0;
}
int nx_msync(void *address, size_t length, int flags) {
  return mmap_msync_fake(address, length, flags);
}
int nx_pthread_attr_getguardsize(const void *attr, size_t *size) {
  (void)attr; if (size) *size = 0; return 0;
}
int nx_pthread_attr_setschedpolicy(void *attr, int policy) {
  (void)attr; (void)policy; return 0;
}
int nx_pthread_setschedparam(void *thread, int policy, const void *param) {
  (void)thread; (void)policy; (void)param; return 0;
}
int nx_pthread_getschedparam(void *thread, int *policy, void *param) {
  (void)thread; if (policy) *policy = 0; if (param) *(int *)param = 0; return 0;
}
int nx_pthread_rwlock_destroy(void **lock) {
  if (lock && *lock) { free(*lock); *lock = NULL; } return 0;
}
int nx_sched_get_priority_min(int policy) { (void)policy; return 0; }
int nx_sched_get_priority_max(int policy) { (void)policy; return 63; }
int nx_sched_getparam(int pid, void *param) { (void)pid; if (param) *(int *)param = 0; return 0; }
int nx_sched_getscheduler(int pid) { (void)pid; return 0; }
/* A C wrapper would make setjmp save this shim's already-dead frame.  Tail
 * branches leave the guest caller's LR and SP intact in the newlib jmp_buf.
 * newlib longjmp, like ISO C/Bionic, translates a requested value of 0 to 1. */
/* devkitA64 GCC does not implement AArch64's naked attribute.  Define these
 * entry points as assembler-only functions so no compiler prologue can be
 * inserted before the tail branches.  x0/x1 already match both callees. */
__asm__(
  ".pushsection .text.nx_sigsetjmp,\"ax\",%progbits\n"
  ".balign 4\n"
  ".global nx_sigsetjmp\n"
  ".type nx_sigsetjmp, %function\n"
  "nx_sigsetjmp:\n"
  "b setjmp\n"
  ".size nx_sigsetjmp, .-nx_sigsetjmp\n"
  ".popsection\n"
  ".pushsection .text.nx_siglongjmp,\"ax\",%progbits\n"
  ".balign 4\n"
  ".global nx_siglongjmp\n"
  ".type nx_siglongjmp, %function\n"
  "nx_siglongjmp:\n"
  "b longjmp\n"
  ".size nx_siglongjmp, .-nx_siglongjmp\n"
  ".popsection\n"
);
int nx_fstat64(int fd, void *stat_buffer) { return fstat_fake(fd, stat_buffer); }

/* libnx's fsdev file object stores an FsFile followed by its POSIX flags and
 * sequential cursor.  Its public devoptab structSize plus fsdev mount lookup
 * are checked before using the mirror, so another descriptor backend fails
 * with ESPIPE rather than being reinterpreted. */
typedef struct {
  FsFile file;
  int flags;
  int64_t offset;
  FsTimeStampRaw timestamps;
} NxFsdevFile;
_Static_assert(offsetof(NxFsdevFile, flags) == sizeof(FsFile),
               "libnx fsdev flags follow FsFile");

#define NX_FS_TRANSFER_PAGE 0x10000u
#define NX_FS_TRACKED_FILES 128u
#define NX_FS_TRACKED_PATH  600u

static NxFileIoDiagnostics g_file_io_diagnostics;
static uint32_t g_fs_write_requires_bounce;

/* The direct-transfer fallbacks bounce through one page-sized buffer.
 * Guest worker threads can run on small stacks, so a 64 KiB stack array is
 * a silent overflow hazard; keep one page-aligned buffer per thread
 * instead (the FS IPC write path requires 0x1000 alignment).  Allocation
 * failure leaves the errno ENOMEM path, matching a full transfer arena. */
static unsigned char *nx_fs_transfer_page(void) {
  static __thread unsigned char *tls_page;
  if (!tls_page)
    tls_page = memalign(0x1000, NX_FS_TRANSFER_PAGE);
  return tls_page;
}

typedef struct {
  Mutex lock;
  NxFsdevFile *file;
  int64_t logical_size;
  int64_t physical_size;
  char path[NX_FS_TRACKED_PATH];
  unsigned used;
  unsigned acquisitions;
  unsigned initialized;
  unsigned bulk_eligible;
  uint64_t size_operation_started_tick;
  uint32_t size_operation_kind;
} NxTrackedFileSize;

static Mutex g_file_size_registry_lock;
static NxTrackedFileSize g_file_sizes[NX_FS_TRACKED_FILES];

static void nx_file_size_release(NxTrackedFileSize *entry);

#define FILE_IO_ADD(field, value) \
  __atomic_fetch_add(&g_file_io_diagnostics.field, (uint64_t)(value), \
                     __ATOMIC_RELAXED)

enum {
  NX_FILE_SIZE_OP_EXTEND = 1,
  NX_FILE_SIZE_OP_FALLBACK = 2,
};

static void nx_file_size_operation_begin(NxTrackedFileSize *entry,
                                         uint32_t kind) {
  if (!entry) return;
  __atomic_store_n(&entry->size_operation_started_tick,
                   armGetSystemTick(), __ATOMIC_RELAXED);
  __atomic_store_n(&entry->size_operation_kind, kind, __ATOMIC_RELEASE);
}

static void nx_file_size_operation_end(NxTrackedFileSize *entry) {
  if (!entry) return;
  __atomic_store_n(&entry->size_operation_kind, 0, __ATOMIC_RELEASE);
}

void nx_file_io_get_diagnostics(NxFileIoDiagnostics *out) {
  if (!out) return;
#define FILE_IO_LOAD(field) \
  out->field = __atomic_load_n(&g_file_io_diagnostics.field, __ATOMIC_RELAXED)
  FILE_IO_LOAD(read_calls);
  FILE_IO_LOAD(read_bytes);
  FILE_IO_LOAD(read_failures);
  FILE_IO_LOAD(write_calls);
  FILE_IO_LOAD(write_bytes);
  FILE_IO_LOAD(write_failures);
  FILE_IO_LOAD(size_queries);
  FILE_IO_LOAD(size_cache_hits);
  FILE_IO_LOAD(size_query_failures);
  FILE_IO_LOAD(size_extensions);
  FILE_IO_LOAD(size_extension_failures);
  FILE_IO_LOAD(preallocation_extensions);
  FILE_IO_LOAD(preallocation_fallbacks);
  FILE_IO_LOAD(preallocated_bytes);
  FILE_IO_LOAD(finalize_calls);
  FILE_IO_LOAD(finalize_failures);
  FILE_IO_LOAD(finalized_bytes);
  FILE_IO_LOAD(direct_writes);
  FILE_IO_LOAD(direct_write_failures);
  FILE_IO_LOAD(bounce_writes);
  FILE_IO_LOAD(bounce_bytes);
#undef FILE_IO_LOAD
  out->size_operations_active = 0;
  out->oldest_size_operation_ms = 0;
  out->oldest_size_operation_kind = 0;
  out->oldest_size_operation_slot = UINT32_MAX;
  const uint64_t now_tick = armGetSystemTick();
  for (unsigned i = 0; i < NX_FS_TRACKED_FILES; ++i) {
    const uint32_t kind = __atomic_load_n(
      &g_file_sizes[i].size_operation_kind, __ATOMIC_ACQUIRE);
    if (!kind) continue;
    const uint64_t started = __atomic_load_n(
      &g_file_sizes[i].size_operation_started_tick, __ATOMIC_RELAXED);
    ++out->size_operations_active;
    const uint64_t age_ms = now_tick >= started
      ? armTicksToNs(now_tick - started) / UINT64_C(1000000) : 0;
    if (out->oldest_size_operation_slot == UINT32_MAX ||
        age_ms > out->oldest_size_operation_ms) {
      out->oldest_size_operation_ms = age_ms;
      out->oldest_size_operation_kind = kind;
      out->oldest_size_operation_slot = i;
    }
  }
}

uint32_t nx_file_io_identify_lock(uintptr_t address, uint32_t *slot_out,
                                  uint32_t *word_out) {
  if (slot_out) *slot_out = UINT32_MAX;
  if (word_out) *word_out = 0;
  if (address == (uintptr_t)&g_file_size_registry_lock) {
    if (word_out)
      *word_out = __atomic_load_n(
        (const uint32_t *)&g_file_size_registry_lock, __ATOMIC_RELAXED);
    return 1;
  }
  for (unsigned i = 0; i < NX_FS_TRACKED_FILES; ++i) {
    if (address != (uintptr_t)&g_file_sizes[i].lock) continue;
    if (slot_out) *slot_out = i;
    if (word_out)
      *word_out = __atomic_load_n(
        (const uint32_t *)&g_file_sizes[i].lock, __ATOMIC_RELAXED);
    return 2;
  }
  return 0;
}

static NxFsdevFile *nx_positional_fsdev_file(int fd) {
  __handle *handle = __get_handle(fd);
  if (!handle || !handle->fileStruct) { errno = EBADF; return NULL; }
  const devoptab_t *device = devoptab_list[handle->device];
  if (!device || !device->name || device->structSize != sizeof(NxFsdevFile) ||
      !fsdevGetDeviceFileSystem(device->name)) {
    errno = ESPIPE;
    return NULL;
  }
  return (NxFsdevFile *)handle->fileStruct;
}

static NxTrackedFileSize *nx_file_size_acquire(NxFsdevFile *file,
                                                int create,
                                                int bulk_eligible) {
  if (!file) return NULL;
  mutexLock(&g_file_size_registry_lock);
  NxTrackedFileSize *entry = NULL;
  NxTrackedFileSize *free_entry = NULL;
  for (unsigned i = 0; i < NX_FS_TRACKED_FILES; ++i) {
    NxTrackedFileSize *candidate = &g_file_sizes[i];
    if (candidate->used && candidate->file == file) {
      entry = candidate;
      break;
    }
    if (!candidate->used && !candidate->acquisitions && !free_entry)
      free_entry = candidate;
  }
  if (!entry && create && free_entry) {
    entry = free_entry;
    /* acquisitions == 0 guarantees no owner or waiter can hold this lock. */
    mutexLock(&entry->lock);
    entry->file = file;
    entry->logical_size = 0;
    entry->physical_size = 0;
    entry->path[0] = '\0';
    entry->initialized = 0;
    entry->bulk_eligible = bulk_eligible != 0;
    __atomic_store_n(&entry->size_operation_started_tick, 0,
                     __ATOMIC_RELAXED);
    __atomic_store_n(&entry->size_operation_kind, 0, __ATOMIC_RELAXED);
    entry->acquisitions = 1;
    entry->used = 1;
    mutexUnlock(&g_file_size_registry_lock);
    return entry;
  }
  if (entry) {
    ++entry->acquisitions;
  }
  mutexUnlock(&g_file_size_registry_lock);
  if (entry) {
    mutexLock(&entry->lock);
    if (!entry->used || entry->file != file) {
      nx_file_size_release(entry);
      return NULL;
    }
    if (bulk_eligible) entry->bulk_eligible = 1;
  }
  return entry;
}

static void nx_file_size_release(NxTrackedFileSize *entry) {
  if (!entry) return;
  mutexUnlock(&entry->lock);
  mutexLock(&g_file_size_registry_lock);
  --entry->acquisitions;
  mutexUnlock(&g_file_size_registry_lock);
}

static int nx_file_size_cached(NxFsdevFile *file, int64_t *size_out) {
  NxTrackedFileSize *entry = nx_file_size_acquire(file, 0, 0);
  if (!entry) return 0;
  const int available = entry->initialized;
  if (available && size_out) *size_out = entry->logical_size;
  nx_file_size_release(entry);
  return available;
}

void nx_file_io_track_open(int fd, const char *path, int writable) {
  if (!writable || !path) return;
  const int saved_errno = errno;
  NxFsdevFile *file = nx_positional_fsdev_file(fd);
  if (!file) { errno = saved_errno; return; }
  const size_t root_length = strlen(GAME_HOME "/files");
  const int bulk_eligible =
    !strncmp(path, GAME_HOME "/files", root_length) &&
    (path[root_length] == '/' || path[root_length] == '\0');
  NxTrackedFileSize *entry = nx_file_size_acquire(file, 1, bulk_eligible);
  if (entry && !entry->path[0])
    snprintf(entry->path, sizeof entry->path, "%s", path);
  nx_file_size_release(entry);
  errno = saved_errno;
}

static int nx_file_size_initialize_locked(NxFsdevFile *file,
                                           NxTrackedFileSize *entry) {
  if (!entry) return 0;
  if (!entry->initialized) {
    FILE_IO_ADD(size_queries, 1);
    int64_t current_size = 0;
    if (R_FAILED(fsFileGetSize(&file->file, &current_size)) ||
        current_size < 0) {
      FILE_IO_ADD(size_query_failures, 1);
      errno = EIO;
      return -1;
    }
    entry->logical_size = current_size;
    entry->physical_size = current_size;
    entry->initialized = 1;
  } else {
    FILE_IO_ADD(size_cache_hits, 1);
  }
  return 0;
}

static Result nx_fsdev_write_chunk(NxFsdevFile *file,
                                   NxTrackedFileSize *entry, int64_t offset,
                                   const void *buffer, size_t count) {
  const int64_t end = offset + (int64_t)count;
  const int extends = entry && end > entry->physical_size;
  if (extends) nx_file_size_operation_begin(entry, NX_FILE_SIZE_OP_EXTEND);
  const Result result = fsFileWrite(&file->file, offset, buffer, count,
                                    FsWriteOption_None);
  if (extends) nx_file_size_operation_end(entry);
  if (R_FAILED(result)) {
    if (extends) FILE_IO_ADD(size_extension_failures, 1);
    return result;
  }
  if (extends) FILE_IO_ADD(size_extensions, 1);
  if (entry && end > entry->logical_size) entry->logical_size = end;
  if (entry && end > entry->physical_size) entry->physical_size = end;
  return result;
}

int nx_file_io_logical_size(int fd, int64_t *size_out) {
  if (!size_out) return 0;
  const int saved_errno = errno;
  NxFsdevFile *file = nx_positional_fsdev_file(fd);
  if (!file) { errno = saved_errno; return 0; }
  const int result = nx_file_size_cached(file, size_out);
  errno = saved_errno;
  return result;
}

int nx_file_io_logical_size_path(const char *path, int64_t *size_out) {
  if (!path || !size_out) return 0;
  const int saved_errno = errno;
  int result = 0;
  mutexLock(&g_file_size_registry_lock);
  for (unsigned i = 0; i < NX_FS_TRACKED_FILES; ++i) {
    NxTrackedFileSize *entry = &g_file_sizes[i];
    if (!entry->used || strcmp(entry->path, path)) continue;
    ++entry->acquisitions;
    mutexUnlock(&g_file_size_registry_lock);
    mutexLock(&entry->lock);
    if (entry->used && entry->initialized && !strcmp(entry->path, path)) {
      *size_out = entry->logical_size;
      result = 1;
    }
    nx_file_size_release(entry);
    errno = saved_errno;
    return result;
  }
  mutexUnlock(&g_file_size_registry_lock);
  errno = saved_errno;
  return result;
}

void nx_file_io_finalize_fd(int fd) {
  const int saved_errno = errno;
  NxFsdevFile *file = nx_positional_fsdev_file(fd);
  if (!file) { errno = saved_errno; return; }

  NxTrackedFileSize *entry = nx_file_size_acquire(file, 0, 0);
  if (!entry) { errno = saved_errno; return; }
  /* Every close retires the shared fileStruct pointer.  A surviving duplicate
   * lazily recreates the cache, avoiding stale entries if concurrent closes
   * both observe a non-final newlib refcount. */
  mutexLock(&g_file_size_registry_lock);
  entry->used = 0;
  entry->file = NULL;
  entry->initialized = 0;
  entry->bulk_eligible = 0;
  entry->path[0] = '\0';
  mutexUnlock(&g_file_size_registry_lock);
  nx_file_size_release(entry);
  errno = saved_errno;
}

void nx_file_io_note_truncate(int fd, int64_t length) {
  if (length < 0) return;
  const int saved_errno = errno;
  NxFsdevFile *file = nx_positional_fsdev_file(fd);
  if (!file) { errno = saved_errno; return; }
  NxTrackedFileSize *entry = nx_file_size_acquire(file, 0, 0);
  if (entry) {
    entry->logical_size = length;
    entry->physical_size = length;
    entry->initialized = 1;
    nx_file_size_release(entry);
  }
  errno = saved_errno;
}

static long nx_fsdev_pread(NxFsdevFile *file, void *buffer, size_t count,
                           int64_t offset) {
  FILE_IO_ADD(read_calls, 1);
  int64_t logical_size = 0;
  if (nx_file_size_cached(file, &logical_size)) {
    if (offset >= logical_size) return 0;
    const uint64_t available = (uint64_t)(logical_size - offset);
    if ((uint64_t)count > available) count = (size_t)available;
  }
  uint64_t bytes = 0;
  Result result = fsFileRead(&file->file, offset, buffer, count,
                             FsReadOption_None, &bytes);
  if (R_SUCCEEDED(result) && bytes) {
    if (bytes > count) bytes = count;
    FILE_IO_ADD(read_bytes, bytes);
    return (long)bytes;
  }
  if (R_SUCCEEDED(result)) {
    int64_t file_size = 0;
    if (R_SUCCEEDED(fsFileGetSize(&file->file, &file_size)) &&
        offset >= file_size)
      return 0;
  }

  /* Retry every direct-transfer rejection, plus an anomalous zero-byte read
   * before EOF, through a bounce page.  Different FS/libnx revisions can
   * report inaccessible mapped-buffer classes with different Result values. */
  unsigned char *page = nx_fs_transfer_page();
  if (!page) {
    FILE_IO_ADD(read_failures, 1);
    errno = ENOMEM;
    return -1;
  }
  size_t done = 0;
  while (done < count) {
    const size_t wanted = count - done < NX_FS_TRANSFER_PAGE
      ? count - done : NX_FS_TRANSFER_PAGE;
    bytes = 0;
    result = fsFileRead(&file->file, offset + (int64_t)done, page, wanted,
                        FsReadOption_None, &bytes);
    if (bytes > wanted) bytes = wanted;
    if (R_FAILED(result)) {
      FILE_IO_ADD(read_failures, 1);
      if (done) FILE_IO_ADD(read_bytes, done);
      if (done) return (long)done;
      errno = EIO;
      return -1;
    }
    memcpy((unsigned char *)buffer + done, page, (size_t)bytes);
    done += (size_t)bytes;
    if (bytes < wanted) break;
  }
  FILE_IO_ADD(read_bytes, done);
  return (long)done;
}

static long nx_fsdev_pwrite_locked(NxFsdevFile *file,
                                   NxTrackedFileSize *entry,
                                   const void *buffer, size_t count,
                                   int64_t offset) {
  FILE_IO_ADD(write_calls, 1);
  Result result;
  /* The Android image passes Unity mmap-backed buffers which Horizon FS does
   * not accept as IPC transfer memory.  Probe until the first rejection, then
   * avoid paying for one known-failing FS command on every small TLS chunk. */
  if (!__atomic_load_n(&g_fs_write_requires_bounce, __ATOMIC_RELAXED)) {
    result = nx_fsdev_write_chunk(file, entry, offset, buffer, count);
    if (R_SUCCEEDED(result)) {
      FILE_IO_ADD(direct_writes, 1);
      FILE_IO_ADD(write_bytes, count);
      if ((file->flags & O_SYNC) && R_FAILED(fsFileFlush(&file->file))) {
        FILE_IO_ADD(write_failures, 1);
        errno = EIO;
        return -1;
      }
      return (long)count;
    }
    FILE_IO_ADD(direct_write_failures, 1);
    __atomic_store_n(&g_fs_write_requires_bounce, 1, __ATOMIC_RELAXED);
  }

  unsigned char *page = nx_fs_transfer_page();
  if (!page) {
    FILE_IO_ADD(write_failures, 1);
    errno = ENOMEM;
    return -1;
  }
  size_t done = 0;
  while (done < count) {
    const size_t wanted = count - done < NX_FS_TRANSFER_PAGE
      ? count - done : NX_FS_TRANSFER_PAGE;
    memcpy(page, (const unsigned char *)buffer + done, wanted);
    result = nx_fsdev_write_chunk(file, entry, offset + (int64_t)done, page,
                                  wanted);
    if (R_FAILED(result)) {
      FILE_IO_ADD(write_failures, 1);
      if (done) {
        FILE_IO_ADD(write_bytes, done);
        FILE_IO_ADD(bounce_bytes, done);
        if ((file->flags & O_SYNC) && R_FAILED(fsFileFlush(&file->file))) {
          errno = EIO;
          return -1;
        }
        return (long)done;
      }
      errno = EIO;
      return -1;
    }
    done += wanted;
  }
  FILE_IO_ADD(bounce_writes, 1);
  FILE_IO_ADD(bounce_bytes, done);
  FILE_IO_ADD(write_bytes, done);
  if ((file->flags & O_SYNC) && R_FAILED(fsFileFlush(&file->file))) {
    FILE_IO_ADD(write_failures, 1);
    errno = EIO;
    return -1;
  }
  return (long)done;
}

static long nx_fsdev_pwrite(NxFsdevFile *file, const void *buffer,
                            size_t count, int64_t offset) {
  NxTrackedFileSize *entry = nx_file_size_acquire(file, 1, 0);
  if (nx_file_size_initialize_locked(file, entry) != 0) {
    FILE_IO_ADD(write_calls, 1);
    FILE_IO_ADD(write_failures, 1);
    nx_file_size_release(entry);
    return -1;
  }
  const long result = nx_fsdev_pwrite_locked(file, entry, buffer, count,
                                              offset);
  nx_file_size_release(entry);
  return result;
}

static long nx_pread_backend(int fd, void *buffer, size_t count, long offset) {
  if (offset < 0 || count > (size_t)(INT64_MAX - offset)) {
    errno = EINVAL;
    return -1;
  }
  if (!buffer && count) { errno = EFAULT; return -1; }
  NxFsdevFile *file = nx_positional_fsdev_file(fd);
  if (!file) return -1;
  if ((file->flags & O_ACCMODE) == O_WRONLY) { errno = EBADF; return -1; }
  if (!count) return 0;
  return nx_fsdev_pread(file, buffer, count, offset);
}
static long nx_pwrite_backend(int fd, const void *buffer, size_t count,
                              long offset) {
  if (offset < 0 || count > (size_t)(INT64_MAX - offset)) {
    errno = EINVAL;
    return -1;
  }
  if (!buffer && count) { errno = EFAULT; return -1; }
  NxFsdevFile *file = nx_positional_fsdev_file(fd);
  if (!file) return -1;
  if ((file->flags & O_ACCMODE) == O_RDONLY) { errno = EBADF; return -1; }
  if (!count) return 0;
  return nx_fsdev_pwrite(file, buffer, count, offset);
}

/* libnx fsdev's normal read/write path transfers directly to the supplied
 * buffer.  Some guest mappings are valid CPU memory but are rejected by FS as
 * IPC transfer buffers.  Keep the shared open-file cursor on the original
 * descriptor and reuse the positional safe-buffer fallback above.  Holding
 * the route stripe also serializes the cursor with dup/close/replacement. */
static long nx_fsdev_read(NxFsdevFile *file, void *buffer, size_t count) {
  if ((file->flags & O_ACCMODE) == O_WRONLY) { errno = EBADF; return -1; }
  if (file->offset < 0 || count > (size_t)(INT64_MAX - file->offset)) {
    errno = EINVAL;
    return -1;
  }
  size_t total = 0;
  while (total < count) {
    const long chunk = nx_fsdev_pread(file, (unsigned char *)buffer + total,
                                     count - total,
                                     file->offset + (int64_t)total);
    if (chunk < 0) return total ? (long)total : -1;
    if (!chunk) break;
    total += (size_t)chunk;
  }
  file->offset += (int64_t)total;
  return (long)total;
}

static long nx_fsdev_write(NxFsdevFile *file, const void *buffer,
                           size_t count) {
  if ((file->flags & O_ACCMODE) == O_RDONLY) { errno = EBADF; return -1; }
  NxTrackedFileSize *entry = nx_file_size_acquire(file, 1, 0);
  if ((file->flags & O_APPEND) &&
      nx_file_size_initialize_locked(file, entry) != 0) {
    FILE_IO_ADD(write_calls, 1);
    FILE_IO_ADD(write_failures, 1);
    nx_file_size_release(entry);
    return -1;
  }
  int64_t offset = file->offset;
  if (file->flags & O_APPEND) {
    if (entry) {
      offset = entry->logical_size;
    } else if (R_FAILED(fsFileGetSize(&file->file, &offset)) || offset < 0) {
      nx_file_size_release(entry);
      FILE_IO_ADD(write_calls, 1);
      FILE_IO_ADD(write_failures, 1);
      errno = EIO;
      return -1;
    }
  }
  if (!(file->flags & O_APPEND) &&
      nx_file_size_initialize_locked(file, entry) != 0) {
    FILE_IO_ADD(write_calls, 1);
    FILE_IO_ADD(write_failures, 1);
    nx_file_size_release(entry);
    return -1;
  }
  if (offset < 0 || count > (size_t)(INT64_MAX - offset)) {
    nx_file_size_release(entry);
    errno = EINVAL;
    return -1;
  }
  const long result = nx_fsdev_pwrite_locked(file, entry, buffer, count,
                                              offset);
  if (result > 0) file->offset = offset + result;
  nx_file_size_release(entry);
  return result;
}

static int host_fd_is_socket(int fd) {
  int socket_type = 0;
  socklen_t socket_type_len = sizeof(socket_type);
  const int saved = errno;
  const int result =
    getsockopt(fd, SOL_SOCKET, SO_TYPE, &socket_type, &socket_type_len) == 0;
  errno = saved;
  return result;
}

static long nx_read_backend(int fd, void *buffer, size_t count) {
  NxFsdevFile *file = nx_positional_fsdev_file(fd);
  if (file) return nx_fsdev_read(file, buffer, count);
  if (host_fd_is_socket(fd)) return recv_fake(fd, buffer, count, 0);
  /* Consoles and other native devoptabs are not fsdev files. */
  return read(fd, buffer, count);
}

static long nx_write_backend(int fd, const void *buffer, size_t count) {
  NxFsdevFile *file = nx_positional_fsdev_file(fd);
  if (file) return nx_fsdev_write(file, buffer, count);
  if (host_fd_is_socket(fd)) return send_fake(fd, buffer, count, 0);
  return write(fd, buffer, count);
}

/* Android's stdout/stderr descriptors are guest logging endpoints, not libnx
 * consoles.  Passing them to newlib's native write() selects the default
 * software-console devoptab even though this title never initialized a
 * framebuffer console; ConsoleSwRenderer_drawChar then dereferences its null
 * renderer state.  Consume the write exactly as Android's logging fallback
 * expects instead. */
static long nx_standard_stream_write(int fd, const void *buffer, size_t count) {
  (void)fd;
  (void)buffer;
  return (long)count;
}

long nx_read(int fd, void *buffer, size_t count) {
  if (!buffer && count) { errno = EFAULT; return -1; }
  if (count > (size_t)LONG_MAX) { errno = EINVAL; return -1; }
  uint32_t stripe;
  if (!nx_fd_route_source_lock(fd, &stripe)) return -1;
  AssetPackOperation asset = {0};
  FakeFdOperation fake = {0};
  long result;
  if (asset_pack_operation_acquire(fd, &asset)) {
    result = asset_pack_operation_read(&asset, buffer, count);
    asset_pack_operation_release(&asset);
  } else if (fakefd_operation_acquire(fd, &fake)) {
    result = fakefd_operation_read(&fake, buffer, count);
    fakefd_operation_release(&fake);
  } else if (fakefd_is_fake(fd) || nx_epoll_is_fd(fd)) {
    errno = EBADF;
    result = -1;
  } else {
    result = nx_read_backend(fd, buffer, count);
  }
  const int saved = errno;
  nx_fd_route_source_unlock(stripe);
  errno = saved;
  return result;
}

long nx_write(int fd, const void *buffer, size_t count) {
  if (!buffer && count) { errno = EFAULT; return -1; }
  if (count > (size_t)LONG_MAX) { errno = EINVAL; return -1; }
  if (fd == STDOUT_FILENO || fd == STDERR_FILENO)
    return nx_standard_stream_write(fd, buffer, count);
  uint32_t stripe;
  if (!nx_fd_route_source_lock(fd, &stripe)) return -1;
  AssetPackOperation asset = {0};
  FakeFdOperation fake = {0};
  long result;
  if (asset_pack_operation_acquire(fd, &asset)) {
    asset_pack_operation_release(&asset);
    errno = EBADF;
    result = -1;
  } else if (fakefd_operation_acquire(fd, &fake)) {
    result = fakefd_operation_write(&fake, buffer, count);
    fakefd_operation_release(&fake);
  } else if (fakefd_is_fake(fd) || nx_epoll_is_fd(fd)) {
    errno = EBADF;
    result = -1;
  } else {
    result = nx_write_backend(fd, buffer, count);
  }
  const int saved = errno;
  nx_fd_route_source_unlock(stripe);
  errno = saved;
  return result;
}

long nx_pread(int fd, void *buffer, size_t count, long offset) {
  if (offset < 0 || count > (size_t)(INT64_MAX - offset)) {
    errno = EINVAL;
    return -1;
  }
  if (!buffer && count) { errno = EFAULT; return -1; }
  uint32_t stripe;
  if (!nx_fd_route_source_lock(fd, &stripe)) return -1;
  AssetPackOperation asset = {0};
  FakeFdOperation fake = {0};
  long result;
  if (asset_pack_operation_acquire(fd, &asset)) {
    result = asset_pack_operation_pread(&asset, buffer, count, offset);
    asset_pack_operation_release(&asset);
  } else if (fakefd_operation_acquire(fd, &fake)) {
    fakefd_operation_release(&fake);
    errno = ESPIPE;
    result = -1;
  } else if (fakefd_is_fake(fd)) {
    errno = EBADF;
    result = -1;
  } else {
    result = nx_pread_backend(fd, buffer, count, offset);
  }
  const int saved = errno;
  nx_fd_route_source_unlock(stripe);
  errno = saved;
  return result;
}

long nx_pwrite(int fd, const void *buffer, size_t count, long offset) {
  if (offset < 0 || count > (size_t)(INT64_MAX - offset)) {
    errno = EINVAL;
    return -1;
  }
  if (!buffer && count) { errno = EFAULT; return -1; }
  uint32_t stripe;
  if (!nx_fd_route_source_lock(fd, &stripe)) return -1;
  AssetPackOperation asset = {0};
  FakeFdOperation fake = {0};
  long result;
  if (asset_pack_operation_acquire(fd, &asset)) {
    asset_pack_operation_release(&asset);
    errno = EROFS;
    result = -1;
  } else if (fakefd_operation_acquire(fd, &fake)) {
    fakefd_operation_release(&fake);
    errno = ESPIPE;
    result = -1;
  } else if (fakefd_is_fake(fd)) {
    errno = EBADF;
    result = -1;
  } else {
    result = nx_pwrite_backend(fd, buffer, count, offset);
  }
  const int saved = errno;
  nx_fd_route_source_unlock(stripe);
  errno = saved;
  return result;
}
long nx_writev(int fd, const void *vectors_ptr, int count) {
  typedef struct { void *base; size_t length; } NxIovec;
  const NxIovec *vectors = vectors_ptr;
  if (!vectors || count < 0) { errno = EINVAL; return -1; }
  long total = 0;
  for (int i = 0; i < count; ++i) {
    long wrote = write_fake(fd, vectors[i].base, vectors[i].length);
    if (wrote < 0) return total ? total : -1;
    total += wrote;
    if ((size_t)wrote != vectors[i].length) break;
  }
  return total;
}
int nx_fdatasync(int fd) { return nx_fsync(fd); }
int nx_pipe2(int fds[2], int flags) { return fakefd_pipe2(fds, flags); }
int nx_ftruncate(int fd, long length) {
  if (length < 0) { errno = EINVAL; return -1; }
  uint32_t stripe;
  if (!nx_fd_route_source_lock(fd, &stripe)) return -1;
  NxFsdevFile *file = nx_positional_fsdev_file(fd);
  const int lookup_errno = errno;
  NxTrackedFileSize *entry = file
    ? nx_file_size_acquire(file, 1, 0) : NULL;
  if (!file) errno = lookup_errno;
  const int result = ftruncate(fd, length);
  const int saved_errno = errno;
  if (result == 0 && entry) {
    entry->logical_size = length;
    entry->physical_size = length;
    entry->initialized = 1;
  }
  nx_file_size_release(entry);
  nx_fd_route_source_unlock(stripe);
  errno = saved_errno;
  return result;
}
int nx_fsync(int fd) { return fsync(fd); }
int nx_regcomp(void *compiled, const char *pattern, int flags) {
  (void)compiled; (void)pattern; (void)flags; return 0;
}
int nx_regexec(const void *compiled, const char *text, size_t matches, void *match, int flags) {
  (void)compiled; (void)text; (void)matches; (void)match; (void)flags; return 1;
}
void nx_regfree(void *compiled) { (void)compiled; }
void nx_libc_init(void) { }
