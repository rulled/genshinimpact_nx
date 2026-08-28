/* Serialized, constructor-reentrant Android DT_NEEDED/dlopen loader.
 *
 * The main Unity image is registered by so_load().  Optional libraries are
 * streamed into the unused tail of the same dedicated module arena and kept
 * resident for process lifetime: unloading executable aliases while guest
 * callbacks may still exist is less safe than Android's usual NODELETE-like
 * behavior for game plugins. */

#include <switch.h>
#include <EGL/egl.h>

#include <ctype.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "imports.h"
#include "plugin_loader.h"
#include "so_util.h"
#include "vulkan_bridge.h"
#include "vulkan_egl_stubs.h"

#define PLUGIN_MAX_MODULES 32
#define PLUGIN_MAX_DEPS    24
#define PLUGIN_NAME_MAX    96
#define PLUGIN_PATH_MAX    512
#define PLUGIN_LOAD_ALIGN  0x10000u

_Static_assert(
  ALIGN_MEM(UINT64_C(0xb360af000), PLUGIN_LOAD_ALIGN) ==
    UINT64_C(0xb360b0000),
  "ALIGN_MEM must retain upper bits of high module-arena addresses");

/* Bionic flag values.  LAZY/NOW and LOCAL/GLOBAL do not change the eager
 * relocation strategy, but NOLOAD is honored. */
#define BIONIC_RTLD_NOLOAD 4

enum PluginState {
  PLUGIN_FREE,
  PLUGIN_LOADING,
  PLUGIN_INITIALIZING,
  PLUGIN_READY,
  PLUGIN_FAILED,
};

typedef struct PluginEntry {
  so_module module;
  char basename[PLUGIN_NAME_MAX];
  so_module *deps[PLUGIN_MAX_DEPS];
  size_t dep_count;
  unsigned refs;
  int jni_onload_state;
  int jni_version;
  enum PluginState state;
  char failure[192];
} PluginEntry;

static PluginEntry g_plugins[PLUGIN_MAX_MODULES];
static uintptr_t g_arena_at;
static size_t g_arena_left;
static char g_data_root[PLUGIN_PATH_MAX];
static int g_initialized;

static Mutex g_loader_lock;
static Handle g_loader_owner;
static unsigned g_loader_depth;
/* dlerror state is per calling thread on Android/POSIX.  Keeping both the
 * pending diagnostic and the returned stable copy in host TLS prevents a
 * concurrent telemetry dlopen/dlsym from consuming or overwriting Unity's
 * failure reason.  The loader lock still protects module state and recursive
 * dependency diagnostics. */
static _Thread_local char g_error[384];
static _Thread_local char g_return_error[384];
static _Thread_local int g_error_pending;
static int g_host_handle_token;
static int g_default_handle_token;
static int g_optional_handle_token;
static so_module *g_main_module;

extern void *firebase_stub_lookup(const char *symbol);

static void loader_lock(void) {
  const Handle self = threadGetCurHandle();
  if (g_loader_depth && g_loader_owner == self) {
    ++g_loader_depth;
    return;
  }
  mutexLock(&g_loader_lock);
  g_loader_owner = self;
  g_loader_depth = 1;
}

static void loader_unlock(void) {
  if (--g_loader_depth) return;
  g_loader_owner = 0;
  mutexUnlock(&g_loader_lock);
}

static void set_error(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(g_error, sizeof(g_error), fmt, ap);
  va_end(ap);
  g_error_pending = 1;
}

static void copy_string(char *dst, size_t cap, const char *src) {
  if (!dst || !cap) return;
  if (!src) src = "";
  const size_t count = strnlen(src, cap - 1);
  memcpy(dst, src, count);
  dst[count] = 0;
}

static const char *path_basename(const char *path) {
  const char *slash = path ? strrchr(path, '/') : NULL;
  return slash ? slash + 1 : path;
}

/* IL2CPP preserves the managed DllImport module name.  The exact client uses
 * bare CRIWARE names such as `cri_ware_unity`, whereas Android's filesystem
 * contains the conventional lib*.so files.  ClassLoader.findLibrary performs
 * this decoration for Java loads, but the native P/Invoke dlopen path does
 * not pass through ClassLoader.  Keep the aliases exact instead of applying a
 * broad prefix/suffix rule to unaudited plug-ins. */
static const char *canonical_cri_library(const char *name) {
  if (!name) return name;
  if (!strcmp(name, "cri_ware_unity") ||
      !strcmp(name, "cri_ware_unity.so") ||
      !strcmp(name, "libcri_ware_unity"))
    return "libcri_ware_unity.so";
  if (!strcmp(name, "cri_vip_unity") ||
      !strcmp(name, "cri_vip_unity.so") ||
      !strcmp(name, "libcri_vip_unity"))
    return "libcri_vip_unity.so";
  if (!strcmp(name, "cri_mana_vpx") ||
      !strcmp(name, "cri_mana_vpx.so") ||
      !strcmp(name, "libcri_mana_vpx"))
    return "libcri_mana_vpx.so";
  return name;
}

static int valid_basename(const char *name) {
  if (!name || !*name || strchr(name, '\\')) return 0;
  const size_t len = strlen(name);
  if (len < 4 || len >= PLUGIN_NAME_MAX || strcmp(name + len - 3, ".so"))
    return 0;
  for (size_t i = 0; i < len; ++i) {
    const unsigned char c = (unsigned char)name[i];
    if (!isalnum(c) && c != '_' && c != '-' && c != '.' && c != '+')
      return 0;
  }
  return strcmp(name, ".so") != 0 && strstr(name, "..") == NULL;
}

static int is_host_library(const char *name) {
  static const char *const names[] = {
    "libandroid.so", "libc.so", "libdl.so", "libEGL.so",
    "libGLESv1_CM.so", "libGLESv2.so", "libGLESv3.so", "libjnigraphics.so",
    "liblog.so", "libm.so", "libmediandk.so", "libOpenSLES.so",
    "libstdc++.so", "libvulkan.so", "libz.so", "ld-android.so",
  };
  for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); ++i)
    if (!strcmp(name, names[i])) return 1;
  return 0;
}

static PluginEntry *entry_for_module(const so_module *module) {
  for (size_t i = 0; i < PLUGIN_MAX_MODULES; ++i)
    if (g_plugins[i].state != PLUGIN_FREE &&
        g_plugins[i].state != PLUGIN_FAILED &&
        &g_plugins[i].module == module)
      return &g_plugins[i];
  return NULL;
}

static PluginEntry *entry_for_name(const char *basename) {
  for (size_t i = 0; i < PLUGIN_MAX_MODULES; ++i)
    if (g_plugins[i].state != PLUGIN_FREE &&
        !strcmp(g_plugins[i].basename, basename))
      return &g_plugins[i];
  return NULL;
}

static PluginEntry *alloc_entry(const char *basename) {
  PluginEntry *failed = NULL;
  for (size_t i = 0; i < PLUGIN_MAX_MODULES; ++i) {
    if (g_plugins[i].state == PLUGIN_FREE) {
      memset(&g_plugins[i], 0, sizeof(g_plugins[i]));
      snprintf(g_plugins[i].basename, sizeof(g_plugins[i].basename), "%s", basename);
      return &g_plugins[i];
    }
    if (!failed && g_plugins[i].state == PLUGIN_FAILED)
      failed = &g_plugins[i];
  }
  if (failed) {
    memset(failed, 0, sizeof(*failed));
    snprintf(failed->basename, sizeof(failed->basename), "%s", basename);
  }
  return failed;
}

static void cache_failure(PluginEntry *entry) {
  if (!entry) return;
  copy_string(entry->failure, sizeof(entry->failure),
              g_error_pending ? g_error : "unknown loader error");
  entry->state = PLUGIN_FAILED;
}

static int build_library_path(const char *basename, char *out, size_t cap) {
  const int n = snprintf(out, cap, "%s/lib/arm64-v8a/%s", g_data_root, basename);
  return n > 0 && (size_t)n < cap ? 0 : -1;
}

static int regular_file_exists(const char *path) {
  struct stat st;
  return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

static void *resolve_host_symbol(const char *symbol) {
  if (!symbol) return NULL;
  if (!strncmp(symbol, "vk", 2)) {
    void *vk = nx_vk_lookup(symbol);
    if (vk) return vk;
  }
  const uintptr_t shim = dynlib_find_export(symbol);
  if (shim) return (void *)shim;
  void *firebase = firebase_stub_lookup(symbol);
  if (firebase) return firebase;
  if (!strncmp(symbol, "gl", 2) || !strncmp(symbol, "egl", 3)) {
#ifdef VULKAN_ONLY
    void *compat = nx_egl_gles_compat_lookup(symbol);
    if (compat) return compat;
    return (void *)nx_eglGetProcAddress_stub(symbol);
#else
    return (void *)eglGetProcAddress(symbol);
#endif
  }
  return NULL;
}

static void *search_module_scope(so_module *module, const char *symbol,
                                 so_module **visited, size_t *visited_count);

static uintptr_t resolve_import(const char *name, void *opaque) {
  PluginEntry *importer = opaque;
  if (!importer || !name) return 0;

  /* Android resolves this local plugin group after the process-wide host and
   * main-image groups.  Never scan so_list here: a previously dlopened,
   * unrelated local plugin must not satisfy a later module's relocation. */
  void *result = resolve_host_symbol(name);
  if (result) return (uintptr_t)result;

  so_module *visited[PLUGIN_MAX_MODULES + 1];
  size_t visited_count = 0;
  visited[visited_count++] = &importer->module;
  if (g_main_module && g_main_module != &importer->module) {
    result = search_module_scope(g_main_module, name, visited, &visited_count);
    if (result) return (uintptr_t)result;
  }

  /* entry->deps preserves direct DT_NEEDED order.  Depth-first traversal of
   * each dependency preserves each child's declared order as well. */
  for (size_t i = 0; i < importer->dep_count; ++i) {
    result = search_module_scope(importer->deps[i], name,
                                 visited, &visited_count);
    if (result) return (uintptr_t)result;
  }
  return 0;
}

static int add_dependency(PluginEntry *entry, so_module *dependency) {
  if (!dependency) return -1;
  for (size_t i = 0; i < entry->dep_count; ++i)
    if (entry->deps[i] == dependency) return 0;
  if (entry->dep_count == PLUGIN_MAX_DEPS) return -1;
  entry->deps[entry->dep_count++] = dependency;
  return 0;
}

static so_module *load_bundled_locked(const char *basename, int dependency) {
  so_module *existing = so_find_module(basename);
  if (existing) {
    PluginEntry *known = entry_for_module(existing);
    if (known && !dependency) ++known->refs;
    return existing;
  }

  PluginEntry *entry = entry_for_name(basename);
  if (entry && entry->state == PLUGIN_FAILED) {
    set_error("dlopen %s: previous load failed: %s", basename,
              entry->failure[0] ? entry->failure : "unknown loader error");
    return NULL;
  }
  if (!entry) entry = alloc_entry(basename);
  if (!entry) {
    set_error("dlopen %s: module registry is full", basename);
    return NULL;
  }
  if (entry->state == PLUGIN_LOADING || entry->state == PLUGIN_INITIALIZING ||
      entry->state == PLUGIN_READY)
    return &entry->module;

  char path[PLUGIN_PATH_MAX];
  if (build_library_path(basename, path, sizeof(path)) != 0) {
    set_error("dlopen %s: staged library path is too long", basename);
    cache_failure(entry);
    return NULL;
  }
  if (!regular_file_exists(path)) {
    set_error("dlopen %s: not present under lib/arm64-v8a", basename);
    cache_failure(entry);
    return NULL;
  }

  const uintptr_t aligned = ALIGN_MEM(g_arena_at, PLUGIN_LOAD_ALIGN);
  const size_t padding = aligned - g_arena_at;
  if (padding >= g_arena_left) {
    set_error("dlopen %s: module arena is exhausted", basename);
    cache_failure(entry);
    return NULL;
  }
  entry->state = PLUGIN_LOADING;
  const uintptr_t arena_before = g_arena_at;
  const size_t arena_left_before = g_arena_left;
  const int load_rc = so_load(&entry->module, path, (void *)aligned,
                              g_arena_left - padding);
  if (load_rc != 0) {
    set_error("dlopen %s: ELF loader error %d", basename, load_rc);
    cache_failure(entry);
    return NULL;
  }
  const size_t consumed = padding + ALIGN_MEM(entry->module.load_size,
                                               PLUGIN_LOAD_ALIGN);
  if (consumed > g_arena_left) {
    set_error("dlopen %s: aligned image does not fit module arena", basename);
    goto fail;
  }
  g_arena_at += consumed;
  g_arena_left -= consumed;

  for (size_t i = 0;; ++i) {
    const char *needed = NULL;
    const int needed_rc = so_get_needed(&entry->module, i, &needed);
    if (needed_rc == 0) break;
    if (needed_rc < 0 || !needed || !valid_basename(needed)) {
      set_error("dlopen %s: invalid DT_NEEDED entry", basename);
      goto fail;
    }
    if (is_host_library(needed)) continue;
    so_module *dep = so_find_module(needed);
    if (!dep) dep = load_bundled_locked(needed, 1);
    if (!dep) {
      char nested[sizeof(g_error)];
      snprintf(nested, sizeof(nested), "%s", g_error_pending ? g_error : "unknown error");
      set_error("dlopen %s: dependency %s failed: %s", basename, needed, nested);
      goto fail;
    }
    if (add_dependency(entry, dep) != 0) {
      set_error("dlopen %s: too many DT_NEEDED dependencies", basename);
      goto fail;
    }
  }

  char preflight[192];
  if (so_preflight_imports(&entry->module, resolve_import, entry,
                           preflight, sizeof(preflight)) != 0) {
    set_error("dlopen %s: %s", basename,
              preflight[0] ? preflight : "import preflight failed");
    goto fail;
  }

  /* Relatives and definitions are image-local.  Undefined imports use the
   * identical callback and opaque scope in both preflight and mutation. */
  so_relocate(&entry->module);
  if (so_resolve_imports(&entry->module, resolve_import, entry,
                         preflight, sizeof(preflight)) != 0) {
    set_error("dlopen %s: %s", basename,
              preflight[0] ? preflight : "import relocation failed");
    goto fail;
  }
  entry->state = PLUGIN_INITIALIZING;
  so_finalize(&entry->module);
  so_flush_caches(&entry->module);
  so_execute_init_array(&entry->module);
  so_free_temp(&entry->module);
  entry->state = PLUGIN_READY;
  entry->refs = dependency ? 0 : 1;
  return &entry->module;

fail: {
    char failure[sizeof(entry->failure)];
    copy_string(failure, sizeof(failure),
                g_error_pending ? g_error : "unknown loader error");
    so_discard_unfinalized(&entry->module);
    /* Reclaim the raw arena slice when no recursively loaded dependency was
     * placed after it.  Successful dependencies stay resident and prevent a
     * rewind across their images. */
    if (g_arena_at == arena_before + consumed) {
      g_arena_at = arena_before;
      g_arena_left = arena_left_before;
    }
    copy_string(entry->failure, sizeof(entry->failure), failure);
    entry->state = PLUGIN_FAILED;
    return NULL;
  }
}

int plugin_loader_jni_onload(void *handle, void *java_vm) {
  typedef int (*JniOnLoadFn)(void *, void *);

  if (!handle || !java_vm) {
    set_error("JNI_OnLoad: invalid module handle or JavaVM");
    return -1;
  }

  loader_lock();
  if (!so_is_loaded_module((so_module *)handle)) {
    set_error("JNI_OnLoad: invalid module handle %p", handle);
    loader_unlock();
    return -1;
  }

  /* libyuanshen.so is initialized explicitly by main.c. */
  PluginEntry *entry = entry_for_module((so_module *)handle);
  if (!entry) {
    loader_unlock();
    return 0;
  }
  if (entry->jni_onload_state == 2) {
    const int version = entry->jni_version;
    loader_unlock();
    return version;
  }
  if (entry->jni_onload_state < 0) {
    set_error("JNI_OnLoad %s: previous initialization failed", entry->basename);
    loader_unlock();
    return -1;
  }
  /* A re-entrant load of the same library from JNI_OnLoad is already in the
   * Android runtime's initializing state. */
  if (entry->jni_onload_state == 1) {
    loader_unlock();
    return 0;
  }

  JniOnLoadFn on_load =
    (JniOnLoadFn)so_try_find_addr_rx(&entry->module, "JNI_OnLoad");
  if (!on_load) {
    entry->jni_onload_state = 2;
    entry->jni_version = 0;
    loader_unlock();
    return 0;
  }

  entry->jni_onload_state = 1;
  const int version = on_load(java_vm, NULL);
  if (version != 0x00010001 && version != 0x00010002 &&
      version != 0x00010004 && version != 0x00010006) {
    entry->jni_onload_state = -1;
    set_error("JNI_OnLoad %s returned unsupported version 0x%x",
              entry->basename, (unsigned)version);
    loader_unlock();
    return -1;
  }

  entry->jni_version = version;
  entry->jni_onload_state = 2;
  loader_unlock();
  return version;
}

int plugin_loader_init(void *arena_base, size_t arena_size,
                       const char *data_root) {
  if (!arena_base || !arena_size || !data_root || !*data_root) return -1;
  loader_lock();
  size_t root_len = strlen(data_root);
  if (root_len >= sizeof(g_data_root)) {
    set_error("plugin loader: data root is too long");
    loader_unlock();
    return -1;
  }
  snprintf(g_data_root, sizeof(g_data_root), "%s", data_root);
  while (root_len && g_data_root[root_len - 1] == '/')
    g_data_root[--root_len] = 0;
  if (!root_len) {
    set_error("plugin loader: data root cannot be filesystem root");
    loader_unlock();
    return -1;
  }
  g_arena_at = (uintptr_t)arena_base;
  g_arena_left = arena_size;
  g_main_module = so_find_module("libyuanshen.so");
  if (!g_main_module) {
    set_error("plugin loader: main libyuanshen.so is not loaded");
    loader_unlock();
    return -1;
  }
  g_initialized = 1;
  loader_unlock();
  return 0;
}

void *plugin_loader_dlopen(const char *name, int flags) {
  loader_lock();
  if (!name) {
    loader_unlock();
    return &g_default_handle_token;
  }
  const char *basename = canonical_cri_library(path_basename(name));
  if (!valid_basename(basename)) {
    set_error("dlopen: unsafe or unsupported library name '%s'", name);
    loader_unlock();
    return NULL;
  }
  /* Unity also enumerates packaged native plug-ins and calls dlopen directly,
   * independently of Java System.load, so the Java-side MTR no-op is not
   * enough: this path would still map MiHoYoMTRSDK, whose OpenSSL constructor
   * traps on CNTVCT_EL0 at RVA 0x16e968.  Return a unique successful inert
   * handle instead; Unity's plug-in scanner only probes the optional
   * UnityPlugin* exports and safely accepts null dlsym results. */
  if (!strcmp(basename, "libMiHoYoMTRSDK.so")) {
    loader_unlock();
    return &g_optional_handle_token;
  }
  if (is_host_library(basename)) {
    loader_unlock();
    return &g_host_handle_token;
  }

  /* Genshin merges these usual Unity libraries into libyuanshen.so. */
  if (!strcmp(basename, "libunity.so") || !strcmp(basename, "libmain.so") ||
      !strcmp(basename, "libil2cpp.so"))
    basename = "libyuanshen.so";

  so_module *module = so_find_module(basename);
  if (module) {
    PluginEntry *entry = entry_for_module(module);
    if (entry) ++entry->refs;
    loader_unlock();
    return module;
  }
  if (flags & BIONIC_RTLD_NOLOAD) {
    set_error("dlopen %s: RTLD_NOLOAD module is not loaded", basename);
    loader_unlock();
    return NULL;
  }
  if (!g_initialized) {
    set_error("dlopen %s: plugin module arena is not initialized", basename);
    loader_unlock();
    return NULL;
  }
  module = load_bundled_locked(basename, 0);
  loader_unlock();
  return module;
}

int plugin_loader_dlclose(void *handle) {
  loader_lock();
  if (!handle || handle == &g_host_handle_token ||
      handle == &g_optional_handle_token ||
      handle == &g_default_handle_token || (uintptr_t)handle == UINTPTR_MAX) {
    loader_unlock();
    return 0;
  }
  if (!so_is_loaded_module((so_module *)handle)) {
    set_error("dlclose: invalid module handle %p", handle);
    loader_unlock();
    return -1;
  }
  PluginEntry *entry = entry_for_module((so_module *)handle);
  if (entry && entry->refs) --entry->refs;
  /* Deliberately resident: guest code frequently retains native callbacks. */
  loader_unlock();
  return 0;
}

const char *plugin_loader_dlerror(void) {
  loader_lock();
  const char *result = NULL;
  if (g_error_pending) {
    snprintf(g_return_error, sizeof(g_return_error), "%s", g_error);
    g_error_pending = 0;
    result = g_return_error;
  }
  loader_unlock();
  return result;
}

static void *search_module_scope(so_module *module, const char *symbol,
                                 so_module **visited, size_t *visited_count) {
  for (size_t i = 0; i < *visited_count; ++i)
    if (visited[i] == module) return NULL;
  if (*visited_count < PLUGIN_MAX_MODULES + 1)
    visited[(*visited_count)++] = module;
  else
    return NULL;
  void *result = so_resolve_in_module(module, symbol);
  if (result) return result;
  PluginEntry *entry = entry_for_module(module);
  if (!entry) return NULL;
  for (size_t i = 0; i < entry->dep_count; ++i) {
    result = search_module_scope(entry->deps[i], symbol, visited, visited_count);
    if (result) return result;
  }
  return NULL;
}

void *plugin_loader_dlsym(void *handle, const char *symbol) {
  loader_lock();
  if (!symbol || !*symbol) {
    set_error("dlsym: empty symbol name");
    loader_unlock();
    return NULL;
  }

  void *result = NULL;
  if (!handle || handle == &g_default_handle_token ||
      (uintptr_t)handle == UINTPTR_MAX) {
    result = so_resolve_external(symbol);
    if (!result) result = resolve_host_symbol(symbol);
  } else if (handle == &g_host_handle_token) {
    result = resolve_host_symbol(symbol);
  } else if (handle == &g_optional_handle_token) {
    /* Deliberately no exports: this represents a successfully skipped
     * optional Android telemetry plug-in, not the host symbol namespace. */
    result = NULL;
  } else if (so_is_loaded_module((so_module *)handle)) {
    so_module *visited[PLUGIN_MAX_MODULES + 1];
    size_t visited_count = 0;
    result = search_module_scope((so_module *)handle, symbol,
                                 visited, &visited_count);
    if (!result) result = resolve_host_symbol(symbol);
  } else {
    set_error("dlsym: invalid module handle %p", handle);
    loader_unlock();
    return NULL;
  }

  if (!result) set_error("dlsym %s: symbol not found", symbol);
  loader_unlock();
  return result;
}
