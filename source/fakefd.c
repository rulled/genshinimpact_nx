/* In-process file-descriptor pipes used by the bionic shim. */

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <limits.h>
#include <poll.h>
#include <fcntl.h>
#include <unistd.h>
#include <switch.h>

#include "config.h"
#include "libc_shim.h"

#define FAKE_FD_BASE 0x40000000
#define MAX_FAKE_FDS 32
#define MAX_FAKE_ALIASES 32
#define PIPE_ATOMIC_CAP 4096u
#define PIPE_DEFAULT_CAP 4096u
#define PIPE_MAX_CAP (1024u * 1024u)
#define FAKE_FD_SELECT_BASE (1024 - MAX_FAKE_FDS)
#define BIONIC_O_NONBLOCK 0x800
#define BIONIC_O_CLOEXEC  0x80000

enum { FD_PIPE_R = 1, FD_PIPE_W, FD_EVENT, FD_RANDOM };

typedef struct Pipe Pipe;
typedef struct {
  Pipe *pipe;
  int kind;
  int nonblock; /* open-file-description state, shared by dup(). */
  unsigned refs; /* descriptors plus active operations. */
} FakeOpen;

struct Pipe {
  uint8_t *buf;
  size_t head, len, capacity;
  int readers, writers;
  uint64_t event_value;
  int event_semaphore;
  FakeOpen read_open, write_open, event_open, random_open;
};

typedef struct { FakeOpen *open; int cloexec; } FakeFd;
typedef struct { int used, fd; FakeFd descriptor; } FakeAlias;

static FakeFd g_fds[MAX_FAKE_FDS];
static FakeAlias g_aliases[MAX_FAKE_ALIASES];
/* libnx documents zero as the static initial state for both primitives. */
static Mutex g_lock;
static CondVar g_cond;

static FakeAlias *alias_locked(int fd) {
  for (int i = 0; i < MAX_FAKE_ALIASES; i++)
    if (g_aliases[i].used == 1 && g_aliases[i].fd == fd) return &g_aliases[i];
  return NULL;
}

static FakeFd *descriptor_locked(int fd, FakeAlias **alias_out) {
  if (alias_out) *alias_out = NULL;
  if (fd >= FAKE_FD_BASE && fd < FAKE_FD_BASE + MAX_FAKE_FDS) {
    FakeFd *descriptor = &g_fds[fd - FAKE_FD_BASE];
    return descriptor->open ? descriptor : NULL;
  }
  FakeAlias *alias = alias_locked(fd);
  if (!alias) return NULL;
  if (alias_out) *alias_out = alias;
  return &alias->descriptor;
}

static void open_retain_locked(FakeOpen *open) { open->refs++; }

static void open_release_locked(FakeOpen *open) {
  if (--open->refs != 0) return;
  Pipe *pipe = open->pipe;
  if (open->kind == FD_PIPE_R) pipe->readers = 0;
  else pipe->writers = 0;
  condvarWakeAll(&g_cond);
  if (!pipe->readers && !pipe->writers) {
    free(pipe->buf);
    free(pipe);
  }
}

static int alloc_high_locked(void) {
  for (int i = 0; i < MAX_FAKE_FDS; i++)
    if (!g_fds[i].open) return i;
  return -1;
}

static FakeAlias *reserve_alias_locked(int fd) {
  for (int i = 0; i < MAX_FAKE_ALIASES; i++) if (!g_aliases[i].used) {
    /* State 2 reserves capacity without changing target classification while
     * the public dup2 route still owns the replacement transition. */
    g_aliases[i].used = 2;
    g_aliases[i].fd = fd;
    return &g_aliases[i];
  }
  return NULL;
}

int fakefd_is_fake(int fd) {
  if (fd >= FAKE_FD_BASE && fd < FAKE_FD_BASE + MAX_FAKE_FDS) return 1;
  mutexLock(&g_lock);
  int result = alias_locked(fd) != NULL;
  mutexUnlock(&g_lock);
  return result;
}

int fakefd_is_live(int fd) {
  mutexLock(&g_lock);
  const int result = descriptor_locked(fd, NULL) != NULL;
  mutexUnlock(&g_lock);
  return result;
}

int fakefd_operation_acquire(int fd, FakeFdOperation *operation) {
  if (!operation) { errno = EINVAL; return 0; }
  operation->open = NULL;
  mutexLock(&g_lock);
  FakeFd *descriptor = descriptor_locked(fd, NULL);
  if (descriptor) {
    operation->open = descriptor->open;
    open_retain_locked(descriptor->open);
  }
  mutexUnlock(&g_lock);
  return operation->open != NULL;
}

void fakefd_operation_release(FakeFdOperation *operation) {
  if (!operation || !operation->open) return;
  mutexLock(&g_lock);
  FakeOpen *open = operation->open;
  operation->open = NULL;
  open_release_locked(open);
  mutexUnlock(&g_lock);
}

int fakefd_select_bit(int fd) {
  return fd >= FAKE_FD_BASE && fd < FAKE_FD_BASE + MAX_FAKE_FDS
    ? FAKE_FD_SELECT_BASE + fd - FAKE_FD_BASE : fd;
}

int fakefd_from_select_bit(int bit) {
  mutexLock(&g_lock);
  int low_alias = alias_locked(bit) != NULL;
  mutexUnlock(&g_lock);
  if (low_alias) return bit;
  return bit >= FAKE_FD_SELECT_BASE && bit < 1024
    ? FAKE_FD_BASE + bit - FAKE_FD_SELECT_BASE : -1;
}

int fakefd_pipe2(int fds[2], int flags) {
  if (!fds) { errno = EFAULT; return -1; }
  if (flags & ~(BIONIC_O_NONBLOCK | BIONIC_O_CLOEXEC)) {
    errno = EINVAL;
    return -1;
  }
  Pipe *pipe = calloc(1, sizeof(*pipe));
  if (!pipe) { errno = ENOMEM; return -1; }
  pipe->buf = malloc(PIPE_DEFAULT_CAP);
  if (!pipe->buf) { free(pipe); errno = ENOMEM; return -1; }
  pipe->capacity = PIPE_DEFAULT_CAP;
  mutexLock(&g_lock);
  int read_slot = alloc_high_locked();
  if (read_slot >= 0) g_fds[read_slot].open = (FakeOpen *)(uintptr_t)1;
  int write_slot = alloc_high_locked();
  if (read_slot >= 0) g_fds[read_slot].open = NULL;
  if (read_slot < 0 || write_slot < 0) {
    mutexUnlock(&g_lock);
    free(pipe->buf);
    free(pipe);
    errno = EMFILE;
    return -1;
  }
  pipe->readers = pipe->writers = 1;
  pipe->read_open = (FakeOpen){ pipe, FD_PIPE_R,
    (flags & BIONIC_O_NONBLOCK) != 0, 1 };
  pipe->write_open = (FakeOpen){ pipe, FD_PIPE_W,
    (flags & BIONIC_O_NONBLOCK) != 0, 1 };
  g_fds[read_slot] = (FakeFd){ &pipe->read_open,
    (flags & BIONIC_O_CLOEXEC) != 0 };
  g_fds[write_slot] = (FakeFd){ &pipe->write_open,
    (flags & BIONIC_O_CLOEXEC) != 0 };
  mutexUnlock(&g_lock);
  fds[0] = FAKE_FD_BASE + read_slot;
  fds[1] = FAKE_FD_BASE + write_slot;
  return 0;
}

int fakefd_pipe(int fds[2]) { return fakefd_pipe2(fds, 0); }

int fakefd_eventfd(unsigned initial_value, int flags) {
  const int allowed = 1 /*EFD_SEMAPHORE*/ | BIONIC_O_NONBLOCK |
                      BIONIC_O_CLOEXEC;
  if (flags & ~allowed) { errno = EINVAL; return -1; }
  Pipe *counter = calloc(1, sizeof(*counter));
  if (!counter) { errno = ENOMEM; return -1; }
  counter->event_value = initial_value;
  counter->event_semaphore = (flags & 1) != 0;
  counter->event_open = (FakeOpen){ counter, FD_EVENT,
    (flags & BIONIC_O_NONBLOCK) != 0, 1 };
  mutexLock(&g_lock);
  int slot = alloc_high_locked();
  if (slot >= 0)
    g_fds[slot] = (FakeFd){ &counter->event_open,
      (flags & BIONIC_O_CLOEXEC) != 0 };
  mutexUnlock(&g_lock);
  if (slot < 0) { free(counter); errno = EMFILE; return -1; }
  return FAKE_FD_BASE + slot;
}

int fakefd_random(int flags) {
  if (flags & ~(BIONIC_O_NONBLOCK | BIONIC_O_CLOEXEC)) {
    errno = EINVAL;
    return -1;
  }
  Pipe *source = calloc(1, sizeof(*source));
  if (!source) { errno = ENOMEM; return -1; }
  source->random_open = (FakeOpen){ source, FD_RANDOM,
    (flags & BIONIC_O_NONBLOCK) != 0, 1 };
  mutexLock(&g_lock);
  int slot = alloc_high_locked();
  if (slot >= 0)
    g_fds[slot] = (FakeFd){ &source->random_open,
      (flags & BIONIC_O_CLOEXEC) != 0 };
  mutexUnlock(&g_lock);
  if (slot < 0) { free(source); errno = EMFILE; return -1; }
  return FAKE_FD_BASE + slot;
}

int fakefd_dup(int fd) {
  mutexLock(&g_lock);
  FakeFd *source = descriptor_locked(fd, NULL);
  int slot = source ? alloc_high_locked() : -1;
  if (!source || slot < 0) {
    mutexUnlock(&g_lock);
    errno = source ? EMFILE : EBADF;
    return -1;
  }
  open_retain_locked(source->open);
  g_fds[slot] = (FakeFd){ source->open, 0 };
  mutexUnlock(&g_lock);
  return FAKE_FD_BASE + slot;
}

static int open_reservation(void) {
  int fd = open(GAME_HOME "/genshinimpact_nx.nro", O_RDONLY);
  if (fd < 0) fd = open(CA_BUNDLE_PATH, O_RDONLY);
  return fd;
}

int fakefd_dup2(int fd, int target) {
  if (target < 0) { errno = EBADF; return -1; }
  mutexLock(&g_lock);
  FakeFd *source = descriptor_locked(fd, NULL);
  if (!source) { mutexUnlock(&g_lock); errno = EBADF; return -1; }
  if (fd == target) { mutexUnlock(&g_lock); return target; }
  FakeOpen *open = source->open;
  open_retain_locked(open); /* survives target replacement/concurrent close. */

  if (target >= FAKE_FD_BASE && target < FAKE_FD_BASE + MAX_FAKE_FDS) {
    FakeFd *destination = &g_fds[target - FAKE_FD_BASE];
    if (destination->open) open_release_locked(destination->open);
    *destination = (FakeFd){ open, 0 };
    mutexUnlock(&g_lock);
    return target;
  }
  if (target >= 1024) {
    open_release_locked(open);
    mutexUnlock(&g_lock);
    errno = EBADF;
    return -1;
  }
  /* Reserve the bookkeeping slot before mutating target.  The reserved state
   * is invisible to classification, so close_fake_backend still observes the
   * real old target. */
  FakeAlias *reserved = reserve_alias_locked(target);
  if (!reserved) {
    open_release_locked(open);
    mutexUnlock(&g_lock);
    errno = EMFILE;
    return -1;
  }
  mutexUnlock(&g_lock);

  int holder = open_reservation();
  if (holder < 0) {
    int saved = errno;
    mutexLock(&g_lock);
    memset(reserved, 0, sizeof(*reserved));
    open_release_locked(open);
    mutexUnlock(&g_lock);
    errno = saved;
    return -1;
  }
  if (holder != target) {
    /* The public dup2 wrapper already owns the target route transition. */
    const int close_result = close_fake_backend(target);
    if (close_result < 0 && errno != EBADF) {
      int saved = errno;
      close(holder);
      mutexLock(&g_lock);
      memset(reserved, 0, sizeof(*reserved));
      open_release_locked(open);
      mutexUnlock(&g_lock);
      errno = saved;
      return -1;
    }
    if (dup2(holder, target) < 0) {
      int saved = errno;
      close(holder);
      mutexLock(&g_lock);
      memset(reserved, 0, sizeof(*reserved));
      open_release_locked(open);
      mutexUnlock(&g_lock);
      errno = saved;
      return -1;
    }
    close(holder);
  }

  mutexLock(&g_lock);
  reserved->used = 1;
  reserved->descriptor = (FakeFd){ open, 0 };
  mutexUnlock(&g_lock);
  return target;
}

long fakefd_operation_write(FakeFdOperation *operation, const void *buf,
                            unsigned long n) {
  if (!operation || !operation->open) { errno = EBADF; return -1; }
  mutexLock(&g_lock);
  FakeOpen *open = operation->open;
  if (open->kind == FD_EVENT) {
    if (n < sizeof(uint64_t)) { mutexUnlock(&g_lock); errno = EINVAL; return -1; }
    if (!buf) { mutexUnlock(&g_lock); errno = EFAULT; return -1; }
    uint64_t value;
    memcpy(&value, buf, sizeof(value));
    if (value == UINT64_MAX) { mutexUnlock(&g_lock); errno = EINVAL; return -1; }
    open_retain_locked(open);
    Pipe *counter = open->pipe;
    while (value > UINT64_MAX - UINT64_C(1) - counter->event_value) {
      if (open->nonblock) {
        open_release_locked(open); mutexUnlock(&g_lock);
        errno = EAGAIN; return -1;
      }
      condvarWait(&g_cond, &g_lock);
    }
    counter->event_value += value;
    condvarWakeAll(&g_cond);
    open_release_locked(open);
    mutexUnlock(&g_lock);
    return sizeof(uint64_t);
  }
  if (!n) { mutexUnlock(&g_lock); return 0; }
  if (!buf) { mutexUnlock(&g_lock); errno = EFAULT; return -1; }
  if (open->kind != FD_PIPE_W) {
    mutexUnlock(&g_lock); errno = EBADF; return -1;
  }
  open_retain_locked(open);
  Pipe *pipe = open->pipe;
  size_t wrote = 0;
  const uint8_t *source = buf;
  while (wrote < n) {
    size_t available = pipe->capacity - pipe->len;
    while ((available == 0 ||
            (wrote == 0 && n <= PIPE_ATOMIC_CAP && available < n)) &&
           pipe->readers) {
      if (open->nonblock) {
        open_release_locked(open); mutexUnlock(&g_lock);
        if (wrote) return (long)wrote;
        errno = EAGAIN; return -1;
      }
      condvarWait(&g_cond, &g_lock);
      available = pipe->capacity - pipe->len;
    }
    if (!pipe->readers) {
      open_release_locked(open); mutexUnlock(&g_lock);
      if (wrote) return (long)wrote;
      errno = EPIPE; return -1;
    }
    size_t chunk = (size_t)n - wrote;
    if (chunk > available) chunk = available;
    size_t tail = (pipe->head + pipe->len) % pipe->capacity;
    size_t first = pipe->capacity - tail;
    if (first > chunk) first = chunk;
    memcpy(pipe->buf + tail, source + wrote, first);
    memcpy(pipe->buf, source + wrote + first, chunk - first);
    pipe->len += chunk;
    wrote += chunk;
    condvarWakeAll(&g_cond);
    if (open->nonblock && wrote < n) break;
  }
  open_release_locked(open);
  mutexUnlock(&g_lock);
  return (long)wrote;
}

long fakefd_write(int fd, const void *buf, unsigned long n) {
  FakeFdOperation operation;
  if (!fakefd_operation_acquire(fd, &operation)) { errno = EBADF; return -1; }
  long result = fakefd_operation_write(&operation, buf, n);
  fakefd_operation_release(&operation);
  return result;
}

long fakefd_operation_read(FakeFdOperation *operation, void *buf,
                           unsigned long n) {
  if (!operation || !operation->open) { errno = EBADF; return -1; }
  mutexLock(&g_lock);
  FakeOpen *open = operation->open;
  if (open->kind == FD_RANDOM) {
    if (!n) { mutexUnlock(&g_lock); return 0; }
    if (!buf) { mutexUnlock(&g_lock); errno = EFAULT; return -1; }
    if (n > (unsigned long)LONG_MAX) {
      mutexUnlock(&g_lock); errno = EINVAL; return -1;
    }
    open_retain_locked(open);
    mutexUnlock(&g_lock);
    randomGet(buf, (size_t)n);
    mutexLock(&g_lock);
    open_release_locked(open);
    mutexUnlock(&g_lock);
    return (long)n;
  }
  if (open->kind == FD_EVENT) {
    if (n < sizeof(uint64_t)) { mutexUnlock(&g_lock); errno = EINVAL; return -1; }
    if (!buf) { mutexUnlock(&g_lock); errno = EFAULT; return -1; }
    open_retain_locked(open);
    Pipe *counter = open->pipe;
    while (!counter->event_value) {
      if (open->nonblock) {
        open_release_locked(open); mutexUnlock(&g_lock);
        errno = EAGAIN; return -1;
      }
      condvarWait(&g_cond, &g_lock);
    }
    uint64_t value = counter->event_semaphore ? UINT64_C(1) :
                                                counter->event_value;
    counter->event_value -= value;
    memcpy(buf, &value, sizeof(value));
    condvarWakeAll(&g_cond);
    open_release_locked(open);
    mutexUnlock(&g_lock);
    return sizeof(uint64_t);
  }
  if (!n) { mutexUnlock(&g_lock); return 0; }
  if (!buf) { mutexUnlock(&g_lock); errno = EFAULT; return -1; }
  if (open->kind != FD_PIPE_R) {
    mutexUnlock(&g_lock); errno = EBADF; return -1;
  }
  open_retain_locked(open);
  Pipe *pipe = open->pipe;
  while (!pipe->len && pipe->writers) {
    if (open->nonblock) {
      open_release_locked(open); mutexUnlock(&g_lock);
      errno = EAGAIN; return -1;
    }
    condvarWait(&g_cond, &g_lock);
  }
  size_t got = 0;
  uint8_t *destination = buf;
  while (got < n && pipe->len) {
    destination[got++] = pipe->buf[pipe->head];
    pipe->head = (pipe->head + 1) % pipe->capacity;
    pipe->len--;
  }
  if (got) condvarWakeAll(&g_cond);
  open_release_locked(open);
  mutexUnlock(&g_lock);
  return (long)got;
}

long fakefd_read(int fd, void *buf, unsigned long n) {
  FakeFdOperation operation;
  if (!fakefd_operation_acquire(fd, &operation)) { errno = EBADF; return -1; }
  long result = fakefd_operation_read(&operation, buf, n);
  fakefd_operation_release(&operation);
  return result;
}

int fakefd_close(int fd) {
  mutexLock(&g_lock);
  FakeAlias *alias = NULL;
  FakeFd *descriptor = descriptor_locked(fd, &alias);
  if (!descriptor) { mutexUnlock(&g_lock); errno = EBADF; return -1; }
  FakeOpen *open = descriptor->open;
  if (alias) {
    memset(alias, 0, sizeof(*alias));
  } else {
    memset(descriptor, 0, sizeof(*descriptor));
  }
  open_release_locked(open);
  mutexUnlock(&g_lock);
  if (alias) (void)close(fd); /* release the native reservation. */
  return 0;
}

int fakefd_fcntl(int fd, int command, intptr_t argument) {
  mutexLock(&g_lock);
  FakeFd *descriptor = descriptor_locked(fd, NULL);
  if (!descriptor) { mutexUnlock(&g_lock); errno = EBADF; return -1; }
  int result = 0;
  switch (command) {
    case 0:       /* Bionic F_DUPFD */
    case 1030: {  /* Bionic F_DUPFD_CLOEXEC */
      if (argument < 0 || argument > INT_MAX) {
        mutexUnlock(&g_lock); errno = EINVAL; return -1;
      }
      int first = argument <= FAKE_FD_BASE ? 0 : (int)argument - FAKE_FD_BASE;
      if (first < 0) first = 0;
      int slot = -1;
      for (int i = first; i < MAX_FAKE_FDS; i++) if (!g_fds[i].open) {
        slot = i;
        break;
      }
      if (slot < 0) {
        mutexUnlock(&g_lock); errno = EMFILE; return -1;
      }
      open_retain_locked(descriptor->open);
      g_fds[slot] = (FakeFd){ descriptor->open, command == 1030 };
      result = FAKE_FD_BASE + slot;
      break;
    }
    case 1: result = descriptor->cloexec; break; /* F_GETFD */
    case 2: descriptor->cloexec = (argument & 1) != 0; break; /* F_SETFD */
    case 3: /* F_GETFL */
      result = (descriptor->open->kind == FD_EVENT ? 2 :
                descriptor->open->kind == FD_PIPE_W ? 1 : 0) |
               (descriptor->open->nonblock ? BIONIC_O_NONBLOCK : 0);
      break;
    case 4: /* F_SETFL */
      descriptor->open->nonblock = (argument & BIONIC_O_NONBLOCK) != 0;
      condvarWakeAll(&g_cond);
      break;
    case 1031: { /* Bionic/Linux F_SETPIPE_SZ */
      if (descriptor->open->kind != FD_PIPE_R &&
          descriptor->open->kind != FD_PIPE_W) {
        mutexUnlock(&g_lock); errno = EINVAL; return -1;
      }
      if (argument <= 0 || (uint64_t)argument > PIPE_MAX_CAP) {
        mutexUnlock(&g_lock); errno = argument <= 0 ? EINVAL : EPERM; return -1;
      }
      size_t capacity = ((size_t)argument + PIPE_ATOMIC_CAP - 1u) &
                        ~(PIPE_ATOMIC_CAP - 1u);
      if (capacity < PIPE_DEFAULT_CAP) capacity = PIPE_DEFAULT_CAP;
      Pipe *pipe = descriptor->open->pipe;
      if (capacity < pipe->len) {
        mutexUnlock(&g_lock); errno = EBUSY; return -1;
      }
      if (capacity != pipe->capacity) {
        uint8_t *replacement = malloc(capacity);
        if (!replacement) { mutexUnlock(&g_lock); errno = ENOMEM; return -1; }
        for (size_t i = 0; i < pipe->len; i++)
          replacement[i] = pipe->buf[(pipe->head + i) % pipe->capacity];
        free(pipe->buf);
        pipe->buf = replacement;
        pipe->head = 0;
        pipe->capacity = capacity;
        condvarWakeAll(&g_cond);
      }
      result = (int)pipe->capacity;
      break;
    }
    case 1032: /* Bionic/Linux F_GETPIPE_SZ */
      if (descriptor->open->kind != FD_PIPE_R &&
          descriptor->open->kind != FD_PIPE_W) {
        mutexUnlock(&g_lock); errno = EINVAL; return -1;
      }
      result = (int)descriptor->open->pipe->capacity;
      break;
    default:
      mutexUnlock(&g_lock); errno = EINVAL; return -1;
  }
  mutexUnlock(&g_lock);
  return result;
}

int fakefd_ioctl(int fd, unsigned long request, void *argument) {
  if (!argument) { errno = EFAULT; return -1; }
  mutexLock(&g_lock);
  FakeFd *descriptor = descriptor_locked(fd, NULL);
  if (!descriptor) { mutexUnlock(&g_lock); errno = EBADF; return -1; }
  if (request == 0x5421ul) { /* Android FIONBIO */
    descriptor->open->nonblock = *(const int *)argument != 0;
    condvarWakeAll(&g_cond);
  } else if (request == 0x541bul) { /* Android FIONREAD */
    size_t available = descriptor->open->kind == FD_EVENT
      ? (descriptor->open->pipe->event_value ? sizeof(uint64_t) : 0)
      : descriptor->open->kind == FD_RANDOM ? (size_t)INT_MAX
      : descriptor->open->pipe->len;
    *(int *)argument = available > INT_MAX ? INT_MAX : (int)available;
  } else {
    mutexUnlock(&g_lock); errno = ENOTTY; return -1;
  }
  mutexUnlock(&g_lock);
  return 0;
}

short fakefd_poll_revents(int fd, short events) {
  mutexLock(&g_lock);
  FakeFd *descriptor = descriptor_locked(fd, NULL);
  if (!descriptor) { mutexUnlock(&g_lock); return POLLNVAL; }
  FakeOpen *open = descriptor->open;
  Pipe *pipe = open->pipe;
  short ready = 0;
  if (open->kind == FD_EVENT) {
    if (pipe->event_value) ready |= events & (POLLIN | POLLRDNORM);
    if (pipe->event_value < UINT64_MAX - UINT64_C(1))
      ready |= events & (POLLOUT | POLLWRNORM | POLLWRBAND);
  } else if (open->kind == FD_RANDOM) {
    ready |= events & (POLLIN | POLLRDNORM);
  } else if (open->kind == FD_PIPE_R) {
    if (pipe->len) ready |= events & (POLLIN | POLLRDNORM);
    if (!pipe->writers) ready |= POLLHUP;
  } else {
    if (!pipe->readers) ready |= POLLERR;
    else if (pipe->len < pipe->capacity)
      ready |= events & (POLLOUT | POLLWRNORM | POLLWRBAND);
  }
  mutexUnlock(&g_lock);
  return ready;
}

void fakefd_wait(unsigned long long timeout_ns) {
  mutexLock(&g_lock);
  (void)condvarWaitTimeout(&g_cond, &g_lock, timeout_ns);
  mutexUnlock(&g_lock);
}
