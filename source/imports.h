/* Guest ELF import resolution. */

#ifndef __IMPORTS_H__
#define __IMPORTS_H__

#include <stddef.h>
#include <pthread.h>
#include <switch.h>
#include "so_util.h"

/* A strong reference to a wrapper-created guest thread.  The native Thread
 * and Handle remain valid until nx_guest_thread_release(), including while a
 * collector keeps the target paused. */
#ifndef NX_GUEST_THREAD_REF_DECLARED
#define NX_GUEST_THREAD_REF_DECLARED
typedef struct NxGuestThreadRef NxGuestThreadRef;
#endif

NxGuestThreadRef *nx_guest_thread_acquire(pthread_t thread);
void nx_guest_thread_release(NxGuestThreadRef *ref);
/* Capture strong references to every currently live guest thread under one
 * registry-lock acquisition.  Stop-the-world code must take this snapshot
 * before pausing its first target; it can then resolve the whole collection
 * cycle without ever waiting on a mutex which a paused target may own. */
int nx_guest_thread_snapshot(NxGuestThreadRef **refs, size_t capacity,
                             size_t *count_out);
pthread_t nx_guest_thread_pthread(const NxGuestThreadRef *ref);
Thread *nx_guest_thread_native(const NxGuestThreadRef *ref);
Handle nx_guest_thread_handle(const NxGuestThreadRef *ref);
uintptr_t nx_guest_thread_pointer(const NxGuestThreadRef *ref);
uintptr_t nx_guest_thread_entry(const NxGuestThreadRef *ref);
/* Published Bionic sigset_t for asynchronous GC-signal delivery.  The value
 * is lock-free because a collector may inspect it while the target owns an
 * unrelated runtime mutex. */
uint64_t nx_guest_thread_signal_mask(const NxGuestThreadRef *ref);
/* True only while the target owns or waits for a wrapper lock which must not
 * be interrupted by emulated asynchronous signal delivery. */
int nx_guest_thread_gc_critical(const NxGuestThreadRef *ref);
/* Publish ownership attempts for locks introduced by the wrapper itself.
 * The entry/leave pair is nesting-safe and intentionally not exported to the
 * guest import table.  Callers must clear a failed nonblocking attempt before
 * waiting so the collector can still suspend queued threads. */
void nx_guest_gc_critical_enter(void);
void nx_guest_gc_critical_leave(void);
int nx_guest_thread_is_self(const NxGuestThreadRef *ref);
int nx_guest_thread_is_live(const NxGuestThreadRef *ref);

typedef struct {
  uint64_t create_calls;
  uint64_t create_successes;
  uint64_t create_failures;
  uint64_t fallback_successes;
  uint64_t detached_creates;
  uint64_t reaped_threads;
  uint64_t reap_failures;
  uint64_t registered_threads;
  uint64_t live_threads;
  uint64_t exiting_threads;
  uint64_t exited_threads;
  uint64_t detached_pending;
  uint64_t external_threads;
  uint64_t stack_bytes;
  uint64_t peak_stack_bytes;
  uint64_t last_requested_stack;
  uint64_t last_attempted_stack;
  uint64_t free_thread_count;
  uint64_t process_memory_used;
  uint64_t process_memory_total;
  uint64_t heap_arena;
  uint64_t heap_used;
  uint64_t heap_free;
  uint64_t heap_top_free;
  int32_t last_host_error;
  int32_t last_failure_stage;
  uint32_t free_thread_count_valid;
  uint32_t process_memory_valid;
} NxGuestThreadDiagnostics;

/* Counter-only lifecycle/resource telemetry for long-running downloader tests. */
void nx_guest_thread_get_diagnostics(NxGuestThreadDiagnostics *out);

/* Lock-free state for diagnosing a frame blocked on the wrapper's shared
 * Bionic pthread-object registry.  Reading this must remain safe while the
 * registry owner is stopped. */
typedef struct {
  uintptr_t lock_address;
  uint32_t lock_word;
} NxPthreadStorageDiagnostics;

void nx_pthread_storage_get_diagnostics(NxPthreadStorageDiagnostics *out);

extern DynLibFunction dynlib_functions[];
uintptr_t dynlib_find_export(const char *name);
extern size_t dynlib_numfunctions;

/* Android arm64 uses a signed 64-bit long for pthread_condattr_t. */
typedef int64_t BionicPthreadCondAttr;
int pthread_condattr_init_fake(BionicPthreadCondAttr *attr);
int pthread_condattr_destroy_fake(BionicPthreadCondAttr *attr);
int pthread_condattr_setclock_fake(BionicPthreadCondAttr *attr, int bionic_clock_id);
int pthread_attr_init_fake(void *attr);
/* Create/join/detach wrapper-owned threads with a Bionic TPIDR_EL0 block so
 * callbacks into the Android guest can safely use its TLS and JNI surface. */
int pthread_create_fake(pthread_t *thread, const void *bionic_attr,
                        void *entry, void *arg);
int pthread_join_fake(pthread_t thread, void **retval);
int pthread_detach_fake(pthread_t thread);
/* Exercise explicit pthread initialization on deliberately non-zero guest
 * storage before any Android constructor relies on the ABI bridge. */
int pthread_storage_self_test(void);

void resolve_module_imports(so_module *mod);

extern const char *g_abort_source;

#endif
