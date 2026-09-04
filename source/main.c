/* Genshin Impact 7.0.0 Android/Unity host for Nintendo Switch (libnx). */

#include <switch.h>
#include <SDL2/SDL.h>

#include <errno.h>
#include <dirent.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "android_native_unity.h"
#include "asset_pack.h"
#include "combo_auth.h"
#include "combo_bridge.h"
#include "combo_crypto.h"
#include "config.h"
#include "device_profile.h"
#include "error.h"
#include "genshin_compat.h"
#include "imports.h"
#include "jni_fake.h"
#include "libc_shim.h"
#include "memory_broker.h"
#include "opensles.h"
#include "panic_capture.h"
#include "plugin_loader.h"
#include "sbrk_extend.h"
#include "so_util.h"
#include "unity_entrypoints.h"
#include "unity_jni.h"
#include "util.h"
#include "vulkan_egl_stubs.h"

#define DATA_ROOT       GAME_HOME
#define LIB_GAME        "lib/arm64-v8a/libyuanshen.so"
#define JNI_VERSION_1_6 0x00010006
#define STARTUP_METADATA_SIZE ((size_t)3932952)
#define STARTUP_METADATA_MAP_SIZE ((size_t)0x3c1000)
#define NETWORK_BSD_SESSION_COUNT 16u
/* KCP bulk download rides on UDP datagrams.  libnx reserves datagram
 * receive queues up front (there is no udp max growth field), so the only
 * way to enlarge them is a bigger udp_rx_buf_size at socketInitialize.
 * The BSD service rejects oversized reservations; main() falls back to the
 * hardware-proven default tuple in that case. */
#define NETWORK_UDP_RX_BUF_CANDIDATE 0x40000u

void unity_environment_init(const char *data_root); /* unity_glue.c */

static void *heap_so_base;
static size_t heap_so_limit;

/* mmap_fake and the overcommit allocator consume these regions. */
void *g_mmap_arena_base;
size_t g_mmap_arena_size;
void *g_oc_pool_base;
size_t g_oc_pool_size;
int g_oc_want;
static size_t g_initial_heap_bytes;
static size_t g_fixed_heap_bytes;
static size_t g_loader_heap_prefix_bytes;
size_t g_kernel_heap_bytes;
static Result g_heap_resize_result;
static uintptr_t g_heap_suffix_base;
uintptr_t g_kernel_heap_base;
void *g_heap_donor_base;
size_t g_heap_donor_capacity;
size_t g_heap_donor_active_bytes;
size_t g_heap_donor_kernel_offset;
NxMemoryBackingBackend g_memory_backing_backend;
static uint64_t g_system_resource_size_total;

static so_module game_mod;
static int nifm_started;
static int socket_started;

/* The monolithic Android image is about 354 MiB after loading.  Its source
 * pages are donated to the executable alias by so_finalize(), so they remain
 * outside newlib's ordinary allocation range.  Everything else grows through
 * one of two demand-backed virtual-memory paths.  A process with nonzero
 * SystemResourceSize can use svcMapPhysicalMemory.  Stock hbloader NPDMs set
 * that field to zero, so the wrapper retains only a small heap-donor suffix,
 * grows it in bounded steps, and aliases live pages with the already-proven
 * self-process code-memory SVCs.
 *
 * nx-hbloader creates one kernel heap, consumes its low pages as the NRO code
 * donors, and passes only the remaining suffix as EntryType_OverrideHeap.  A
 * later svcSetHeapSize still takes a size relative to the original kernel heap
 * base, not relative to that suffix.  Preserve the loader-owned prefix when
 * shrinking and expose only the override suffix to newlib/modules. */
void __libnx_initheap(void) {
  void *addr = NULL;
  size_t size = 0;
  const int overridden = envHasHeapOverride();

  if (overridden) {
    addr = envGetHeapOverrideAddr();
    size = envGetHeapOverrideSize();
  }

  extern char *fake_heap_start;
  extern char *fake_heap_end;
  const size_t requested = OC_MANAGED_BYTES + SO_REGION_BYTES;
  const size_t heap_quantum = (size_t)2 * 1024 * 1024;
  _Static_assert(((OC_MANAGED_BYTES + SO_REGION_BYTES) &
                  ((size_t)2 * 1024 * 1024 - 1u)) == 0,
                 "fixed Horizon heap must be 2 MiB aligned");
  g_initial_heap_bytes = size;
  g_loader_heap_prefix_bytes = 0;
  g_kernel_heap_bytes = requested;
  g_kernel_heap_base = 0;
  g_heap_donor_base = NULL;
  g_heap_donor_capacity = 0;
  g_heap_donor_active_bytes = 0;
  g_heap_donor_kernel_offset = 0;
  g_system_resource_size_total = 0;
  if (R_SUCCEEDED(svcGetInfo(&g_system_resource_size_total,
                             InfoType_SystemResourceSizeTotal,
                             CUR_PROCESS_HANDLE, 0)) &&
      g_system_resource_size_total != 0) {
    g_memory_backing_backend = NX_MEMORY_BACKEND_PHYSICAL;
  } else {
    g_system_resource_size_total = 0;
    g_memory_backing_backend = NX_MEMORY_BACKEND_HEAP_ALIAS;
  }

  void *kernel_heap = NULL;
  size_t retained = requested;
  size_t required = requested;
  if (overridden) {
    u64 heap_region_base = 0;
    u64 heap_region_size = 0;
    const Result base_result = svcGetInfo(
      &heap_region_base, InfoType_HeapRegionAddress,
      CUR_PROCESS_HANDLE, 0);
    const Result size_result = svcGetInfo(
      &heap_region_size, InfoType_HeapRegionSize,
      CUR_PROCESS_HANDLE, 0);
    const uintptr_t override_base = (uintptr_t)addr;
    if (R_FAILED(base_result) || R_FAILED(size_result) || !addr ||
        !heap_region_base ||
        override_base < (uintptr_t)heap_region_base) {
      diagAbortWithResult(
        MAKERESULT(Module_Libnx, LibnxError_HeapAllocFailed));
    }

    if (override_base > UINTPTR_MAX - OC_MANAGED_BYTES ||
        override_base + OC_MANAGED_BYTES > UINTPTR_MAX - (0x4000u - 1u)) {
      diagAbortWithResult(
        MAKERESULT(Module_Libnx, LibnxError_HeapAllocFailed));
    }
    const uintptr_t module_base = ALIGN_MEM(
      override_base + OC_MANAGED_BYTES, 0x4000);
    const size_t module_offset = (size_t)(module_base - override_base);
    if (module_base < override_base ||
        module_offset > SIZE_MAX - SO_REGION_BYTES) {
      diagAbortWithResult(
        MAKERESULT(Module_Libnx, LibnxError_HeapAllocFailed));
    }
    required = module_offset + SO_REGION_BYTES;
    if (size < required) {
      diagAbortWithResult(
        MAKERESULT(Module_Libnx, LibnxError_HeapAllocFailed));
    }

    const size_t prefix = (size_t)(override_base -
                                   (uintptr_t)heap_region_base);
    if ((u64)prefix > heap_region_size ||
        (u64)size > heap_region_size - (u64)prefix ||
        prefix > SIZE_MAX - required ||
        prefix + required > SIZE_MAX - (heap_quantum - 1u)) {
      diagAbortWithResult(
        MAKERESULT(Module_Libnx, LibnxError_HeapAllocFailed));
    }

    /* The hbloader suffix can begin on a 4 KiB boundary even though the
     * kernel heap end must remain 2 MiB aligned.  Keep at most one alignment
     * quantum of harmless suffix padding rather than truncating a live page. */
    const size_t fixed_kernel_total =
      (prefix + required + heap_quantum - 1u) & ~(heap_quantum - 1u);
    if (fixed_kernel_total < prefix ||
        fixed_kernel_total - prefix > size) {
      diagAbortWithResult(
        MAKERESULT(Module_Libnx, LibnxError_HeapAllocFailed));
    }

    g_loader_heap_prefix_bytes = prefix;
    retained = fixed_kernel_total - prefix;
    size_t target_kernel_total = fixed_kernel_total;
    if (g_memory_backing_backend == NX_MEMORY_BACKEND_HEAP_ALIAS) {
      if (prefix > SIZE_MAX - size) {
        diagAbortWithResult(
          MAKERESULT(Module_Libnx, LibnxError_HeapAllocFailed));
      }
      const size_t original_kernel_total = prefix + size;
      if ((original_kernel_total & (heap_quantum - 1u)) != 0 ||
          original_kernel_total > (size_t)heap_region_size ||
          original_kernel_total < fixed_kernel_total) {
        diagAbortWithResult(
          MAKERESULT(Module_Libnx, LibnxError_HeapAllocFailed));
      }
      const size_t donor_capacity =
        original_kernel_total - fixed_kernel_total;
      size_t donor_initial = OC_HEAP_DONOR_INITIAL_BYTES;
      if (donor_initial > donor_capacity) donor_initial = donor_capacity;
      donor_initial &= ~(heap_quantum - 1u);
      if (donor_initial < OC_DYNAMIC_SEGMENT_BYTES ||
          donor_capacity % OC_HEAP_DONOR_UNIT_BYTES != 0) {
        diagAbortWithResult(
          MAKERESULT(Module_Libnx, LibnxError_HeapAllocFailed));
      }
      target_kernel_total += donor_initial;
      g_heap_donor_capacity = donor_capacity;
      g_heap_donor_active_bytes = donor_initial;
      g_heap_donor_kernel_offset = fixed_kernel_total;
    }
    g_kernel_heap_bytes = target_kernel_total;
    g_heap_resize_result = svcSetHeapSize(&kernel_heap,
                                         target_kernel_total);
    if (R_FAILED(g_heap_resize_result))
      diagAbortWithResult(g_heap_resize_result);
    if (kernel_heap != (void *)(uintptr_t)heap_region_base) {
      diagAbortWithResult(
        MAKERESULT(Module_Libnx, LibnxError_HeapAllocFailed));
    }
  } else {
    size_t target = requested;
    if (g_memory_backing_backend == NX_MEMORY_BACKEND_HEAP_ALIAS) {
      if (target > SIZE_MAX - OC_HEAP_DONOR_INITIAL_BYTES) {
        diagAbortWithResult(
          MAKERESULT(Module_Libnx, LibnxError_HeapAllocFailed));
      }
      target += OC_HEAP_DONOR_INITIAL_BYTES;
      g_heap_donor_capacity = OC_HEAP_DONOR_INITIAL_BYTES;
      g_heap_donor_active_bytes = OC_HEAP_DONOR_INITIAL_BYTES;
      g_heap_donor_kernel_offset = requested;
    }
    g_initial_heap_bytes = target;
    g_kernel_heap_bytes = target;
    g_heap_resize_result = svcSetHeapSize(&kernel_heap, target);
    if (R_FAILED(g_heap_resize_result))
      diagAbortWithResult(g_heap_resize_result);
    if (!kernel_heap) {
      diagAbortWithResult(
        MAKERESULT(Module_Libnx, LibnxError_HeapAllocFailed));
    }
    addr = kernel_heap;
  }
  g_kernel_heap_base = (uintptr_t)kernel_heap;
  g_fixed_heap_bytes = retained;
  g_heap_suffix_base = (uintptr_t)addr;
  if (g_memory_backing_backend == NX_MEMORY_BACKEND_HEAP_ALIAS) {
    if (!g_heap_donor_kernel_offset || !g_heap_donor_capacity ||
        !g_heap_donor_active_bytes ||
        g_kernel_heap_base > UINTPTR_MAX - g_heap_donor_kernel_offset) {
      diagAbortWithResult(
        MAKERESULT(Module_Libnx, LibnxError_HeapAllocFailed));
    }
    g_heap_donor_base = (void *)(g_kernel_heap_base +
                                 g_heap_donor_kernel_offset);
    if ((uintptr_t)g_heap_donor_base !=
        (uintptr_t)addr + retained) {
      diagAbortWithResult(
        MAKERESULT(Module_Libnx, LibnxError_HeapAllocFailed));
    }
  }

  fake_heap_start = (char *)addr;
  fake_heap_end = (char *)addr + OC_MANAGED_BYTES;
  heap_so_base = (void *)ALIGN_MEM((uintptr_t)addr + OC_MANAGED_BYTES, 0x4000);
  const size_t module_offset =
    (size_t)((uintptr_t)heap_so_base - (uintptr_t)addr);
  if ((uintptr_t)heap_so_base < (uintptr_t)addr ||
      module_offset > retained ||
      SO_REGION_BYTES > retained - module_offset) {
    diagAbortWithResult(
      MAKERESULT(Module_Libnx, LibnxError_HeapAllocFailed));
  }
  heap_so_limit = retained - module_offset;
  g_oc_pool_base = NULL;
  g_oc_pool_size = 0;
  g_mmap_arena_base = NULL;
  g_mmap_arena_size = 0;
  g_oc_want = 1;
}

static int make_dir(const char *path) {
  return mkdir(path, 0777) == 0 || errno == EEXIST;
}

static void make_runtime_dirs(void) {
  static const char *dirs[] = {
    DATA_ROOT "/files", DATA_ROOT "/cache", DATA_ROOT "/no_backup",
    DATA_ROOT "/shared_prefs",
  };
  for (size_t i = 0; i < sizeof(dirs) / sizeof(dirs[0]); ++i)
    if (!make_dir(dirs[i]))
      fatal_error("Could not create runtime directory:\n%s", dirs[i]);
}

static int safe_data_child(const char *relative) {
  if (!relative || !*relative || relative[0] == '/' ||
      strchr(relative, '\\') || strchr(relative, ':'))
    return 0;
  const char *part = relative;
  for (const char *cursor = relative;; ++cursor) {
    if (*cursor == '/' || *cursor == 0) {
      const size_t length = (size_t)(cursor - part);
      if (!length || (length == 1 && part[0] == '.') ||
          (length == 2 && part[0] == '.' && part[1] == '.'))
        return 0;
      if (!*cursor) break;
      part = cursor + 1;
    }
  }
  return 1;
}

/* Delete only an already-validated, explicitly named child of DATA_ROOT.
 * lstat prevents an extracted symlink from redirecting cleanup elsewhere. */
static int remove_tree_path(const char *path) {
  struct stat st;
  if (lstat(path, &st) != 0) return errno == ENOENT;
  if (!S_ISDIR(st.st_mode)) return unlink(path) == 0;
  DIR *dir = opendir(path);
  if (!dir) return 0;
  int ok = 1;
  struct dirent *entry;
  while (ok && (entry = readdir(dir)) != NULL) {
    if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
    char child[1024];
    const int length = snprintf(child, sizeof child, "%s/%s", path,
                                entry->d_name);
    if (length < 0 || (size_t)length >= sizeof child ||
        !remove_tree_path(child))
      ok = 0;
  }
  if (closedir(dir) != 0) ok = 0;
  return ok && rmdir(path) == 0;
}

static int remove_data_child(const char *relative) {
  if (!safe_data_child(relative)) return 0;
  char path[1024];
  const int length = snprintf(path, sizeof path, "%s/%s", DATA_ROOT, relative);
  if (length < 0 || (size_t)length >= sizeof path) return 0;
  struct stat st;
  if (lstat(path, &st) != 0) return errno == ENOENT;
  if (!remove_tree_path(path)) return 0;

  return 1;
}

static int dex_name(const char *name) {
  if (!name || strncmp(name, "classes", 7)) return 0;
  const char *cursor = name + 7;
  if (!strcmp(cursor, ".dex")) return 1;
  if (*cursor < '0' || *cursor > '9') return 0;
  while (*cursor >= '0' && *cursor <= '9') ++cursor;
  return !strcmp(cursor, ".dex");
}

static int cleanup_android_extraction(void) {
  static const char *children[] = {
    "META-INF", "res", "kotlin", "original", "stamp-cert-sha256",
    "lib/armeabi-v7a", "lib/x86", "lib/x86_64",
    "apk", "port", "AndroidManifest.xml", "resources.arsc",
    "manifest.json", "xapk-manifest.json", "icon.png", "base.apk",
    "genshinimpact_nx.nacp",
  };
  for (size_t i = 0; i < sizeof children / sizeof children[0]; ++i)
    if (!remove_data_child(children[i])) return 0;

  DIR *root = opendir(DATA_ROOT);
  if (!root) return 0;
  char dex_files[64][NAME_MAX + 1];
  size_t dex_count = 0;
  struct dirent *entry;
  while ((entry = readdir(root)) != NULL) {
    if (!dex_name(entry->d_name)) continue;
    if (dex_count == sizeof dex_files / sizeof dex_files[0]) {
      closedir(root);
      return 0;
    }
    snprintf(dex_files[dex_count++], sizeof dex_files[0], "%s",
             entry->d_name);
  }
  if (closedir(root) != 0) return 0;
  for (size_t i = 0; i < dex_count; ++i)
    if (!remove_data_child(dex_files[i])) return 0;
  return 1;
}

static void check_syscalls(void) {
  if (hosversionBefore(6, 0, 0))
    fatal_error("Horizon OS 6.0.0 or newer is required for complete thread contexts.");
  if (!envIsSyscallHinted(0x32)) fatal_error("svcSetThreadActivity is unavailable.");
  if (!envIsSyscallHinted(0x33)) fatal_error("svcGetThreadContext3 is unavailable.");
  if (g_memory_backing_backend == NX_MEMORY_BACKEND_PHYSICAL) {
    if (!envIsSyscallHinted(0x2c))
      fatal_error("svcMapPhysicalMemory is unavailable.");
    if (!envIsSyscallHinted(0x2d))
      fatal_error("svcUnmapPhysicalMemory is unavailable.");
  }
  if (!envIsSyscallHinted(0x77)) fatal_error("svcMapProcessCodeMemory is unavailable.");
  if (!envIsSyscallHinted(0x78)) fatal_error("svcUnmapProcessCodeMemory is unavailable.");
  if (!envIsSyscallHinted(0x73)) fatal_error("svcSetProcessMemoryPermission is unavailable.");
  if (envGetOwnProcessHandle() == INVALID_HANDLE)
    fatal_error("Own process handle is unavailable. Launch through title override.");
}

static int bootstrap_memory_range_state(const void *address, size_t length,
                                        unsigned permissions,
                                        unsigned memory_type) {
  const uintptr_t begin = (uintptr_t)address;
  if (!begin || !length || length > UINTPTR_MAX - begin) return 0;
  const uintptr_t end = begin + length;
  for (uintptr_t at = begin; at < end; ) {
    MemoryInfo info = {0};
    u32 page_info = 0;
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

/* Prove after newlib is live that the pre-main shrink retained exactly the
 * fixed hbloader suffix plus the optional initial donor floor, and that its
 * released tail is genuinely free.  The suffix
 * anchor is captured inside __libnx_initheap while the HBABI override is still
 * authoritative; fake_heap_start/end are allocator configuration exports, not
 * process-heap ownership records, and must not be used as late verifier state.
 *
 * Do not require every interior page to retain one memory state: libnx may
 * legitimately loan heap pages for console, transfer-memory, or caller-owned
 * stack mappings before this check runs.  The untouched last retained page
 * and the first released page define the kernel heap boundary unambiguously. */
static void validate_fixed_heap_reclaim(void) {
  extern char *fake_heap_start;
  extern char *fake_heap_end;
  const uintptr_t suffix = g_heap_suffix_base;
  const uintptr_t module_base = (uintptr_t)heap_so_base;
  const size_t module_offset = module_base >= suffix
    ? (size_t)(module_base - suffix) : SIZE_MAX;
  if (!suffix ||
      module_offset < OC_MANAGED_BYTES ||
      module_offset > g_fixed_heap_bytes ||
      SO_REGION_BYTES > g_fixed_heap_bytes - module_offset ||
      g_fixed_heap_bytes > UINTPTR_MAX - suffix ||
      module_base != ALIGN_MEM(suffix + OC_MANAGED_BYTES, 0x4000) ||
      heap_so_limit != g_fixed_heap_bytes - module_offset) {
    fatal_error("Fixed heap bootstrap geometry changed (suffix=%p fixed=0x%zx module=%p/0x%zx limit=0x%zx).",
                (void *)suffix, g_fixed_heap_bytes, (void *)module_base,
                module_offset, heap_so_limit);
  }

  const uintptr_t retained_end = suffix + g_fixed_heap_bytes;
  const int donor_backend =
    g_memory_backing_backend == NX_MEMORY_BACKEND_HEAP_ALIAS;
  const uintptr_t donor_base = (uintptr_t)g_heap_donor_base;
  const size_t donor_active = __atomic_load_n(
    &g_heap_donor_active_bytes, __ATOMIC_ACQUIRE);
  const uintptr_t kernel_end = donor_backend
    ? donor_base + donor_active : retained_end;
  if ((donor_backend &&
       (!donor_base || donor_base != retained_end || !donor_active ||
        donor_active > g_heap_donor_capacity ||
        donor_base > UINTPTR_MAX - donor_active ||
        g_kernel_heap_base > UINTPTR_MAX - g_kernel_heap_bytes ||
        g_kernel_heap_base + g_kernel_heap_bytes != kernel_end)) ||
      (!donor_backend && (g_heap_donor_base || g_heap_donor_capacity ||
                          g_heap_donor_active_bytes))) {
    fatal_error("Heap donor bootstrap geometry changed (backend=%u base=%p active=0x%zx capacity=0x%zx kernel=0x%zx).",
                (unsigned)g_memory_backing_backend,
                g_heap_donor_base, donor_active,
                g_heap_donor_capacity, g_kernel_heap_bytes);
  }
  MemoryInfo retained_info = {0};
  u32 retained_page_info = 0;
  const uintptr_t retained_probe = retained_end - 1u;
  if (R_FAILED(svcQueryMemory(&retained_info, &retained_page_info,
                              retained_probe)) ||
      retained_info.addr > retained_probe ||
      retained_info.size > UINT64_MAX - retained_info.addr ||
      retained_info.addr + retained_info.size <= retained_probe ||
      retained_info.type != MemType_Heap || retained_info.perm != Perm_Rw) {
    fatal_error("Last retained heap page is not RW (address %p type %u perm 0x%x).",
                (void *)retained_probe, (unsigned)retained_info.type,
                (unsigned)retained_info.perm);
  }
  if (g_loader_heap_prefix_bytes) {
    if (g_loader_heap_prefix_bytes > suffix ||
        g_kernel_heap_bytes < g_loader_heap_prefix_bytes ||
        suffix - g_loader_heap_prefix_bytes >
          UINTPTR_MAX - g_kernel_heap_bytes ||
        suffix - g_loader_heap_prefix_bytes != g_kernel_heap_base ||
        g_kernel_heap_base > UINTPTR_MAX - g_kernel_heap_bytes ||
        g_kernel_heap_base + g_kernel_heap_bytes != kernel_end) {
      fatal_error("HBABI heap prefix/suffix ownership invariant failed.");
    }
    if (donor_backend &&
        !bootstrap_memory_range_state(g_heap_donor_base, donor_active,
                                      Perm_Rw, MemType_Heap)) {
      fatal_error("Initial heap donor bank is not complete RW Horizon heap memory.");
    }
    const size_t live_suffix = g_fixed_heap_bytes +
      (donor_backend ? donor_active : 0);
    if (g_initial_heap_bytes > live_suffix) {
      MemoryInfo info;
      u32 page_info = 0;
      if (R_FAILED(svcQueryMemory(&info, &page_info, kernel_end)) ||
          info.addr > kernel_end ||
          info.size > UINT64_MAX - info.addr ||
          info.addr + info.size <= kernel_end ||
          info.type != MemType_Unmapped || info.perm != 0) {
        fatal_error("HBABI heap tail was not released to Horizon.");
      }
    }
  }
}

typedef struct {
  volatile uint32_t ready;
  volatile uint32_t run;
  Thread *thread;
} ThreadContextProbe;

static void *thread_context_probe_entry(void *opaque) {
  ThreadContextProbe *probe = opaque;
  probe->thread = threadGetSelf();
  __atomic_store_n(&probe->ready, 1, __ATOMIC_RELEASE);
  while (__atomic_load_n(&probe->run, __ATOMIC_ACQUIRE))
    svcSleepThread(UINT64_C(1000000));
  return NULL;
}

/* Fail before Unity starts if the kernel cannot perform the primitive used by
 * the exact-client stop-the-world bridge.  The test deliberately captures a
 * wrapper-owned thread and resumes it on every post-pause path. */
static int check_thread_context_bridge(void) {
  ThreadContextProbe probe = { .run = 1 };
  pthread_t pthread;
  int safe_to_join = 1;
  int error = pthread_create(&pthread, NULL, thread_context_probe_entry, &probe);
  if (error) return error;

  unsigned waits = 0;
  while (!__atomic_load_n(&probe.ready, __ATOMIC_ACQUIRE) && waits++ < 5000)
    svcSleepThread(UINT64_C(1000000));
  if (!__atomic_load_n(&probe.ready, __ATOMIC_ACQUIRE) || !probe.thread) {
    error = ETIMEDOUT;
  } else {
    Result pause_result = threadPause(probe.thread);
    if (R_FAILED(pause_result)) {
      error = (int)pause_result;
    } else {
      ThreadContext context;
      memset(&context, 0, sizeof(context));
      Result dump_result = threadDumpContext(&context, probe.thread);
      Result resume_result = threadResume(probe.thread);
      if (R_FAILED(dump_result)) error = (int)dump_result;
      if (R_FAILED(resume_result)) {
        error = (int)resume_result;
        safe_to_join = 0;
      }
      else if (!error &&
               (!context.sp || !context.pc.x || ((uintptr_t)context.sp & 15u)))
        error = EPROTO;
    }
  }

  __atomic_store_n(&probe.run, 0, __ATOMIC_RELEASE);
  if (!safe_to_join) return error;
  const int join_result = pthread_join(pthread, NULL);
  if (!error && join_result) error = join_result;
  return error;
}

static int file_contains(const char *path, const char *needle) {
  int packed = 0;
  int fd = -1;
  if (asset_pack_active() && !strncmp(path, "assets/", 7)) {
    fd = asset_pack_open_path(path);
    packed = fd >= 0 && asset_pack_fd_is(fd);
  }
  if (fd < 0) fd = open(path, O_RDONLY);
  if (fd < 0) return 0;
  const size_t want = strlen(needle);
  uint8_t *buf = malloc(1024 * 1024 + want);
  if (!buf) {
    if (packed) asset_pack_close_fd(fd); else close(fd);
    return 0;
  }
  size_t carry = 0;
  int found = 0;
  while (!found) {
    const long got = packed ? asset_pack_read_fd(fd, buf + carry, 1024 * 1024) :
                              (long)read(fd, buf + carry, 1024 * 1024);
    if (got < 0) break;
    const size_t used = carry + (size_t)got;
    for (size_t i = 0; i + want <= used; ++i) {
      if (!memcmp(buf + i, needle, want)) { found = 1; break; }
    }
    if (!got) break;
    carry = want > 1 && used >= want - 1 ? want - 1 : used;
    memmove(buf, buf + used - carry, carry);
  }
  free(buf);
  if (packed) asset_pack_close_fd(fd); else close(fd);
  return found;
}

static int data_regular_size(const char *path, uint64_t *size) {
  if (asset_pack_active() && !strncmp(path, "assets/", 7)) {
    uint64_t ino = 0;
    if (asset_pack_stat_relative(path + 7, size, &ino)) return 1;
  }
  struct stat st;
  if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) return 0;
  if (size) *size = (uint64_t)st.st_size;
  return 1;
}

static void require_regular(const char *path, uint64_t minimum_size) {
  uint64_t size = 0;
  if (!data_regular_size(path, &size) || size < minimum_size)
    fatal_error("Missing or incomplete game file:\n%s\n\nExtract the supported arm64 APK contents beside genshinimpact_nx.nro and launch again.",
                path);
}

static void check_data(void) {
  require_regular(LIB_GAME, 300ULL * 1024 * 1024);
  require_regular("assets/bin/Data/globalgamemanagers", 1024);
  require_regular("assets/bin/Data/Managed/Metadata/global-metadata.dat", 1024 * 1024);
  require_regular("assets/bin/Data/Managed/Metadata/global-metadata.md5", 32);
  require_regular("assets/bin/Data/Managed/Metadata/startup-metadata.dat", 1024 * 1024);
  require_regular("assets/bin/Data/boot.config", 16);
  require_regular("assets/bin/Data/level0", 1024);
  require_regular("assets/AssetBundles/data_revision", 1);
  require_regular("assets/data_versions_streaming", 1);
  require_regular("assets/res_versions_streaming", 1);
  require_regular("assets/svc_catalog", 1);
  require_regular("certs/cacert.pem", 100 * 1024);
  if (!file_contains("certs/cacert.pem", "-----BEGIN CERTIFICATE-----"))
    fatal_error("The wrapper TLS CA bundle is missing or invalid. Copy the complete release beside the extracted APK contents.");
  if (!file_contains("assets/bin/Data/globalgamemanagers", "2017.4.30f1"))
    fatal_error("Unsupported game data. This wrapper requires Genshin Impact %s / Unity 2017.4.30f1.",
                SS_VERSION_NAME);
}

static int exact_game_library_hash(void) {
  static const uint8_t expected[SHA256_HASH_SIZE] = {
    0x26,0xc8,0x62,0xb1,0x47,0xd2,0x82,0x2a,
    0x39,0xe5,0x46,0x4e,0x76,0x16,0x11,0x76,
    0x7a,0xba,0xec,0x1a,0x54,0x16,0x98,0xac,
    0x53,0xf8,0x0c,0x13,0x5a,0x9a,0x42,0xd1,
  };
  uint8_t *buffer = malloc(1024 * 1024);
  FILE *file = fopen(LIB_GAME, "rb");
  if (!buffer || !file) { free(buffer); if (file) fclose(file); return 0; }
  Sha256Context context;
  sha256ContextCreate(&context);
  size_t got;
  while ((got = fread(buffer, 1, 1024 * 1024, file)) != 0)
    sha256ContextUpdate(&context, buffer, got);
  const int read_ok = !ferror(file);
  uint8_t digest[SHA256_HASH_SIZE];
  sha256ContextGetHash(&context, digest);
  fclose(file);
  free(buffer);
  return read_ok && !memcmp(digest, expected, sizeof expected);
}

static int loose_assets_present(void) {
  struct stat st;
  return stat(DATA_ROOT "/assets/bin/Data/globalgamemanagers", &st) == 0 &&
         S_ISREG(st.st_mode);
}

static int client_metadata_version_matches(void) {
  static const char path[] = DATA_ROOT "/no_backup/nx_client_version";
  static const char expected[] = SS_VERSION_NAME "\n";
  char actual[sizeof expected];
  FILE *file = fopen(path, "rb");
  if (!file) return 0;
  const size_t got = fread(actual, 1, sizeof actual, file);
  const int read_ok = !ferror(file);
  const int close_ok = fclose(file) == 0;
  return read_ok && close_ok && got == sizeof expected - 1u &&
         !memcmp(actual, expected, sizeof expected - 1u);
}

static int update_client_metadata_version(void) {
  static const char path[] = DATA_ROOT "/no_backup/nx_client_version";
  static const char temporary[] =
    DATA_ROOT "/no_backup/.nx_client_version.tmp";
  static const char contents[] = SS_VERSION_NAME "\n";
  FILE *file = fopen(temporary, "wb");
  if (!file) return 0;
  int ok = fwrite(contents, 1, sizeof contents - 1u, file) ==
             sizeof contents - 1u &&
           fflush(file) == 0 && fsync(fileno(file)) == 0;
  if (fclose(file) != 0) ok = 0;
  if (ok && rename(temporary, path) == 0) return 1;
  /* FAT does not consistently replace an existing destination. */
  if (ok && unlink(path) == 0 && rename(temporary, path) == 0) return 1;
  unlink(temporary);
  return 0;
}

static void refresh_client_metadata_cache(int assets_rebuilt) {
  if (!assets_rebuilt && client_metadata_version_matches()) return;
  if (!remove_data_child("files/il2cpp/Metadata/global-metadata.dat") ||
      !remove_data_child("files/il2cpp/Metadata/startup-metadata.dat") ||
      !update_client_metadata_version())
    fatal_error("The client assets are valid, but stale IL2CPP metadata could not be invalidated.");
}

/* First boot and client upgrades accept the normal APK extraction layout
 * directly.  The exact library is always verified, and a pack is reusable only
 * when its header names this client version.  A replacement pack is fully
 * written and verified before the old pair is changed; only then are stale
 * derived metadata and redundant loose files removed. */
static void prepare_game_data(void) {
  const int loose_assets = loose_assets_present();
  /* Loose assets are an explicit staging set.  Never let an older valid pack
   * shadow them, even when both builds happen to share a versionCode. */
  const int existing_pack = loose_assets
    ? 0 : asset_pack_open_existing(DATA_ROOT, SS_VERSION_CODE);
  check_data();

  startup_status_update("Verifying the extracted Android client");
  if (!exact_game_library_hash())
    fatal_error("Unsupported libyuanshen.so. This wrapper requires SHA-256:\n%s",
                "26c862b147d2822a39e5464e761611767abaec1a541698ac53f80c135a9a42d1");

  if (!existing_pack) {
    if (!asset_pack_build(DATA_ROOT "/assets", DATA_ROOT, SS_VERSION_CODE))
      fatal_error("Could not optimize the extracted APK assets. No source files were removed.\n\n%s",
                  asset_pack_error());
  }
  refresh_client_metadata_cache(!existing_pack);

  if (loose_assets) {
    startup_status_update("Cleaning loose Android assets");
    if (!remove_data_child("assets") || !make_dir(DATA_ROOT "/assets"))
      fatal_error("The optimized assets are valid, but the loose APK assets could not be cleaned up.");
  }
  if (!cleanup_android_extraction())
    fatal_error("The Android-only APK extraction files could not be cleaned up.");
}

/* global-metadata.dat is packaged with an MHY protection header and the exact
 * client derives a readable copy below files/il2cpp.  A killed/failed prior
 * run can leave a zero or truncated cache that Unity otherwise accepts as an
 * existing candidate and maps with length one.  Remove only that impossible
 * derived state so the client performs its normal regeneration path. */
static void discard_incomplete_il2cpp_metadata_cache(void) {
  static const char *paths[] = {
    DATA_ROOT "/files/il2cpp/Metadata/global-metadata.dat",
    DATA_ROOT "/files/il2cpp/Metadata/startup-metadata.dat",
  };
  for (size_t i = 0; i < sizeof paths / sizeof paths[0]; ++i) {
    struct stat st;
    if (stat(paths[i], &st) != 0 || !S_ISREG(st.st_mode) ||
        st.st_size >= 1024 * 1024)
      continue;
    (void)unlink(paths[i]);
  }
}

/* Exercise the exact open -> dup -> page-rounded mmap path the first
 * nativeRender needs.  This runs after the 4104 MiB slab is reserved, then
 * releases its temporary arena mapping before constructors. */
static void check_startup_metadata_mapping(void) {
  static const unsigned char expected_header[16] = {
    0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00,
    0x03, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00,
  };
  const char *path =
    "assets/bin/Data/Managed/Metadata/startup-metadata.dat";
  int source = open_fake(path, 0);
  if (source < 0)
    fatal_error("Could not open startup metadata for the mmap self-test (errno %d).",
                errno);
  int duplicate = dup_fake(source);
  if (duplicate < 0) {
    const int saved = errno;
    close_fake(source);
    fatal_error("Could not duplicate startup metadata for the mmap self-test (errno %d).",
                saved);
  }
  void *mapping = mmap_fake(NULL, STARTUP_METADATA_MAP_SIZE, 1, 1,
                            duplicate, 0);
  if (mapping == (void *)-1) {
    const int saved = errno;
    close_fake(duplicate);
    close_fake(source);
    fatal_error("Startup metadata mmap self-test failed (errno %d).", saved);
  }
  int valid = memcmp(mapping, expected_header, sizeof expected_header) == 0;
  for (size_t i = STARTUP_METADATA_SIZE;
       valid && i < STARTUP_METADATA_MAP_SIZE; ++i)
    if (((const unsigned char *)mapping)[i] != 0) valid = 0;
  const int unmap_result = munmap_fake(mapping, STARTUP_METADATA_MAP_SIZE);
  const int duplicate_close = close_fake(duplicate);
  const int source_close = close_fake(source);
  if (!valid || unmap_result != 0 || duplicate_close != 0 || source_close != 0)
    fatal_error("Startup metadata mmap self-test produced invalid contents or descriptor teardown.");
}

/* The client reads this digest with ordinary sequential read(), separately
 * from both metadata mmaps.  Exercise the same open/read/EOF/close route before
 * constructors so a stale duplicate, cursor bug, or truncated staged digest
 * fails with a direct wrapper diagnostic instead of a managed startup throw. */
static void check_global_metadata_digest_read(void) {
  const char *path =
    "assets/bin/Data/Managed/Metadata/global-metadata.md5";
  char digest[33];
  memset(digest, 0, sizeof(digest));
  int fd = open_fake(path, O_RDONLY);
  if (fd < 0)
    fatal_error("Could not open global metadata digest for the sequential-read self-test (errno %d).",
                errno);
  const long got = read_fake(fd, digest, 32);
  unsigned char extra = 0;
  const long eof = got == 32 ? read_fake(fd, &extra, 1) : -1;
  const int close_result = close_fake(fd);
  int valid = got == 32 && eof == 0 && close_result == 0;
  for (size_t i = 0; valid && i < 32; ++i)
    if (!((digest[i] >= '0' && digest[i] <= '9') ||
          (digest[i] >= 'a' && digest[i] <= 'f') ||
          (digest[i] >= 'A' && digest[i] <= 'F')))
      valid = 0;
  if (!valid)
    fatal_error("Global metadata digest sequential-read self-test failed: read=%ld eof=%ld close=%d errno=%d.",
                got, eof, close_result, errno);
}

/* Exercise the direct extracted-package asset path and the exact
 * seek-before-read sequence the client uses.  After first boot this
 * transparently traverses the optimized pack. */
static void check_globalgamemanagers_seek_read(void) {
  static const unsigned char expected_header[32] = {
    0x00, 0x00, 0x84, 0xea, 0x00, 0x1e, 0x19, 0x94,
    0x00, 0x00, 0x00, 0x11, 0x00, 0x00, 0x85, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x32, 0x30, 0x31, 0x37,
    0x2e, 0x34, 0x2e, 0x33, 0x30, 0x66, 0x31, 0x00,
  };
  const char *path = managed_path(
    GAME_HOME "/assets/bin/Data/globalgamemanagers");
  unsigned char *buffer = malloc(7168);
  if (!buffer)
    fatal_error("Could not allocate the globalgamemanagers seek/read self-test buffer.");
  int fd = open_fake(path, O_RDONLY);
  if (fd < 0) {
    const int saved = errno;
    free(buffer);
    fatal_error("Could not open globalgamemanagers for the seek/read self-test (errno %d).",
                saved);
  }
  const long position = z_lseek(fd, 0, SEEK_SET);
  const long got = position == 0 ? read_fake(fd, buffer, 7168) : -1;
  const int operation_errno = errno;
  const int close_result = close_fake(fd);
  const int valid = position == 0 && got == 7168 && close_result == 0 &&
                    memcmp(buffer, expected_header, sizeof expected_header) == 0;
  free(buffer);
  if (!valid)
    fatal_error("globalgamemanagers seek/read self-test failed: seek=%ld read=%ld close=%d errno=%d.",
                position, got, close_result, operation_errno);
}

static void check_synthetic_cpu_topology_read(void) {
  const char *path = "/sys/devices/system/cpu/online";
  char value[8] = {0};
  int fd = open_fake(path, O_RDONLY);
  if (fd < 0)
    fatal_error("Could not open the synthetic CPU topology for its read self-test (errno %d).",
                errno);
  const long got = read_fake(fd, value, sizeof(value) - 1);
  unsigned char extra = 0;
  const long eof = got == 4 ? read_fake(fd, &extra, 1) : -1;
  const int operation_errno = errno;
  const int close_result = close_fake(fd);
  if (got != 4 || eof != 0 || close_result != 0 || memcmp(value, "0-2\n", 4))
    fatal_error("Synthetic CPU topology read self-test failed: read=%ld eof=%ld close=%d errno=%d.",
                got, eof, close_result, operation_errno);
}

static void check_force_vulkan_sentinel(void) {
  FILE *file = fopen_fake(DATA_ROOT "/files/_FORCE_VULKAN_", "r");
  if (!file)
    fatal_error("Private Vulkan renderer sentinel self-test failed to open (errno %d).",
                errno);
  const int value = fgetc_fake(file);
  const int eof = fgetc_fake(file);
  const int close_result = fclose_fake(file);
  if (value != 1 || eof != EOF || close_result != 0)
    fatal_error("Private Vulkan renderer sentinel self-test failed: value=%d eof=%d close=%d.",
                value, eof, close_result);
}

/* Unity enumerates Android EGL framebuffer capabilities even when its selected
 * renderer is Vulkan.  The loaderless NVK SDK's EGL anchors intentionally
 * fail, so verify every configuration query is routed to the deterministic
 * compatibility object before guest initialization. */
static void check_vulkan_egl_configuration(void) {
  static const EGLint attributes[] = {
    EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
    EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
    EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8,
    EGL_ALPHA_SIZE, 8, EGL_DEPTH_SIZE, 24, EGL_STENCIL_SIZE, 8,
    EGL_NONE,
  };
  EGLDisplay display = nx_eglGetDisplay_stub(EGL_DEFAULT_DISPLAY);
  EGLConfig config = NULL;
  EGLint major = 0, minor = 0, count = 0;
  EGLint red = 0, green = 0, blue = 0, alpha = 0, depth = 0, stencil = 0;
  GLint shader_range[2] = {0, 0}, shader_precision = 0;
  int valid = display != EGL_NO_DISPLAY &&
              nx_eglInitialize_stub(display, &major, &minor) == EGL_TRUE &&
              major == 1 && minor == 4 &&
              nx_eglChooseConfig_stub(display, attributes, NULL, 0, &count) == EGL_TRUE &&
              count == 1 &&
              nx_eglChooseConfig_stub(display, attributes, &config, 1, &count) == EGL_TRUE &&
              config != NULL && count == 1 &&
              nx_eglGetConfigAttrib_stub(display, config, EGL_RED_SIZE, &red) == EGL_TRUE &&
              nx_eglGetConfigAttrib_stub(display, config, EGL_GREEN_SIZE, &green) == EGL_TRUE &&
              nx_eglGetConfigAttrib_stub(display, config, EGL_BLUE_SIZE, &blue) == EGL_TRUE &&
              nx_eglGetConfigAttrib_stub(display, config, EGL_ALPHA_SIZE, &alpha) == EGL_TRUE &&
              nx_eglGetConfigAttrib_stub(display, config, EGL_DEPTH_SIZE, &depth) == EGL_TRUE &&
              nx_eglGetConfigAttrib_stub(display, config, EGL_STENCIL_SIZE, &stencil) == EGL_TRUE &&
              red == 8 && green == 8 && blue == 8 && alpha == 8 &&
              depth == 24 && stencil == 8 &&
              nx_eglGetError_stub() == EGL_SUCCESS;
  nx_glGetShaderPrecisionFormat_stub(GL_FRAGMENT_SHADER, GL_HIGH_FLOAT,
                                     shader_range, &shader_precision);
  valid = valid && shader_range[0] == 127 && shader_range[1] == 127 &&
          shader_precision == 23 && nx_glGetError_stub() == GL_NO_ERROR;
  const EGLBoolean terminated = nx_eglTerminate_stub(display);
  if (!valid || terminated != EGL_TRUE)
    fatal_error("Vulkan-only EGL configuration self-test failed: EGL 0x%x.",
                (unsigned)nx_eglGetError_stub());
}

static void initialize_sparse_arena(void) {
  if (g_oc_want != 1) return;
  /* Prepare the shared sparse/dynamic extent arena and Unity slab atomically.
   * Physical commits use the alias region; zero-SystemResourceSize launchers
   * use reversible heap-donor code aliases in the ASLR region. */
  const int prepared = nx_alias_memory_arenas_prepare();
  u64 alias_address = 0, alias_size = 0, alias_extra = 0;
  u64 stack_address = 0, stack_size = 0;
  u64 aslr_address = 0, aslr_size = 0, system_resource = 0;
  (void)svcGetInfo(&alias_address, InfoType_AliasRegionAddress,
                   CUR_PROCESS_HANDLE, 0);
  (void)svcGetInfo(&alias_size, InfoType_AliasRegionSize,
                   CUR_PROCESS_HANDLE, 0);
  if (R_FAILED(svcGetInfo(&alias_extra, InfoType_AliasRegionExtraSize,
                           CUR_PROCESS_HANDLE, 0)))
    alias_extra = 0;
  (void)svcGetInfo(&stack_address, InfoType_StackRegionAddress,
                   CUR_PROCESS_HANDLE, 0);
  (void)svcGetInfo(&stack_size, InfoType_StackRegionSize,
                   CUR_PROCESS_HANDLE, 0);
  (void)svcGetInfo(&aslr_address, InfoType_AslrRegionAddress,
                   CUR_PROCESS_HANDLE, 0);
  (void)svcGetInfo(&aslr_size, InfoType_AslrRegionSize,
                   CUR_PROCESS_HANDLE, 0);
  (void)svcGetInfo(&system_resource, InfoType_SystemResourceSizeTotal,
                   CUR_PROCESS_HANDLE, 0);
  if (!prepared) {
    extern const char *g_oc_arena_failure_stage;
    const NxMemoryBackingBackend backend = nx_memory_backing_backend();
    const size_t dyn_target = nx_dynamic_arena_target_bytes();
    FILE *df = fopen(DATA_ROOT "/arena_debug.txt", "w");
    if (df) {
      fprintf(df, "failure_stage=%s\n", g_oc_arena_failure_stage);
      fprintf(df, "backend=%d system_resource=0x%llx\n",
              (int)backend, (unsigned long long)system_resource);
      fprintf(df, "aslr_addr=0x%llx aslr_size=0x%llx (%llu MiB)\n",
              (unsigned long long)aslr_address,
              (unsigned long long)aslr_size,
              (unsigned long long)(aslr_size / (1024 * 1024)));
      fprintf(df, "alias_addr=0x%llx alias_size=0x%llx (%llu MiB) extra=0x%llx\n",
              (unsigned long long)alias_address,
              (unsigned long long)alias_size,
              (unsigned long long)(alias_size / (1024 * 1024)),
              (unsigned long long)alias_extra);
      fprintf(df, "stack_addr=0x%llx stack_size=0x%llx (%llu MiB)\n",
              (unsigned long long)stack_address,
              (unsigned long long)stack_size,
              (unsigned long long)(stack_size / (1024 * 1024)));
      fprintf(df, "donor_capacity=0x%zx (%zu MiB) donor_active=0x%zx (%zu MiB)\n",
              g_heap_donor_capacity, g_heap_donor_capacity / (1024 * 1024),
              g_heap_donor_active_bytes, g_heap_donor_active_bytes / (1024 * 1024));
      fprintf(df, "oc_dynamic_arena_bytes=0x%zx (%zu MiB)\n",
              (size_t)OC_DYNAMIC_ARENA_BYTES,
              (size_t)OC_DYNAMIC_ARENA_BYTES / (1024 * 1024));
      fprintf(df, "unity_slab_bytes=0x%llx (%llu MiB)\n",
              (unsigned long long)GENSHIN_UNITY_SLAB_MAP_BYTES,
              (unsigned long long)(GENSHIN_UNITY_SLAB_MAP_BYTES / (1024 * 1024)));
      fprintf(df, "large_search=0x%llx (%llu MiB)\n",
              (unsigned long long)((u64)OC_DYNAMIC_ARENA_BYTES +
               (u64)GENSHIN_UNITY_SLAB_MAP_BYTES + MMAP_ARENA_ALIGN),
              (unsigned long long)(((u64)OC_DYNAMIC_ARENA_BYTES +
               (u64)GENSHIN_UNITY_SLAB_MAP_BYTES + MMAP_ARENA_ALIGN) /
               (1024 * 1024)));
      fprintf(df, "aslr_threshold=0x%llx (%llu GiB)\n",
              (unsigned long long)((u64)2 * 1024 * 1024 * 1024),
              (unsigned long long)2);
      fprintf(df, "dyn_target=0x%zx (%zu MiB)\n",
              dyn_target, dyn_target / (1024 * 1024));
      fprintf(df, "oc_want=%d\n", g_oc_want);
      fclose(df);
    }
    return;
  }
  g_oc_want = 2;
}

/* Prove the exact libnx caller-owned stack lifecycle before Unity constructors
 * can create workers.  svcMapMemory temporarily aliases this pool source into
 * the native stack region; pthread_join/threadClose must restore it to direct
 * RW heap before the broker is allowed to recycle the pages. */
static void *sparse_stack_self_test_thread(void *opaque) {
  volatile uint8_t stack_probe[16 * 1024];
  uint8_t checksum = 0;
  for (size_t i = 0; i < sizeof(stack_probe); ++i) {
    stack_probe[i] = (uint8_t)(i * 17u + 3u);
    checksum ^= stack_probe[i];
  }
  return checksum == 0 ? opaque : NULL;
}

static const char *g_sparse_guest_test_failure = "not run";

static int sparse_guest_spill_self_test_failed(
    const char *stage, void *allocation, size_t requested, size_t usable,
    const NxSparseArenaDiagnostics *before) {
  (void)allocation;
  (void)requested;
  (void)usable;
  (void)before;
  g_sparse_guest_test_failure = stage;
  return 0;
}

static int sparse_guest_spill_self_test(void) {
  if (g_oc_want != 2)
    return sparse_guest_spill_self_test_failed(
      "arena-selection", NULL, 0, 0, NULL);
  const size_t request =
    nx_memory_backing_backend() == NX_MEMORY_BACKEND_HEAP_ALIAS
      ? OC_HEAP_DONOR_INITIAL_BYTES + OC_DYNAMIC_SEGMENT_BYTES
      : (size_t)1024u * 1024u;
  NxSparseArenaDiagnostics before = {0}, during = {0}, after = {0};
  if (!nx_sparse_pool_guest_arena_prepare())
    return sparse_guest_spill_self_test_failed(
      "arena-prepare", NULL, 0, 0, NULL);
  nx_sparse_arena_get_diagnostics(&before);

  void *allocation = nx_sparse_pool_spill_alloc_aligned(request, 64u);
  if (!allocation || ((uintptr_t)allocation & 63u))
    return sparse_guest_spill_self_test_failed(
      "allocate", allocation, 0, 0, &before);
  size_t requested = 0, usable = 0;
  if (!nx_sparse_pool_spill_query(allocation, &requested, &usable) ||
      requested != request || usable < request ||
      !nx_sparse_pool_contains_address(allocation)) {
    (void)nx_sparse_pool_spill_release(allocation);
    return sparse_guest_spill_self_test_failed(
      "query", allocation, requested, usable, &before);
  }

  volatile uint8_t *bytes = (volatile uint8_t *)allocation;
  bytes[0] = 0x5a;
  bytes[request / 2u] = 0xa5;
  bytes[request - 1u] = 0x3c;
  if (bytes[0] != 0x5a || bytes[request / 2u] != 0xa5 ||
      bytes[request - 1u] != 0x3c) {
    (void)nx_sparse_pool_spill_release(allocation);
    return sparse_guest_spill_self_test_failed(
      "read-write", allocation, requested, usable, &before);
  }
  nx_sparse_arena_get_diagnostics(&during);
  if (!nx_sparse_pool_spill_release(allocation))
    return sparse_guest_spill_self_test_failed(
      "release", allocation, requested, usable, &before);
  nx_sparse_arena_get_diagnostics(&after);

  uint32_t mismatch = 0;
  const uint64_t expected_record_capacity =
    before.pool_capacity_bytes / 0x1000u +
    before.donor_capacity_bytes / OC_HEAP_DONOR_UNIT_BYTES;
  if (during.spill_bytes < before.spill_bytes + request) mismatch |= 1u << 0;
  if (during.dynamic_mapped_bytes <
      before.dynamic_mapped_bytes + OC_DYNAMIC_SEGMENT_BYTES) mismatch |= 1u << 1;
  if (during.committed_bytes != before.committed_bytes) mismatch |= 1u << 2;
  if (during.reserved_bytes != before.reserved_bytes) mismatch |= 1u << 3;
  if (during.pool_free_bytes >= before.pool_free_bytes) mismatch |= 1u << 4;
  if (before.ownership_record_capacity != expected_record_capacity)
    mismatch |= 1u << 5;
  if (during.ownership_records_in_use !=
      before.ownership_records_in_use + 1u) mismatch |= 1u << 6;
  if (after.ownership_records_in_use != before.ownership_records_in_use) mismatch |= 1u << 7;
  if (during.peak_ownership_records_in_use <
      during.ownership_records_in_use) mismatch |= 1u << 8;
  if (after.ownership_record_exhaustions !=
      before.ownership_record_exhaustions) mismatch |= 1u << 9;
  if (after.spill_bytes != before.spill_bytes) mismatch |= 1u << 10;
  if (after.committed_bytes != before.committed_bytes) mismatch |= 1u << 11;
  if (after.reserved_bytes != before.reserved_bytes) mismatch |= 1u << 12;
  if (after.dynamic_mapped_bytes != before.dynamic_mapped_bytes) mismatch |= 1u << 13;
  if (after.pool_free_bytes != before.pool_free_bytes) mismatch |= 1u << 14;
  if (after.quarantined_bytes != before.quarantined_bytes) mismatch |= 1u << 15;
  if (before.backing_backend == NX_MEMORY_BACKEND_HEAP_ALIAS) {
    if (during.donor_active_bytes <= before.donor_active_bytes)
      mismatch |= 1u << 16;
    if (during.donor_used_bytes < request)
      mismatch |= 1u << 17;
    if (after.donor_used_bytes != before.donor_used_bytes)
      mismatch |= 1u << 18;
    if (after.donor_active_bytes != before.donor_active_bytes)
      mismatch |= 1u << 19;
    if (after.donor_grow_calls <= before.donor_grow_calls ||
        after.donor_shrink_calls <= before.donor_shrink_calls)
      mismatch |= 1u << 20;
  }
  if (mismatch) {
    return sparse_guest_spill_self_test_failed(
      "accounting", NULL, requested, usable, &before);
  }
  g_sparse_guest_test_failure = "none";
  return 1;
}

static const char *g_sparse_mmap_test_failure = "not run";

static int sparse_alias_mmap_self_test_failed(
    const char *stage, void *reservation,
    const NxSparseArenaDiagnostics *before) {
  (void)reservation;
  (void)before;
  g_sparse_mmap_test_failure = stage;
  return 0;
}

/* Prove the second owner class in the shared alias extent map. The 64 MiB
 * reservation consumes no physical memory; first touch maps one 1 MiB coarse
 * run, MADV_DONTNEED returns it, and exact unmap restores the virtual extent. */
static int sparse_alias_mmap_self_test(void) {
  const size_t reservation_size = 64u * 1024u * 1024u;
  const size_t commit_size = OC_SPARSE_COMMIT_GRANULE_BYTES;
  NxSparseArenaDiagnostics before = {0}, during = {0}, after = {0};
  nx_sparse_arena_get_diagnostics(&before);
  void *reservation = mmap_fake(NULL, reservation_size, 0,
                                0x02 | 0x20, -1, 0);
  if (!reservation || reservation == (void *)-1)
    return sparse_alias_mmap_self_test_failed(
      "reserve", reservation, &before);

  MemoryInfo info = {0};
  u32 page_info = 0;
  if (R_FAILED(svcQueryMemory(&info, &page_info, (uintptr_t)reservation)) ||
      info.type != MemType_Unmapped || info.perm != 0) {
    (void)munmap_fake(reservation, reservation_size);
    return sparse_alias_mmap_self_test_failed(
      "virtual-only", reservation, &before);
  }
  /* Exercise the exact raw AArch64 ABI used by the scene-entry security
   * worker, not just the imported libc mprotect veneer. */
  if (syscall_fake(226L, reservation, (size_t)4096u, 0x01 | 0x02) != 0) {
    (void)munmap_fake(reservation, reservation_size);
    return sparse_alias_mmap_self_test_failed(
      "raw-mprotect-226", reservation, &before);
  }
  volatile uint8_t *bytes = (volatile uint8_t *)reservation;
  bytes[0] = 0x6d;
  bytes[4095] = 0xa7;
  if (bytes[0] != 0x6d || bytes[4095] != 0xa7 ||
      R_FAILED(svcQueryMemory(&info, &page_info, (uintptr_t)reservation)) ||
      info.type != nx_memory_backing_mapped_type() ||
      (info.perm & Perm_Rw) != Perm_Rw) {
    (void)munmap_fake(reservation, reservation_size);
    return sparse_alias_mmap_self_test_failed(
      "read-write", reservation, &before);
  }

  nx_sparse_arena_get_diagnostics(&during);
  if (during.reserved_bytes != before.reserved_bytes + reservation_size ||
      during.committed_bytes != before.committed_bytes + commit_size) {
    (void)munmap_fake(reservation, reservation_size);
    return sparse_alias_mmap_self_test_failed(
      "accounting-live", reservation, &before);
  }
  if (madvise_fake(reservation, commit_size, 4) != 0 ||
      R_FAILED(svcQueryMemory(&info, &page_info, (uintptr_t)reservation)) ||
      info.type != MemType_Unmapped || info.perm != 0) {
    (void)munmap_fake(reservation, reservation_size);
    return sparse_alias_mmap_self_test_failed(
      "discard", reservation, &before);
  }
  if (munmap_fake(reservation, reservation_size) != 0)
    return sparse_alias_mmap_self_test_failed(
      "release", reservation, &before);
  nx_sparse_arena_get_diagnostics(&after);
  if (after.reserved_bytes != before.reserved_bytes ||
      after.committed_bytes != before.committed_bytes ||
      after.pool_free_bytes != before.pool_free_bytes)
    return sparse_alias_mmap_self_test_failed(
      "accounting-restored", NULL, &before);
  g_sparse_mmap_test_failure = "none";
  return 1;
}

static int sparse_thread_stack_self_test(void) {
  if (g_oc_want != 2) return 0;
  const size_t backing_size = ((size_t)512 + 64) * 1024;
  NxSparseArenaDiagnostics before = {0};
  NxSparseArenaDiagnostics after = {0};
  nx_sparse_arena_get_diagnostics(&before);

  void *backing = nx_sparse_pool_thread_alloc(backing_size);
  if (!backing) return 0;
  memset(backing, 0, backing_size);

  pthread_attr_t attr;
  if (pthread_attr_init(&attr) != 0) {
    (void)nx_sparse_pool_thread_release(backing);
    return 0;
  }
  const int stack_result = pthread_attr_setstack(&attr, backing, backing_size);
  pthread_t thread = (pthread_t)0;
  int create_result = stack_result;
  if (create_result == 0)
    create_result = pthread_create(&thread, &attr,
                                   sparse_stack_self_test_thread, backing);
  (void)pthread_attr_destroy(&attr);
  if (create_result != 0) {
    (void)nx_sparse_pool_thread_release(backing);
    return 0;
  }

  void *returned = NULL;
  if (pthread_join(thread, &returned) != 0 || returned != backing)
    return 0;
  if (!nx_sparse_pool_thread_release(backing)) return 0;

  nx_sparse_arena_get_diagnostics(&after);
  return after.thread_pool_bytes == before.thread_pool_bytes &&
         after.peak_thread_pool_bytes >= before.thread_pool_bytes +
                                           backing_size;
}

static int load_game_module(void) {
  const int rc = so_load(&game_mod, LIB_GAME, heap_so_base, heap_so_limit);
  if (rc < 0) return rc;

  const size_t used = ALIGN_MEM(game_mod.load_size, 0x4000);
  heap_so_base = (uint8_t *)heap_so_base + used;
  heap_so_limit -= used;
  resolve_module_imports(&game_mod);

  return 0;
}

static int module_contains(const void *p, size_t bytes) {
  const uintptr_t begin = (uintptr_t)game_mod.load_virtbase;
  const uintptr_t end = begin + game_mod.load_size;
  const uintptr_t at = (uintptr_t)p;
  return at >= begin && at <= end && bytes <= end - at;
}

static int module_contains_string(const char *p) {
  const uintptr_t end = (uintptr_t)game_mod.load_virtbase + game_mod.load_size;
  if (!module_contains(p, 1)) return 0;
  size_t available = end - (uintptr_t)p;
  if (available > 256) available = 256;
  return memchr(p, 0, available) != NULL;
}

/* The exact Unity 2017 AndroidJavaClass helper resolves a name through the
 * managed java.lang.Class.forName generic path, which needs an ART class
 * loader.  A raw JNI FindClass is the equivalent lookup in this no-ART
 * wrapper, but AndroidJavaClass stores its result in m_jclass (+0x18) while
 * the two native consumers read AndroidJavaObject.m_jobject (+0x10), so the
 * consumer at RVA 0x14196EFC would otherwise see null.
 *
 * Swap '/'->'.' to '.'->'/', call AndroidJNISafe.FindClass, construct the
 * client's AndroidJavaClass(IntPtr), and change only the two fingerprinted
 * m_jobject loads to m_jclass loads.  Every original instruction is checked
 * before any RX write, so another client version cannot be modified. */
static void patch_unity_java_class_resolution(void) {
  uint32_t *const replace_chars = (uint32_t *)(
    (uintptr_t)game_mod.load_virtbase +
    GENSHIN_JAVA_CLASS_REPLACE_CHARS_RVA);
  uint32_t *const generic_call = (uint32_t *)(
    (uintptr_t)game_mod.load_virtbase + GENSHIN_JAVA_CLASS_GENERIC_CALL_RVA);
  uint32_t *const object_consumer = (uint32_t *)(
    (uintptr_t)game_mod.load_virtbase +
    GENSHIN_JAVA_CLASS_OBJECT_CONSUMER_RVA);
  uint32_t *const class_consumer = (uint32_t *)(
    (uintptr_t)game_mod.load_virtbase +
    GENSHIN_JAVA_CLASS_CLASS_CONSUMER_RVA);
  static const uint32_t expected_replace[] = {
    UINT32_C(0x528005e1), /* mov w1, #0x2f */
    UINT32_C(0x528005c2), /* mov w2, #0x2e */
  };
  static const uint32_t patched_replace[] = {
    UINT32_C(0x528005c1), /* mov w1, #0x2e */
    UINT32_C(0x528005e2), /* mov w2, #0x2f */
  };
  static const uint32_t expected_generic[] = {
    UINT32_C(0xb40001e0), UINT32_C(0xf9400288),
    UINT32_C(0xb940b108), UINT32_C(0x340003a8),
    UINT32_C(0x90017e69), UINT32_C(0xf941d529),
    UINT32_C(0xf9400129), UINT32_C(0x8b080128),
    UINT32_C(0xf9400001), UINT32_C(0xaa0003f5),
    UINT32_C(0xaa0803e0), UINT32_C(0x9767b7a9),
    UINT32_C(0x2a0003e8), UINT32_C(0xaa1503e0),
    UINT32_C(0x360003c8), UINT32_C(0xb9401a88),
    UINT32_C(0x34000288), UINT32_C(0xf9001280),
  };
  static const uint32_t patched_generic[] = {
    UINT32_C(0xb4000420), /* null string -> existing exception path */
    UINT32_C(0xaa1503e0), /* mov x0, x21 */
    UINT32_C(0x9767f134), /* bl AndroidJNISafe.FindClass veneer, RVA 0x0F823298 */
    UINT32_C(0xb40003c0), /* null jclass -> existing exception path */
    UINT32_C(0xaa0003f4), /* mov x20, x0 (owned local jclass) */
    UINT32_C(0xd0016608), /* adrp x8, AndroidJavaClass metadata page */
    UINT32_C(0xf943d508), /* ldr x8, [x8, #0x7a8] */
    UINT32_C(0xaa0803e0), /* mov x0, x8 */
    UINT32_C(0x9767b7b2), /* bl il2cpp_object_new, RVA 0x0F814CA8 */
    UINT32_C(0xb4000320), /* allocation failure -> existing exception path */
    UINT32_C(0xaa0003f3), /* mov x19, x0 */
    UINT32_C(0xaa1403e1), /* mov x1, x20 */
    UINT32_C(0x97580e92), /* bl AndroidJavaClass(IntPtr), RVA 0x0F42A838 */
    UINT32_C(0xaa1303e0), /* mov x0, x19 */
    UINT32_C(0xa9424ff4), /* ldp x20, x19, [sp, #32] */
    UINT32_C(0xa94157f6), /* ldp x22, x21, [sp, #16] */
    UINT32_C(0xf84307fe), /* ldr x30, [sp], #48 */
    UINT32_C(0xd65f03c0), /* ret */
  };
  static const uint32_t expected_consumer = UINT32_C(0xf9400a68); /* ldr x8, [x19, #0x10] */
  static const uint32_t patched_consumer = UINT32_C(0xf9400e68);  /* ldr x8, [x19, #0x18] */
  _Static_assert(sizeof(expected_replace) == sizeof(patched_replace),
                 "Java class spelling patch size changed");
  _Static_assert(sizeof(expected_generic) == sizeof(patched_generic),
                 "Java class resolver patch size changed");
  _Static_assert((GENSHIN_JAVA_CLASS_GENERIC_CALL_RVA + 8u * 4u) -
                   GENSHIN_IL2CPP_OBJECT_NEW_RVA == UINT64_C(0x2612138),
                 "il2cpp_object_new branch displacement changed");
  _Static_assert((GENSHIN_JAVA_CLASS_GENERIC_CALL_RVA + 12u * 4u) -
                   GENSHIN_ANDROIDJAVACLASS_CTOR_RVA == UINT64_C(0x29fc5b8),
                 "AndroidJavaClass constructor branch displacement changed");

  if (!module_contains(replace_chars, sizeof(expected_replace)) ||
      !module_contains(generic_call, sizeof(expected_generic)) ||
      !module_contains(object_consumer, sizeof(*object_consumer)) ||
      !module_contains(class_consumer, sizeof(*class_consumer)) ||
      memcmp(replace_chars, expected_replace, sizeof(expected_replace)) ||
      memcmp(generic_call, expected_generic, sizeof(expected_generic)) ||
      *object_consumer != expected_consumer ||
      *class_consumer != expected_consumer)
    fatal_error("Unity Java class resolver instructions do not match the exact supported client.");
  if (so_patch_code(replace_chars, patched_replace, sizeof(patched_replace)) ||
      so_patch_code(generic_call, patched_generic, sizeof(patched_generic)) ||
      so_patch_code(object_consumer, &patched_consumer, sizeof(patched_consumer)) ||
      so_patch_code(class_consumer, &patched_consumer, sizeof(patched_consumer)) ||
      memcmp(replace_chars, patched_replace, sizeof(patched_replace)) ||
      memcmp(generic_call, patched_generic, sizeof(patched_generic)) ||
      *object_consumer != patched_consumer ||
      *class_consumer != patched_consumer)
    fatal_error("Could not install the exact Unity raw-JNI class resolver patch.");
}

/* Consumed by unity_slab_dispatch.s after the exact four-instruction selector
 * has been fingerprinted.  Keeping the guest slot address here lets the bridge
 * reproduce the original load and NZCV result after its C call. */
uintptr_t genshin_unity_slab_aligned_slot;
uintptr_t genshin_unity_slab_activate_continue;
extern void genshin_unity_slab_activate_dispatch(void);

static void patch_unity_slab_activation(void) {
  uint32_t *const sequence = (uint32_t *)(
    (uintptr_t)game_mod.load_virtbase +
    GENSHIN_UNITY_SLAB_ACTIVATE_SEQUENCE_RVA);
  static const uint32_t expected[] = {
    UINT32_C(0xd0085968), /* adrp x8, aligned slab global */
    UINT32_C(0x92403ee9), /* and x9, x23, #0xffff */
    UINT32_C(0xf947c908), /* ldr x8, [x8, #0xf90] */
    UINT32_C(0xab095d1a), /* adds x26, x8, x9, lsl #23 */
  };
  struct {
    uint32_t ldr_x8_literal;
    uint32_t br_x8;
    uint64_t target;
  } replacement = {
    UINT32_C(0x58000048), /* ldr x8, .+8 */
    UINT32_C(0xd61f0100), /* br x8 */
    (uint64_t)(uintptr_t)&genshin_unity_slab_activate_dispatch,
  };
  _Static_assert(sizeof(replacement) == sizeof(expected),
                 "absolute Unity slab activation branch size changed");
  if (!module_contains(sequence, sizeof(expected)) ||
      memcmp(sequence, expected, sizeof(expected)))
    fatal_error("Unity slab chunk selector does not match the exact supported client.");

  genshin_unity_slab_aligned_slot =
    (uintptr_t)game_mod.load_virtbase + GENSHIN_UNITY_SLAB_ALIGNED_RVA;
  genshin_unity_slab_activate_continue =
    (uintptr_t)game_mod.load_virtbase +
    GENSHIN_UNITY_SLAB_ACTIVATE_CONTINUE_RVA;
  if (so_patch_code(sequence, &replacement, sizeof(replacement)) ||
      memcmp(sequence, &replacement, sizeof(replacement)))
    fatal_error("Could not install the Unity slab on-demand commit bridge.");
}

/* 1224 inlined il2cpp_string_new_len, so the NRO reimplements it via the
 * game's own GC helpers (see genshin_il2cpp_string_new_len). */
typedef void *(*GenshinIl2CppTypeResolverFn)(void *type_ptr);
typedef void *(*GenshinIl2CppSizedAllocFn)(void *klass, uint32_t size,
                                           void *alloc_vtable);
static int readable_object_span(const void *object, size_t bytes);
static void *genshin_il2cpp_string_new_len(const char *ascii, int32_t length);

/* Runtime continuation consumed by the caller-scoped naked dispatcher below.
 * It is published only after the exact source image and the replaced
 * four-instruction sequence both pass their fingerprints. */
uintptr_t genshin_mmoron_directory_continue;

static int managed_path_to_ascii(const void *object, char *output,
                                 size_t output_size) {
  if (!output || output_size < 2u ||
      !readable_object_span(object, 20u))
    return 0;
  int32_t length = 0;
  memcpy(&length, (const uint8_t *)object + 16u, sizeof(length));
  if (length < 0 || (size_t)length >= output_size ||
      !readable_object_span(object, 20u + 2u * (size_t)length))
    return 0;
  for (int32_t i = 0; i < length; ++i) {
    uint16_t character = 0;
    memcpy(&character, (const uint8_t *)object + 20u + 2u * (size_t)i,
           sizeof(character));
    if (character < 0x20u || character > 0x7eu) return 0;
    output[i] = (char)character;
  }
  output[length] = '\0';
  return 1;
}

/* Reproduce the exact managed sequence replaced at RVA 0xC684DD4.  Only its
 * application-path input is normalized; the original Unity getter and both
 * original System.IO.Path implementations still execute.  Keeping this at the
 * Mmoron call site avoids changing exception/unwind behavior for any unrelated
 * Path.GetDirectoryName caller. */
__attribute__((noinline, used, visibility("hidden")))
void *genshin_mmoron_directory_sequence_bridge(void *parameters) {
  const uintptr_t module_base = (uintptr_t)game_mod.load_virtbase;
  typedef void *(*UnityApplicationPathGetterFn)(void);
  typedef void *(*ManagedPathUnaryFn)(void *);
  typedef void *(*ManagedPathBinaryFn)(void *, void *);
  UnityApplicationPathGetterFn application_path_getter =
    (UnityApplicationPathGetterFn)(module_base +
      GENSHIN_UNITY_APPLICATION_PATH_GETTER_RVA);
  ManagedPathUnaryFn get_directory_name =
    (ManagedPathUnaryFn)(module_base +
      GENSHIN_PATH_GET_DIRECTORY_NAME_RVA);
  ManagedPathBinaryFn combine =
    (ManagedPathBinaryFn)(module_base + GENSHIN_PATH_COMBINE_RVA);

  void *path = application_path_getter();

  char original[768];
  const int readable = managed_path_to_ascii(path, original, sizeof(original));
  const char *normalized = readable ? original : NULL;
  if (normalized && !strncmp(normalized, "jar:file://sdmc:/", 16u))
    normalized += 16u;
  else if (normalized && !strncmp(normalized, "file://sdmc:/", 12u))
    normalized += 12u;
  else if (normalized && !strncmp(normalized, "sdmc:/", 6u))
    normalized += 5u;

  /* Path.GetDirectoryName needs more than a root and this exact old Mono
   * implementation rejects colons.  A virtual base.apk path matches Android's
   * Application.dataPath shape; the file is not opened by this operation. */
  if (!normalized || normalized[0] != '/' || normalized[1] == '\0' ||
      strchr(normalized, ':'))
    normalized = managed_path(GAME_HOME "/base.apk");

  void *selected = path;
  if (!readable || strcmp(normalized, original)) {
    selected = genshin_il2cpp_string_new_len(normalized,
                                             (int32_t)strlen(normalized));
    if (!selected) selected = path;
  }

  void *directory = get_directory_name(selected);
  void *relative = NULL;
  if (readable_object_span(parameters, 0xa0u))
    memcpy(&relative, (const uint8_t *)parameters + 0x98u,
           sizeof(relative));
  return combine(directory, relative);
}

/* Implemented in mmoron_path_dispatch.s.  A true assembly entry point is
 * required here: GCC's AArch64 backend warns that its C `naked` attribute is
 * ignored, even when the current optimizer happens to omit a prologue. */
extern void genshin_mmoron_directory_sequence_dispatch(void);

static void patch_mmoron_managed_directory_path(void) {
  uint32_t *const sequence = (uint32_t *)(
    (uintptr_t)game_mod.load_virtbase +
    GENSHIN_MMORON_DIRECTORY_SEQUENCE_RVA);
  static const uint32_t expected[] = {
    UINT32_C(0x94b69a7c), /* bl Unity application-path getter thunk */
    UINT32_C(0x9777f80f), /* bl Path.GetDirectoryName */
    UINT32_C(0xf9404e81), /* ldr x1, [x20, #0x98] */
    UINT32_C(0x9777f659), /* bl Path.Combine */
  };
  struct {
    uint32_t ldr_x16_literal;
    uint32_t br_x16;
    uint64_t target;
  } replacement = {
    UINT32_C(0x58000050), /* ldr x16, .+8 */
    UINT32_C(0xd61f0200), /* br x16 */
    (uint64_t)(uintptr_t)&genshin_mmoron_directory_sequence_dispatch,
  };
  _Static_assert(sizeof(replacement) == sizeof(expected),
                 "absolute managed-path branch size changed");
  if (!module_contains(sequence, sizeof(expected)) ||
      memcmp(sequence, expected, sizeof(expected)))
    fatal_error("Mmoron managed application-path sequence does not match the exact supported client.");

  genshin_mmoron_directory_continue =
    (uintptr_t)game_mod.load_virtbase +
    GENSHIN_MMORON_DIRECTORY_CONTINUE_RVA;
  if (so_patch_code(sequence, &replacement, sizeof(replacement)) ||
      memcmp(sequence, &replacement, sizeof(replacement)))
    fatal_error("Could not install the exact Mmoron managed-path normalizer.");
}

static int readable_object_span(const void *object, size_t bytes) {
  const uintptr_t address = (uintptr_t)object;
  MemoryInfo info;
  u32 page_info = 0;
  if (!object || address > UINTPTR_MAX - bytes ||
      R_FAILED(svcQueryMemory(&info, &page_info, (u64)address)) ||
      info.type == MemType_Unmapped || !(info.perm & Perm_R) ||
      address < info.addr || bytes > info.size ||
      address - info.addr > info.size - bytes)
    return 0;
  return 1;
}

/* Reimplemented il2cpp_string_new_len for the 1224 client.  The original
 * helper was inlined by the compiler (no callable (char*,len)->Il2CppString*
 * exists in the RX segment), so the legacy GENSHIN_IL2CPP_STRING_NEW_LEN_RVA
 * cannot be used.  This replicates the game's own Il2CppString construction at
 * RVA 0x79b9814 via its native GC helpers:
 *   1. load the Il2CppString alloc-vtable from the GC slab (page + 0x700);
 *   2. resolve the type holder (0x782A890) and read [holder] for the class;
 *   3. sized-allocate (0x4128E94) with size = len*2 + 0x14 + 0x2;
 *   4. store the int32 length @ +0x10 and zero-extend ASCII to UTF-16 @ +0x14.
 * Returns a GC-managed Il2CppString* or NULL when the GC slab/type is not yet
 * initialized (callers treat NULL as a repair failure). */
static void *genshin_il2cpp_string_new_len(const char *ascii, int32_t length) {
  if (!ascii || length < 0 || length > 0x7fff)
    return NULL;
  const uintptr_t base = (uintptr_t)game_mod.load_virtbase;
  void **const gc_vtable_slot = (void **)(base +
    GENSHIN_IL2CPP_GC_PAGE_RVA + GENSHIN_IL2CPP_GC_ALLOC_VTABLE_OFFSET);
  void *const *const type_holder =
    (void *const *)(base + GENSHIN_IL2CPP_STRING_TYPE_PTR_RVA);
  if (!module_contains(gc_vtable_slot, sizeof(*gc_vtable_slot)) ||
      !module_contains(type_holder, sizeof(*type_holder)) ||
      !module_contains((const void *)(base + GENSHIN_IL2CPP_TYPE_RESOLVER_RVA), 4) ||
      !module_contains((const void *)(base + GENSHIN_IL2CPP_SIZED_ALLOC_RVA), 4))
    return NULL;
  void *const alloc_vtable = __atomic_load_n(gc_vtable_slot, __ATOMIC_ACQUIRE);
  if (!alloc_vtable)
    return NULL;
  GenshinIl2CppTypeResolverFn resolve_type =
    (GenshinIl2CppTypeResolverFn)(base + GENSHIN_IL2CPP_TYPE_RESOLVER_RVA);
  void *holder = resolve_type((void *)type_holder);
  if (!holder)
    return NULL;
  void *klass = __atomic_load_n((void *volatile *)holder, __ATOMIC_ACQUIRE);
  if (!klass)
    return NULL;
  const uint32_t size = (uint32_t)length * 2u +
                        (uint32_t)GENSHIN_IL2CPP_STRING_HEADER_BYTES +
                        (uint32_t)GENSHIN_IL2CPP_STRING_TERM_BYTES;
  GenshinIl2CppSizedAllocFn sized_alloc =
    (GenshinIl2CppSizedAllocFn)(base + GENSHIN_IL2CPP_SIZED_ALLOC_RVA);
  void *object = sized_alloc(klass, size, alloc_vtable);
  if (!object)
    return NULL;
  /* int32 length @ +0x10 */
  memcpy((uint8_t *)object + 0x10, &length, sizeof(length));
  /* ASCII -> zero-extended UTF-16 @ +0x14, NUL-terminated */
  uint8_t *const chars = (uint8_t *)object + 0x14;
  for (int32_t i = 0; i < length; ++i) {
    chars[2 * i] = (uint8_t)ascii[i];
    chars[2 * i + 1] = 0;
  }
  chars[2 * length] = 0;
  chars[2 * length + 1] = 0;
  return object;
}

static int writable_object_span(const void *object, size_t bytes) {
  const uintptr_t address = (uintptr_t)object;
  MemoryInfo info;
  u32 page_info = 0;
  if (!object || address > UINTPTR_MAX - bytes ||
      R_FAILED(svcQueryMemory(&info, &page_info, (u64)address)) ||
      info.type == MemType_Unmapped || !(info.perm & Perm_W) ||
      address < info.addr || bytes > info.size ||
      address - info.addr > info.size - bytes)
    return 0;
  return 1;
}

typedef void (*GenshinTransferRefillFn)(void *, void *, uint32_t);

/* Replacement for the exact leaf callback at RVA 0x5130DAC.  For ordinary
 * mapped destinations this is instruction-for-instruction equivalent at the
 * data-structure level: select the transfer cursor, copy four bytes, and
 * advance it (or enter the client's refill path).  Hardware instead reached
 * this callback with base=0x41 and field_offset=0x10.  That value is a compact
 * serialized identifier, never a mapped native object.  Preserve evidence,
 * consume the input field, and turn the caller's owner slot into null so the
 * unresolved reference follows Unity's missing-object path rather than
 * corrupting address 0x51. */
static void genshin_transfer_int32_guard(const void *descriptor_, void *context_) {
  const uint8_t *descriptor = descriptor_;
  uint8_t *context = context_;
  int32_t field_offset = 0;
  int32_t relative_adjustment = 0;
  uintptr_t destination_base = 0;
  uintptr_t stream = 0;
  memcpy(&field_offset, descriptor + 36u, sizeof(field_offset));
  memcpy(&destination_base, context + 8u, sizeof(destination_base));
  memcpy(&relative_adjustment, context + 24u,
         sizeof(relative_adjustment));
  memcpy(&stream, context + 40u, sizeof(stream));

  const int64_t destination_delta = context[0]
    ? (int64_t)field_offset
    : (int64_t)field_offset + (int64_t)relative_adjustment - INT64_C(16);
  uintptr_t destination = destination_base;
  if (destination_delta >= 0)
    destination += (uintptr_t)destination_delta;
  else
    destination -= (uintptr_t)(-destination_delta);

  uint8_t *cursor = NULL;
  uint8_t *end = NULL;
  uint8_t *const cursor_state = (uint8_t *)(stream + 72u);
  memcpy(&cursor, cursor_state, sizeof(cursor));
  memcpy(&end, (const uint8_t *)stream + 88u, sizeof(end));

  const uintptr_t caller = (uintptr_t)__builtin_return_address(0);
  const uintptr_t expected_caller =
    (uintptr_t)game_mod.load_virtbase + GENSHIN_TRANSFER_INT32_RETURN_RVA;
  const int low_destination = destination_base < UINT64_C(0x10000) ||
                              destination < UINT64_C(0x10000);
  if (low_destination) {
    void **owner_slot = NULL;
    void *owner_value = NULL;
    /* The exact caller builds this context at outer_sp+0x70 and keeps the
     * address of its owner slot at outer_sp+0x08. */
    if (caller == expected_caller &&
        readable_object_span(context - 104u, sizeof(owner_slot))) {
      memcpy(&owner_slot, context - 104u, sizeof(owner_slot));
      if (readable_object_span(owner_slot, sizeof(owner_value)))
        memcpy(&owner_value, owner_slot, sizeof(owner_value));
    }

    if (owner_slot && owner_value == (void *)destination_base &&
        writable_object_span(owner_slot, sizeof(void *))) {
      void *null_owner = NULL;
      memcpy(owner_slot, &null_owner, sizeof(null_owner));
    }
    destination_base = 0;
    memcpy(context + 8u, &destination_base, sizeof(destination_base));

    uint32_t discarded = 0;
    if (cursor && end && (uintptr_t)cursor <= (uintptr_t)end &&
        (size_t)((uintptr_t)end - (uintptr_t)cursor) >= sizeof(discarded)) {
      memcpy(&discarded, cursor, sizeof(discarded));
      cursor += sizeof(discarded);
      memcpy(cursor_state, &cursor, sizeof(cursor));
    } else {
      GenshinTransferRefillFn refill = (GenshinTransferRefillFn)(
        (uintptr_t)game_mod.load_virtbase + GENSHIN_TRANSFER_REFILL_RVA);
      refill(cursor_state, &discarded, sizeof(discarded));
    }
    return;
  }

  void *const output = (void *)destination;
  if (cursor && end && (uintptr_t)cursor <= (uintptr_t)end &&
      (size_t)((uintptr_t)end - (uintptr_t)cursor) >= sizeof(uint32_t)) {
    uint32_t value = 0;
    memcpy(&value, cursor, sizeof(value));
    memcpy(output, &value, sizeof(value));
    cursor += sizeof(value);
    memcpy(cursor_state, &cursor, sizeof(cursor));
  } else {
    GenshinTransferRefillFn refill = (GenshinTransferRefillFn)(
      (uintptr_t)game_mod.load_virtbase + GENSHIN_TRANSFER_REFILL_RVA);
    refill(cursor_state, output, sizeof(uint32_t));
  }
}

static void patch_genshin_transfer_int32_guard(void) {
  uint32_t *const callback = (uint32_t *)(
    (uintptr_t)game_mod.load_virtbase + GENSHIN_TRANSFER_INT32_RVA);
  static const uint32_t expected[] = {
    UINT32_C(0xaa0003e8), /* mov x8, x0 */
    UINT32_C(0xf9401420), /* ldr x0, [x1, #40] */
    UINT32_C(0xf9400429), /* ldr x9, [x1, #8] */
    UINT32_C(0xb9802508), /* ldrsw x8, [x8, #36] */
  };
  struct {
    uint32_t ldr_x16_literal;
    uint32_t br_x16;
    uint64_t target;
  } replacement = {
    UINT32_C(0x58000050), /* ldr x16, .+8 */
    UINT32_C(0xd61f0200), /* br x16 */
    (uint64_t)(uintptr_t)&genshin_transfer_int32_guard,
  };
  _Static_assert(sizeof(replacement) == sizeof(expected),
                 "absolute transfer guard branch size changed");
  if (!module_contains(callback, sizeof(expected)) ||
      memcmp(callback, expected, sizeof(expected)))
    fatal_error("SerializedFile int32 transfer callback does not match the exact supported client.");
  if (so_patch_code(callback, &replacement, sizeof(replacement)) ||
      memcmp(callback, &replacement, sizeof(replacement)))
    fatal_error("Could not install the exact SerializedFile int32 transfer guard.");
}

static int managed_string_equals(const void *object, const char *ascii,
                                 size_t ascii_length) {
  if (!ascii || ascii_length > (SIZE_MAX - 20u) / 2u) return 0;
  if (!readable_object_span(object, 20u + 2u * ascii_length)) return 0;
  int32_t managed_length = 0;
  memcpy(&managed_length, (const uint8_t *)object + 16u,
         sizeof(managed_length));
  if (managed_length < 0 || (size_t)managed_length != ascii_length) return 0;
  for (size_t i = 0; i < ascii_length; ++i) {
    uint16_t managed_character = 0;
    memcpy(&managed_character, (const uint8_t *)object + 20u + 2u * i,
           sizeof(managed_character));
    if (managed_character != (uint8_t)ascii[i]) return 0;
  }
  return 1;
}

/* The class-name slot is already non-null after the first render, so pointer
 * presence alone is not a useful invariant.  The exact Unity 2017 helper at
 * RVA 0x141BED68 loads the forName method-name literal from RVA 0x15CBFF08
 * before entering JNI.  Verify both IL2CPP String objects and repair only a
 * null or non-matching value. */
static void repair_combo_managed_class_name(void) {
  const uintptr_t module_base = (uintptr_t)game_mod.load_virtbase;
  /* The Combo class-name slot is an IL2CPP metadata-field pointer reached
   * through a runtime double indirection, so it has no statically derivable
   * RVA for an arbitrary recompiled image.  The sentinel UINT64_MAX marks it
   * unresolved; the Combo-name repair below is then skipped (IL2CPP's own
   * MiHoYoSDK.Awake has already populated the slot before this runs). */
  const int combo_slot_known =
    GENSHIN_COMBO_CLASS_NAME_SLOT_RVA != UINT64_C(0xFFFFFFFFFFFFFFFF);
  void **const class_name_slot = combo_slot_known
    ? (void **)(module_base + GENSHIN_COMBO_CLASS_NAME_SLOT_RVA)
    : NULL;
  void **const for_name_slot =
    (void **)(module_base + GENSHIN_JAVA_FOR_NAME_SLOT_RVA);
  void *const *const empty_args_slot =
    (void *const *)(module_base + GENSHIN_EMPTY_OBJECT_ARGS_SLOT_RVA);
  /* 1224 inlined il2cpp_string_new_len, so the legacy
   * GENSHIN_IL2CPP_STRING_NEW_LEN_RVA is not a callable helper.  The repair
   * fallback below uses genshin_il2cpp_string_new_len (which drives the game's
   * own GC helpers); verify those RVAs land in the exact client image. */
  const uintptr_t type_resolver_address =
    module_base + GENSHIN_IL2CPP_TYPE_RESOLVER_RVA;
  const uintptr_t sized_alloc_address =
    module_base + GENSHIN_IL2CPP_SIZED_ALLOC_RVA;
  /* The forName slot, the empty-args slot, and the GC helpers are all
   * statically verified; a miss here is a real version mismatch. */
  if (!module_contains(for_name_slot, sizeof(*for_name_slot)) ||
      !module_contains(empty_args_slot, sizeof(*empty_args_slot)) ||
      !module_contains((const void *)type_resolver_address, 4) ||
      (type_resolver_address & 3u) ||
      !module_contains((const void *)sized_alloc_address, 4) ||
      (sized_alloc_address & 3u))
    fatal_error("MiHoYoSDK managed bootstrap RVAs are outside the exact client image.");
  if (combo_slot_known &&
      !module_contains(class_name_slot, sizeof(*class_name_slot)))
    fatal_error("MiHoYoSDK Combo class-name slot is outside the exact client image.");

  static const char class_name[] = GENSHIN_COMBO_CLASS_NAME;
  _Static_assert(sizeof(class_name) - 1u == 33u,
                 "exact Combo bridge class-name length changed");
  void *current = NULL;
  if (combo_slot_known) {
    current = __atomic_load_n(class_name_slot, __ATOMIC_ACQUIRE);
    if (managed_string_equals(current, class_name, sizeof(class_name) - 1u)) {
    } else {
      void *const created =
        genshin_il2cpp_string_new_len(class_name,
                                      (int32_t)(sizeof(class_name) - 1u));
      if (!created ||
          !managed_string_equals(created, class_name, sizeof(class_name) - 1u))
        fatal_error("IL2CPP returned an invalid MiHoYoSDK class-name string.");

      void *expected = current;
      (void)__atomic_compare_exchange_n(class_name_slot, &expected, created, 0,
                                        __ATOMIC_RELEASE, __ATOMIC_ACQUIRE);
    }
  }

  static const char for_name[] = GENSHIN_JAVA_FOR_NAME;
  _Static_assert(sizeof(for_name) - 1u == 7u,
                 "exact JavaLangClass method-name length changed");
  current = __atomic_load_n(for_name_slot, __ATOMIC_ACQUIRE);
  if (managed_string_equals(current, for_name, sizeof(for_name) - 1u)) {
  } else {
    void *const created =
      genshin_il2cpp_string_new_len(for_name,
                                    (int32_t)(sizeof(for_name) - 1u));
    if (!created ||
        !managed_string_equals(created, for_name, sizeof(for_name) - 1u))
      fatal_error("IL2CPP returned an invalid JavaLangClass forName string.");

    void *expected = current;
    (void)__atomic_compare_exchange_n(for_name_slot, &expected, created, 0,
                                      __ATOMIC_RELEASE, __ATOMIC_ACQUIRE);
  }

  void *const empty_args = __atomic_load_n(empty_args_slot, __ATOMIC_ACQUIRE);
  if (!empty_args) {
    return;
  }
  if (!readable_object_span(empty_args, 32u)) {
    return;
  }
  uint64_t argument_count = UINT64_MAX;
  memcpy(&argument_count, (const uint8_t *)empty_args + 24u,
         sizeof(argument_count));
}

static void validate_unity_slab_client_state(const void *reservation,
                                             size_t reservation_size) {
  const uintptr_t module_base = (uintptr_t)game_mod.load_virtbase;
  const uint64_t *raw_slot = (const uint64_t *)(module_base +
                                GENSHIN_UNITY_SLAB_RAW_RVA);
  const uint64_t *length_slot = (const uint64_t *)(module_base +
                                   GENSHIN_UNITY_SLAB_LENGTH_RVA);
  const uint64_t *aligned_slot = (const uint64_t *)(module_base +
                                     GENSHIN_UNITY_SLAB_ALIGNED_RVA);
  const uintptr_t *mmap_got_slot = (const uintptr_t *)(module_base +
                                    GENSHIN_UNITY_MMAP_GOT_RVA);
  if (!module_contains(raw_slot, sizeof(*raw_slot)) ||
      !module_contains(length_slot, sizeof(*length_slot)) ||
      !module_contains(aligned_slot, sizeof(*aligned_slot)) ||
      !module_contains(mmap_got_slot, sizeof(*mmap_got_slot)))
    fatal_error("Unity slab globals are outside the exact client image.");

  uint64_t raw, length, aligned;
  uintptr_t mmap_target;
  memcpy(&raw, raw_slot, sizeof(raw));
  memcpy(&length, length_slot, sizeof(length));
  memcpy(&aligned, aligned_slot, sizeof(aligned));
  memcpy(&mmap_target, mmap_got_slot, sizeof(mmap_target));
  if (raw > UINT64_MAX - (GENSHIN_UNITY_SLAB_ALIGNMENT - 1u))
    fatal_error("Unity slab allocator published an overflowing raw base.");
  const uint64_t expected_aligned =
    (raw + GENSHIN_UNITY_SLAB_ALIGNMENT - 1u) &
    ~(GENSHIN_UNITY_SLAB_ALIGNMENT - 1u);
  if (raw != (uint64_t)(uintptr_t)reservation ||
      length != GENSHIN_UNITY_SLAB_MAP_BYTES ||
      reservation_size != (size_t)GENSHIN_UNITY_SLAB_MAP_BYTES ||
      aligned != expected_aligned || aligned < raw ||
      aligned - raw > length ||
      GENSHIN_UNITY_SLAB_USABLE_BYTES > length - (aligned - raw) ||
      !mmap_validate_unity_slab_reservation(reservation, reservation_size))
    fatal_error("Unity slab allocator did not claim the exact reserved mapping.");
}

/* The stop-the-world bridge in libc_shim.c intentionally uses private RVAs
 * from one exact libyuanshen.so.  Staging already pins the complete SHA-256;
 * this in-memory gate makes an accidentally replaced library fail closed
 * before constructors can create collector threads and reach those offsets.
 * The fingerprints are immutable AArch64 instructions in the executable LOAD
 * segment, so Android relocations do not rewrite them. */
#define GENSHIN_EXACT_LOAD_SIZE ((size_t)UINT64_C(0x15240000))
#define GENSHIN_EXACT_SHA256 \
  "26c862b147d2822a39e5464e761611767abaec1a541698ac53f80c135a9a42d1"

typedef struct {
  uintptr_t rva;
  uint8_t bytes[16];
} GameFingerprint;

static int supported_game_image(void) {
  static const GameFingerprint fingerprints[] = {
    /* FP1: GC-signal gate.  1206 adrp x8,#0x15d9b000; ldr w8,[x8,#0xa78]
     * -> 1224 adrp x8,#0x14df3000; ldr w8,[x8,#0x230] (page + offset both
     * relocated; the str x30/stp x20,x19 prologue is unchanged). */
    { UINT64_C(0x044ca4e0),
      { 0xfe, 0x0f, 0x1e, 0xf8, 0xf4, 0x4f, 0x01, 0xa9,
        0x48, 0x49, 0x08, 0xb0, 0x08, 0x31, 0x42, 0xb9 } },
    /* FP2/FP3: retry loops that BL the same PLT thunk (1206 0x72dcad4 ->
     * 1224 0x7856f78; 6 callers in both).  The cbz w0; str wzr; b loop was
     * reorganized to cbnz w0; add; add, and the GC metadata field moved from
     * ldr w1,[xN,#0xa78] to ldr w1,[xN,#0x230]. */
    { UINT64_C(0x044c00d4),
      { 0xa9, 0x5b, 0xce, 0x94, 0xc0, 0x00, 0x00, 0x35,
        0xd6, 0x06, 0x00, 0x91, 0x18, 0x23, 0x00, 0x91 } },
    { UINT64_C(0x044c53b4),
      { 0xf1, 0x46, 0xce, 0x94, 0xc0, 0x00, 0x00, 0x35,
        0xb5, 0x06, 0x00, 0x91, 0xf7, 0x22, 0x00, 0x91 } },
    /* FP4: il2cpp_string_new_len prologue.  Resolved by caller trace: 2064
     * callers in 1206, 821 in 1224, with 64% post-BL instruction overlap.
     * The 16-byte prologue is byte-identical to 1206. */
    { UINT64_C(0x0413f2cc),
      { 0xff, 0x43, 0x01, 0xd1, 0xfe, 0x13, 0x00, 0xf9,
        0xf6, 0x57, 0x03, 0xa9, 0xf4, 0x4f, 0x04, 0xa9 } },
    /* FP5: mov x20,x0; ldr x1,[x8,#0x5d0]; bl; adrp.  The first two
     * instructions are byte-identical; unique 4-instruction window in 1224. */
    { UINT64_C(0x0ea5dc20),
      { 0xf4, 0x03, 0x00, 0xaa, 0x01, 0xe9, 0x42, 0xf9,
        0x6c, 0x15, 0x5e, 0x96, 0x88, 0x03, 0x03, 0xd0 } },
  };

  if (game_mod.load_size != GENSHIN_EXACT_LOAD_SIZE) return 0;
  for (size_t i = 0; i < sizeof(fingerprints) / sizeof(fingerprints[0]); ++i) {
    const uintptr_t rva = fingerprints[i].rva;
    /* so_load() has populated load_base here, but load_virtbase is only a
     * reservation until so_finalize() maps the executable alias.  Reading the
     * latter before svcMapProcessCodeMemory data-aborts on hardware. */
    if (rva > game_mod.load_size ||
        sizeof(fingerprints[i].bytes) > game_mod.load_size - rva)
      return 0;
    const uint8_t *at = (const uint8_t *)game_mod.load_base + rva;
    if (memcmp(at, fingerprints[i].bytes,
               sizeof(fingerprints[i].bytes)) != 0)
      return 0;
  }
  return 1;
}

static void validate_native_table(const GenshinJniNativeMethod *table,
                                  size_t count, const char *label) {
  if (!module_contains(table, count * sizeof(*table)))
    fatal_error("%s JNI table is outside libyuanshen.so.", label);
  for (size_t i = 0; i < count; ++i) {
    if (!module_contains_string(table[i].name) ||
        !module_contains_string(table[i].signature) ||
        !module_contains(table[i].function, 4) ||
        ((uintptr_t)table[i].function & 3))
      fatal_error("Invalid %s JNI registration entry %u.", label, (unsigned)i);
  }
}

static void *find_native(const GenshinJniNativeMethod *table, size_t count,
                         const char *name, const char *signature) {
  for (size_t i = 0; i < count; ++i)
    if (!strcmp(table[i].name, name) &&
        !strcmp(table[i].signature, signature))
      return table[i].function;
  fatal_error("Required Unity JNI method is missing:\n%s %s", name, signature);
}

typedef int (*JniOnLoadFn)(void *vm, void *reserved);
typedef uint8_t (*NativeLoaderLoadFn)(void *env, void *clazz, void *path);
typedef uint8_t (*NativePauseFn)(void *env, void *thiz);

/* Every function below is normally entered by ART as a Java-to-native call.
 * ART installs a local-reference frame for that boundary and discards it when
 * the native method returns.  We invoke the registered functions directly, so
 * reproduce that lifetime here; nativeRender in particular runs once per frame
 * and would otherwise retain every temporary Java object until thread exit. */
static void jni_boundary_begin(const char *name) {
  if (jni_push_local_frame(256) != 0)
    fatal_error("Could not create JNI local frame for %s.", name);
}

static void jni_boundary_end(void) {
  (void)jni_pop_local_frame(NULL);
}

static inline uintptr_t read_thread_pointer(void) {
  uintptr_t value;
  __asm__ volatile("mrs %0, s3_3_c13_c0_2" : "=r"(value));
  return value;
}

static inline void write_thread_pointer(uintptr_t value) {
  __asm__ volatile("msr s3_3_c13_c0_2, %0" : : "r"(value));
}

static void shutdown_host(void) {
  android_native_vibration_shutdown();
  opensles_shutdown();
  combo_auth_shutdown();
  SDL_Quit();
  if (socket_started) socketExit();
  if (nifm_started) nifmExit();
}

/* Android's connected flag is advisory; the resolver/socket calls carry the
 * actual errors.  Do not query nifm:u synchronously after initializing BSD:
 * that status IPC can outlast the client's update timeout while the network
 * service is busy. */
static void initialize_network_state(void) {
  g_net_on = socket_started != 0;
}

static volatile sig_atomic_t g_crash_signal = -1;
static volatile sig_atomic_t g_render_frame = -1;
static volatile sig_atomic_t g_render_in_progress;

static size_t crash_report_append_text(char *report, size_t capacity,
                                       size_t length, const char *text) {
  while (*text && length < capacity) report[length++] = *text++;
  return length;
}

static size_t crash_report_append_int(char *report, size_t capacity,
                                      size_t length, int value) {
  char digits[16];
  size_t count = 0;
  unsigned magnitude = (unsigned)value;
  if (value < 0) {
    if (length < capacity) report[length++] = '-';
    magnitude = 0u - magnitude;
  }
  do {
    digits[count++] = (char)('0' + magnitude % 10u);
    magnitude /= 10u;
  } while (magnitude && count < sizeof(digits));
  while (count && length < capacity) report[length++] = digits[--count];
  return length;
}

static void crash_signal_handler(int sig) {
  g_crash_signal = sig;
  const int fd = open(DATA_ROOT "/crash_signal.txt",
                      O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (fd >= 0) {
    char report[96];
    size_t length = crash_report_append_text(report, sizeof(report), 0,
                                             "signal=");
    length = crash_report_append_int(report, sizeof(report), length, sig);
    length = crash_report_append_text(report, sizeof(report), length,
                                      " frame=");
    length = crash_report_append_int(report, sizeof(report), length,
                                     g_render_frame);
    length = crash_report_append_text(report, sizeof(report), length,
                                      " in_render=");
    length = crash_report_append_int(report, sizeof(report), length,
                                     g_render_in_progress);
    if (length < sizeof(report)) report[length++] = '\n';
    (void)write(fd, report, length);
    close(fd);
  }
  _exit(sig);
}

static void log_crash_exit(const char *reason, void *caller_ra) {
  FILE *f = fopen(DATA_ROOT "/crash_exit.txt", "w");
  if (f) {
    u64 used = 0, total = 0;
    svcGetInfo(&used, InfoType_UsedMemorySize, CUR_PROCESS_HANDLE, 0);
    svcGetInfo(&total, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0);
    fprintf(f, "%s frame=%d in_render=%d used=%lluM total=%lluM\n", reason,
            g_render_frame, g_render_in_progress,
            (unsigned long long)(used / (1024 * 1024)),
            (unsigned long long)(total / (1024 * 1024)));
    fprintf(f, "il2cpp_base=0x%lx il2cpp_size=0x%lx\n",
            (unsigned long)g_il2cpp_base,
            (unsigned long)g_il2cpp_size);

    extern void _start(void);
    extern char __bss_end__[];
    const uintptr_t host_base = (uintptr_t)&_start;
    const uintptr_t host_end = (uintptr_t)__bss_end__;

    /* Walk the stack via frame pointers starting from abort's caller. */
    uintptr_t addrs[16];
    int naddrs = 0;
    if (caller_ra) addrs[naddrs++] = (uintptr_t)caller_ra;

    /* Get abort()'s frame pointer and walk up. */
    void *fp = __builtin_frame_address(0);
    for (int depth = 0; depth < 14 && fp; depth++) {
      uintptr_t fp_addr = (uintptr_t)fp;
      MemoryInfo mi;
      u32 pi;
      if (R_FAILED(svcQueryMemory(&mi, &pi, fp_addr)) ||
          !(mi.perm & Perm_R) || mi.type == MemType_Unmapped ||
          fp_addr < mi.addr ||
          sizeof(uint64_t) * 2 > (size_t)(mi.addr + mi.size - fp_addr))
        break;
      uint64_t frame[2];
      memcpy(frame, fp, sizeof(frame));
      if (frame[1]) addrs[naddrs++] = (uintptr_t)frame[1];
      if (!frame[0] || frame[0] <= (uint64_t)fp_addr) break;
      fp = (void *)(uintptr_t)frame[0];
    }

    for (int i = 0; i < naddrs; i++) {
      uintptr_t a = addrs[i];
      MemoryInfo mi;
      u32 pi;
      Result qr = svcQueryMemory(&mi, &pi, a);
      if (g_il2cpp_base && a >= g_il2cpp_base &&
          a - g_il2cpp_base < g_il2cpp_size) {
        fprintf(f, "  bt%d=0x%lx guest+0x%lx", i,
                (unsigned long)a, (unsigned long)(a - g_il2cpp_base));
      } else if (a >= host_base && a < host_end) {
        fprintf(f, "  bt%d=0x%lx host+0x%lx", i,
                (unsigned long)a, (unsigned long)(a - host_base));
      } else {
        fprintf(f, "  bt%d=0x%lx absolute", i, (unsigned long)a);
      }
      if (R_SUCCEEDED(qr)) {
        fprintf(f, " mem[addr=0x%lx size=0x%lx type=%d perm=0x%x]",
                (unsigned long)mi.addr, (unsigned long)mi.size,
                (int)mi.type, (unsigned)mi.perm);
      }
      fprintf(f, "\n");
    }
    fflush(f);
    fclose(f);
  }
}

void exit(int status) {
  log_crash_exit(g_abort_source ? g_abort_source : "exit",
                 __builtin_return_address(0));
  _exit(status);
}

void abort(void) {
  const char *reason = g_abort_source ? g_abort_source : "abort";
  void *caller = __builtin_return_address(0);
  register uintptr_t sp_val asm("sp");
  FILE *f = fopen(DATA_ROOT "/crash_exit.txt", "w");
  if (f) {
    u64 used = 0, total = 0;
    svcGetInfo(&used, InfoType_UsedMemorySize, CUR_PROCESS_HANDLE, 0);
    svcGetInfo(&total, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0);
    fprintf(f, "%s frame=%d in_render=%d used=%lluM total=%lluM\n", reason,
            g_render_frame, g_render_in_progress,
            (unsigned long long)(used / (1024 * 1024)),
            (unsigned long long)(total / (1024 * 1024)));
    fprintf(f, "il2cpp_base=0x%lx il2cpp_size=0x%lx\n",
            (unsigned long)g_il2cpp_base,
            (unsigned long)g_il2cpp_size);

    extern void _start(void);
    extern char __bss_end__[];
    const uintptr_t host_base = (uintptr_t)&_start;
    const uintptr_t host_end = (uintptr_t)__bss_end__;

    fprintf(f, "caller=0x%lx\n", (unsigned long)(uintptr_t)caller);
    if (caller) {
      uintptr_t a = (uintptr_t)caller;
      if (g_il2cpp_base && a >= g_il2cpp_base &&
          a - g_il2cpp_base < g_il2cpp_size)
        fprintf(f, "  caller=guest+0x%lx\n", (unsigned long)(a - g_il2cpp_base));
      else if (a >= host_base && a < host_end)
        fprintf(f, "  caller=host+0x%lx\n", (unsigned long)(a - host_base));
      else
        fprintf(f, "  caller=absolute\n");
    }

    fprintf(f, "stack scan sp=0x%lx:\n", (unsigned long)sp_val);
    for (int i = 0; i < 1024; i++) {
      uintptr_t addr = sp_val + (uintptr_t)i * 8;
      MemoryInfo mi;
      u32 pi;
      if (R_FAILED(svcQueryMemory(&mi, &pi, addr)) ||
          !(mi.perm & Perm_R) || mi.type == MemType_Unmapped ||
          addr < mi.addr ||
          sizeof(uint64_t) > (size_t)(mi.addr + mi.size - addr))
        continue;
      uint64_t val = *(uint64_t *)addr;
      if (!val) continue;
      if ((val >= host_base && val < host_end) ||
          (g_il2cpp_base && val >= g_il2cpp_base &&
           val < g_il2cpp_base + g_il2cpp_size)) {
        fprintf(f, "  [sp+0x%x]=0x%llx", i * 8,
                (unsigned long long)val);
        if (val >= host_base && val < host_end)
          fprintf(f, " host+0x%llx", (unsigned long long)(val - host_base));
        else
          fprintf(f, " guest+0x%llx",
                  (unsigned long long)(val - g_il2cpp_base));
        fprintf(f, "\n");
      }
    }
    panic_capture_report(f);
    {
      NxSparseArenaDiagnostics diag = {0};
      nx_sparse_arena_get_diagnostics(&diag);
      const unsigned long long MiB = 1024ull * 1024ull;
      fprintf(f,
              "pool backend=%u committed=%lluMiB peak_committed=%lluMiB "
              "pool_free=%lluMiB largest_free=%lluMiB\n",
              diag.backing_backend,
              diag.committed_bytes / MiB,
              diag.peak_committed_bytes / MiB,
              diag.pool_free_bytes / MiB,
              diag.pool_largest_free_bytes / MiB);
      fprintf(f,
              "donor cap=%lluMiB active=%lluMiB used=%lluMiB/peak=%lluMiB "
              "grow=%llu shrink=%llu last_resize=0x%x\n",
              diag.donor_capacity_bytes / MiB,
              diag.donor_active_bytes / MiB,
              diag.donor_used_bytes / MiB,
              diag.donor_peak_used_bytes / MiB,
              (unsigned long long)diag.donor_grow_calls,
              (unsigned long long)diag.donor_shrink_calls,
              diag.donor_last_resize_result);
      fprintf(f,
              "alloc_failures guest=%llu host=%llu thread=%llu "
              "dynamic_mapped=%lluMiB/peak=%lluMiB last_map=0x%x\n",
              (unsigned long long)diag.guest_allocation_failures,
              (unsigned long long)diag.host_allocation_failures,
              (unsigned long long)diag.thread_allocation_failures,
              diag.dynamic_mapped_bytes / MiB,
              diag.peak_dynamic_mapped_bytes / MiB,
              diag.last_map_result);
      fprintf(f, "backing_unmap ok=%llu fail=%llu\n",
              (unsigned long long)diag.backing_unmap_ok,
              (unsigned long long)diag.backing_unmap_fail);
    }
    sbrk_extension_report(f);
    memory_broker_histogram_report(f);
    fflush(f);
    fclose(f);
  }
  _exit(1);
}

int main(int argc, char **argv) {
  (void)argc;
  (void)argv;

  signal(SIGSEGV, crash_signal_handler);
  signal(SIGABRT, crash_signal_handler);
  signal(SIGILL, crash_signal_handler);
  signal(SIGBUS, crash_signal_handler);
  signal(SIGFPE, crash_signal_handler);

  /* Host Rust (NVK/NAK) panics print their message with raw write(2).  Bind
   * fd 2 to a durable file early so the payload survives instead of hitting
   * the uninitialized software-console devoptab.  Guest writes to fd 1/2 keep
   * flowing through the nx_write logging endpoints, unaffected. */
  {
    static char rust_backtrace_env[] = "RUST_BACKTRACE=1";
    int err_fd = open(DATA_ROOT "/stderr.txt",
                      O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (err_fd >= 0) {
      if (err_fd != STDERR_FILENO) {
        dup2(err_fd, STDERR_FILENO);
        close(err_fd);
      }
    }
    putenv(rust_backtrace_env);
  }

  /* NVK/Mesa env vars for the resource-download device-lost investigation
   * (vkQueueSubmit #1001 returns VK_ERROR_DEVICE_LOST after
   * FilesDownloadPipe StartDownload).  NVK is statically linked into this NRO
   * and reads the same environ; getenv_fake (libc_shim.c) passes unknown vars
   * through to the real getenv, so these reach the driver without code changes
   * to the Vulkan path.
   *
   * NVK_DEBUG=push_sync serializes each push-buffer submission: if the
   * device-lost disappears under it, the hang is timing/watchdog-induced
   * (download-burst overrunning the nvhost channel) and a fence-throttle in
   * the existing nx_vkQueueSubmit hook is the productionizable fix.  If it
   * persists on the same submit, the offending command buffer is
   * deterministic and Mesa-side.  push_dump captures that buffer to
   * stderr.txt (bound above).
   *
   * MESA_VK_ABORT_ON_DEVICE_LOST turns the first device-lost into a SIGABRT
   * instead of a silent -4 return the game then mishandles; combined with
   * RUST_BACKTRACE=1 this yields a backtrace at the failing submit. */
  {
    static char nvk_debug_env[] = "NVK_DEBUG=push_sync";
    static char nvk_dump_env[] = "NVK_DEBUG=push_sync,push_dump";
    static char abort_env[] = "MESA_VK_ABORT_ON_DEVICE_LOST=1";
    /* push_sync alone first; add push_dump on a follow-up build if the hang
     * survives serialization, to keep stderr.txt from filling on good runs. */
    (void)nvk_dump_env;
    putenv(nvk_debug_env);
    putenv(abort_env);
  }

  startup_status_begin("Validating the Android client");
  if (chdir(DATA_ROOT) != 0)
    fatal_error("Could not enter %s. Copy the staged game directory to the SD card.",
                DATA_ROOT);
  make_runtime_dirs();
  unlink(DATA_ROOT "/fatal.txt");
  unlink(DATA_ROOT "/run_log.txt");
  unlink(DATA_ROOT "/crash_frame.txt");
  unlink(DATA_ROOT "/crash_signal.txt");
  unlink(DATA_ROOT "/crash_exit.txt");
  unlink(DATA_ROOT "/arena_debug.txt");

  /* Validate the kernel-heap boundary before APK/asset-pack work performs any
   * ordinary allocations. */
  check_syscalls();
  validate_fixed_heap_reclaim();

  /* Optional harvested-device identity (see tools/harvest_device_profile.ps1):
   * must be ready before the Passport client, JNI fake and Unity property
   * lookups read device identity. */
  device_profile_init();
  libc_shim_apply_device_profile();

  const char *config_path = DATA_ROOT "/" CONFIG_NAME;
  if (read_config(config_path) != 0) write_config(config_path);
  if (!config.force_vulkan)
    fatal_error("force_vulkan must remain enabled; the supplied NVK driver is the supported renderer.");
  prepare_game_data();
  discard_incomplete_il2cpp_metadata_cache();
  const int context_bridge_result = check_thread_context_bridge();
  if (context_bridge_result)
    fatal_error("Thread pause/context/resume self-test failed (0x%x).",
                (unsigned)context_bridge_result);

  if (!pthread_storage_self_test())
    fatal_error("Android pthread storage initialization self-test failed.");

  initialize_sparse_arena();
  if (!sparse_guest_spill_self_test())
    fatal_error("Dynamic guest allocation lifecycle self-test failed at %s.",
                g_sparse_guest_test_failure);
  if (!sparse_alias_mmap_self_test())
    fatal_error("Sparse alias mmap lifecycle self-test failed at %s.",
                g_sparse_mmap_test_failure);

  if (!sparse_thread_stack_self_test())
    fatal_error("Sparse caller-owned pthread stack lifecycle self-test failed.");
  void *unity_slab_reservation = NULL;
  size_t unity_slab_reservation_size = 0;

  startup_status_update("Starting network services");
  const Result nifm_result = nifmInitialize(NifmServiceType_User);
  nifm_started = R_SUCCEEDED(nifm_result);
  /* Unity performs networking from many workers.  Raise only the BSD IPC
   * session count and the datagram receive queue: this exact
   * default-TCP-buffer/16-session tuple reached the installer and
   * sustained the first large resource transfer on hardware.  Oversized
   * TCP per-socket buffers later stalled post-login server dispatch, so
   * TCP sizes stay at the defaults; UDP only carries the KCP download,
   * which the small default queue (0xA500) overflows under bursts. */
  SocketInitConfig socket_config = *socketGetDefaultInitConfig();
  socket_config.num_bsd_sessions = NETWORK_BSD_SESSION_COUNT;
  const uint32_t default_udp_rx_buf_size = socket_config.udp_rx_buf_size;
  socket_config.udp_rx_buf_size = NETWORK_UDP_RX_BUF_CANDIDATE;
  Result socket_result = socketInitialize(&socket_config);
  if (R_FAILED(socket_result) &&
      socket_config.udp_rx_buf_size != default_udp_rx_buf_size) {
    /* socketInitialize already cleaned up its partial state on failure. */
    socket_config.udp_rx_buf_size = default_udp_rx_buf_size;
    socket_result = socketInitialize(&socket_config);
  }
  const uint32_t effective_udp_rx_buf_size = socket_config.udp_rx_buf_size;
  const uint32_t configured_udp_rx_max =
    socket_config.tcp_rx_buf_max_size; /* datagram promotion ceiling */
  socket_started = R_SUCCEEDED(socket_result);
  g_net_on = socket_started;
  network_configure_long_stream_receive_window(
    socket_started ? socket_config.tcp_rx_buf_size : 0,
    socket_started ? socket_config.tcp_rx_buf_max_size : 0);
  network_configure_datagram_receive_window(
    socket_started ? effective_udp_rx_buf_size : 0,
    socket_started ? configured_udp_rx_max : 0);

  if (!socket_started)
    fatal_error("Nintendo Switch socket services could not be initialized.");
  if (combo_auth_init() != 0)
    fatal_error("Could not initialize the credential-safe Passport HTTPS client.");

  initialize_network_state();

  (void)android_native_update_mode();

  SDL_SetMainReady();
  if (SDL_Init(SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER) < 0)
    fatal_error("SDL_Init failed: %s", SDL_GetError());
  (void)opensles_initialize();

  startup_status_update("Loading libyuanshen.so (first boot may take a while)");
  int load_result = load_game_module();
  if (load_result < 0)
    fatal_error("Could not load %s (loader error %d).", LIB_GAME, load_result);
  if (!supported_game_image())
    fatal_error("Unsupported libyuanshen.so. This wrapper requires SHA-256:\n%s",
                GENSHIN_EXACT_SHA256);

  startup_status_update("Preparing Unity virtual memory");
  if (!mmap_prepare_unity_slab_reservation(&unity_slab_reservation,
                                            &unity_slab_reservation_size))
    fatal_error("Unity's 4104 MiB slab partition was not preserved by the alias-region layout.");

  check_synthetic_cpu_topology_read();
  check_force_vulkan_sentinel();
  check_vulkan_egl_configuration();
  check_globalgamemanagers_seek_read();
  check_global_metadata_digest_read();
  check_startup_metadata_mapping();
  g_il2cpp_base = (uintptr_t)game_mod.load_virtbase;
  g_il2cpp_size = game_mod.load_size;

  if (config.enable_plugins &&
      plugin_loader_init(heap_so_base, heap_so_limit, DATA_ROOT) != 0) {
    const char *why = plugin_loader_dlerror();
    fatal_error("Could not initialize plugin loader%s%s.",
                why ? ": " : "", why ? why : "");
  }
  startup_status_update("Finalizing Android relocations");
  so_finalize(&game_mod);
  so_flush_caches(&game_mod);

  patch_unity_java_class_resolution();
  patch_genshin_transfer_int32_guard();
  patch_mmoron_managed_directory_path();
  patch_unity_slab_activation();

  const uintptr_t host_thread_pointer = read_thread_pointer();
  static uint8_t main_bionic_tls[BIONIC_TLS_SIZE] __attribute__((aligned(16)));
  install_bionic_tls(main_bionic_tls);

  startup_status_update("Running Unity constructors");
  so_execute_init_array(&game_mod);
  if (combo_bridge_init(&game_mod) != 0) {
    const char *why = combo_bridge_error();
    fatal_error("Could not bind exact Unity callback transport%s%s.",
                why ? ": " : "", why ? why : "");
  }

  so_free_temp(&game_mod);

  jni_init();
  if (!jni_reference_self_test())
    fatal_error("JNI local/global/array ownership self-test failed.");

  unity_environment_init(DATA_ROOT);
  if (!unity_storage_paths_self_test())
    fatal_error("Android internal/external storage paths alias each other.");

  android_native_input_init();

  if (combo_crypto_init() != 0) {
    const char *why = combo_crypto_error();
    fatal_error("HoYo Combo native crypto initialization failed%s%s.",
                why ? ": " : "", why ? why : "");
  }

  extern void *fake_vm, *fake_env;
  extern void *fake_unityplayer_thiz, *fake_context_obj, *fake_surface_obj;

  JniOnLoadFn jni_on_load =
    (JniOnLoadFn)so_try_find_addr_rx(&game_mod, "JNI_OnLoad");
  if (!jni_on_load)
    fatal_error("libyuanshen.so JNI_OnLoad failed.");
  jni_boundary_begin("JNI_OnLoad");
  const int jni_version = jni_on_load(fake_vm, NULL);
  jni_boundary_end();
  if (jni_version != JNI_VERSION_1_6)
    fatal_error("libyuanshen.so JNI_OnLoad returned unsupported version 0x%x.",
                jni_version);

  const GenshinJniNativeMethod *loader_table =
    (const GenshinJniNativeMethod *)((uintptr_t)game_mod.load_virtbase +
                                     GENSHIN_NATIVELOADER_TABLE_RVA);
  validate_native_table(loader_table, GENSHIN_NATIVELOADER_COUNT, "NativeLoader");
  NativeLoaderLoadFn native_loader_load = (NativeLoaderLoadFn)find_native(
    loader_table, GENSHIN_NATIVELOADER_COUNT,
    "load", "(Ljava/lang/String;)Z");
  void *library_path = jni_make_string(DATA_ROOT "/" LIB_GAME);
  jni_boundary_begin("NativeLoader.load");
  const int native_loader_ok =
    native_loader_load(fake_env, fake_unityplayer_thiz, library_path);
  jni_boundary_end();
  if (!native_loader_ok)
    fatal_error("Unity NativeLoader.load failed.");

  const GenshinJniNativeMethod *unity_table =
    (const GenshinJniNativeMethod *)((uintptr_t)game_mod.load_virtbase +
                                     GENSHIN_UNITY_TABLE_RVA);
  validate_native_table(unity_table, GENSHIN_UNITY_TABLE_COUNT, "UnityPlayer");

  fn_initJni unity_init_jni = (fn_initJni)find_native(
    unity_table, GENSHIN_UNITY_TABLE_COUNT,
    "initJni", "(Landroid/content/Context;)V");
  fn_gfxstate unity_recreate_gfx = (fn_gfxstate)find_native(
    unity_table, GENSHIN_UNITY_TABLE_COUNT,
    "nativeRecreateGfxState", "(ILandroid/view/Surface;)V");
  fn_z unity_render = (fn_z)find_native(
    unity_table, GENSHIN_UNITY_TABLE_COUNT, "nativeRender", "()Z");
  fn_inject unity_inject = (fn_inject)find_native(
    unity_table, GENSHIN_UNITY_TABLE_COUNT,
    "nativeInjectEvent", "(Landroid/view/InputEvent;)Z");
  fn_v unity_resume = (fn_v)find_native(
    unity_table, GENSHIN_UNITY_TABLE_COUNT, "nativeResume", "()V");
  NativePauseFn unity_pause = (NativePauseFn)find_native(
    unity_table, GENSHIN_UNITY_TABLE_COUNT, "nativePause", "()Z");
  fn_vz unity_focus = (fn_vz)find_native(
    unity_table, GENSHIN_UNITY_TABLE_COUNT, "nativeFocusChanged", "(Z)V");
  fn_v unity_done = (fn_v)find_native(
    unity_table, GENSHIN_UNITY_TABLE_COUNT, "nativeDone", "()V");

  jni_boundary_begin("initJni");
  unity_init_jni(fake_env, fake_unityplayer_thiz, fake_context_obj);
  jni_boundary_end();

  /* The console framebuffer must release the default NWindow before Unity/NVK
   * creates its Vi surface. */
  startup_status_end();
  const int initial_geometry_result = android_native_apply_window_geometry();
  if (initial_geometry_result != 0)
    fatal_error("Could not configure the initial native window geometry (%d).",
                initial_geometry_result);
  jni_boundary_begin("nativeRecreateGfxState");
  unity_recreate_gfx(fake_env, fake_unityplayer_thiz, 0, fake_surface_obj);
  jni_boundary_end();

  jni_boundary_begin("nativeResume/nativeFocusChanged");
  unity_resume(fake_env, fake_unityplayer_thiz);
  unity_focus(fake_env, fake_unityplayer_thiz, 1);
  jni_boundary_end();
  opensles_set_focus(1);
  combo_bridge_set_focus(1);

  (void)android_native_looper_pump_callbacks(8);

  int focused = 1;
  int unity_active = 1;
  int display_recreate_pending = 0;
  int first_render_done = 0;
  int frame_count = 0;
  int applet_running = 1;
  while ((applet_running = appletMainLoop()) && !jni_quit_requested) {
    const int now_focused = appletGetFocusState() == AppletFocusState_InFocus;
    if (android_native_update_mode()) {
      display_recreate_pending = 1;
    }
    if (now_focused != focused) {
      if (!now_focused) {
        opensles_set_focus(0);
        combo_bridge_set_focus(0);
      }
      if (!now_focused || !display_recreate_pending) {
        jni_boundary_begin("focus lifecycle");
        if (!now_focused) {
          if (unity_active) {
            unity_focus(fake_env, fake_unityplayer_thiz, 0);
            (void)unity_pause(fake_env, fake_unityplayer_thiz);
          }
        } else {
          unity_resume(fake_env, fake_unityplayer_thiz);
          unity_focus(fake_env, fake_unityplayer_thiz, 1);
        }
        jni_boundary_end();
        unity_active = now_focused;
        if (now_focused) {
          opensles_set_focus(1);
          combo_bridge_set_focus(1);
        }
      }
      /* When a display recreation is pending, keep Unity and Combo paused;
       * the transaction below will perform the only resume/focus transition. */
      focused = now_focused;
    }
    if (display_recreate_pending && focused) {
      opensles_set_focus(0);
      combo_bridge_set_focus(0);
      int geometry_result = 0;
      jni_boundary_begin("nativeRecreateGfxState");
      if (unity_active) {
        unity_focus(fake_env, fake_unityplayer_thiz, 0);
        (void)unity_pause(fake_env, fake_unityplayer_thiz);
      }
      /* Destroy the old swapchain first.  libnx forbids changing NWindow
       * dimensions while any producer slots remain registered. */
      unity_recreate_gfx(fake_env, fake_unityplayer_thiz, 0, NULL);
      geometry_result = android_native_apply_window_geometry();
      if (geometry_result == 0) {
        unity_recreate_gfx(fake_env, fake_unityplayer_thiz, 0,
                           fake_surface_obj);
        unity_resume(fake_env, fake_unityplayer_thiz);
        unity_focus(fake_env, fake_unityplayer_thiz, 1);
      }
      jni_boundary_end();
      if (geometry_result != 0)
        fatal_error("Could not apply dock-mode native window geometry (%d).",
                    geometry_result);
      unity_active = 1;
      display_recreate_pending = 0;
      opensles_set_focus(1);
      combo_bridge_set_focus(1);
    }
    (void)android_native_looper_pump_callbacks(8);
    if (!focused) {
      svcSleepThread(16 * 1000 * 1000ULL);
      continue;
    }
    android_native_vibration_update();
    jni_boundary_begin("nativeInjectEvent");
    android_native_feed_hid(
      (uint8_t (*)(void *, void *, void *, int))unity_inject,
      fake_env, fake_unityplayer_thiz);
    jni_boundary_end();
    if (frame_count % 120 == 0) {
      NxSparseArenaDiagnostics diag = {0};
      nx_sparse_arena_get_diagnostics(&diag);
      NetworkTransportDiagnostics net = {0};
      network_get_transport_diagnostics(&net);
      FILE *lf = fopen(DATA_ROOT "/run_log.txt", "ab");
      if (lf) {
        const unsigned long long MiB = 1024ull * 1024ull;
        fprintf(lf,
                "[I] main: frame %d used=%lluM total=%lluM "
                "donor=%lluM/%lluM mapped=%lluM unmap=%llu/%llu\n",
                frame_count,
                diag.system_used_memory_bytes / MiB,
                diag.system_total_memory_bytes / MiB,
                diag.donor_used_bytes / MiB,
                diag.donor_active_bytes / MiB,
                diag.dynamic_mapped_bytes / MiB,
                (unsigned long long)diag.backing_unmap_ok,
                (unsigned long long)diag.backing_unmap_fail);
        /* Network transport telemetry.  rxall counts payload from BOTH the
         * TCP and UDP paths (the counter is fed by every bionic recv
         * variant), so bulk KCP traffic is included here as well; the
         * dedicated net-udp line below separates it.  long_stream_window_*
         * show whether the stream SO_RCVBUF promotion fired and what
         * effective window each bulk stream got.  Without this the ~350
         * kbit/s download cap was unobservable. */
        const unsigned long long KiB = 1024ull;
        fprintf(lf,
                "[I] net: rxall=%lluB/%llu rxfail=%llu wblock=%llu "
                "rcvbuf win att=%llu ok=%llu fail=%llu eff=%lluB "
                "largest=%lluB streams=%llu/%llu\n",
                (unsigned long long)net.received_bytes,
                (unsigned long long)net.recv_calls,
                (unsigned long long)net.recv_failures,
                (unsigned long long)net.recv_would_block,
                (unsigned long long)net.long_stream_window_attempts,
                (unsigned long long)net.long_stream_window_successes,
                (unsigned long long)net.long_stream_window_failures,
                (unsigned long long)net.last_long_stream_window_effective,
                (unsigned long long)net.largest_stream_received_bytes,
                (unsigned long long)net.receiving_stream_sockets,
                (unsigned long long)net.tracked_stream_sockets);
        /* UDP/KCP transport telemetry.  Genshin's bulk resource download runs
         * over KCP (reliable UDP via recvmsg/recvfrom), so the TCP line above
         * freezes at the control-traffic total while gigabytes flow here.
         * Diff udp_recv_bytes between two consecutive heartbeat lines (120
         * frames apart) for actual download throughput.  udp_receive_errors
         * rising against udp_recv_calls indicates packet loss / KCP
         * retransmit pressure, which is a candidate root of the ~350 kbit/s
         * cap. */
        fprintf(lf,
                "[I] net-udp: recv=%lluB/%llu sent=%lluB/%llu "
                "rxfail=%llu dgram=%llu largest=%lluB "
                "win att=%llu ok=%llu fail=%llu eff=%lluB tgt=%lluB\n",
                (unsigned long long)net.udp_received_bytes,
                (unsigned long long)net.udp_recv_calls,
                (unsigned long long)net.udp_sent_bytes,
                (unsigned long long)net.udp_send_calls,
                (unsigned long long)net.udp_receive_errors,
                (unsigned long long)net.tracked_datagram_sockets,
                (unsigned long long)net.largest_datagram_received_bytes,
                (unsigned long long)net.datagram_window_attempts,
                (unsigned long long)net.datagram_window_successes,
                (unsigned long long)net.datagram_window_failures,
                (unsigned long long)net.last_datagram_window_effective,
                (unsigned long long)net.datagram_receive_window_target);
        (void)KiB;
        /* Donor headroom + file-IO trim-stall telemetry.  During the download
         * verification phase (~50GB of .blk hashed at once) the heap-donor pool
         * approaches its ceiling and finalize_fd fsFileSetSize IPCs can stall.
         * headroom = donor_capacity - donor_active; when it nears zero the
         * next file-backed mmap can trip svcSetHeapSize under memory pressure
         * and wedge the system.  fio_active/oldest_ms expose a stuck trim:
         * oldest_ms climbing across heartbeats = a finalize_fd IPC not
         * returning.  pool_free=UINT64_MAX is the "broker busy" sentinel set
         * when g_mmap_lock could not be try-locked (the hang signature). */
        NxFileIoDiagnostics fio = {0};
        nx_file_io_get_diagnostics(&fio);
        const unsigned long long donor_cap = diag.donor_capacity_bytes / MiB;
        const unsigned long long donor_head =
          (diag.donor_capacity_bytes > diag.donor_active_bytes)
            ? (diag.donor_capacity_bytes - diag.donor_active_bytes) / MiB
            : 0;
        const unsigned long long pool_free = diag.pool_free_bytes;
        fprintf(lf,
                "[I] mem: cap=%lluM head=%lluM pool_free=%lluB "
                "fio_active=%llu oldest=%llums slot=%u kind=%u fin=%llu/%llu\n",
                donor_cap, donor_head,
                (pool_free == UINT64_MAX) ? UINT64_MAX : pool_free,
                (unsigned long long)fio.size_operations_active,
                (unsigned long long)fio.oldest_size_operation_ms,
                (fio.oldest_size_operation_slot == UINT32_MAX)
                  ? 0u : fio.oldest_size_operation_slot,
                fio.oldest_size_operation_kind,
                (unsigned long long)fio.finalize_calls,
                (unsigned long long)fio.finalize_failures);
        fprintf(lf,
                "[I] fio: rd=%lluB/%llu fail=%llu wr=%lluB/%llu fail=%llu "
                "size=q%llu/h%llu/qfail%llu/ext%llu/efail%llu "
                "direct=%llu/%llu bounce=%lluB/%llu\n",
                (unsigned long long)fio.read_bytes,
                (unsigned long long)fio.read_calls,
                (unsigned long long)fio.read_failures,
                (unsigned long long)fio.write_bytes,
                (unsigned long long)fio.write_calls,
                (unsigned long long)fio.write_failures,
                (unsigned long long)fio.size_queries,
                (unsigned long long)fio.size_cache_hits,
                (unsigned long long)fio.size_query_failures,
                (unsigned long long)fio.size_extensions,
                (unsigned long long)fio.size_extension_failures,
                (unsigned long long)fio.direct_writes,
                (unsigned long long)fio.direct_write_failures,
                (unsigned long long)fio.bounce_bytes,
                (unsigned long long)fio.bounce_writes);
        fclose(lf);
      }
    }
    g_render_frame = frame_count;
    g_render_in_progress = 1;
    jni_boundary_begin("nativeRender");
    const int render_continues = unity_render(fake_env, fake_unityplayer_thiz);
    jni_boundary_end();
    g_render_in_progress = 0;
    /* Exact-image call chain: nativeRender RVA 0x49c3cd4 reaches
     * 0x49bb288 -> 0x531ff84 -> 0x44974e8 -> 0x44ac054.  That final function
     * performs the 0x100800000-byte mmap through 0x44b5748 and publishes the
     * three allocator globals.  initJni does not reach this path, so the slab
     * invariant becomes meaningful only after the first nativeRender call. */
    if (!first_render_done)
      validate_unity_slab_client_state(unity_slab_reservation,
                                       unity_slab_reservation_size);
    if (!render_continues) break;
    ++frame_count;
    if (!first_render_done) {
      first_render_done = 1;
      repair_combo_managed_class_name();
    }
    combo_bridge_after_render();
    const ComboAuthAction auth_action = combo_auth_next_action();
    if (auth_action == COMBO_AUTH_ACTION_INPUT ||
        auth_action == COMBO_AUTH_ACTION_VERIFIER_INPUT ||
        auth_action == COMBO_AUTH_ACTION_GEETEST) {
      /* The hardware-proven system keyboard is a blocking library applet.
       * Pause Unity and callback delivery while it owns foreground input, but
       * leave the working Vulkan surface/swapchain untouched. */
      opensles_set_focus(0);
      combo_bridge_set_focus(0);
      jni_boundary_begin("HoYoverse credential input pause");
      unity_focus(fake_env, fake_unityplayer_thiz, 0);
      (void)unity_pause(fake_env, fake_unityplayer_thiz);
      jni_boundary_end();
      const uintptr_t guest_thread_pointer = read_thread_pointer();
      write_thread_pointer(host_thread_pointer);
      if (auth_action == COMBO_AUTH_ACTION_INPUT)
        combo_auth_collect_credentials();
      else if (auth_action == COMBO_AUTH_ACTION_VERIFIER_INPUT)
        combo_auth_collect_verification();
      else
        combo_auth_collect_geetest();
      write_thread_pointer(guest_thread_pointer);
      jni_boundary_begin("HoYoverse credential input resume");
      unity_resume(fake_env, fake_unityplayer_thiz);
      unity_focus(fake_env, fake_unityplayer_thiz, 1);
      jni_boundary_end();
      opensles_set_focus(1);
      combo_bridge_set_focus(1);
    } else if (auth_action == COMBO_AUTH_ACTION_NETWORK) {
      /* libcurl, SPL, and libnx use the host newlib TLS layout. Unity installed
       * a Bionic-compatible TP on this thread, so never enter them with it. */
      const uintptr_t guest_thread_pointer = read_thread_pointer();
      write_thread_pointer(host_thread_pointer);
      combo_auth_tick();
      write_thread_pointer(guest_thread_pointer);
    }
  }

  g_render_frame = frame_count;
  FILE *lf = fopen(DATA_ROOT "/run_log.txt", "ab");
  if (lf) {
    fprintf(lf, "[I] main: render loop exited after %d frames "
            "(jni_quit_requested=%d appletMainLoop=%d)\n",
            frame_count, jni_quit_requested, applet_running);
    fclose(lf);
  }

  opensles_set_focus(0);
  combo_bridge_set_focus(0);
  jni_boundary_begin("shutdown focus/pause");
  if (unity_active) unity_focus(fake_env, fake_unityplayer_thiz, 0);
  (void)unity_pause(fake_env, fake_unityplayer_thiz);
  jni_boundary_end();
  combo_bridge_shutdown();
  /* Match Android's surfaceDestroyed ordering so NVK can retire the Vi
   * surface/swapchain before Unity tears down the remaining graphics state. */
  jni_boundary_begin("shutdown graphics/done");
  unity_recreate_gfx(fake_env, fake_unityplayer_thiz, 0, NULL);
  unity_done(fake_env, fake_unityplayer_thiz);
  jni_boundary_end();

  write_thread_pointer(host_thread_pointer);
  shutdown_host();
  extern void NX_NORETURN __libnx_exit(int rc);
  __libnx_exit(0);
}
