#ifndef GENSHIN_BIONIC_COMPAT_H
#define GENSHIN_BIONIC_COMPAT_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdarg.h>

extern uintptr_t nx_stack_chk_guard;
extern void *nx_stdin_ptr;
extern void *nx_stdout_ptr;
extern void *nx_stderr_ptr;
extern char **nx_environ_ptr;

void nx_assert2(const char *file, int line, const char *function, const char *expr);
int nx_cxa_thread_atexit(void (*destructor)(void *), void *object, void *dso);
void nx_run_cxa_thread_destructors(void);
char *nx_fgets_chk(char *dst, size_t dst_size, int count, FILE *file);
void *nx_memcpy_chk(void *dst, const void *src, size_t count, size_t dst_size);
void *nx_memset_chk(void *dst, int value, size_t count, size_t dst_size);
int nx_open_2(const char *path, int flags);
long nx_read_chk(int fd, void *dst, size_t count, size_t dst_size);
int nx_register_atfork(void (*prepare)(void), void (*parent)(void),
                       void (*child)(void), void *dso);
char *nx_strcat_chk(char *dst, const char *src, size_t dst_size);
char *nx_strchr_chk(const char *str, int ch, size_t str_size);
char *nx_strcpy_chk(char *dst, const char *src, size_t dst_size);
char *nx_strncpy_chk2(char *dst, const char *src, size_t count,
                      size_t dst_size, size_t src_size);
char *nx_strrchr_chk(const char *str, int ch, size_t str_size);
char *nx_strncat_chk(char *dst, const char *src, size_t count, size_t dst_size);
int nx_vsprintf_chk(char *dst, int flags, size_t dst_size, const char *fmt, va_list ap);
void nx_fd_clr_chk(int fd, void *set, size_t set_size);
char *nx_gnu_strerror_r(int error, char *buffer, size_t size);
unsigned nx_umask_chk(unsigned mask);
int nx_android_log_buf_write(int buffer_id, int priority, const char *tag, const char *text);
void nx_android_log_assert(const char *condition, const char *tag, const char *format, ...);
void *nx_cmsg_nxthdr(const void *message, const void *control);

uint32_t nx_arc4random(void);
void nx_arc4random_buf(void *buffer, size_t size);
int nx_getentropy(void *buffer, size_t size);

int nx_epoll_create(int size);
int nx_epoll_create1(int flags);
int nx_epoll_ctl(int epfd, int op, int fd, const void *event);
int nx_epoll_wait(int epfd, void *events, int max_events, int timeout_ms);
int nx_epoll_close(int fd);
int nx_epoll_is_fd(int fd);
void nx_epoll_forget_fd(int fd);

/* Payload-free epoll telemetry.  The probe counts show whether one stalled
 * socket is still registered (and whether EPOLLONESHOT left it disabled)
 * without retaining event data or any application-owned pointer. */
typedef struct {
  uint64_t wait_calls;
  uint64_t delivered_events;
  uint64_t wait_timeouts;
  uint64_t wait_failures;
  uint64_t stale_snapshot_retries;
  int32_t last_wait_error;
  uint32_t live_sets;
  uint32_t registered_items;
  uint32_t disabled_items;
  uint32_t probe_registrations;
  uint32_t probe_disabled_registrations;
} NxEpollDiagnostics;

void nx_epoll_get_diagnostics(int probe_fd, NxEpollDiagnostics *out);

int nx_inotify_init1(int flags);
int nx_inotify_add_watch(int fd, const char *path, uint32_t mask);
int nx_signalfd(int fd, const void *mask, int flags);
int nx_eventfd(unsigned initial_value, int flags);
int nx_fork(void);
int nx_execl(const char *path, const char *arg, ...);
int nx_execv(const char *path, char *const argv[]);
int nx_execve(const char *path, char *const argv[], char *const envp[]);
int nx_clone(int (*entry)(void *), void *stack, int flags, void *argument, ...);
int nx_waitpid(int pid, int *status, int options);
int nx_getppid(void);
int nx_getrlimit(int resource, void *limit);
int nx_getrusage(int who, void *usage);
int nx_sysinfo(void *info);
long nx_pathconf(const char *path, int name);
int nx_mincore(void *address, size_t length, unsigned char *vector);
int nx_msync(void *address, size_t length, int flags);
int nx_pthread_attr_getguardsize(const void *attr, size_t *size);
int nx_pthread_attr_setschedpolicy(void *attr, int policy);
int nx_pthread_setschedparam(void *thread, int policy, const void *param);
int nx_pthread_getschedparam(void *thread, int *policy, void *param);
int nx_pthread_rwlock_destroy(void **lock);
int nx_sched_get_priority_min(int policy);
int nx_sched_get_priority_max(int policy);
int nx_sched_getparam(int pid, void *param);
int nx_sched_getscheduler(int pid);
int nx_sigsetjmp(void *env, int save_mask) __attribute__((returns_twice));
void nx_siglongjmp(void *env, int value) __attribute__((noreturn));
int nx_fstat64(int fd, void *stat_buffer);

typedef struct {
  uint64_t read_calls;
  uint64_t read_bytes;
  uint64_t read_failures;
  uint64_t write_calls;
  uint64_t write_bytes;
  uint64_t write_failures;
  uint64_t size_queries;
  uint64_t size_cache_hits;
  uint64_t size_query_failures;
  uint64_t size_extensions;
  uint64_t size_extension_failures;
  uint64_t preallocation_extensions;
  uint64_t preallocation_fallbacks;
  uint64_t preallocated_bytes;
  uint64_t finalize_calls;
  uint64_t finalize_failures;
  uint64_t finalized_bytes;
  uint64_t direct_writes;
  uint64_t direct_write_failures;
  uint64_t bounce_writes;
  uint64_t bounce_bytes;
  uint64_t size_operations_active;
  uint64_t oldest_size_operation_ms;
  uint32_t oldest_size_operation_kind;
  uint32_t oldest_size_operation_slot;
} NxFileIoDiagnostics;

void nx_file_io_get_diagnostics(NxFileIoDiagnostics *out);
/* Return 1 for the registry mutex, 2 for a tracked-file mutex, or 0 when the
 * supplied address is not owned by this subsystem. */
uint32_t nx_file_io_identify_lock(uintptr_t address, uint32_t *slot_out,
                                  uint32_t *word_out);
void nx_file_io_track_open(int fd, const char *path, int writable);
void nx_file_io_finalize_fd(int fd);
void nx_file_io_note_truncate(int fd, int64_t length);
int nx_file_io_logical_size(int fd, int64_t *size_out);
int nx_file_io_logical_size_path(const char *path, int64_t *size_out);
long nx_read(int fd, void *buffer, size_t count);
long nx_write(int fd, const void *buffer, size_t count);
long nx_pread(int fd, void *buffer, size_t count, long offset);
long nx_pwrite(int fd, const void *buffer, size_t count, long offset);
long nx_writev(int fd, const void *vectors, int count);
int nx_fdatasync(int fd);
int nx_pipe2(int fds[2], int flags);
int nx_ftruncate(int fd, long length);
int nx_fsync(int fd);
int nx_regcomp(void *compiled, const char *pattern, int flags);
int nx_regexec(const void *compiled, const char *text, size_t matches, void *match, int flags);
void nx_regfree(void *compiled);
void nx_libc_init(void);

#endif
