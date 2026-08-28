/* Android NDK window, looper, sensor, and input shims used by libunity. */

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <switch.h>

#include "config.h"
#include "libc_shim.h"
#include "android_native_unity.h"

#define NX_WINDOW_MAX_DIMENSION 8192

/* opaque NDK types -> concrete libnx instances */
/* Opaque NDK types are declared in android_native_unity.h; ANativeWindow is
 * backed by the singleton libnx NWindow at runtime. */

static Mutex g_window_state_lock;
static Mutex g_window_apply_lock;
static u32 g_base_w = 1280, g_base_h = 720;
/* A zero requested pair has Android's documented meaning: restore the
 * window's base dimensions.  Keep it distinct from the physical display
 * metrics so a dock transition cannot silently discard Unity's choice. */
static u32 g_requested_w, g_requested_h;
static u32 g_applied_w, g_applied_h;
static uint64_t g_geometry_generation = 1;
static int g_geometry_dirty = 1;
static int g_operation_mode = -1;

static void window_effective_locked(u32 *width, u32 *height) {
  *width = g_requested_w ? g_requested_w : g_base_w;
  *height = g_requested_h ? g_requested_h : g_base_h;
}

static void window_applied_dimensions(u32 *width, u32 *height) {
  mutexLock(&g_window_state_lock);
  if (g_applied_w && g_applied_h) {
    *width = g_applied_w;
    *height = g_applied_h;
  } else {
    window_effective_locked(width, height);
  }
  mutexUnlock(&g_window_state_lock);
}

static uint64_t window_next_generation_locked(void) {
  g_geometry_generation++;
  if (!g_geometry_generation) g_geometry_generation = 1;
  return g_geometry_generation;
}

/* Caller owns g_window_apply_lock. */
static int apply_window_geometry_locked(void) {
  int status = 0;
  Result dimension_result = 0;
  Result transform_result = 0;
  Result query_result = 0;
  u32 requested_w = 0, requested_h = 0;
  u32 actual_w = 0, actual_h = 0;
  u64 slots = 0;
  uint64_t generation = 0;

  /* Never hold the state lock while libnx enters the Binder-backed NWindow
   * implementation. */
  mutexLock(&g_window_state_lock);
  window_effective_locked(&requested_w, &requested_h);
  generation = g_geometry_generation;
  mutexUnlock(&g_window_state_lock);

  NWindow *window = nwindowGetDefault();
  if (!window || !nwindowIsValid(window)) {
    status = -ENODEV;

    return status;
  }

  /* libnx's dimension/transform accessors deliberately do not lock, while
   * configure/release-buffer do use this mutex.  Hold the same mutex across
   * the zero-slot check and geometry transaction to close that race. */
  mutexLock(&window->mutex);
  slots = window->slots_configured;
  query_result = nwindowGetDimensions(window, &actual_w, &actual_h);
  if (R_FAILED(query_result)) {
    status = -EIO;
    mutexUnlock(&window->mutex);
    mutexLock(&g_window_state_lock);
    g_geometry_dirty = 1;
    mutexUnlock(&g_window_state_lock);

    return status;
  }
  if (actual_w == requested_w && actual_h == requested_h) {
    mutexUnlock(&window->mutex);
    mutexLock(&g_window_state_lock);
    g_applied_w = actual_w;
    g_applied_h = actual_h;
    u32 current_w = 0, current_h = 0;
    window_effective_locked(&current_w, &current_h);
    g_geometry_dirty = generation != g_geometry_generation ||
                       current_w != actual_w || current_h != actual_h;
    mutexUnlock(&g_window_state_lock);
    return 0;
  }
  if (slots) {
    status = -EBUSY;
    mutexUnlock(&window->mutex);
    mutexLock(&g_window_state_lock);
    g_geometry_dirty = 1;
    mutexUnlock(&g_window_state_lock);

    return status;
  }

  dimension_result = nwindowSetDimensions(window, requested_w, requested_h);
  if (R_SUCCEEDED(dimension_result))
    transform_result = nwindowSetTransform(window, 0u);
  status = R_FAILED(dimension_result) || R_FAILED(transform_result) ? -EIO : 0;
  query_result = nwindowGetDimensions(window, &actual_w, &actual_h);
  if (R_FAILED(query_result)) {
    actual_w = 0;
    actual_h = 0;
    if (!status) status = -EIO;
  } else if (R_SUCCEEDED(dimension_result) &&
             (actual_w != requested_w || actual_h != requested_h)) {
    status = -EIO;
  }
  slots = window->slots_configured;
  mutexUnlock(&window->mutex);

  mutexLock(&g_window_state_lock);
  if (R_SUCCEEDED(dimension_result)) {
    /* Dimensions changed even if the transform or verification failed. */
    g_applied_w = R_SUCCEEDED(query_result) ? actual_w : requested_w;
    g_applied_h = R_SUCCEEDED(query_result) ? actual_h : requested_h;
  }
  u32 current_w = 0, current_h = 0;
  window_effective_locked(&current_w, &current_h);
  g_geometry_dirty = status != 0 || generation != g_geometry_generation ||
                     current_w != g_applied_w || current_h != g_applied_h;
  mutexUnlock(&g_window_state_lock);

  return status;
}

int android_native_apply_window_geometry(void) {
  mutexLock(&g_window_apply_lock);
  const int status = apply_window_geometry_locked();
  mutexUnlock(&g_window_apply_lock);
  return status;
}

int android_native_update_mode(void) {
  const AppletOperationMode mode = appletGetOperationMode();
  const u32 base_w = mode == AppletOperationMode_Console ? 1920u : 1280u;
  const u32 base_h = mode == AppletOperationMode_Console ? 1080u : 720u;
  int live_change = 0;
  mutexLock(&g_window_state_lock);
  live_change = g_operation_mode >= 0 && g_operation_mode != (int)mode;
  if (g_operation_mode < 0 || live_change) {
    u32 old_w = 0, old_h = 0;
    window_effective_locked(&old_w, &old_h);
    g_base_w = base_w;
    g_base_h = base_h;
    g_operation_mode = (int)mode;
    u32 new_w = 0, new_h = 0;
    window_effective_locked(&new_w, &new_h);
    if (new_w != old_w || new_h != old_h) {
      g_geometry_dirty = 1;
      window_next_generation_locked();
    }
  }
  mutexUnlock(&g_window_state_lock);
  /* Android's Display/DisplayMetrics report the physical mode.  The native
   * window buffer dimensions below may still be changed independently by
   * ANativeWindow_setBuffersGeometry. */
  screen_width = (int)base_w;
  screen_height = (int)base_h;
  return live_change;
}

static ANativeWindow *native_window(void){
  NWindow *w = nwindowGetDefault();
  if (!w || !nwindowIsValid(w) || android_native_apply_window_geometry() != 0)
    return NULL;
  return (ANativeWindow *)w;
}
void     ANativeWindow_acquire(ANativeWindow *w){ (void)w; }                 /* singleton: refcount no-op */
void     ANativeWindow_release(ANativeWindow *w){ (void)w; }
ANativeWindow *ANativeWindow_fromSurface(void *env, void *surface){
  (void)env;
  /* Android's NULL Surface is the destruction signal used by Unity's
   * nativeRecreateGfxState path.  Never turn it back into the singleton or the
   * old Vulkan producer slots can survive a dock/undock teardown. */
  return surface ? native_window() : NULL;
}
int32_t ANativeWindow_getWidth(ANativeWindow *w) {
  (void)w;
  u32 width = 0, height = 0;
  window_applied_dimensions(&width, &height);
  return (int32_t)width;
}
int32_t ANativeWindow_getHeight(ANativeWindow *w) {
  (void)w;
  u32 width = 0, height = 0;
  window_applied_dimensions(&width, &height);
  return (int32_t)height;
}
int32_t  ANativeWindow_setBuffersGeometry(ANativeWindow *w, int32_t width, int32_t height, int32_t format){
  NWindow *window = (NWindow *)w;
  if (!window || !nwindowIsValid(window)) return -EINVAL;
  if ((width == 0) != (height == 0) || width < 0 || height < 0 ||
      width > NX_WINDOW_MAX_DIMENSION || height > NX_WINDOW_MAX_DIMENSION)
    return -EINVAL;
  /* These are the Android/native-window RGB formats Unity may request.  The
   * Vulkan swapchain still owns the actual NWindow buffer format. */
  if (format < 0 || format > 4) return -EINVAL;

  u32 old_requested_w = 0, old_requested_h = 0;
  /* Serialize the whole publish/apply/rollback transaction.  Otherwise two
   * failing callers can resurrect the earlier caller's rejected request. */
  mutexLock(&g_window_apply_lock);
  mutexLock(&g_window_state_lock);
  old_requested_w = g_requested_w;
  old_requested_h = g_requested_h;
  g_requested_w = (u32)width;
  g_requested_h = (u32)height;
  u32 effective_w = 0, effective_h = 0;
  window_effective_locked(&effective_w, &effective_h);
  if (effective_w != g_applied_w || effective_h != g_applied_h)
    g_geometry_dirty = 1;
  window_next_generation_locked();
  mutexUnlock(&g_window_state_lock);

  const int status = apply_window_geometry_locked();
  if (status != 0) {
    /* A failed Android request must not become a latent resize. */
    mutexLock(&g_window_state_lock);
    g_requested_w = old_requested_w;
    g_requested_h = old_requested_h;
    window_next_generation_locked();
    window_effective_locked(&effective_w, &effective_h);
    g_geometry_dirty = effective_w != g_applied_w ||
                       effective_h != g_applied_h;
    mutexUnlock(&g_window_state_lock);
    (void)apply_window_geometry_locked();
  }
  mutexUnlock(&g_window_apply_lock);
  return status;
}

/* Unity uses ALooper as a per-thread wait/wake primitive. */
#define ALOOPER_POLL_WAKE     (-1)
#define ALOOPER_POLL_CALLBACK (-2)
#define ALOOPER_POLL_TIMEOUT  (-3)
#define ALOOPER_POLL_ERROR    (-4)
#define ALOOPER_POLL_SLICE_MS 10
#define MAX_LOOPERS 32
#define MAX_LOOPER_FDS 32

typedef struct {
  int used, fd, ident, events;
  int (*callback)(int, int, void *);
  void *data;
  uint64_t generation;
} LooperFd;

typedef struct {
  int slot, fd, ident, events;
  int (*callback)(int, int, void *);
  void *data;
  uint64_t generation;
} LooperFdSnapshot;

typedef struct {
  int fd, ident, events;
  void *data;
} LooperResponse;

typedef struct {
  int slot, fd, events;
  int (*callback)(int, int, void *);
  void *data;
  uint64_t generation;
} LooperReadyCallback;

struct ALooper {
  CondVar cv;
  int refs;
  int allow_non_callbacks;
  u32 owner;
  int used;
  uint64_t generation;
  uint64_t next_fd_generation;
  uint64_t wake_generation;
  uint64_t consumed_wake_generation;
  int response_at;
  int response_count;
  LooperResponse responses[MAX_LOOPER_FDS];
  LooperFd fds[MAX_LOOPER_FDS];
};
static struct ALooper g_loopers[MAX_LOOPERS];
/* libnx Mutex and CondVar objects are valid when statically zero-initialized.
 * Keeping all looper state behind this one lock also makes a slot's lifetime
 * and its fd table one atomic state machine. */
static Mutex g_loopers_lock;
static CondVar g_looper_key_cv;
static pthread_key_t g_looper_thread_key;
enum {
  LOOPER_KEY_UNINITIALIZED = 0,
  LOOPER_KEY_INITIALIZING,
  LOOPER_KEY_READY,
  LOOPER_KEY_FAILED,
};
static int g_looper_key_state;

typedef struct {
  struct ALooper *looper;
  uint64_t generation;
} LooperThreadCookie;
/* Host C TLS remains available while guest TPIDR_EL0 is installed. */
static _Thread_local LooperThreadCookie g_looper_thread_cookie;

static u32 cur_tid(void){ return (u32)(uintptr_t)threadGetCurHandle(); }

static uint64_t looper_bump_generation(uint64_t value) {
  value++;
  return value ? value : 1;
}

static uint64_t looper_next_fd_generation_locked(struct ALooper *L) {
  L->next_fd_generation = looper_bump_generation(L->next_fd_generation);
  return L->next_fd_generation;
}

static void looper_clear_fd_locked(struct ALooper *L, int slot) {
  LooperFd *entry = &L->fds[slot];
  entry->used = 0;
  entry->fd = -1;
  entry->ident = 0;
  entry->events = 0;
  entry->callback = NULL;
  entry->data = NULL;
  entry->generation = looper_next_fd_generation_locked(L);
}

static void looper_clear_fds_locked(struct ALooper *L) {
  for (int i = 0; i < MAX_LOOPER_FDS; ++i) looper_clear_fd_locked(L, i);
}

static void looper_notify_locked(struct ALooper *L) {
  (void)condvarWakeAll(&L->cv);
}

static void looper_signal_locked(struct ALooper *L) {
  L->wake_generation = looper_bump_generation(L->wake_generation);
  looper_notify_locked(L);
}

static int looper_has_wake_locked(const struct ALooper *L) {
  return L->wake_generation != L->consumed_wake_generation;
}

static void looper_clear_responses_locked(struct ALooper *L) {
  memset(L->responses, 0, sizeof(L->responses));
  L->response_at = 0;
  L->response_count = 0;
}

static int looper_queue_response_locked(struct ALooper *L,
                                        const LooperFdSnapshot *snapshot,
                                        int events) {
  if (L->response_count >= MAX_LOOPER_FDS) return 0;
  L->responses[L->response_count++] = (LooperResponse){
    snapshot->fd, snapshot->ident, events, snapshot->data
  };
  return 1;
}

static int looper_take_response_locked(struct ALooper *L,
                                       LooperResponse *response) {
  if (L->response_at >= L->response_count) {
    looper_clear_responses_locked(L);
    return 0;
  }
  *response = L->responses[L->response_at++];
  if (L->response_at == L->response_count)
    looper_clear_responses_locked(L);
  return 1;
}

static struct ALooper *looper_handle_locked(ALooper *looper) {
  const uintptr_t address = (uintptr_t)looper;
  const uintptr_t base = (uintptr_t)&g_loopers[0];
  const uintptr_t end = (uintptr_t)&g_loopers[MAX_LOOPERS];
  if (address < base || address >= end) return NULL;
  const uintptr_t offset = address - base;
  if (offset % sizeof(g_loopers[0])) return NULL;
  return &g_loopers[offset / sizeof(g_loopers[0])];
}

static int looper_instance_live_locked(const struct ALooper *L, uint64_t generation) {
  return L && L->used && L->generation == generation;
}

static struct ALooper *looper_current_locked(void) {
  struct ALooper *L = looper_handle_locked(
    (ALooper *)g_looper_thread_cookie.looper);
  if (!looper_instance_live_locked(L, g_looper_thread_cookie.generation) ||
      L->owner != cur_tid())
    return NULL;
  return L;
}

static void looper_retire_locked(struct ALooper *L) {
  looper_clear_fds_locked(L);
  looper_clear_responses_locked(L);
  L->owner = 0;
  L->refs = 0;
  L->allow_non_callbacks = 0;
  L->used = 0;
  L->generation = looper_bump_generation(L->generation);
  L->wake_generation = 0;
  L->consumed_wake_generation = 0;
  looper_notify_locked(L);
}

static void looper_disassociate_thread_locked(struct ALooper *L) {
  L->owner = 0;
  L->generation = looper_bump_generation(L->generation);
  if (L->refs > 0) --L->refs;
  if (L->refs == 0) looper_retire_locked(L);
  else looper_notify_locked(L);
}

static void looper_thread_destructor(void *opaque) {
  LooperThreadCookie *cookie = (LooperThreadCookie *)opaque;
  if (!cookie) return;
  mutexLock(&g_loopers_lock);
  struct ALooper *L = looper_handle_locked((ALooper *)cookie->looper);
  if (looper_instance_live_locked(L, cookie->generation))
    looper_disassociate_thread_locked(L);
  cookie->looper = NULL;
  cookie->generation = 0;
  mutexUnlock(&g_loopers_lock);
}

static int looper_key_ensure(void) {
  mutexLock(&g_loopers_lock);
  while (g_looper_key_state == LOOPER_KEY_INITIALIZING) {
    const Result wait_result = condvarWait(&g_looper_key_cv,
                                           &g_loopers_lock);
    if (R_FAILED(wait_result)) {
      mutexUnlock(&g_loopers_lock);
      return 0;
    }
  }
  if (g_looper_key_state == LOOPER_KEY_READY) {
    mutexUnlock(&g_loopers_lock);
    return 1;
  }
  if (g_looper_key_state == LOOPER_KEY_FAILED) {
    mutexUnlock(&g_loopers_lock);
    return 0;
  }
  g_looper_key_state = LOOPER_KEY_INITIALIZING;
  mutexUnlock(&g_loopers_lock);

  pthread_key_t key;
  const int result = pthread_key_create(&key, looper_thread_destructor);

  mutexLock(&g_loopers_lock);
  if (!result) g_looper_thread_key = key;
  g_looper_key_state = result ? LOOPER_KEY_FAILED : LOOPER_KEY_READY;
  (void)condvarWakeAll(&g_looper_key_cv);
  mutexUnlock(&g_loopers_lock);
  return result == 0;
}

static struct ALooper *looper_for_locked(u32 tid, int create, int opts) {
  struct ALooper *current = looper_current_locked();
  if (current || !create) return current;
  for (int i = 0; i < MAX_LOOPERS; ++i) {
    struct ALooper *L = &g_loopers[i];
    if (L->used) continue;
    /* Invalidate any waiter from the previous occupant before publishing the
     * slot to its new owner.  Registrations are cleared on both retirement and
     * allocation so no callback/data pointer can leak across owners. */
    L->generation = looper_bump_generation(L->generation);
    looper_clear_fds_locked(L);
    L->owner = tid;
    L->refs = 1;
    L->allow_non_callbacks = (opts & 1) != 0;
    L->wake_generation = 0;
    L->consumed_wake_generation = 0;
    looper_clear_responses_locked(L);
    L->used = 1;
    return L;
  }
  return NULL;
}

static struct ALooper *looper_for(u32 tid, int create, int opts) {
  mutexLock(&g_loopers_lock);
  struct ALooper *L = looper_for_locked(tid, create, opts);
  mutexUnlock(&g_loopers_lock);
  return L;
}

ALooper *ALooper_prepare(int opts) {
  if (!looper_key_ensure()) return NULL;
  mutexLock(&g_loopers_lock);
  struct ALooper *L = looper_current_locked();
  const int new_association = L == NULL;
  if (new_association) {
    L = looper_for_locked(cur_tid(), 1, opts);
  }
  if (L && new_association) {
    g_looper_thread_cookie.looper = L;
    g_looper_thread_cookie.generation = L->generation;
    if (pthread_setspecific(g_looper_thread_key,
                            &g_looper_thread_cookie) != 0) {
      looper_retire_locked(L);
      g_looper_thread_cookie.looper = NULL;
      g_looper_thread_cookie.generation = 0;
      L = NULL;
    }
  }
  mutexUnlock(&g_loopers_lock);
  return (ALooper *)L;
}

ALooper *ALooper_forThread(void) {
  return (ALooper *)looper_for(cur_tid(), 0, 0);
}

void ALooper_acquire(ALooper *looper) {
  mutexLock(&g_loopers_lock);
  struct ALooper *L = looper_handle_locked(looper);
  if (L && L->used && L->refs < INT_MAX) L->refs++;
  mutexUnlock(&g_loopers_lock);
}

void ALooper_release(ALooper *looper) {
  mutexLock(&g_loopers_lock);
  struct ALooper *L = looper_handle_locked(looper);
  if (L && L->used && L->refs > 0 && --L->refs == 0)
    looper_retire_locked(L);
  mutexUnlock(&g_loopers_lock);
}

void ALooper_wake(ALooper *looper){
  mutexLock(&g_loopers_lock);
  struct ALooper *L = looper_handle_locked(looper);
  if (L && L->used) looper_signal_locked(L);
  mutexUnlock(&g_loopers_lock);
}

int ALooper_addFd(ALooper *looper, int fd, int ident, int events,
                  int (*callback)(int, int, void *), void *data) {
  if (fd < 0 || (!callback && ident < 0)) return -1;
  if (fakefd_is_fake(fd)) {
    if (!fakefd_is_live(fd)) return -1;
  } else if (fcntl(fd, F_GETFD) < 0) {
    return -1;
  }
  mutexLock(&g_loopers_lock);
  struct ALooper *L = looper_handle_locked(looper);
  if (!L || !L->used) {
    mutexUnlock(&g_loopers_lock);
    return -1;
  }
  if (!callback && !L->allow_non_callbacks) {
    mutexUnlock(&g_loopers_lock);
    return -1;
  }
  int free_slot = -1;
  for (int i = 0; i < MAX_LOOPER_FDS; ++i) {
    if (L->fds[i].used && L->fds[i].fd == fd) { free_slot = i; break; }
    if (!L->fds[i].used && free_slot < 0) free_slot = i;
  }
  if (free_slot >= 0) {
    LooperFd *entry = &L->fds[free_slot];
    entry->used = 1;
    entry->fd = fd;
    entry->ident = ident;
    entry->events = events;
    entry->callback = callback;
    entry->data = data;
    entry->generation = looper_next_fd_generation_locked(L);
    looper_notify_locked(L);
  }
  mutexUnlock(&g_loopers_lock);
  return free_slot >= 0 ? 1 : -1;
}

int ALooper_removeFd(ALooper *looper, int fd) {
  int removed = 0;
  mutexLock(&g_loopers_lock);
  struct ALooper *L = looper_handle_locked(looper);
  if (L && L->used) {
    for (int i = 0; i < MAX_LOOPER_FDS; ++i) {
      if (!L->fds[i].used || L->fds[i].fd != fd) continue;
      looper_clear_fd_locked(L, i);
      looper_notify_locked(L);
      removed = 1;
      break;
    }
  }
  mutexUnlock(&g_loopers_lock);
  return removed;
}

void android_native_looper_forget_fd(int fd) {
  mutexLock(&g_loopers_lock);
  for (int looper = 0; looper < MAX_LOOPERS; ++looper) {
    struct ALooper *L = &g_loopers[looper];
    if (!L->used) continue;
    int changed = 0;
    for (int slot = 0; slot < MAX_LOOPER_FDS; ++slot) {
      if (!L->fds[slot].used || L->fds[slot].fd != fd) continue;
      looper_clear_fd_locked(L, slot);
      changed = 1;
    }
    if (changed) looper_notify_locked(L);
  }
  mutexUnlock(&g_loopers_lock);
}

static uint64_t looper_now_ns(void) {
  return armTicksToNs(armGetSystemTick());
}

static int looper_snapshot_live_locked(struct ALooper *L, uint64_t looper_generation,
                                       const LooperFdSnapshot *snapshot) {
  if (!looper_instance_live_locked(L, looper_generation)) return 0;
  if (snapshot->slot < 0 || snapshot->slot >= MAX_LOOPER_FDS) return 0;
  const LooperFd *entry = &L->fds[snapshot->slot];
  return entry->used && entry->fd == snapshot->fd &&
         entry->generation == snapshot->generation;
}

static int looper_revents(short revents) {
  int events = 0;
  if (revents & POLLIN) events |= 1;
  if (revents & POLLOUT) events |= 2;
  if (revents & POLLERR) events |= 4;
  if (revents & POLLHUP) events |= 8;
  if (revents & POLLNVAL) events |= 16;
  return events;
}

int ALooper_pollOnce(int timeoutMillis, int *outFd, int *outEvents, void **outData){
  if (outFd) *outFd = 0;
  if (outEvents) *outEvents = 0;
  if (outData) *outData = NULL;

  const uint64_t start_ns = looper_now_ns();
  const uint64_t timeout_ns = timeoutMillis < 0 ? UINT64_MAX
    : (uint64_t)(unsigned)timeoutMillis * UINT64_C(1000000);

  mutexLock(&g_loopers_lock);
  /* Android requires prepare() first.  Polling must not silently allocate a
   * looper with different callback permissions. */
  struct ALooper *L = looper_for_locked(cur_tid(), 0, 0);
  if (!L) {
    mutexUnlock(&g_loopers_lock);
    return ALOOPER_POLL_ERROR;
  }
  const uint64_t looper_generation = L->generation;
  LooperResponse pending;
  if (looper_take_response_locked(L, &pending)) {
    mutexUnlock(&g_loopers_lock);
    if (outFd) *outFd = pending.fd;
    if (outEvents) *outEvents = pending.events;
    if (outData) *outData = pending.data;
    return pending.ident;
  }
  mutexUnlock(&g_loopers_lock);

  for (;;) {
    LooperFdSnapshot snapshot[MAX_LOOPER_FDS];
    LooperReadyCallback ready_callbacks[MAX_LOOPER_FDS];
    struct pollfd poll_fds[MAX_LOOPER_FDS];
    int fd_count = 0;

    mutexLock(&g_loopers_lock);
    if (!looper_instance_live_locked(L, looper_generation)) {
      mutexUnlock(&g_loopers_lock);
      return ALOOPER_POLL_ERROR;
    }
    if (looper_take_response_locked(L, &pending)) {
      mutexUnlock(&g_loopers_lock);
      if (outFd) *outFd = pending.fd;
      if (outEvents) *outEvents = pending.events;
      if (outData) *outData = pending.data;
      return pending.ident;
    }
    const int wake_before_poll = looper_has_wake_locked(L);
    for (int i = 0; i < MAX_LOOPER_FDS; ++i) {
      const LooperFd *entry = &L->fds[i];
      if (!entry->used) continue;
      snapshot[fd_count] = (LooperFdSnapshot){
        i, entry->fd, entry->ident, entry->events,
        entry->callback, entry->data, entry->generation
      };
      poll_fds[fd_count].fd = entry->fd;
      poll_fds[fd_count].events = 0;
      if (entry->events & 1) poll_fds[fd_count].events |= POLLIN;
      if (entry->events & 2) poll_fds[fd_count].events |= POLLOUT;
      poll_fds[fd_count].revents = 0;
      fd_count++;
    }

    const uint64_t elapsed_ns = looper_now_ns() - start_ns;
    if (!fd_count) {
      if (wake_before_poll) {
        L->consumed_wake_generation = L->wake_generation;
        mutexUnlock(&g_loopers_lock);
        return ALOOPER_POLL_WAKE;
      }
      if (timeoutMillis == 0 ||
          (timeout_ns != UINT64_MAX && elapsed_ns >= timeout_ns)) {
        mutexUnlock(&g_loopers_lock);
        return ALOOPER_POLL_TIMEOUT;
      }
      const uint64_t wait_ns = timeout_ns == UINT64_MAX
        ? UINT64_MAX : timeout_ns - elapsed_ns;
      const Result wait_result = condvarWaitTimeout(&L->cv, &g_loopers_lock,
                                                    wait_ns);
      mutexUnlock(&g_loopers_lock);
      if (R_FAILED(wait_result) &&
          R_VALUE(wait_result) != R_VALUE(KERNELRESULT(TimedOut)))
        return ALOOPER_POLL_ERROR;
      /* A condition variable can wake spuriously.  Recheck the generation,
       * wake flag, registrations, and original monotonic deadline. */
      continue;
    }

    int poll_timeout;
    if (wake_before_poll || timeoutMillis == 0 ||
        (timeout_ns != UINT64_MAX && elapsed_ns >= timeout_ns)) {
      /* A wake or expired deadline still gets one final zero-time fd probe;
       * ready descriptors and callbacks take precedence over WAKE/TIMEOUT. */
      poll_timeout = 0;
    } else if (timeout_ns == UINT64_MAX) {
      poll_timeout = ALOOPER_POLL_SLICE_MS;
    } else {
      uint64_t remaining_ns = timeout_ns - elapsed_ns;
      uint64_t slice_ns = UINT64_C(ALOOPER_POLL_SLICE_MS) * UINT64_C(1000000);
      if (remaining_ns < slice_ns) slice_ns = remaining_ns;
      poll_timeout = (int)((slice_ns + UINT64_C(999999)) / UINT64_C(1000000));
      if (poll_timeout < 1) poll_timeout = 1;
    }
    mutexUnlock(&g_loopers_lock);

    /* poll_fake preserves the common eight-byte pollfd layout while routing
     * synthetic pipe/eventfd descriptors around BSD's native poll(). */
    int ready = poll_fake(poll_fds, (unsigned long)fd_count, poll_timeout);
    if (ready < 0 && errno == EINTR) {
      /* AOSP treats an interrupted wait as a wake instead of restarting an
       * already-expired finite deadline indefinitely. */
      mutexLock(&g_loopers_lock);
      if (looper_instance_live_locked(L, looper_generation))
        L->consumed_wake_generation = L->wake_generation;
      mutexUnlock(&g_loopers_lock);
      return ALOOPER_POLL_WAKE;
    }
    if (ready < 0) return ALOOPER_POLL_ERROR;

    mutexLock(&g_loopers_lock);
    if (!looper_instance_live_locked(L, looper_generation)) {
      mutexUnlock(&g_loopers_lock);
      return ALOOPER_POLL_ERROR;
    }
    /* Consume only wakes observed before callbacks begin.  A callback which
     * calls ALooper_wake() schedules the following poll instead. */
    const uint64_t consume_wake_through = L->wake_generation;
    int ready_callback_count = 0;
    if (ready > 0) {
      for (int i = 0; i < fd_count; ++i) {
        if (!poll_fds[i].revents ||
            !looper_snapshot_live_locked(L, looper_generation, &snapshot[i])) continue;
        const int events = looper_revents(poll_fds[i].revents);
        if (!snapshot[i].callback) {
          (void)looper_queue_response_locked(L, &snapshot[i], events);
          continue;
        }
        ready_callbacks[ready_callback_count++] = (LooperReadyCallback){
          snapshot[i].slot, snapshot[i].fd, events, snapshot[i].callback,
          snapshot[i].data, snapshot[i].generation
        };
      }
    }

    if (ready_callback_count) {
      /* Copy the complete ready batch before guest code runs.  Android allows
       * one already-signalled callback even if an earlier callback removes or
       * replaces its registration. */
      mutexUnlock(&g_loopers_lock);
      for (int i = 0; i < ready_callback_count; ++i) {
        const LooperReadyCallback *callback = &ready_callbacks[i];
        const int keep = callback->callback(callback->fd, callback->events,
                                            callback->data);
        if (!keep) {
          mutexLock(&g_loopers_lock);
          if (looper_instance_live_locked(L, looper_generation)) {
            LooperFd *entry = &L->fds[callback->slot];
            /* A return-zero callback may remove only the exact registration
             * copied into this batch, never a replacement made by a callback. */
            if (entry->used && entry->fd == callback->fd &&
                entry->generation == callback->generation)
              looper_clear_fd_locked(L, callback->slot);
          }
          mutexUnlock(&g_loopers_lock);
        }
      }
      mutexLock(&g_loopers_lock);
      if (!looper_instance_live_locked(L, looper_generation)) {
        mutexUnlock(&g_loopers_lock);
        return ALOOPER_POLL_CALLBACK;
      }
    }

    if (looper_take_response_locked(L, &pending)) {
      L->consumed_wake_generation = consume_wake_through;
      mutexUnlock(&g_loopers_lock);
      if (outFd) *outFd = pending.fd;
      if (outEvents) *outEvents = pending.events;
      if (outData) *outData = pending.data;
      return pending.ident;
    }
    if (ready_callback_count) {
      L->consumed_wake_generation = consume_wake_through;
      mutexUnlock(&g_loopers_lock);
      return ALOOPER_POLL_CALLBACK;
    }
    if (looper_has_wake_locked(L)) {
      L->consumed_wake_generation = L->wake_generation;
      mutexUnlock(&g_loopers_lock);
      return ALOOPER_POLL_WAKE;
    }
    mutexUnlock(&g_loopers_lock);

    if (timeoutMillis == 0) return ALOOPER_POLL_TIMEOUT;
    if (timeout_ns != UINT64_MAX && looper_now_ns() - start_ns >= timeout_ns)
      return ALOOPER_POLL_TIMEOUT;
    /* The slice elapsed or every ready entry was concurrently invalidated.
     * Rebuild the fd set and keep waiting against the original deadline. */
  }
}

int android_native_looper_pump_callbacks(unsigned max_batches) {
  mutexLock(&g_loopers_lock);
  struct ALooper *L = looper_current_locked();
  /* A generic host pump has no owner for identifier responses.  It may drive
   * only loopers which were prepared without ALLOW_NON_CALLBACKS. */
  const int callback_only = L && !L->allow_non_callbacks;
  mutexUnlock(&g_loopers_lock);
  if (!callback_only) return 0;

  unsigned batches = 0;
  for (unsigned attempt = 0; attempt < max_batches; ++attempt) {
    const int result = ALooper_pollOnce(0, NULL, NULL, NULL);
    if (result == ALOOPER_POLL_CALLBACK) {
      batches++;
      continue;
    }
    if (result == ALOOPER_POLL_WAKE) continue;
    /* TIMEOUT/ERROR end the zero-time drain.  A nonnegative identifier is
     * impossible for a callback-only looper and is never consumed further. */
    break;
  }
  return (int)batches;
}
/* Sensors are unavailable. */
void *ASensorManager_getInstance(void){ static int x; return &x; }
int   ASensorManager_getSensorList(void *m, void **list){ (void)m; if(list)*list=NULL; return 0; }
void *ASensorManager_getDefaultSensor(void *m, int type){ (void)m;(void)type; return NULL; }
void *ASensorManager_createEventQueue(void *m, void *looper, int ident, void *cb, void *data){
  (void)m;(void)looper;(void)ident;(void)cb;(void)data; static int q; return &q; }
int   ASensorManager_destroyEventQueue(void *m, void *q){ (void)m;(void)q; return 0; }

int   ASensorEventQueue_enableSensor (void *q, const void *s){ (void)q;(void)s; return -1; }
int   ASensorEventQueue_disableSensor(void *q, const void *s){ (void)q;(void)s; return 0; }
int   ASensorEventQueue_setEventRate (void *q, const void *s, int32_t us){ (void)q;(void)s;(void)us; return 0; }
int   ASensorEventQueue_getEvents    (void *q, void *ev, size_t n){ (void)q;(void)ev;(void)n; return 0; }
int   ASensorEventQueue_hasEvents    (void *q){ (void)q; return 0; }

const char *ASensor_getName      (const void *s){ (void)s; return ""; }
const char *ASensor_getVendor    (const void *s){ (void)s; return ""; }
int         ASensor_getType      (const void *s){ (void)s; return 0; }
float       ASensor_getResolution(const void *s){ (void)s; return 0.0f; }
int         ASensor_getMinDelay  (const void *s){ (void)s; return 0; }

void android_get_orientation(float *x, float *y, float *z){
  if (x) *x = 0.0f;
  if (y) *y = 0.0f;
  if (z) *z = 0.0f;
}

/* Handheld touch is injected directly. In docked mode, the left stick controls
 * a virtual cursor and A taps it. */
#include "unity_input.h"

static PadState g_pad;
static HidTouchScreenState g_touch;
static int   g_prev_touch = 0;        /* pointers down last frame */
static float g_cursor_x = 640, g_cursor_y = 360;
static float g_last_tx = 640, g_last_ty = 360;
static int   g_prev_a = 0;

#define VIBRATION_HANDLE_CAP 8
static HidVibrationDeviceHandle g_vibration_handles[VIBRATION_HANDLE_CAP];
static int g_vibration_count;
static int g_vibration_active;
static u64 g_vibration_deadline;
static Mutex g_vibration_lock;

static void vibration_add(HidNpadIdType id, HidNpadStyleTag style, int count) {
  if (g_vibration_count + count > VIBRATION_HANDLE_CAP) return;
  if (R_SUCCEEDED(hidInitializeVibrationDevices(
        &g_vibration_handles[g_vibration_count], count, id, style)))
    g_vibration_count += count;
}

static void vibration_send(const HidVibrationValue *value) {
  bool permitted = false;
  if (R_FAILED(hidIsVibrationPermitted(&permitted)) || !permitted) return;
  for (int i = 0; i < g_vibration_count; i++) {
    bool mounted = false;
    if (R_SUCCEEDED(hidIsVibrationDeviceMounted(g_vibration_handles[i], &mounted)) && mounted)
      hidSendVibrationValue(g_vibration_handles[i], value);
  }
}

static void vibration_start(int length_ms, float low, float high) {
  if (length_ms <= 0) return;
  if (length_ms > 1000) length_ms = 1000;
  HidVibrationValue value = { low, 160.0f, high, 320.0f };
  mutexLock(&g_vibration_lock);
  vibration_send(&value);
  g_vibration_deadline = armGetSystemTick() + armNsToTicks((u64)length_ms * 1000000ULL);
  g_vibration_active = 1;
  mutexUnlock(&g_vibration_lock);
}

void android_native_input_init(void){
  padConfigureInput(1, HidNpadStyleSet_NpadStandard);
  padInitializeDefault(&g_pad);
  hidInitializeTouchScreen();
  mutexInit(&g_vibration_lock);
  vibration_add(HidNpadIdType_No1, HidNpadStyleTag_NpadFullKey, 2);
  vibration_add(HidNpadIdType_No1, HidNpadStyleTag_NpadJoyDual, 2);
  vibration_add(HidNpadIdType_No1, HidNpadStyleTag_NpadJoyLeft, 1);
  vibration_add(HidNpadIdType_No1, HidNpadStyleTag_NpadJoyRight, 1);
  vibration_add(HidNpadIdType_Handheld, HidNpadStyleTag_NpadHandheld, 2);
}

void android_native_vibration_standard(int length_ms) {
  vibration_start(length_ms, 0.55f, 0.40f);
}

void android_native_vibration_haptic(int style) {
  static const struct { uint16_t ms; float low, high; } effects[] = {
    { 100, 0.90f, 0.65f }, { 75, 0.65f, 0.50f }, { 45, 0.35f, 0.30f },
    { 55, 0.45f, 0.80f },  { 85, 0.35f, 0.25f }, { 35, 0.30f, 0.65f },
    { 25, 0.20f, 0.45f },  { 120, 0.70f, 0.35f }, { 100, 0.45f, 0.70f },
    { 130, 0.70f, 0.45f }, { 180, 0.90f, 0.70f }, { 20, 0.18f, 0.35f },
    { 90, 0.25f, 0.65f },  { 140, 0.45f, 0.25f }, { 250, 0.40f, 0.30f },
    { 60, 0.50f, 0.40f },  { 75, 0.40f, 0.25f }, { 220, 0.35f, 0.18f },
    { 15, 0.12f, 0.22f },
  };
  unsigned index = (unsigned)style;
  if (index >= sizeof(effects) / sizeof(effects[0])) index = 15;
  vibration_start(effects[index].ms, effects[index].low, effects[index].high);
}

void android_native_vibration_update(void) {
  mutexLock(&g_vibration_lock);
  if (g_vibration_active && armGetSystemTick() >= g_vibration_deadline) {
    HidVibrationValue stop = { 0.0f, 160.0f, 0.0f, 320.0f };
    vibration_send(&stop);
    g_vibration_active = 0;
  }
  mutexUnlock(&g_vibration_lock);
}

void android_native_vibration_shutdown(void) {
  HidVibrationValue stop = { 0.0f, 160.0f, 0.0f, 320.0f };
  mutexLock(&g_vibration_lock);
  vibration_send(&stop);
  g_vibration_active = 0;
  mutexUnlock(&g_vibration_lock);
}

/* nativeInjectEvent(env, thiz, event, flags). */
typedef uint8_t (*inject_fn)(void*,void*,void*,int);

void android_native_feed_hid(inject_fn inject, void *env, void *thiz){
  padUpdate(&g_pad);
  u32 buffer_w = 0, buffer_h = 0;
  window_applied_dimensions(&buffer_w, &buffer_h);

  int n = hidGetTouchScreenStates(&g_touch, 1);
  if (n > 0 && g_touch.count > 0){
    int   ids[UI_MAX_POINTERS]; float xs[UI_MAX_POINTERS]; float ys[UI_MAX_POINTERS];
    int c = g_touch.count > UI_MAX_POINTERS ? UI_MAX_POINTERS : g_touch.count;
    const float PANEL_W = 1280.0f, PANEL_H = 720.0f;
    for (int i=0;i<c;i++){ ids[i]=(int)g_touch.touches[i].finger_id;
      float px=(float)g_touch.touches[i].x, py=(float)g_touch.touches[i].y;
      xs[i]=px * ((float)buffer_w / PANEL_W);
      ys[i]=py * ((float)buffer_h / PANEL_H);
    }
    g_last_tx = xs[0]; g_last_ty = ys[0];
    int action = g_prev_touch ? AMOTION_ACTION_MOVE : AMOTION_ACTION_DOWN;
    inject(env, thiz, unity_motionevent(action, c, ids, xs, ys), 0);
    g_prev_touch = c;
    return;
  }
  if (g_prev_touch){
    int   ids[1]={0}; float xs[1]={g_last_tx}, ys[1]={g_last_ty};
    inject(env, thiz, unity_motionevent(AMOTION_ACTION_UP, 1, ids, xs, ys), 0);
    g_prev_touch = 0;
    return;
  }

  HidAnalogStickState ls = padGetStickPos(&g_pad, 0);
  g_cursor_x += (ls.x / 32767.0f) * 14.0f;
  g_cursor_y -= (ls.y / 32767.0f) * 14.0f;
  if (g_cursor_x < 0) g_cursor_x = 0;
  if (g_cursor_x > buffer_w) g_cursor_x = buffer_w;
  if (g_cursor_y < 0) g_cursor_y = 0;
  if (g_cursor_y > buffer_h) g_cursor_y = buffer_h;

  int a = (padGetButtons(&g_pad) & HidNpadButton_A) ? 1 : 0;
  int ids[1]={0}; float xs[1]={g_cursor_x}, ys[1]={g_cursor_y};
  if (a && !g_prev_a)      inject(env, thiz, unity_motionevent(AMOTION_ACTION_DOWN, 1, ids, xs, ys), 0);
  else if (a && g_prev_a)  inject(env, thiz, unity_motionevent(AMOTION_ACTION_MOVE, 1, ids, xs, ys), 0);
  else if (!a && g_prev_a) inject(env, thiz, unity_motionevent(AMOTION_ACTION_UP,   1, ids, xs, ys), 0);
  g_prev_a = a;

  static int prev_b = 0;
  int b = (padGetButtons(&g_pad) & HidNpadButton_B) ? 1 : 0;
  if (b && !prev_b) inject(env, thiz, unity_keyevent(AKEY_ACTION_DOWN, AKEYCODE_BACK), 0);
  if (!b && prev_b) inject(env, thiz, unity_keyevent(AKEY_ACTION_UP,   AKEYCODE_BACK), 0);
  prev_b = b;
}
