/* Runtime loader for optional Android shared libraries bundled with Genshin. */

#ifndef GENSHIN_PLUGIN_LOADER_H
#define GENSHIN_PLUGIN_LOADER_H

#include <stddef.h>

/* Call once after libyuanshen.so has consumed the front of the module arena.
 * `arena_base`/`arena_size` are the remaining, 0x4000-aligned module zone and
 * `data_root` is the staged game root containing lib/arm64-v8a. */
int plugin_loader_init(void *arena_base, size_t arena_size,
                       const char *data_root);

void *plugin_loader_dlopen(const char *name, int flags);
int plugin_loader_dlclose(void *handle);
const char *plugin_loader_dlerror(void);
void *plugin_loader_dlsym(void *handle, const char *symbol);

/* Android's System.load/System.loadLibrary invokes JNI_OnLoad after dlopen.
 * Keep that VM-specific step separate so ordinary native dlopen retains its
 * normal semantics.  Returns the negotiated JNI version, zero when the module
 * has no JNI_OnLoad (or is the already initialized main image), and -1 on
 * failure. */
int plugin_loader_jni_onload(void *handle, void *java_vm);

#endif
