/* Bionic-to-libnx libc compatibility. */

#ifndef __LIBC_SHIM_H__
#define __LIBC_SHIM_H__

#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <wchar.h>
#include <stddef.h>
#include <time.h>
#include <pthread.h>
#include <switch.h>

#ifndef NX_GUEST_THREAD_REF_DECLARED
#define NX_GUEST_THREAD_REF_DECLARED
typedef struct NxGuestThreadRef NxGuestThreadRef;
#endif

/* Descriptor routing is virtualized across native, packed-asset, and
 * synthetic descriptors.  A route ticket protects the short backend-capture
 * phase; blocking I/O always runs through a retained backend lease. */
typedef struct {
  uint32_t stripe;
  uint64_t sequence;
  int fd;
} NxFdRouteTicket;

typedef struct {
  uint32_t source_stripe;
  uint32_t target_stripe;
  int target_fd;
  unsigned state;
} NxFdRoutePair;

int nx_fd_route_snapshot(int fd, NxFdRouteTicket *ticket);
int nx_fd_route_validate(const NxFdRouteTicket *ticket);
int nx_fd_route_source_lock(int fd, uint32_t *stripe_out);
void nx_fd_route_source_unlock(uint32_t stripe);
int nx_fd_route_replace_begin(int fd, NxFdRoutePair *guard);
int nx_fd_route_pair_begin(int source, int target, NxFdRoutePair *guard);
void nx_fd_route_pair_release(NxFdRoutePair *guard);
void nx_fd_route_replace_end(NxFdRoutePair *guard);

typedef struct {
  void *open;
} FakeFdOperation;
int fakefd_operation_acquire(int fd, FakeFdOperation *operation);
void fakefd_operation_release(FakeFdOperation *operation);
long fakefd_operation_read(FakeFdOperation *operation, void *buf,
                           unsigned long n);
long fakefd_operation_write(FakeFdOperation *operation, const void *buf,
                            unsigned long n);

NxGuestThreadRef *nx_guest_thread_acquire(pthread_t thread);
void nx_guest_thread_release(NxGuestThreadRef *ref);
pthread_t nx_guest_thread_pthread(const NxGuestThreadRef *ref);
Thread *nx_guest_thread_native(const NxGuestThreadRef *ref);
Handle nx_guest_thread_handle(const NxGuestThreadRef *ref);
uintptr_t nx_guest_thread_pointer(const NxGuestThreadRef *ref);
uintptr_t nx_guest_thread_entry(const NxGuestThreadRef *ref);
int nx_guest_thread_is_self(const NxGuestThreadRef *ref);
int nx_guest_thread_is_live(const NxGuestThreadRef *ref);

#define NX_GUEST_CONTEXT_MAX_FRAMES 12
typedef struct {
  uint32_t pause_result;
  uint32_t dump_result;
  uint32_t resume_result;
  uint32_t frame_count;
  uintptr_t guest_thread_pointer;
  uint64_t pc;
  uint64_t lr;
  uint64_t sp;
  uint64_t fp;
  uint64_t frames[NX_GUEST_CONTEXT_MAX_FRAMES];
} NxGuestThreadContextSnapshot;

int nx_guest_thread_capture_context(pthread_t thread,
                                    NxGuestThreadContextSnapshot *out);
int nx_gc_context_capture_begin(void);
void nx_gc_context_capture_end(void);

/* Strip a devoptab device prefix from paths returned to managed code. */
const char *managed_path(const char *p);

void *__memmove_chk_fake(void *dst, const void *src, size_t n, size_t dstlen);
size_t __strlen_chk_fake(const char *s, size_t slen);
int __vsnprintf_chk_fake(char *s, size_t maxlen, int flag, size_t slen, const char *fmt, va_list va);
void __FD_SET_chk_fake(int fd, void *set, size_t setlen);
int  __FD_ISSET_chk_fake(int fd, const void *set, size_t setlen);

typedef struct FakePropInfo FakePropInfo;
int __system_property_get_fake(const char *name, char *value);
const FakePropInfo *__system_property_find_fake(const char *name);
int __system_property_read_fake(const FakePropInfo *info, char *name,
                                char *value);
unsigned long getauxval_fake(unsigned long type);
long syscall_fake(long number, ...);
void sincosf_fake(float x, float *s, float *c);
void android_set_abort_message_fake(const char *msg);
size_t __ctype_get_mb_cur_max_fake(void);
long sysconf_fake(int name);

/* Android/Bionic and newlib use different numeric clock IDs. */
int clock_gettime_fake(int bionic_clock_id, struct timespec *value);
int clock_getres_fake(int bionic_clock_id, struct timespec *value);

// fs
int open_fake(const char *path, int flags, ...);
int access_fake(const char *path, int mode);
int mkdir_fake(const char *path, unsigned mode);
int truncate_fake(const char *path, long length);
int unlink_fake(const char *path);
int rmdir_fake(const char *path);
int remove_fake(const char *path);
int rename_fake(const char *old_path, const char *new_path);
int utime_fake(const char *path, const void *times);
int utimes_fake(const char *path, const void *times);
int flock_fake(int fd, int operation);
int futimens_fake(int fd, const void *times);
void fd_metadata_copy(int source, int target);
/* Commit a read-ahead descriptor's virtual cursor to the native open-file
 * description and retire its private cache before ownership is transferred. */
int ra_flush_detach(int fd);
struct bionic_stat;
int stat_fake(const char *path, struct bionic_stat *st);
int fstat_fake(int fd, struct bionic_stat *st);
int lstat_fake(const char *path, struct bionic_stat *st);
void *readdir_fake(void *dirp);
int closedir_fake(void *dirp);
int scandir_fake(const char *path, void ***names,
                 int (*filter)(const void *), int (*compare)(const void *, const void *));
int alphasort_fake(const void *left, const void *right);
int mkstemp_fake(char *template_path);
char *realpath_fake(const char *path, char *resolved);
int strerror_r_fake(int err, char *buf, size_t len);
char *strerror_fake(int err);
int statfs_fake(const char *path, void *buf);
FILE *fopen_fake(const char *path, const char *mode);
FILE *fdopen_fake(int fd, const char *mode);

// locale
void *newlocale_fake(int mask, const char *locale, void *base);
void freelocale_fake(void *loc);
void *uselocale_fake(void *loc);
int iswalpha_l_fake(int wc, void *loc); int iswblank_l_fake(int wc, void *loc);
int iswcntrl_l_fake(int wc, void *loc); int iswdigit_l_fake(int wc, void *loc);
int iswlower_l_fake(int wc, void *loc); int iswprint_l_fake(int wc, void *loc);
int iswpunct_l_fake(int wc, void *loc); int iswspace_l_fake(int wc, void *loc);
int iswupper_l_fake(int wc, void *loc); int iswxdigit_l_fake(int wc, void *loc);
int towlower_l_fake(int wc, void *loc); int towupper_l_fake(int wc, void *loc);
int strcoll_l_fake(const char *a, const char *b, void *loc);
size_t strxfrm_l_fake(char *dst, const char *src, size_t n, void *loc);
size_t strftime_l_fake(char *s, size_t max, const char *fmt, const void *tm, void *loc);
long double strtold_l_fake(const char *s, char **end, void *loc);
long long strtoll_l_fake(const char *s, char **end, int base, void *loc);
unsigned long long strtoull_l_fake(const char *s, char **end, int base, void *loc);
int wcscoll_l_fake(const wchar_t *a, const wchar_t *b, void *loc);
size_t wcsxfrm_l_fake(wchar_t *dst, const wchar_t *src, size_t n, void *loc);
size_t mbsnrtowcs_fake(wchar_t *dst, const char **src, size_t nms, size_t len, void *ps);
size_t wcsnrtombs_fake(char *dst, const wchar_t **src, size_t nwc, size_t len, void *ps);

// stdio over fake __sF
extern uint8_t fake_sF[3][152];
size_t fwrite_fake(const void *ptr, size_t size, size_t n, FILE *f);
size_t fread_fake(void *ptr, size_t size, size_t n, FILE *f);
int fputc_fake(int c, FILE *f);
int fgetc_fake(FILE *f);
int fputs_fake(const char *s, FILE *f);
int fflush_fake(FILE *f);
int fclose_fake(FILE *f);
int ferror_fake(FILE *f);
int feof_fake(FILE *f);
int fileno_fake(FILE *f);
void clearerr_fake(FILE *f);
int fscanf_fake(FILE *f, const char *fmt, ...);
void rewind_fake(FILE *f);
FILE *freopen_fake(const char *path, const char *mode, FILE *f);
int ungetc_fake(int c, FILE *f);
int setvbuf_fake(FILE *f, char *buffer, int mode, size_t size);
void setbuf_fake(FILE *f, char *buffer);
wint_t fputwc_fake(wchar_t wc, FILE *f);
wint_t getwc_fake(FILE *f);
wint_t ungetwc_fake(wint_t wc, FILE *f);
int fseek_fake(FILE *f, long off, int whence);
long ftell_fake(FILE *f);
char *fgets_fake(char *s, int n, FILE *f);
int fprintf_fake(FILE *f, const char *fmt, ...);
int vfprintf_fake(FILE *f, const char *fmt, va_list va);
int vfscanf_fake(FILE *f, const char *fmt, va_list va);

// memory
int posix_memalign_fake(void **out, size_t align, size_t size);
/* Sparse-arena counters plus a bounded locked largest-run snapshot used by
 * allocation diagnostics. */
typedef struct {
  uint64_t virtual_capacity_bytes;
  uint64_t pool_capacity_bytes;
  uint64_t reserved_bytes;
  uint64_t peak_reserved_bytes;
  uint64_t committed_bytes;
  uint64_t peak_committed_bytes;
  uint64_t spill_limit_bytes;
  uint64_t spill_bytes;
  uint64_t peak_spill_bytes;
  uint64_t guest_backing_bytes;
  uint64_t host_spill_limit_bytes;
  uint64_t host_spill_bytes;
  uint64_t peak_host_spill_bytes;
  uint64_t thread_pool_limit_bytes;
  uint64_t thread_pool_bytes;
  uint64_t peak_thread_pool_bytes;
  uint64_t pool_critical_reserve_bytes;
  uint64_t pool_largest_free_bytes;
  uint64_t guest_allocation_failures;
  uint64_t host_allocation_failures;
  uint64_t thread_allocation_failures;
  uint64_t quarantined_bytes;
  uint64_t map_call_count;
  uint64_t map_retry_count;
  uint32_t last_map_result;
  uint32_t reserved0;
  uint64_t pool_free_bytes;
  uint64_t ownership_record_capacity;
  uint64_t ownership_records_in_use;
  uint64_t peak_ownership_records_in_use;
  uint64_t ownership_record_exhaustions;
  uint64_t dynamic_mapped_bytes;
  uint64_t peak_dynamic_mapped_bytes;
  uint64_t system_total_memory_bytes;
  uint64_t system_used_memory_bytes;
  uint64_t system_available_memory_bytes;
  uint64_t system_resource_size_bytes;
  uint64_t donor_capacity_bytes;
  uint64_t donor_active_bytes;
  uint64_t donor_used_bytes;
  uint64_t donor_peak_used_bytes;
  uint64_t donor_grow_calls;
  uint64_t donor_shrink_calls;
  uint32_t donor_last_resize_result;
  uint32_t backing_backend;
  /* Code-alias decommit accounting for the heap-donor backing path.  ok means
   * svcUnmapProcessCodeMemory recycled the source into the donor; fail means
   * the writable alias stayed mapped for safe reuse. */
  uint64_t backing_unmap_ok;
  uint64_t backing_unmap_fail;
} NxSparseArenaDiagnostics;

typedef enum {
  NX_MEMORY_BACKEND_NONE = 0,
  NX_MEMORY_BACKEND_PHYSICAL = 1,
  NX_MEMORY_BACKEND_HEAP_ALIAS = 2,
} NxMemoryBackingBackend;

void nx_sparse_arena_get_diagnostics(NxSparseArenaDiagnostics *out);
NxMemoryBackingBackend nx_memory_backing_backend(void);
MemoryType nx_memory_backing_mapped_type(void);
size_t nx_dynamic_arena_target_bytes(void);
/* Reserve every demand-backed destination as one atomic layout before any of
 * the three allocators are exposed. Physical mappings use Horizon's alias
 * region; the zero-SystemResourceSize fallback uses the large ASLR/code region
 * and a dynamically resized heap-donor bank. */
int nx_alias_memory_arenas_prepare(void);
/* Sparse mappings plus guest, host/NVK, and caller-owned thread allocations
 * share one virtual extent arena. Sparse reservations grow low-to-high while
 * dynamic owners pack high-to-low. Physical segments are committed only while
 * live; exact dynamic ownership still routes through these helpers. */
int nx_dynamic_arena_prepare(void);
int nx_sparse_pool_guest_arena_prepare(void);
void *nx_sparse_pool_spill_alloc(size_t size);
/* Preserve power-of-two guest alignment when the failed newlib entry point was
 * memalign/posix_memalign.  The requested size remains the ownership key. */
void *nx_sparse_pool_spill_alloc_aligned(size_t size, size_t alignment);
int nx_sparse_pool_spill_release(void *pointer);
int nx_sparse_pool_spill_query(const void *pointer, size_t *requested_out,
                               size_t *usable_out);
/* Host pointers are routed by the linker broker; thread backing is released
 * only after join restores its temporarily borrowed source pages. */
void *nx_sparse_pool_host_alloc_aligned(size_t size, size_t alignment);
void *nx_sparse_pool_thread_alloc(size_t size);
int nx_sparse_pool_thread_release(void *pointer);
int nx_sparse_pool_owned_release(void *pointer);
int nx_sparse_pool_owned_query(const void *pointer, size_t *requested_out,
                               size_t *usable_out);
/* Lock-free range classification for allocator wrappers.  A dynamic-arena or
 * sparse-window pointer must never be forwarded to newlib even when ownership
 * metadata cannot be locked recursively or a live stack aliases its source. */
int nx_sparse_pool_contains_address(const void *pointer);
/* Reserve the exact Unity 2017 slab as virtual address space. */
typedef struct {
  uint64_t large_calls;
  uint64_t exact_tuple_calls;
  uint64_t successful_claims;
  uint64_t exact_unmaps;
  uint64_t discard_calls;
  uint64_t discarded_bytes;
  uintptr_t last_discard_address;
  uint64_t last_discard_length;
  int32_t last_discard_advice;
  uint32_t last_discard_decision; /* 0=none, 1=partial clear, 2=decommit */
  uintptr_t last_caller;
  uintptr_t last_address_hint;
  uintptr_t last_claim_result;
  uint64_t last_length;
  int64_t last_offset;
  int32_t last_prot;
  int32_t last_flags;
  int32_t last_fd;
  uint32_t last_decision; /* 0=mismatch, 1=state miss, 2=claimed */
  uintptr_t reservation_base;
  uint64_t reservation_size;
  uint32_t reservation_state;
  uint32_t committed_chunks;
  uint32_t peak_committed_chunks;
  uint32_t last_map_result;
  uint64_t physical_commit_calls;
  uint64_t physical_decommit_calls;
} UnitySlabMmapDiagnostics;

int mmap_prepare_unity_slab_reservation(void **base_out, size_t *size_out);
int mmap_validate_unity_slab_reservation(const void *base, size_t size);
int mmap_get_unity_slab_diagnostics(UnitySlabMmapDiagnostics *out);
void *mmap_commit_unity_slab_chunk(void *chunk);
void *mmap_fake(void *addr, size_t length, int prot, int flags, int fd, long offset);
int munmap_fake(void *addr, size_t length);
int mmap_msync_fake(void *addr, size_t length, int flags);
void *mremap_fake(void *old_addr, size_t old_size, size_t new_size, int flags, ...);
int mprotect_fake(void *addr, size_t len, int prot);
int madvise_fake(void *addr, size_t len, int advice);

// fd routing (fake pipe vs real files)
long read_fake(int fd, void *buf, size_t count);
long z_lseek(int fd, long off, int whence);   /* real lseek; also services lseek64 */
long write_fake(int fd, const void *buf, size_t count);
int  close_fake(int fd);
/* Internal backend close used while the caller owns a route replacement. */
int  close_fake_backend(int fd);
int  dup_fake(int fd);
int  pipe_fake(int fds[2]);
int  poll_fake(void *fds, unsigned long nfds, int timeout);
int  select_fake(int n, void *r, void *w, void *e, void *t);

/* Lock-free read-ahead state for the independent main-loop watchdog.  The
 * watchdog must not take g_ra_lock: the stopped frame may itself be waiting
 * for, or owning, that lock during final resource verification. */
typedef struct {
  uintptr_t lock_address;
  uint64_t lock_attempts;
  uint64_t lock_acquisitions;
  uint64_t lock_waiters;
  uint64_t owner_started_tick;
  uint64_t owner_request_bytes;
  uint64_t read_calls;
  uint64_t read_bytes;
  uint64_t read_failures;
  uintptr_t owner_thread;
  uint32_t owner_handle;
  uint32_t owner_active;
  uint32_t owner_kind;
  int32_t owner_fd;
  uint32_t lock_word;
} NxReadAheadDiagnostics;

void nx_read_ahead_get_diagnostics(NxReadAheadDiagnostics *out);

// networking
typedef struct {
  uint64_t recv_calls;
  uint64_t received_bytes;
  uint64_t recv_failures;
  uint64_t last_receive_tick_ns;
  uint64_t poll_wait_calls;
  uint64_t poll_wait_failures;
  uint64_t tracked_stream_sockets;
  uint64_t receiving_stream_sockets;
  uint64_t stalled_stream_sockets;
  uint64_t longest_stream_idle_ms;
} NetworkTransportProgress;

/* Unlike the full periodic report, this snapshot performs atomic loads only;
 * in particular it never enters epoll or BSD service diagnostics. */
int network_get_transport_progress_fast(NetworkTransportProgress *out);

typedef struct {
  uint64_t dns_calls;
  uint64_t dns_successes;
  uint64_t dns_failures;
  int32_t last_dns_error;
  uint64_t resolver_pool_workers;
  uint64_t resolver_jobs_queued;
  uint64_t resolver_jobs_completed;
  uint64_t resolver_jobs_abandoned;
  uint64_t resolver_jobs_coalesced;
  uint64_t resolver_cache_hits;
  uint64_t resolver_cache_stores;
  uint64_t resolver_pool_failures;
  uint64_t resolver_default_deadlines;
  uint64_t reverse_numeric_results;
  uint64_t ipv6_results_filtered;
  uint64_t socket_calls;
  uint64_t socket_successes;
  uint64_t socket_failures;
  int32_t last_socket_error;
  uint64_t connect_calls;
  uint64_t connect_immediate_successes;
  uint64_t connect_in_progress;
  uint64_t connect_failures;
  int32_t last_connect_error;
  uint64_t connect_error_checks;
  uint64_t connect_error_clear;
  uint64_t connect_error_failures;
  int32_t last_connect_completion_error;
  uint64_t send_calls;
  uint64_t sent_bytes;
  uint64_t send_would_block;
  uint64_t send_failures;
  int32_t last_send_error;
  uint64_t recv_calls;
  uint64_t received_bytes;
  uint64_t recv_eof;
  uint64_t recv_would_block;
  uint64_t recv_failures;
  int32_t last_recv_error;
  uint64_t last_receive_tick_ns;
  uint64_t long_stream_receive_window_target;
  uint64_t long_stream_window_attempts;
  uint64_t long_stream_window_successes;
  uint64_t long_stream_window_failures;
  int32_t last_long_stream_window_error;
  uint64_t last_long_stream_window_effective;
  /* Datagram receive-window promotion (KCP/UDP bulk download).  Mirrors the
   * stream fields: att/ok/fail count one-shot SO_RCVBUF promotions per
   * datagram socket, eff is the effective window the BSD service granted. */
  uint64_t datagram_receive_window_target;
  uint64_t datagram_window_attempts;
  uint64_t datagram_window_successes;
  uint64_t datagram_window_failures;
  int32_t last_datagram_window_error;
  uint64_t last_datagram_window_effective;
  uint64_t tracked_stream_sockets;
  uint64_t receiving_stream_sockets;
  uint64_t stalled_stream_sockets;
  uint64_t largest_stream_received_bytes;
  uint64_t largest_stream_idle_ms;
  uint64_t longest_stream_idle_ms;
  uint64_t poll_readiness_probes;
  uint64_t poll_readiness_hits;
  uint64_t poll_readiness_probe_failures;
  uint64_t poll_wait_calls;
  uint64_t poll_wait_failures;
  uint64_t poll_stale_snapshot_recoveries;
  uint64_t poll_invalid_fd_recoveries;
  uint64_t poll_inactive_fd_recoveries;
  uint64_t poll_reused_fd_recoveries;
  uint64_t poll_ebadf_fallback_recoveries;
  uint64_t poll_ebadf_retries;
  uint64_t poll_route_snapshot_retries;
  uint64_t poll_probe_other_errors;
  int32_t last_poll_wait_error;
  uint64_t epoll_wait_calls;
  uint64_t epoll_delivered_events;
  uint64_t epoll_wait_timeouts;
  uint64_t epoll_wait_failures;
  uint64_t epoll_stale_snapshot_retries;
  int32_t last_epoll_wait_error;
  uint32_t epoll_live_sets;
  uint32_t epoll_registered_items;
  uint32_t epoll_disabled_items;
  uint32_t stalled_queued_epoll_registrations;
  uint32_t stalled_queued_epoll_disabled_registrations;
  uint64_t stalled_stream_queue_probes;
  uint64_t stalled_stream_queue_probe_failures;
  uint64_t stalled_streams_with_queued_data;
  uint64_t stalled_stream_queued_bytes;
  uint64_t largest_stalled_stream_queued_bytes;
  int32_t stalled_queued_fd;
  uint32_t stalled_queued_recv_inflight;
  uint64_t stalled_queued_generation;
  uint64_t stalled_queued_socket_bytes;
  uint64_t stalled_queued_last_recv_enter_tick_ns;
  uint64_t stalled_queued_last_poll_tick_ns;
  uintptr_t stalled_queued_recv_thread;
  uintptr_t stalled_queued_poll_thread;
  /* UDP/KCP transport.  Genshin's bulk resource download runs over KCP
   * (reliable UDP via recvmsg/recvfrom), NOT TCP, so the TCP receive-window
   * telemetry above stays frozen while gigabytes flow here.  These aggregate
   * the per-socket udp_* counters so the download throughput is observable. */
  uint64_t udp_recv_calls;
  uint64_t udp_received_bytes;
  uint64_t udp_send_calls;
  uint64_t udp_sent_bytes;
  uint64_t udp_receive_errors;
  uint64_t tracked_datagram_sockets;
  uint64_t largest_datagram_received_bytes;
  uint64_t last_udp_receive_tick_ns;
} NetworkTransportDiagnostics;

int network_get_transport_diagnostics(NetworkTransportDiagnostics *out);
void network_configure_long_stream_receive_window(uint32_t initial_size,
                                                  uint32_t maximum_size);
void network_configure_datagram_receive_window(uint32_t initial_size,
                                               uint32_t maximum_size);
void libc_shim_apply_device_profile(void);
void network_track_duplicate(int source, int target);
int socket_fake(int d, int t, int p);
int connect_fake(int s, const void *a, unsigned l);
int bind_fake(int s, const void *a, unsigned l);
int listen_fake(int s, int b);
int accept_fake(int s, void *a, void *l);
long send_fake(int s, const void *b, size_t l, int f);
long sendto_fake(int s, const void *b, size_t l, int f, const void *a, unsigned al);
long recv_fake(int s, void *b, size_t l, int f);
long recvfrom_fake(int s, void *b, size_t l, int f, void *a, void *al);
int shutdown_fake(int s, int how);
int socketpair_fake(int domain, int type, int protocol, int pair[2]);
int setsockopt_fake(int s, int lv, int n, const void *v, unsigned l);
int getsockopt_fake(int s, int lv, int n, void *v, void *l);
int getsockname_fake(int s, void *a, void *l);
int getpeername_fake(int s, void *a, void *l);
int getaddrinfo_fake(const char *node, const char *svc, const void *hints, void **res);
void freeaddrinfo_fake(void *res);
unsigned alarm_fake(unsigned seconds);
long getrandom_fake(void *buffer, size_t size, unsigned flags);
int fcntl_shim(int fd, int cmd, intptr_t arg);
int ioctl_fake(int fd, unsigned long request, ...);
extern int g_net_on;
long recvmsg_fake(int s, void *msg, int flags);
long sendmsg_fake(int s, const void *msg, int flags);
int inet_pton_shim(int af, const char *src, void *dst);
const char *inet_ntop_shim(int af, const void *src, char *dst, unsigned size);
int getnameinfo_fake(const void *a, unsigned al, char *h, unsigned hl, char *s, unsigned sl, int f);
void *gethostbyname_fake(const char *name);
void *gethostbyaddr_fake(const void *address, unsigned length, int family);
int gethostname_fake(char *name, size_t len);
unsigned if_nametoindex_fake(const char *n);
int kill_fake(int pid, int sig);
int getpid_fake(void);
int gettid_fake(void);
int sched_yield_fake(void);
time_t timegm_fake(struct tm *value);
void *getpwuid_fake(int uid);
int getpwuid_r_fake(int uid, void *pwd, char *buffer, size_t buffer_len, void **result);
char *getenv_fake(const char *name);
char *getcwd_fake(char *buf, size_t size);

// dynamic loader
void *dlopen_fake(const char *name, int flags);
int dlclose_fake(void *h);
const char *dlerror_fake(void);
void *dlsym_fake(void *handle, const char *symbol);

// pthread extras
int pthread_rwlock_init_fake(void **rw, const void *attr);
int pthread_rwlock_destroy_fake(void **rw);
int pthread_rwlock_rdlock_fake(void **rw);
int pthread_rwlock_wrlock_fake(void **rw);
int pthread_rwlock_unlock_fake(void **rw);
int sem_init_fake(void *s, int pshared, unsigned int value);
int sem_destroy_fake(void *s);
int sem_post_fake(void *s);
int sem_wait_fake(void *s);
int sem_trywait_fake(void *s);
int sem_timedwait_fake(void *s, const struct timespec *abs);
int sem_getvalue_fake(void *s, int *val);

/* IL2CPP GC signal acknowledgement. */
extern uintptr_t g_il2cpp_base;
extern size_t g_il2cpp_size;
extern volatile uint32_t g_gc_bridge_suspends;
extern volatile uint32_t g_gc_bridge_resumes;
extern volatile uint32_t g_gc_bridge_failures;
extern volatile uint32_t g_gc_bridge_capture_retries;
extern volatile uint32_t g_gc_bridge_host_captures;
extern volatile uint32_t g_gc_bridge_pause_failures;
extern volatile uint32_t g_gc_bridge_dump_failures;
extern volatile uint32_t g_gc_bridge_snapshot_cycles;
extern volatile uint32_t g_gc_bridge_snapshot_failures;
extern volatile uint32_t g_gc_bridge_snapshot_threads;
extern volatile uint32_t g_gc_bridge_active_targets;
extern volatile uint32_t g_gc_bridge_worker_dependency_releases;
extern volatile uint32_t g_gc_bridge_worker_release_failures;
extern volatile uint32_t g_gc_bridge_signal_mask_deferrals;
extern volatile uint32_t g_gc_bridge_deferred_deliveries;
extern volatile uint32_t g_gc_bridge_mutex_dependency_releases;
extern volatile uint32_t g_gc_bridge_mutex_release_failures;
extern volatile uint32_t g_futex_change_wakes;
int pthread_kill_gc(pthread_t t, int sig);

#endif
