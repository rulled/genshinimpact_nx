/* Minimal JNI environment for Unity and IL2CPP.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>
#include <string.h>
#include <strings.h>
#include <limits.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <switch.h>

#include "config.h"
#include "android_identity.h"
#include "jni_fake.h"
#include "android_native_unity.h"
#include "imports.h"
#include "libc_shim.h"
#include "plugin_loader.h"
#include "unity_jni.h"
#include "unity_input.h"

#define JNI_OK 0
#define JNI_ERR (-1)
#define JNI_EDETACHED (-2)
#define JNI_EVERSION (-3)
#define JNI_VERSION_1_6 0x00010006

typedef uint64_t juint;

enum {
  TAG_OBJECT = 0x4f424a31, // 'OBJ1'  heap object (freeable)
  TAG_INTERN = 0x4f424a32, // 'OBJ2'  pooled object (never freed)
  TAG_STRING = 0x53545231, // 'STR1'
  TAG_OBJARR = 0x4f415231, // 'OAR1'
  TAG_PRIARR = 0x50415231, // 'PAR1'
  TAG_DIRECT = 0x44425231, // 'DBR1'
  TAG_SIGNATURE = 0x53494731, // 'SIG1'
  TAG_SIG_INTERN = 0x53494732, // 'SIG2' exact APK signer, never freed
  TAG_PROXY   = 0x50525831, // 'PRX1' bitter.jnibridge interface proxy
  TAG_HANDLER = 0x484e4431, // 'HND1' android.os.Handler with callback
  TAG_MESSAGE = 0x4d534731, // 'MSG1' android.os.Message with target
  TAG_ID     = 0x4d494431, // 'MID1'  pooled, never freed
  TAG_CLASS  = 0x434c5331, // 'CLS1'  pooled, never freed
};

/* Every reclaimable fake Java value begins with this header.  Pooled values
 * use the same concrete types but are recognized by tag/address and never
 * participate in reference counting. */
typedef struct { uint32_t tag, refs; } FakeRefHeader;
typedef struct { uint32_t tag, refs; char label[192]; } FakeObject;
typedef struct { uint32_t tag, refs; char *utf; } FakeString;
typedef struct { uint32_t tag, refs; int len; void **items; } FakeObjArray;
typedef struct { uint32_t tag, refs; int len; int elem_size; void *data; } FakePriArray;
typedef struct { uint32_t tag, refs; void *address; int64_t capacity; } FakeDirectBuffer;
typedef struct { uint32_t tag, refs; size_t len; uint8_t *data; } FakeSignature;
#define MAX_PROXY_INTERFACES 4
typedef struct {
  uint32_t tag, refs;
  char label[192];
  int64_t native_handle;
  void *native_invoke;
  int interface_count;
  char interfaces[MAX_PROXY_INTERFACES][128];
} FakeInterfaceProxy;
typedef struct {
  uint32_t tag, refs;
  char label[192];
  void *callback;
} FakeHandler;
typedef struct {
  uint32_t tag, refs;
  char label[192];
  void *target;
  int what;
  int delivered;
} FakeMessage;
typedef struct { uint32_t tag; char cls[256]; char name[128]; char sig[512]; } FakeID;
typedef struct { uint32_t tag; char name[256]; } FakeClass;

_Static_assert(offsetof(FakeObject, refs) == 4, "fake object ref header");
_Static_assert(offsetof(FakeString, refs) == 4, "fake string ref header");
_Static_assert(offsetof(FakeObjArray, refs) == 4, "fake object-array ref header");
_Static_assert(offsetof(FakePriArray, refs) == 4, "fake primitive-array ref header");
_Static_assert(offsetof(FakeDirectBuffer, refs) == 4, "fake direct-buffer ref header");
_Static_assert(offsetof(FakeSignature, refs) == 4, "fake signature ref header");
_Static_assert(offsetof(FakeInterfaceProxy, refs) == 4, "fake proxy ref header");
_Static_assert(offsetof(FakeHandler, refs) == 4, "fake handler ref header");
_Static_assert(offsetof(FakeMessage, refs) == 4, "fake message ref header");

static FakeSignature apk_signer_signature;

volatile int jni_quit_requested = 0;

/* Local references and frames. */
#define MAX_LOCALS 262144
#define MAX_FRAMES 128
typedef struct {
  void **refs;
  int top, capacity;
  int frames[MAX_FRAMES];
  int frame_top;
} LocalState;
static _Thread_local LocalState local_state;
static Mutex locals_lock;
static _Thread_local void *pending_exception;
static _Thread_local int vm_attached;
/* ART's global-reference table grows as required.  A fixed 4096-entry ceiling
 * is much too small for this client: the Combo SDK is first constructed only
 * after Unity's renderer and managed startup have retained their Java peers.
 * Keep a bounded host allocation, but put the ceiling well beyond the
 * AndroidJavaObject population observed during startup. */
#define MAX_GLOBAL_REFS 65536
typedef struct { void *obj; uint32_t count; } GlobalRef;
static GlobalRef global_refs[MAX_GLOBAL_REFS];
static int global_ref_count;

/* Android's BaseDexClassLoader.findLibrary receives an unadorned soname and
 * returns the absolute nativeLibraryDir path.  Resolve safe basenames that are
 * actually staged (plus Unity's three aliases, which the native loader maps
 * onto the already-loaded libyuanshen image).  This client also probes absent
 * optional modules such as HoYoChannel during its boot-critical managed SDK
 * construction.  Those probes must receive an empty String; returning Java
 * null prevents the constructor from completing.  Return the empty sentinel
 * only for a missing safe basename and a real path for staged libraries.  Keep this lookup free
 * of extra policy comparisons; it sits on the timing-sensitive first/second
 * render SDK boundary.  Unsupported GME voice libraries are suppressed later,
 * at the actual System.load boundary. */
static void *classloader_find_library(void *name_object) {
  const char *requested = jni_string_utf(name_object);
  if (!requested || !requested[0]) return NULL;
  for (const unsigned char *p = (const unsigned char *)requested; *p; ++p) {
    if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
          (*p >= '0' && *p <= '9') || *p == '_' || *p == '-' || *p == '.' ||
          *p == '+'))
      return NULL;
  }

  char basename[192];
  const size_t requested_length = strlen(requested);
  const int already_decorated = requested_length >= 6 &&
    !strncmp(requested, "lib", 3) &&
    !strcmp(requested + requested_length - 3, ".so");
  const int basename_length = already_decorated
    ? snprintf(basename, sizeof basename, "%s", requested)
    : snprintf(basename, sizeof basename, "lib%s.so", requested);
  if (basename_length <= 0 || (size_t)basename_length >= sizeof basename)
    return NULL;

  char path[512];
  const int path_length = snprintf(path, sizeof path,
    "%s/lib/arm64-v8a/%s", GAME_HOME, basename);
  if (path_length <= 0 || (size_t)path_length >= sizeof path) return NULL;
  struct stat status;
  const int alias = !strcmp(basename, "libunity.so") ||
                    !strcmp(basename, "libmain.so") ||
                    !strcmp(basename, "libil2cpp.so");
  const int found = alias ||
    (stat(path, &status) == 0 && S_ISREG(status.st_mode));

  return jni_make_local_string(found ? managed_path(path) : "");
}

static int retain_ref(void *ref);
static void free_ref(void *ref);
static void *object_class_raw(void *obj);

static void *reg_local(void *ref) {
  if (ref) {
    LocalState *s=&local_state;
    if (s->top == s->capacity && s->capacity < MAX_LOCALS) {
      int next=s->capacity ? s->capacity*2 : 256; if(next>MAX_LOCALS)next=MAX_LOCALS;
      void **p=realloc(s->refs,(size_t)next*sizeof(*p)); if(p){s->refs=p;s->capacity=next;}
    }
    if (s->top < s->capacity) s->refs[s->top++] = ref;
    else { free_ref(ref); return NULL; }
  }
  return ref;
}

void *jni_track_local(void *ref) { return reg_local(ref); }

/* Intern constant strings outside the local-reference table. */
#define MAX_ISTR 4096
static FakeString istr_pool[MAX_ISTR];
static int istr_count = 0;

static void erase_bytes(void *memory, size_t size) {
  volatile unsigned char *p = memory;
  while (p && size--) *p++ = 0;
}

static int address_in_range(const void *pointer, const void *begin,
                            size_t bytes) {
  const uintptr_t address = (uintptr_t)pointer;
  const uintptr_t first = (uintptr_t)begin;
  return address >= first && address - first < bytes;
}

static int is_interned_string(const void *ref) {
  return address_in_range(ref, istr_pool, sizeof(istr_pool));
}

static int managed_ref_tag(uint32_t tag) {
  return tag == TAG_OBJECT || tag == TAG_STRING || tag == TAG_OBJARR ||
         tag == TAG_PRIARR || tag == TAG_DIRECT || tag == TAG_SIGNATURE ||
         tag == TAG_PROXY || tag == TAG_HANDLER || tag == TAG_MESSAGE;
}

static int retain_managed_ref(FakeRefHeader *header) {
  uint32_t refs = __atomic_load_n(&header->refs, __ATOMIC_ACQUIRE);
  while (refs && refs < UINT32_MAX - 1) {
    if (__atomic_compare_exchange_n(&header->refs, &refs, refs + 1, 0,
                                    __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
      return 1;
  }
  return 0;
}

static int release_managed_ref(FakeRefHeader *header) {
  uint32_t refs = __atomic_load_n(&header->refs, __ATOMIC_ACQUIRE);
  while (refs) {
    if (__atomic_compare_exchange_n(&header->refs, &refs, refs - 1, 0,
                                    __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
      return refs == 1;
  }
  return 0;
}

/* Return one when the reference is safe to expose through another JNI handle.
 * Reclaimable values gain a strong owner; interned/static/external values are
 * process-owned and therefore require no counter update. */
static int retain_ref(void *ref) {
  if (!ref) return 1;
  if (is_interned_string(ref))
    return 1;
  const uint32_t tag = *(const uint32_t *)ref;
  if (managed_ref_tag(tag)) return retain_managed_ref((FakeRefHeader *)ref);
  if (unity_is_handle(ref)) return unity_retain_handle(ref);
  /* Classes, method IDs, interned objects, the static APK signer, and input
   * event storage are stable outside the local table. */
  return 1;
}

static void free_ref(void *ref) {
  if (!ref)
    return;
  if (is_interned_string(ref))
    return;  // interned string -- pooled, never freed
  const uint32_t tag = *(uint32_t *)ref;
  if (managed_ref_tag(tag) && !release_managed_ref((FakeRefHeader *)ref))
    return;
  switch (tag) {
    case TAG_STRING: {
      FakeString *s = ref;
      if (s->utf) erase_bytes(s->utf, strlen(s->utf));
      free(s->utf);
      erase_bytes(s, sizeof(*s));
      free(s);
      break;
    }
    case TAG_PRIARR: {
      FakePriArray *a = ref;
      if (a->data && a->len > 0 && a->elem_size > 0 &&
          (size_t)a->len <= SIZE_MAX / (size_t)a->elem_size)
        erase_bytes(a->data, (size_t)a->len * (size_t)a->elem_size);
      free(a->data); free(a); break;
    }
    case TAG_OBJARR: {
      FakeObjArray *a = ref;
      for (int i = 0; i < a->len; ++i) free_ref(a->items[i]);
      free(a->items); free(a); break;
    }
    case TAG_DIRECT: free(ref); break;
    case TAG_SIGNATURE: {
      FakeSignature *s = ref;
      erase_bytes(s->data, s->len);
      free(s->data);
      free(s);
      break;
    }
    case TAG_PROXY: free(ref); break;
    case TAG_HANDLER: {
      FakeHandler *handler = ref;
      void *callback = handler->callback;
      handler->callback = NULL;
      free(handler);
      free_ref(callback);
      break;
    }
    case TAG_MESSAGE: {
      FakeMessage *message = ref;
      void *target = message->target;
      message->target = NULL;
      free(message);
      free_ref(target);
      break;
    }
    case TAG_OBJECT: free(ref); break;
    default:
      /* Unity's stateful handles have their own tag and deep destructor. */
      (void)unity_release_handle(ref);
      break; // TAG_INTERN / TAG_ID / TAG_CLASS are pooled
  }
}

/* The VM keeps a thrown object alive independently of the local that was
 * passed to Throw.  Replacing or clearing the exception drops that root. */
static int set_pending_exception(void *exception) {
  if (pending_exception == exception) return 1;
  if (!retain_ref(exception)) return 0;
  void *previous = pending_exception;
  pending_exception = exception;
  free_ref(previous);
  return 1;
}

static int set_pending_message(const char *message) {
  void *exception = jni_make_string(message ? message : "Java exception");
  return exception && set_pending_exception(exception);
}

static void delete_local(void *ref) {
  if (!ref)
    return;
  LocalState *s=&local_state;
  for (int i = s->top - 1; i >= 0; i--) {
    if (s->refs[i] == ref) {
      if (i + 1 < s->top)
        memmove(&s->refs[i], &s->refs[i + 1],
                (size_t)(s->top - i - 1) * sizeof(*s->refs));
      --s->top;
      for (int frame = 0; frame < s->frame_top; ++frame)
        if (s->frames[frame] > i) --s->frames[frame];
      free_ref(ref);
      break;
    }
  }
}

/* Intern stateless object handles by label. */
#define MAX_IOBJ 2048
static FakeObject iobj_pool[MAX_IOBJ];
static int iobj_count = 0;
void *jni_make_object(const char *label) {
  const char *l = (label && label[0]) ? label : "obj";
  mutexLock(&locals_lock);
  void *r = NULL;
  for (int i = 0; i < iobj_count; i++)
    if (!strcmp(iobj_pool[i].label, l)) { r = &iobj_pool[i]; break; }
  if (!r) {
    if (iobj_count >= MAX_IOBJ) {
      FakeObject *o = calloc(1,sizeof(*o));
      if (o) { o->tag=TAG_INTERN; snprintf(o->label,sizeof(o->label),"%s",l); }
      r=o;
    } else {
      FakeObject *o = &iobj_pool[iobj_count++];
      o->tag = TAG_INTERN;
      strncpy(o->label, l, sizeof(o->label) - 1);
      o->label[sizeof(o->label) - 1] = '\0';
      r = o;
    }
  }
  mutexUnlock(&locals_lock);
  return r;
}

void *jni_make_string(const char *utf) {
  const char *u = utf ? utf : "";
  mutexLock(&locals_lock);
  for (int i = 0; i < istr_count; i++)            // repeats reuse the pooled string
    if (!strcmp(istr_pool[i].utf, u)) { void *r = &istr_pool[i]; mutexUnlock(&locals_lock); return r; }
  if (istr_count < MAX_ISTR) {
    char *copy = strdup(u);
    if (!copy) { mutexUnlock(&locals_lock); return NULL; }
    FakeString *s = &istr_pool[istr_count++];
    s->tag = TAG_STRING;
    s->refs = UINT32_MAX;
    s->utf = copy;
    mutexUnlock(&locals_lock);
    return s;                                      // pooled, not reg_local'd
  }
  mutexUnlock(&locals_lock);
  FakeString *s = calloc(1, sizeof(*s));           // pool full: one-off local string
  if (!s) return NULL;
  s->tag = TAG_STRING;
  s->refs = 1;
  s->utf = strdup(u);
  if (!s->utf) { free(s); return NULL; }
  return reg_local(s);
}

void *jni_make_local_string(const char *utf) {
  FakeString *s = calloc(1, sizeof(*s));
  if (!s) return NULL;
  s->tag = TAG_STRING;
  s->refs = 1;
  s->utf = strdup(utf ? utf : "");
  if (!s->utf) { free(s); return NULL; }
  return reg_local(s);
}

static void *make_pri_array_adopt(void *data, int len, int elem_size) {
  FakePriArray *a = calloc(1, sizeof(*a));
  if (!a) { free(data); return NULL; }
  a->tag = TAG_PRIARR;
  a->refs = 1;
  a->len = len;
  a->elem_size = elem_size;
  a->data = data;
  return reg_local(a);
}

static void *make_signature_copy(const void *data, size_t size) {
  if ((!data && size) || size > INT32_MAX) return NULL;
  FakeSignature *signature = calloc(1, sizeof(*signature));
  if (!signature) return NULL;
  signature->data = malloc(size ? size : 1);
  if (!signature->data) { free(signature); return NULL; }
  if (size) memcpy(signature->data, data, size);
  signature->tag = TAG_SIGNATURE;
  signature->refs = 1;
  signature->len = size;
  return reg_local(signature);
}

static void *make_apk_signature(void) {
  return apk_signer_signature.tag == TAG_SIG_INTERN
    ? &apk_signer_signature : NULL;
}

static void init_apk_signature(void) {
  size_t size = 0;
  const uint8_t *certificate = android_identity_signer(&size);
  memset(&apk_signer_signature, 0, sizeof(apk_signer_signature));
  if (!certificate) return;
  apk_signer_signature.tag = TAG_SIG_INTERN;
  apk_signer_signature.len = size;
  apk_signer_signature.data = (uint8_t *)certificate;
}

static void *make_apk_signature_array(void) {
  void *signature = make_apk_signature();
  if (!signature) return jni_make_object_array(0, NULL);
  void *items[] = { signature };
  return jni_make_object_array(1, items);
}

static const char *obj_str(void *jstr) {
  FakeString *s = jstr;
  if (s && s->tag == TAG_STRING)
    return s->utf;
  return "";
}

/* Internal strings use ordinary UTF-8. JNI exposes UTF-16 and modified UTF-8. */
static const unsigned char *utf8_next(const unsigned char *p, uint32_t *cp_out) {
  uint32_t cp;
  unsigned need;
  const unsigned char c = *p;
  if (!c) { *cp_out = 0; return p; }
  if (c < 0x80) { *cp_out = c; return p + 1; }
  if ((c & 0xe0) == 0xc0) { cp = c & 0x1f; need = 1; }
  else if ((c & 0xf0) == 0xe0) { cp = c & 0x0f; need = 2; }
  else if ((c & 0xf8) == 0xf0) { cp = c & 0x07; need = 3; }
  else { *cp_out = 0xfffd; return p + 1; }
  for (unsigned i = 0; i < need; ++i) {
    const unsigned char x = p[i + 1];
    if (!x || (x & 0xc0) != 0x80) { *cp_out = 0xfffd; return p + 1; }
    cp = (cp << 6) | (x & 0x3f);
  }
  if (cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff)) cp = 0xfffd;
  *cp_out = cp;
  return p + need + 1;
}

static juint utf16_len(const char *str) {
  const unsigned char *p = (const unsigned char *)(str ? str : "");
  juint n = 0;
  while (*p) { uint32_t cp; p = utf8_next(p, &cp); n += cp >= 0x10000 ? 2u : 1u; }
  return n;
}

static juint jni_utf8_to_utf16(const char *str, uint16_t *out) {
  const unsigned char *p = (const unsigned char *)(str ? str : "");
  juint n = 0;
  while (*p) {
    uint32_t cp; p = utf8_next(p, &cp);
    if (cp < 0x10000) {
      if (out) out[n] = (uint16_t)cp;
      ++n;
    } else {
      cp -= 0x10000;
      if (out) { out[n] = (uint16_t)(0xd800 + (cp >> 10)); out[n + 1] = (uint16_t)(0xdc00 + (cp & 0x3ff)); }
      n += 2;
    }
  }
  return n;
}

static size_t mutf8_unit_size(uint16_t u) {
  if (u == 0) return 2;
  if (u < 0x80) return 1;
  if (u < 0x800) return 2;
  return 3;
}

static char *utf16_to_mutf8(const uint16_t *u, size_t count, size_t *len_out) {
  size_t n = 0;
  for (size_t i = 0; i < count; ++i) n += mutf8_unit_size(u[i]);
  char *out = malloc(n + 1);
  if (!out) { if (len_out) *len_out = 0; return NULL; }
  size_t o = 0;
  for (size_t i = 0; i < count; ++i) {
    uint16_t c = u[i];
    if (c && c < 0x80) out[o++] = (char)c;
    else if (c < 0x800) { out[o++] = (char)(0xc0 | (c >> 6)); out[o++] = (char)(0x80 | (c & 0x3f)); }
    else { out[o++] = (char)(0xe0 | (c >> 12)); out[o++] = (char)(0x80 | ((c >> 6) & 0x3f)); out[o++] = (char)(0x80 | (c & 0x3f)); }
  }
  out[o] = '\0';
  if (len_out) *len_out = o;
  return out;
}

#define MAX_CLASSES 2048
static FakeClass class_pool[MAX_CLASSES];
static int class_count = 0;

static void *intern_class(const char *name) {
  mutexLock(&locals_lock);
  for (int i = 0; i < class_count; i++)
    if (!strcmp(class_pool[i].name, name))
      { void *r=&class_pool[i]; mutexUnlock(&locals_lock); return r; }
  if (class_count >= MAX_CLASSES) {
    FakeClass *c=calloc(1,sizeof(*c)); if(!c){mutexUnlock(&locals_lock);return NULL;}
    c->tag=TAG_CLASS; snprintf(c->name,sizeof(c->name),"%s",name); mutexUnlock(&locals_lock); return c;
  }
  FakeClass *c = &class_pool[class_count++];
  c->tag = TAG_CLASS;
  strncpy(c->name, name, sizeof(c->name) - 1);
  c->name[sizeof(c->name) - 1] = '\0';
  mutexUnlock(&locals_lock);
  return c;
}

static const char *class_name_of(void *cls) {
  FakeClass *c = cls;
  return (c && c->tag == TAG_CLASS) ? c->name : "";
}

static FakeObject *g_asset_mgr = NULL;      // android.content.res.AssetManager

static void *get_asset_manager_obj(void) {
  if (!g_asset_mgr) {
    g_asset_mgr = calloc(1, sizeof(*g_asset_mgr));
    g_asset_mgr->tag = TAG_INTERN;
    strcpy(g_asset_mgr->label, "android/content/res/AssetManager");
  }
  return g_asset_mgr;
}

/* Cached ClassLoader handle. */
static FakeObject *g_classloader = NULL;
static void *get_classloader_obj(void) {
  if (!g_classloader) {
    g_classloader = calloc(1, sizeof(*g_classloader));
    g_classloader->tag = TAG_INTERN;
    strcpy(g_classloader->label, "java/lang/ClassLoader");
  }
  return g_classloader;
}

#define MAX_IDS 8192
static FakeID id_pool[MAX_IDS];
static int id_count = 0;

static FakeID *get_id(const char *cls, const char *name, const char *sig) {
  mutexLock(&locals_lock);
  for (int i = 0; i < id_count; i++)
    if (!strcmp(id_pool[i].name, name) && !strcmp(id_pool[i].sig, sig) &&
        !strcmp(id_pool[i].cls, cls))
      { FakeID *r=&id_pool[i]; mutexUnlock(&locals_lock); return r; }
  if (id_count >= MAX_IDS) {
    FakeID *id=calloc(1,sizeof(*id)); if(!id){mutexUnlock(&locals_lock);return NULL;}
    id->tag=TAG_ID; snprintf(id->cls,sizeof(id->cls),"%s",cls?cls:"");
    snprintf(id->name,sizeof(id->name),"%s",name?name:""); snprintf(id->sig,sizeof(id->sig),"%s",sig?sig:"");
    mutexUnlock(&locals_lock); return id;
  }
  FakeID *id = &id_pool[id_count++];
  id->tag = TAG_ID;
  strncpy(id->cls, cls ? cls : "", sizeof(id->cls) - 1);
  strncpy(id->name, name, sizeof(id->name) - 1);
  strncpy(id->sig, sig, sizeof(id->sig) - 1);
  id->cls[sizeof(id->cls) - 1] = '\0';
  id->name[sizeof(id->name) - 1] = '\0';
  id->sig[sizeof(id->sig) - 1] = '\0';
  mutexUnlock(&locals_lock);
  return id;
}

static int sig_returns(const char *sig, const char *ret) {
  const char *rp = strchr(sig, ')');
  return rp && strstr(rp + 1, ret) == rp + 1;
}

static int name_has(const char *name, const char *sub) { return strstr(name, sub) != NULL; }

static const char *first_string_arg(const char *sig, va_list va);

static const char *lang_code(void) {
  if (config.language == LANG_JA) return "ja";
  if (config.language == LANG_EN) return "en";
  /* Auto selects Japanese or English from the system locale. */
  static int ja = -1;
  if (ja < 0) {
    ja = 0;
    u64 code; SetLanguage sl;
    if (R_SUCCEEDED(setInitialize())) {
      if (R_SUCCEEDED(setGetSystemLanguage(&code)) && R_SUCCEEDED(setMakeLanguage(code, &sl)))
        ja = (sl == SetLanguage_JA);
      setExit();
    }
  }
  return ja ? "ja" : "en";
}

/* Read the first String argument from a JNI vararg list. */
static const char *first_string_arg(const char *sig, va_list va) {
  const char *p = sig ? strchr(sig, '(') : NULL;
  if (!p) return "";
  for (p++; *p && *p != ')'; p++) {
    switch (*p) {
      case 'I': case 'Z': case 'B': case 'C': case 'S': (void)va_arg(va, int); break;
      case 'F': case 'D': (void)va_arg(va, double); break;
      case 'J': (void)va_arg(va, long long); break;
      case '[':
        (void)va_arg(va, void *);
        if (p[1] == 'L') { p++; while (*p && *p != ';') p++; } else if (p[1]) p++;
        break;
      case 'L': {
        const char *s = obj_str(va_arg(va, void *));
        while (*p && *p != ';') p++;
        if (s && s[0]) return s;
        break;
      }
      default: break;
    }
  }
  return "";
}

const char *jni_string_utf(void *jstr);

/* Native audio properties consumed by CRIWARE. */
static int g_last_output_prop = 0;

static void *getproperty_value(const char *key) {
  int which = 0;
  if (key && strstr(key, "SAMPLE_RATE"))            which = 1;
  else if (key && strstr(key, "FRAMES_PER_BUFFER")) which = 2;
  else                                              which = g_last_output_prop;
  if (which == 1) return jni_make_string("48000");
  /* Match the exact SDL callback period requested by the OpenSL backend. */
  if (which == 2) return jni_make_string("1024");
  return jni_make_string("");
}

static void *system_property_value(const char *key) {
  if (!key) return jni_make_string("");
  if (!strcmp(key,"os.name")) return jni_make_string("Linux");
  if (!strcmp(key,"os.arch")) return jni_make_string("aarch64");
  if (!strcmp(key,"os.version")) return jni_make_string("5.10");
  if (!strcmp(key,"java.vm.name")) return jni_make_string("Dalvik");
  if (!strcmp(key,"java.vm.version")) return jni_make_string("2.1.0");
  if (!strcmp(key,"user.language")) return jni_make_string(lang_code());
  if (!strcmp(key,"file.separator")) return jni_make_string("/");
  if (!strcmp(key,"line.separator")) return jni_make_string("\n");
  if (!strcmp(key,"path.separator")) return jni_make_string(":");
  return jni_make_string("");
}

static void *signature_byte_array(const FakeSignature *signature) {
  if (!signature ||
      (signature->tag != TAG_SIGNATURE && signature->tag != TAG_SIG_INTERN) ||
      signature->len > INT32_MAX)
    return NULL;
  uint8_t *copy = malloc(signature->len ? signature->len : 1);
  if (!copy) return NULL;
  if (signature->len) memcpy(copy, signature->data, signature->len);
  return make_pri_array_adopt(copy, (int)signature->len, 1);
}

static void *signature_chars(const FakeSignature *signature, int as_string) {
  static const char digits[] = "0123456789abcdef";
  if (!signature ||
      (signature->tag != TAG_SIGNATURE && signature->tag != TAG_SIG_INTERN) ||
      signature->len > (SIZE_MAX - 1) / 2 || signature->len > INT32_MAX / 2)
    return NULL;
  const size_t length = signature->len * 2;
  if (as_string) {
    char *chars = malloc(length + 1);
    if (!chars) return NULL;
    for (size_t i = 0; i < signature->len; ++i) {
      chars[i * 2] = digits[signature->data[i] >> 4];
      chars[i * 2 + 1] = digits[signature->data[i] & 15];
    }
    chars[length] = '\0';
    void *result = jni_make_local_string(chars);
    free(chars);
    return result;
  }

  uint16_t *chars = malloc((length ? length : 1) * sizeof(*chars));
  if (!chars) return NULL;
  for (size_t i = 0; i < signature->len; ++i) {
    chars[i * 2] = (uint16_t)digits[signature->data[i] >> 4];
    chars[i * 2 + 1] = (uint16_t)digits[signature->data[i] & 15];
  }
  return make_pri_array_adopt(chars, (int)length, (int)sizeof(*chars));
}

/* Unity 2017 does not ask JNI for plug-in methods directly.  Its
 * com.unity3d.player.ReflectionHelper Java class returns a reflected
 * Constructor/Method/Field, then the native bridge converts that object with
 * FromReflectedMethod/Field.  There is no ART in this wrapper, so preserve the
 * queried target metadata in the already pooled FakeID representation. */
static FakeID *reflection_member_id(const FakeID *request, va_list va) {
  if (!request ||
      !name_has(request->cls, "com/unity3d/player/ReflectionHelper"))
    return NULL;

  void *target_class_object = va_arg(va, void *);
  const char *target_class = class_name_of(target_class_object);
  if (!strcmp(request->name, "getConstructorID")) {
    const char *signature = jni_string_utf(va_arg(va, void *));
    FakeID *result = get_id(target_class, "<init>", signature);

    return result;
  }
  if (!strcmp(request->name, "getMethodID") ||
      !strcmp(request->name, "getFieldID")) {
    const char *member = jni_string_utf(va_arg(va, void *));
    const char *signature = jni_string_utf(va_arg(va, void *));
    FakeID *result = get_id(target_class, member, signature);

    return result;
  }
  return NULL;
}

#define JNI_BRIDGE_CLASS "bitter/jnibridge/JNIBridge"
#define JNI_BRIDGE_INVOKE_SIGNATURE \
  "(JLjava/lang/Class;Ljava/lang/reflect/Method;[Ljava/lang/Object;)Ljava/lang/Object;"

typedef void *(*JniBridgeInvokeFn)(void *, void *, int64_t, void *, void *,
                                   void *);

static int proxy_supports(const FakeInterfaceProxy *proxy,
                          const char *interface_name) {
  if (!proxy || proxy->tag != TAG_PROXY || !interface_name) return 0;
  for (int i = 0; i < proxy->interface_count; ++i)
    if (!strcmp(proxy->interfaces[i], interface_name)) return 1;
  return 0;
}

static void *make_interface_proxy(int64_t native_handle, void *classes) {
  FakeObjArray *interfaces = classes;
  if (!interfaces || interfaces->tag != TAG_OBJARR || interfaces->len <= 0)
    return NULL;
  FakeInterfaceProxy *proxy = calloc(1, sizeof(*proxy));
  if (!proxy) return NULL;
  proxy->tag = TAG_PROXY;
  proxy->refs = 1;
  proxy->native_handle = native_handle;
  proxy->native_invoke = jni_find_registered_native(
    JNI_BRIDGE_CLASS, "invoke", JNI_BRIDGE_INVOKE_SIGNATURE);

  proxy->interface_count = interfaces->len < MAX_PROXY_INTERFACES
    ? interfaces->len : MAX_PROXY_INTERFACES;
  for (int i = 0; i < proxy->interface_count; ++i) {
    const char *name = class_name_of(interfaces->items[i]);
    const size_t length = strnlen(name, sizeof(proxy->interfaces[i]) - 1);
    memcpy(proxy->interfaces[i], name, length);
    proxy->interfaces[i][length] = '\0';
  }
  snprintf(proxy->label, sizeof(proxy->label), "%s",
           proxy->interfaces[0][0] ? proxy->interfaces[0]
                                   : "java/lang/reflect/Proxy");
  return reg_local(proxy);
}

static void *make_handler(void *callback) {
  FakeHandler *handler = calloc(1, sizeof(*handler));
  if (!handler) return NULL;
  handler->tag = TAG_HANDLER;
  handler->refs = 1;
  snprintf(handler->label, sizeof(handler->label), "android/os/Handler");
  if (callback && !retain_ref(callback)) {
    free(handler);
    return NULL;
  }
  handler->callback = callback;
  return reg_local(handler);
}

static void *make_message(void *target, int what) {
  FakeMessage *message = calloc(1, sizeof(*message));
  if (!message) return NULL;
  message->tag = TAG_MESSAGE;
  message->refs = 1;
  snprintf(message->label, sizeof(message->label), "android/os/Message");
  if (target && !retain_ref(target)) {
    free(message);
    return NULL;
  }
  message->target = target;
  message->what = what;
  return reg_local(message);
}

static int dispatch_handler_message(FakeMessage *message) {
  if (!message || message->tag != TAG_MESSAGE ||
      __atomic_exchange_n(&message->delivered, 1, __ATOMIC_ACQ_REL))
    return 0;
  FakeHandler *handler = message->target;
  if (!handler || handler->tag != TAG_HANDLER) {
    return 0;
  }
  FakeInterfaceProxy *proxy = handler->callback;
  if (!proxy_supports(proxy, "android/os/Handler$Callback")) {
    return 0;
  }
  JniBridgeInvokeFn invoke = (JniBridgeInvokeFn)proxy->native_invoke;
  if (!invoke)
    invoke = (JniBridgeInvokeFn)jni_find_registered_native(
      JNI_BRIDGE_CLASS, "invoke", JNI_BRIDGE_INVOKE_SIGNATURE);
  if (!invoke || jni_push_local_frame(16) != JNI_OK) {
    return 0;
  }
  void *argument = message;
  void *arguments = jni_make_object_array(1, &argument);
  FakeID *method = get_id("android/os/Handler$Callback", "handleMessage",
                          "(Landroid/os/Message;)Z");
  if (!arguments || !method) {
    (void)jni_pop_local_frame(NULL);

    return 0;
  }
  (void)invoke(fake_env, intern_class(JNI_BRIDGE_CLASS),
               proxy->native_handle,
               intern_class("android/os/Handler$Callback"), method,
               arguments);
  const int succeeded = pending_exception == NULL;

  (void)jni_pop_local_frame(NULL);
  return succeeded;
}

typedef struct HandlerDispatchNode {
  FakeMessage *message;
  struct HandlerDispatchNode *next;
} HandlerDispatchNode;

/* Android's HandlerThread owns one Looper and processes its MessageQueue in
 * FIFO order on one persistent thread.  Running one pthread per Message lets
 * Unity's two startup messages enter the same native JNIBridge concurrently,
 * which faults in generated client code.  Keep one detached guest-TLS worker
 * and an explicit FIFO instead. */
static Mutex handler_dispatch_lock;
static CondVar handler_dispatch_cond;
static HandlerDispatchNode *handler_dispatch_head;
static HandlerDispatchNode *handler_dispatch_tail;
static int handler_dispatch_active;

static void *handler_dispatch_thread(void *opaque) {
  (void)opaque;
  /* A real Android HandlerThread is attached to ART.  pthread_create_fake
   * already installed this worker's Bionic TPIDR_EL0 block; publish the fake
   * JNIEnv attachment before entering Unity's registered JNIBridge invoker. */
  vm_attached = 1;
  for (;;) {
    mutexLock(&handler_dispatch_lock);
    while (!handler_dispatch_head)
      condvarWait(&handler_dispatch_cond, &handler_dispatch_lock);
    HandlerDispatchNode *node = handler_dispatch_head;
    handler_dispatch_head = node->next;
    if (!handler_dispatch_head) handler_dispatch_tail = NULL;
    mutexUnlock(&handler_dispatch_lock);
    FakeMessage *message = node->message;
    free(node);

    (void)dispatch_handler_message(message);
    free_ref(message);
  }
  __builtin_unreachable();
}

static int enqueue_handler_message(FakeMessage *message) {
  if (!message || message->tag != TAG_MESSAGE ||
      !retain_ref(message))
    return 0;
  HandlerDispatchNode *node = calloc(1, sizeof(*node));
  if (!node) {
    free_ref(message);
    return 0;
  }
  node->message = message;
  pthread_t worker = (pthread_t)0;
  int started_worker = 0;
  mutexLock(&handler_dispatch_lock);
  if (!handler_dispatch_active) {
    handler_dispatch_active = 1;
    const int created = pthread_create_fake(&worker, NULL,
                                             (void *)handler_dispatch_thread,
                                             NULL);
    if (created != 0) {
      handler_dispatch_active = 0;
      mutexUnlock(&handler_dispatch_lock);
      free(node);
      free_ref(message);

      return 0;
    }
    started_worker = 1;
  }
  if (handler_dispatch_tail) handler_dispatch_tail->next = node;
  else handler_dispatch_head = node;
  handler_dispatch_tail = node;

  condvarWakeAll(&handler_dispatch_cond);
  mutexUnlock(&handler_dispatch_lock);
  if (started_worker) {
    /* The detached reaper owns the persistent HandlerThread allocation when
     * the process exits.  A bookkeeping failure cannot cancel the live queue. */
    (void)pthread_detach_fake(worker);
  }
  return 1;
}

static void *act_object(void *recv, const FakeID *id, va_list va) {
  if (!strcmp(id->name, "getClass")) {
    void *result = object_class_raw(recv);
    if (!result)
      (void)set_pending_message("java.lang.NullPointerException: getClass");
    return result;
  }
  if (name_has(id->cls, "java/util/Locale") &&
      !strcmp(id->name, "getDefault"))
    return jni_make_object("java/util/Locale");
  if (name_has(id->cls, "com/unity3d/player/ReflectionHelper") &&
      (!strcmp(id->name, "getConstructorID") ||
       !strcmp(id->name, "getMethodID") ||
       !strcmp(id->name, "getFieldID")))
    return reflection_member_id(id, va);
  if (name_has(id->cls, JNI_BRIDGE_CLASS) &&
      !strcmp(id->name, "newInterfaceProxy") &&
      !strcmp(id->sig, "(J[Ljava/lang/Class;)Ljava/lang/Object;")) {
    const int64_t native_handle = va_arg(va, int64_t);
    return make_interface_proxy(native_handle, va_arg(va, void *));
  }
  /* Unity's native Android input bootstrap creates a HandlerThread, obtains
   * its Looper, constructs a Handler(Looper, Callback), then posts message 0.
   * There is no ART MessageQueue in this wrapper, but returning NULL here is
   * not a harmless unsupported-API result: AndroidJavaObject subsequently
   * dereferences the Looper/Message and the render thread never returns.
   * Keep the scheduler objects opaque and process-owned.  sendToTarget()
   * queues the retained Message on a guest-TLS worker above, matching the
   * thread boundary of Android's HandlerThread closely enough for Unity's
   * registered JNIBridge callback to run without re-entering nativeRender. */
  if (name_has(id->cls, "android/os/HandlerThread") &&
      !strcmp(id->name, "getLooper") &&
      sig_returns(id->sig, "Landroid/os/Looper;"))
    return jni_make_object("android/os/Looper");
  if (name_has(id->cls, "android/os/Handler") &&
      !strcmp(id->name, "obtainMessage") &&
      sig_returns(id->sig, "Landroid/os/Message;")) {
    const int what = strstr(id->sig, "(I)") ? va_arg(va, int) : 0;
    return make_message(recv, what);
  }
  if (name_has(id->cls, "android/os/Message") &&
      !strcmp(id->name, "obtain") &&
      sig_returns(id->sig, "Landroid/os/Message;"))
    return jni_make_object("android/os/Message");
  if (name_has(id->cls, "android/os/Looper") &&
      !strcmp(id->name, "myQueue") &&
      sig_returns(id->sig, "Landroid/os/MessageQueue;"))
    return jni_make_object("android/os/MessageQueue");
  if (name_has(id->cls, "android/view/Choreographer") &&
      !strcmp(id->name, "getInstance") &&
      sig_returns(id->sig, "Landroid/view/Choreographer;"))
    return jni_make_object("android/view/Choreographer");
  if (name_has(id->cls, "provider/Settings$Secure") &&
      !strcmp(id->name, "getString")) {
    (void)va_arg(va, void *); /* ContentResolver */
    const char *key = jni_string_utf(va_arg(va, void *));
    if (!strcmp(key, "android_id")) {
      const char *android_id = android_identity_android_id();
      return android_id[0] ? jni_make_local_string(android_id) : NULL;
    }
    return NULL;
  }
  if (name_has(id->cls, "pm/SigningInfo") &&
      (!strcmp(id->name, "getApkContentsSigners") ||
       !strcmp(id->name, "getSigningCertificateHistory")))
    return make_apk_signature_array();
  if (name_has(id->cls, "pm/PackageInfo") &&
      !strcmp(id->name, "getSigningInfo"))
    return jni_make_object("android/content/pm/SigningInfo");
  /* Preserve PlayerPrefs keys through Uri encoding. */
  if (name_has(id->cls, "net/Uri") && (name_has(id->name, "encode") || name_has(id->name, "decode")))
    return va_arg(va, void *);
  if (name_has(id->name, "AssetManager") || sig_returns(id->sig, "Landroid/content/res/AssetManager;"))
    return get_asset_manager_obj();
  if (name_has(id->name, "ClassLoader") || sig_returns(id->sig, "Ljava/lang/ClassLoader;"))
    return get_classloader_obj();
  if (sig_returns(id->sig, "Ljava/lang/Class;"))
    return intern_class("java/lang/Object");
  if (name_has(id->name, "VersionName")) return jni_make_string(SS_VERSION_NAME);
  if (name_has(id->name, "PackageName")) return jni_make_string(SS_PACKAGE);
  if (name_has(id->name, "DeviceModel")) return jni_make_string("Switch");
  if (name_has(id->cls,"java/lang/System") && name_has(id->name,"getProperty"))
    return system_property_value(jni_string_utf(va_arg(va,void*)));
  if (name_has(id->name, "getProperty"))
    return getproperty_value(jni_string_utf(va_arg(va, void *)));
  if (name_has(id->name, "Language") || name_has(id->name, "language"))
    return jni_make_string(lang_code());
  /* Keep storage state consistent with Environment.MEDIA_MOUNTED. */
  if (name_has(id->cls, "os/Environment")) {
    if (name_has(id->name, "ExternalStorageState")) return jni_make_string("mounted");
    if (name_has(id->name, "Directory")) return jni_make_object("java/io/File"); /* ->getAbsolutePath */
  }
  if (name_has(id->cls, "Locale")) {
    int ja = !strcmp(lang_code(), "ja");
    if (!strcmp(id->name, "getCountry"))     return jni_make_string(ja ? "JP" : "US");
    if (!strcmp(id->name, "getISO3Language"))return jni_make_string(ja ? "jpn" : "eng");
    if (!strcmp(id->name, "getISO3Country")) return jni_make_string(ja ? "JPN" : "USA");
    if (!strcmp(id->name, "toString") || name_has(id->name, "getDisplayName") ||
        name_has(id->name, "getDisplayLanguage"))
      return jni_make_string(ja ? "ja_JP" : "en_US");
  }
  /* Map Android storage directories to the game root. */
  if (name_has(id->name, "DataPath") || name_has(id->name, "StoragePath") ||
      name_has(id->name, "FilesDir") || name_has(id->name, "RootPath") ||
      name_has(id->name, "ObbDir") || name_has(id->name, "AssetPath") ||
      name_has(id->name, "Path") || name_has(id->name, "Dir") ||
      name_has(id->name, "Cache") || name_has(id->name, "Directory")) {
    return jni_make_string(managed_path(GAME_HOME));
  }
  /* Package metadata access requires non-null object handles. */
  if (name_has(id->name, "getPackageInfo"))     return jni_make_object("android/content/pm/PackageInfo");
  if (name_has(id->name, "getApplicationInfo")) return jni_make_object("android/content/pm/ApplicationInfo");
  if (name_has(id->name, "getPackageManager"))  return jni_make_object("android/content/pm/PackageManager");
  if (name_has(id->name, "getResources"))       return jni_make_object("android/content/res/Resources");
  if (name_has(id->name, "getConfiguration"))   return jni_make_object("android/content/res/Configuration");
  if (sig_returns(id->sig, "Ljava/lang/String;")) {
    return jni_make_string(""); // UUID, asset-pack name, etc.
  }
  (void)va;
  return NULL;
}

static juint act_int(const FakeID *id, va_list va) {
  /* DeviceInfo's DEX normally owns a one-second Java worker which refreshes
   * these cached values before the managed game samples the static getters.
   * This wrapper does not execute DEX bytecode, so the generic scalar fallback
   * left every value at zero.  The post-shader data loader samples
   * getMemoryTotal() once per second and must see a coherent nonzero device
   * budget.  Query Horizon directly on every call; svcGetInfo is lock-free
   * with respect to the wrapper allocators and reflects demand-backed pages. */
  if (name_has(id->cls, "com/miHoYo/DeviceInfo/Device")) {
    if (!strcmp(id->name, "getMemoryTotal") ||
        !strcmp(id->name, "getMemoryAvailable") ||
        !strcmp(id->name, "getMemoryApp")) {
      u64 total = 0, used = 0;
      const Result total_result = svcGetInfo(
        &total, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0);
      const Result used_result = svcGetInfo(
        &used, InfoType_UsedMemorySize, CUR_PROCESS_HANDLE, 0);
      if (R_FAILED(total_result) || R_FAILED(used_result) || used > total)
        return 0;
      if (!strcmp(id->name, "getMemoryTotal")) return (juint)total;
      if (!strcmp(id->name, "getMemoryAvailable"))
        return (juint)(total - used);
      /* Android's Debug.getPss() path publishes bytes.  Horizon's committed
       * process usage is the closest truthful equivalent and, unlike the old
       * zero, remains internally consistent with total/available. */
      return (juint)used;
    }
    if (!strcmp(id->name, "getStorageAvailableSize") ||
        !strcmp(id->name, "getStorageTotalSize")) {
      struct statvfs storage;
      if (statvfs("sdmc:/", &storage) != 0) return 0;
      const uint64_t block_size = storage.f_frsize
        ? (uint64_t)storage.f_frsize : (uint64_t)storage.f_bsize;
      const uint64_t blocks = !strcmp(id->name, "getStorageAvailableSize")
        ? (uint64_t)storage.f_bavail : (uint64_t)storage.f_blocks;
      if (block_size && blocks > UINT64_MAX / block_size) return UINT64_MAX;
      return (juint)(blocks * block_size);
    }
  }
  /* Match sysconf_fake() and sched_getaffinity(): Horizon reserves one core
   * for the OS and exposes three application cores.  Returning the generic
   * zero fallback makes Unity clamp the device to one worker and starves its
   * first-render job queues. */
  if (name_has(id->cls, "java/lang/Runtime") &&
      !strcmp(id->name, "availableProcessors"))
    return 3;
  if (name_has(id->name, "parseInt") || name_has(id->name, "parseLong")) {
    const char *s = first_string_arg(id->sig, va);
    return (juint)(s ? strtol(s, NULL, 10) : 0);
  }
  if (name_has(id->name, "playCoreApiMissing")) return 1;
  if (name_has(id->name, "isGooglePlayServicesAvailable")) return 1; /* ConnectionResult.SERVICE_MISSING */
  (void)va;
  return 0;
}

static float act_float(const FakeID *id, va_list va) {
  (void)va;
  float x, y, z;
  android_get_orientation(&x, &y, &z);
  if (name_has(id->name, "OrientationX")) return x;
  if (name_has(id->name, "OrientationY")) return y;
  if (name_has(id->name, "OrientationZ")) return z;
  return 0.0f;
}

static void act_void(void *recv, const FakeID *id, va_list va) {
  if (name_has(id->cls, "android/os/Message") &&
      !strcmp(id->name, "sendToTarget")) {
    if (!enqueue_handler_message((FakeMessage *)recv))
      (void)set_pending_message("could not start Android Handler callback");
    return;
  }
  if (name_has(id->cls, "java/lang/System") &&
      (!strcmp(id->name, "load") || !strcmp(id->name, "loadLibrary"))) {
    const char *requested = jni_string_utf(va_arg(va, void *));
    char library[384];
    if (!requested || !requested[0]) {
      (void)set_pending_message("System.loadLibrary: empty library name");
      return;
    }
    if (!strcmp(id->name, "loadLibrary")) {
      const int n = snprintf(library, sizeof(library), "lib%s.so", requested);
      if (n <= 0 || (size_t)n >= sizeof(library)) {
        (void)set_pending_message("System.loadLibrary: library name is too long");
        return;
      }
    } else {
      const int n = snprintf(library, sizeof(library), "%s", requested);
      if (n <= 0 || (size_t)n >= sizeof(library)) {
        (void)set_pending_message("System.load: library path is too long");
        return;
      }
    }

    /* DEX initializes libgmesdk through GMESDK.<clinit> before Mmoron on
     * Android.  The wrapper does not execute Java class initializers, so
     * mapping this voice-chat family starts a guest worker with no initialized
     * GME JavaVM and it dereferences a null JNIEnv at libgmesdk+0x19de9c.
     *
     * MiHoYoMTRSDK is likewise an optional measurement/telemetry library, not
     * the game's HTTP transport.  Its OpenSSL constructor probes CNTVCT_EL0 at
     * MiHoYoMTRSDK+0x16e968; Horizon does not expose that Android userspace
     * timer probe, producing a synchronous data abort before the resource
     * downloader can start.
     *
     * Preserve Android's successful Java load contract as an intentional
     * no-op for these optional SDKs.  Do not map their dependencies or raise an
     * UnsatisfiedLinkError that would alter managed boot control flow.  These
     * modules are unrelated to CRIWARE music/effects. */
    const char *library_basename = strrchr(library, '/');
    library_basename = library_basename ? library_basename + 1 : library;
    if (!strcmp(library_basename, "libMmoron.so") ||
        !strcmp(library_basename, "libgmesdk.so") ||
        !strcmp(library_basename, "libTencentGME.so") ||
        !strcmp(library_basename, "libMiHoYoMTRSDK.so")) {
      return;
    }

    void *handle = plugin_loader_dlopen(library, 0);
    const int jni_version = handle
      ? plugin_loader_jni_onload(handle, fake_vm) : -1;

    if (!handle || jni_version < 0) {
      const char *why = plugin_loader_dlerror();
      (void)set_pending_message(
        why ? why : "native library initialization failed");
    }
    return;
  }
  if (!strcmp(id->name, "finish") || name_has(id->name, "appEnd") ||
      name_has(id->name, "exitApp"))
    jni_quit_requested = 1;
}

static void *dispatch_object_raw(void *recv, const FakeID *id, va_list va) {
  /* java.lang.Object.getClass() is inherited by every Java object.  Route it
   * before class-specific shims: Unity-owned Activity/Context handles would
   * otherwise be turned into another opaque object instead of a Class.  That
   * corrupts AndroidJavaObject's argument-signature builder (for example,
   * Device.Initialize(Activity) previously became the invalid "(L;)V"). */
  if (!strcmp(id->name, "getClass")) {
    void *result = object_class_raw(recv);
    if (!result)
      (void)set_pending_message("java.lang.NullPointerException: getClass");
    return result;
  }
  /* java.lang.reflect.Array exposes ordinary Java arrays through static
   * reflection calls.  Unity 2017 uses this path while constructing JNI
   * signatures, rather than the raw GetObjectArrayElement JNI slot. */
  if (name_has(id->cls, "java/lang/reflect/Array") &&
      !strcmp(id->name, "get")) {
    FakeObjArray *array = va_arg(va, FakeObjArray *);
    const int index = va_arg(va, int);
    if (!array || array->tag != TAG_OBJARR || index < 0 ||
        index >= array->len)
      return NULL;

    return array->items[index];
  }
  if (name_has(id->cls, "pm/PackageManager") &&
      !strcmp(id->name, "getPackageInfo") &&
      strstr(id->sig, "(Ljava/lang/String;")) {
    const char *package_name = jni_string_utf(va_arg(va, void *));
    if (!strcmp(package_name, SS_PACKAGE))
      return jni_make_object("android/content/pm/PackageInfo");
    (void)set_pending_message(
      "android.content.pm.PackageManager$NameNotFoundException");
    return NULL;
  }
  if (recv && (*(uint32_t *)recv == TAG_SIGNATURE ||
               *(uint32_t *)recv == TAG_SIG_INTERN) &&
      name_has(id->cls, "pm/Signature")) {
    if (!strcmp(id->name, "toByteArray"))
      return signature_byte_array((const FakeSignature *)recv);
    if (!strcmp(id->name, "toChars"))
      return signature_chars((const FakeSignature *)recv, 0);
    if (!strcmp(id->name, "toCharsString"))
      return signature_chars((const FakeSignature *)recv, 1);
  }
  /* Preserve injected MotionEvents beyond the call. */
  if (input_owns_class(id->cls) && !strcmp(id->name, "obtain") &&
      strstr(id->sig, "(Landroid/view/MotionEvent;)")) {
    void *src = va_arg(va, void *);
    return unity_motionevent_obtain(src);
  }
  /* String.getBytes() for PlayerPrefs keys. */
  if (recv && *(uint32_t *)recv == TAG_STRING && name_has(id->name, "getBytes")) {
    const char *u = ((FakeString *)recv)->utf; int n = (int)strlen(u);
    char *d = malloc(n > 0 ? n : 1); if (n) memcpy(d, u, n);
    return make_pri_array_adopt(d, n, 1);
  }
  if (recv && *(uint32_t *)recv == TAG_STRING && !strcmp(id->name, "toString")) return recv;
  if (name_has(id->cls, "ClassLoader") && !strcmp(id->name, "findLibrary") &&
      sig_returns(id->sig, "Ljava/lang/String;"))
    return classloader_find_library(va_arg(va, void *));
  if (name_has(id->cls, "ClassLoader") && !strcmp(id->name, "loadClass")) {
    const char *src = obj_str(va_arg(va, void *));
    char name[256]; snprintf(name, sizeof(name), "%s", src);
    for (char *p = name; *p; ++p) if (*p == '.') *p = '/';
    return intern_class(name);
  }
  if (name_has(id->cls, "java/lang/Class") && !strcmp(id->name, "forName")) {
    const char *src = obj_str(va_arg(va, void *));
    char name[256]; snprintf(name, sizeof(name), "%s", src);
    for (char *p = name; *p; ++p) if (*p == '.') *p = '/';
    void *result = intern_class(name);
    /* Unity 2017's managed AndroidJavaObject.FindClass path calls
     * java.lang.Class.forName rather than the raw JNI FindClass slot.  Record
     * the requested class name only; no method payload reaches this log. */

    return result;
  }
  if (name_has(id->cls, "java/lang/Class") && (!strcmp(id->name, "getName") || !strcmp(id->name, "getCanonicalName"))) {
    const char *src = class_name_of(recv); char name[256]; snprintf(name, sizeof(name), "%s", src);
    for (char *p = name; *p; ++p) if (*p == '/') *p = '.';
    return jni_make_string(name);
  }
  if (unity_owns_class(id->cls)) {
    return unity_dispatch_object(recv,id,va);
  }
  return act_object(recv, id, va);
}

/* A JNI object-returning call always produces a local reference, including
 * when Java returns its receiver or one of its arguments.  Constructors in
 * the dispatch path already register new objects; borrowed results do not, so
 * detect that case centrally and add a distinct strong local owner. */
static void *dispatch_object(void *recv, const FakeID *id, va_list va) {
  const int before = local_state.top;
  void *result = dispatch_object_raw(recv, id, va);
  if (!result) return NULL;
  for (int i = before; i < local_state.top; ++i)
    if (local_state.refs[i] == result) return result;
  if (!retain_ref(result)) return NULL;
  return reg_local(result);
}
static juint dispatch_int(void *recv, const FakeID *id, va_list va) {
  if (name_has(id->cls, "java/lang/Class") &&
      !strcmp(id->name, "isArray")) {
    const char *class_name = class_name_of(recv);
    const juint result = class_name[0] == '[';

    return result;
  }
  if (name_has(id->cls, "java/lang/Class") &&
      !strcmp(id->name, "isPrimitive")) {
    const char *class_name = class_name_of(recv);
    const juint result =
      !strcmp(class_name, "void") || !strcmp(class_name, "boolean") ||
      !strcmp(class_name, "byte") || !strcmp(class_name, "char") ||
      !strcmp(class_name, "short") || !strcmp(class_name, "int") ||
      !strcmp(class_name, "long") || !strcmp(class_name, "float") ||
      !strcmp(class_name, "double") ||
      (class_name[0] && !class_name[1] && strchr("VZBCSIJFD", class_name[0]));

    return result;
  }
  if (name_has(id->cls, "java/lang/reflect/Array") &&
      !strcmp(id->name, "getLength")) {
    void *value = va_arg(va, void *);
    const uint32_t tag = value ? *(const uint32_t *)value : 0;
    const juint result =
      tag == TAG_OBJARR ? (juint)((const FakeObjArray *)value)->len :
      tag == TAG_PRIARR ? (juint)((const FakePriArray *)value)->len : 0;

    return result;
  }
  if (recv && (*(uint32_t *)recv == TAG_SIGNATURE ||
               *(uint32_t *)recv == TAG_SIG_INTERN) &&
      name_has(id->cls, "pm/Signature")) {
    const FakeSignature *signature = recv;
    if (!strcmp(id->name, "equals")) {
      const FakeSignature *other = va_arg(va, const FakeSignature *);
      return other &&
             (other->tag == TAG_SIGNATURE || other->tag == TAG_SIG_INTERN) &&
             other->len == signature->len &&
             !memcmp(other->data, signature->data, signature->len);
    }
    if (!strcmp(id->name, "hashCode")) {
      uint32_t hash = 1;
      for (size_t i = 0; i < signature->len; ++i)
        hash = hash * 31u + (uint32_t)(int32_t)(int8_t)signature->data[i];
      return hash;
    }
  }
  if (name_has(id->cls, "pm/PackageManager") &&
      !strcmp(id->name, "hasSigningCertificate")) {
    const char *package_name = jni_string_utf(va_arg(va, void *));
    void *certificate_array = va_arg(va, void *);
    const int input_type = va_arg(va, int);
    int certificate_size = 0;
    const void *certificate =
      jni_bytearray_data(certificate_array, &certificate_size);
    return certificate_size >= 0 &&
      android_identity_has_signing_certificate(package_name, certificate,
                                               (size_t)certificate_size,
                                               input_type);
  }
  if (recv && *(uint32_t *)recv == TAG_STRING) {
    if (!strcmp(id->name, "length"))   return utf16_len(((FakeString *)recv)->utf);
    if (!strcmp(id->name, "hashCode")) {
      const char *s = ((FakeString *)recv)->utf; const size_t n = utf16_len(s);
      uint16_t *u = malloc((n ? n : 1) * sizeof(*u)); int32_t h = 0;
      if (u) { jni_utf8_to_utf16(s, u); for (size_t i = 0; i < n; ++i) h = h * 31 + u[i]; free(u); }
      return (juint)(uint32_t)h;
    }
    if (!strcmp(id->name, "isEmpty"))  return ((FakeString *)recv)->utf[0] == '\0';
    if (!strcmp(id->name, "equals"))   return !strcmp(((FakeString *)recv)->utf, obj_str(va_arg(va, void *)));
    if (!strcmp(id->name, "equalsIgnoreCase")) return !strcasecmp(((FakeString *)recv)->utf, obj_str(va_arg(va, void *)));
    if (!strcmp(id->name, "compareTo")) return (juint)(int32_t)strcmp(((FakeString *)recv)->utf, obj_str(va_arg(va, void *)));
    if (!strcmp(id->name, "charAt")) {
      int index = va_arg(va, int); const char *s = ((FakeString *)recv)->utf; const size_t n = utf16_len(s);
      uint16_t *u = malloc((n ? n : 1) * sizeof(*u)); uint16_t c = 0;
      if (u) { jni_utf8_to_utf16(s, u); if (index >= 0 && (size_t)index < n) c = u[index]; free(u); }
      return c;
    }
  }
  /* Unbox PlayerPrefs values by receiver type. */
  if (unity_is_boxed(recv)) return unity_boxed_int(recv);
  /* Input getter IDs lose their concrete class, so route by receiver. */
  if (input_owns_recv(recv)) return input_dispatch_int(recv, id, va);
  if (unity_owns_class(id->cls)) return unity_dispatch_int(recv, id, va);
  if (input_owns_class(id->cls)) return input_dispatch_int(recv, id, va);
  return act_int(id, va);
}
static float dispatch_float(void *recv, const FakeID *id, va_list va) {
  if (unity_is_boxed(recv)) return unity_boxed_float(recv);   /* Float.floatValue */
  if (input_owns_recv(recv)) return input_dispatch_float(recv, id, va);
  if (input_owns_class(id->cls)) return input_dispatch_float(recv, id, va);
  if (unity_owns_class(id->cls)) return unity_dispatch_float(recv, id, va);
  return act_float(id, va);
}
static double dispatch_double(void *recv, const FakeID *id, va_list va) {
  return (double)dispatch_float(recv, id, va);
}
static void dispatch_void(void *recv, const FakeID *id, va_list va) {
  if (unity_owns_class(id->cls)) { unity_dispatch_void(recv, id, va); return; }
  act_void(recv, id, va);
}

static juint j_GetVersion(void *env) { (void)env; return JNI_VERSION_1_6; }
static void *j_FindClass(void *env, const char *name) {
  (void)env;
  const char *class_name = name ? name : "?";
  /* JNI FindClass returns a local reference.  Pooled class objects are
   * process-stable, but the handle still has to participate in the caller's
   * local frame so DeleteLocalRef/PopLocalFrame observe the JNI contract. */
  return reg_local(intern_class(class_name));
}

static const char *canonical_object_class(const char *label) {
  if (!label || !label[0]) return "java/lang/Object";
  if (strchr(label, '/') || label[0] == '[') return label;
  if (!strcmp(label, "AssetManager")) return "android/content/res/AssetManager";
  if (!strcmp(label, "ClassLoader")) return "java/lang/ClassLoader";
  if (!strcmp(label, "Context")) return "android/content/Context";
  if (!strcmp(label, "File")) return "java/io/File";
  if (!strcmp(label, "Resources")) return "android/content/res/Resources";
  if (!strcmp(label, "Configuration")) return "android/content/res/Configuration";
  if (!strcmp(label, "DisplayMetrics")) return "android/util/DisplayMetrics";
  if (!strcmp(label, "Display")) return "android/view/Display";
  if (!strcmp(label, "Service")) return "java/lang/Object";
  if (!strcmp(label, "Set")) return "java/util/Set";
  if (!strcmp(label, "String[]")) return "[Ljava/lang/String;";
  return label;
}

static void *object_class_raw(void *obj) {
  if (!obj) return NULL;
  if (address_in_range(obj, class_pool, sizeof(class_pool)))
    return intern_class("java/lang/Class");
  const uint32_t tag = *(const uint32_t *)obj;
  if (tag == TAG_CLASS) return intern_class("java/lang/Class");
  if (tag == TAG_STRING) return intern_class("java/lang/String");
  if (tag == TAG_OBJARR) {
    const FakeObjArray *array = obj;
    if (array->len > 0 && array->items && array->items[0]) {
      const void *first = array->items[0];
      if (is_interned_string(first) || *(const uint32_t *)first == TAG_STRING)
        return intern_class("[Ljava/lang/String;");
      if (address_in_range(first, class_pool, sizeof(class_pool)) ||
          *(const uint32_t *)first == TAG_CLASS)
        return intern_class("[Ljava/lang/Class;");
    }
    return intern_class("[Ljava/lang/Object;");
  }
  if (tag == TAG_PRIARR) {
    const FakePriArray *a = obj;
    return intern_class(a->elem_size == 1 ? "[B" : a->elem_size == 8 ? "[J" : "[I");
  }
  if (tag == TAG_DIRECT) return intern_class("java/nio/DirectByteBuffer");
  if (tag == TAG_SIGNATURE || tag == TAG_SIG_INTERN)
    return intern_class("android/content/pm/Signature");
  if (tag == TAG_INTERN || tag == TAG_OBJECT || tag == TAG_PROXY ||
      tag == TAG_HANDLER || tag == TAG_MESSAGE)
    return intern_class(canonical_object_class(((const FakeObject *)obj)->label));
  if (tag == TAG_ID) return intern_class("java/lang/reflect/Method");
  const char *unity_cls = unity_class_of(obj);
  if (unity_cls) return intern_class(unity_cls);
  if (input_owns_recv(obj))
    return intern_class(input_recv_is_motion(obj) ? "android/view/MotionEvent" : "android/view/KeyEvent");
  return intern_class("java/lang/Object");
}
static void *j_GetObjectClass(void *env, void *obj) {
  (void)env;
  return reg_local(object_class_raw(obj));
}
static void *j_GetMethodID(void *env, void *cls, const char *name, const char *sig) {
  (void)env;
  const char *class_name = class_name_of(cls);
  const char *member = name ? name : "";
  const char *signature = sig ? sig : "";
  void *result = get_id(class_name, member, signature);

  return result;
}
static void *j_GetFieldID(void *env, void *cls, const char *name, const char *sig) {
  (void)env;
  const char *class_name = class_name_of(cls);
  const char *member = name ? name : "";
  const char *signature = sig ? sig : "";
  void *result = get_id(class_name, member, signature);

  return result;
}

/* Decode constructors which carry state needed by native code. */
static void *new_object_dispatch(void *cls, void *mid, void *first_arg, void *second_arg) {
  const char *cn = class_name_of(cls);
  if (cn && strstr(cn, "android/content/pm/Signature")) {
    int size = 0;
    const void *bytes = jni_bytearray_data(first_arg, &size);
    return size >= 0 ? make_signature_copy(bytes, (size_t)size) : NULL;
  }
  if (cn && strstr(cn, "java/lang/String")) {
    FakeID *m = mid;
    if (m && strstr(m->sig, "[B")) {              /* String([B...) */
      int len = 0; char *b = jni_bytearray_data(first_arg, &len);
      if (b && len > 0) { char *t = malloc(len + 1); memcpy(t, b, len); t[len] = 0;
        void *s = jni_make_string(t); free(t); return s; }
      return jni_make_string("");
    }
  }
  if (cn && strstr(cn, "java/io/File")) {
    FakeID *m = mid;
    if (m && strstr(m->sig, "(Ljava/io/File;Ljava/lang/String;)"))
      return unity_make_file(unity_file_path(first_arg), obj_str(second_arg));
    if (m && strstr(m->sig, "(Ljava/lang/String;Ljava/lang/String;)"))
      return unity_make_file(obj_str(first_arg), obj_str(second_arg));
    return unity_make_file(obj_str(first_arg), NULL);
  }
  FakeID *method = mid;
  if (cn && !strcmp(cn, "android/os/Handler") && method &&
      !strcmp(method->sig,
              "(Landroid/os/Looper;Landroid/os/Handler$Callback;)V")) {
    void *result = make_handler(second_arg);

    return result;
  }
  void *result = jni_make_object(cn);

  return result;
}

static int constructor_has_second_object_arg(const FakeID *method) {
  return method &&
    (strstr(method->sig, "Ljava/io/File;Ljava/lang/String;") ||
     strstr(method->sig, "Ljava/lang/String;Ljava/lang/String;") ||
     !strcmp(method->sig,
             "(Landroid/os/Looper;Landroid/os/Handler$Callback;)V"));
}

static void *j_NewObject(void *env, void *cls, void *mid, ...) {
  (void)env;
  FakeID *m = mid; void *a0 = NULL, *a1 = NULL;
  va_list va; va_start(va, mid);
  if (m && strchr(m->sig, '(') && strchr(m->sig, '(')[1] != ')') a0 = va_arg(va, void *);
  if (constructor_has_second_object_arg(m)) a1 = va_arg(va, void *);
  va_end(va);
  return new_object_dispatch(cls, mid, a0, a1);
}
static void *j_NewObjectV(void *env, void *cls, void *mid, va_list va) {
  (void)env; FakeID *m = mid; void *a0 = NULL, *a1 = NULL;
  if (m && strchr(m->sig, '(') && strchr(m->sig, '(')[1] != ')') a0 = va_arg(va, void *);
  if (constructor_has_second_object_arg(m)) a1 = va_arg(va, void *);
  return new_object_dispatch(cls, mid, a0, a1);
}

static void *j_AllocObject(void *env, void *cls) {
  (void)env;
  FakeObject *object = calloc(1, sizeof(*object));
  if (!object) return NULL;
  object->tag = TAG_OBJECT;
  object->refs = 1;
  strncpy(object->label, class_name_of(cls), sizeof(object->label) - 1);
  object->label[sizeof(object->label) - 1] = '\0';
  return reg_local(object);
}

static void *j_NewGlobalRef(void *env, void *obj) {
  (void)env;
  if(!obj){return NULL;}
  mutexLock(&locals_lock);
  for(int i=0;i<global_ref_count;++i)if(global_refs[i].obj==obj){
    if(global_refs[i].count==UINT32_MAX){mutexUnlock(&locals_lock);
      return NULL;}
    ++global_refs[i].count;mutexUnlock(&locals_lock);

    return obj;
  }
  if(global_ref_count>=MAX_GLOBAL_REFS){
    mutexUnlock(&locals_lock);

    return NULL;
  }
  /* A global is an additional root; NewGlobalRef never consumes the caller's
   * local.  One retained owner backs the aggregate table entry until its last
   * alias is deleted.  Pooled/external handles need no retain. */
  if(!retain_ref(obj)){mutexUnlock(&locals_lock);
    return NULL;}
  global_refs[global_ref_count].obj=obj;
  global_refs[global_ref_count].count=1;
  ++global_ref_count;
  mutexUnlock(&locals_lock);

  return obj;
}
static void j_DeleteGlobalRef(void *env, void *obj) {
  (void)env;if(!obj)return;int release=0;mutexLock(&locals_lock);
  for(int i=0;i<global_ref_count;++i)if(global_refs[i].obj==obj){
    if(--global_refs[i].count==0){global_refs[i]=global_refs[--global_ref_count];release=1;}break;}
  mutexUnlock(&locals_lock);if(release)free_ref(obj);
}
static void j_DeleteLocalRef(void *env, void *obj) { (void)env; delete_local(obj); }
static void *j_NewLocalRef(void *env, void *obj) {
  (void)env;
  if(!obj)return NULL;
  if(!retain_ref(obj))return NULL;
  return reg_local(obj);
}
static juint j_IsSameObject(void *env, void *a, void *b) { (void)env; return a == b; }

/* Preserve exact types for input and boxed preference objects. */
static juint j_IsInstanceOf(void *env, void *obj, void *clazz) {
  (void)env;
  const char *cn = class_name_of(clazz);
  /* Input dispatch checks KeyEvent before MotionEvent. */
  if (input_owns_recv(obj)) {
    if (strstr(cn, "MotionEvent")) return input_recv_is_motion(obj) ? 1 : 0;
    if (strstr(cn, "KeyEvent"))    return input_recv_is_motion(obj) ? 0 : 1;
    return 1;
  }
  /* Boxed values require exact primitive wrapper types. */
  int ui = unity_isinstance(obj, cn);
  if (ui >= 0) return (juint)ui;
  if (obj && *(uint32_t *)obj == TAG_STRING) {
    if (strstr(cn, "String")) return 1;
    if (strstr(cn, "Integer") || strstr(cn, "Long") || strstr(cn, "Float") ||
        strstr(cn, "Double")  || strstr(cn, "Boolean") || strstr(cn, "Character") ||
        strstr(cn, "Short")   || strstr(cn, "Byte"))
      return 0;
  }
  if (!obj) return 1; /* null can be cast to every reference type. */
  if (!strcmp(cn, "java/lang/Object")) return 1;
  void *actual_class = object_class_raw(obj);
  const char *actual = class_name_of(actual_class);
  if (!strcmp(actual, cn)) return 1;
  if ((strstr(actual, "Activity") || strstr(actual, "Application")) && !strcmp(cn, "android/content/Context")) return 1;
  if (!strcmp(actual, "java/lang/String") && (!strcmp(cn, "java/lang/CharSequence") || !strcmp(cn, "java/io/Serializable"))) return 1;
  if (actual[0] == '[' && (!strcmp(cn, "java/lang/Cloneable") || !strcmp(cn, "java/io/Serializable"))) return 1;
  return 0;
}
static juint j_EnsureLocalCapacity(void *env, int cap) {
  (void)env; LocalState*s=&local_state;
  if(cap<0 || cap>MAX_LOCALS-s->top)return (juint)JNI_ERR;
  if(s->top+cap<=s->capacity)return JNI_OK;
  int next=s->capacity?s->capacity:256;
  while(next<s->top+cap&&next<MAX_LOCALS)next*=2;
  if(next>MAX_LOCALS)next=MAX_LOCALS;
  void**p=realloc(s->refs,(size_t)next*sizeof(*p));if(!p)return (juint)JNI_ERR;
  s->refs=p;s->capacity=next;return JNI_OK;
}

static void *j_GetSuperclass(void *env, void *clazz) {
  (void)env; const char *cn = class_name_of(clazz);
  if (!cn[0] || !strcmp(cn, "java/lang/Object")) return NULL;
  if (strstr(cn, "Activity")) return reg_local(intern_class("android/content/Context"));
  if (strstr(cn, "Application")) return reg_local(intern_class("android/content/Context"));
  return reg_local(intern_class("java/lang/Object"));
}
static juint j_IsAssignableFrom(void *env, void *sub, void *sup) {
  (void)env; const char *a = class_name_of(sub), *b = class_name_of(sup);
  if (!strcmp(a, b) || !strcmp(b, "java/lang/Object")) return 1;
  if ((strstr(a, "Activity") || strstr(a, "Application")) && !strcmp(b, "android/content/Context")) return 1;
  return 0;
}

static juint j_PushLocalFrame(void *env, int cap) {
  (void)env; LocalState*s=&local_state;
  if(cap<0 || s->frame_top>=MAX_FRAMES || cap>MAX_LOCALS-s->top)return (juint)JNI_ERR;
  if(j_EnsureLocalCapacity(env,cap)!=JNI_OK)return (juint)JNI_ERR;
  s->frames[s->frame_top++]=s->top; return JNI_OK;
}
static void *j_PopLocalFrame(void *env, void *result) {
  (void)env;
  LocalState*s=&local_state;
  if(s->frame_top<=0)return NULL;
  const int mark=s->frames[--s->frame_top];
  int transferred=0;
  for(int i=mark;i<s->top;++i){
    if(s->refs[i]==result&&!transferred){transferred=1;continue;}
    free_ref(s->refs[i]);
  }
  s->top=mark;
  if(result){
    if(transferred)return reg_local(result);
    if(retain_ref(result))return reg_local(result);
    return NULL;
  }
  return result;
}

int jni_push_local_frame(int capacity) {
  return (int)j_PushLocalFrame(fake_env, capacity);
}

void *jni_pop_local_frame(void *result) {
  return j_PopLocalFrame(fake_env, result);
}

#define CALL_VARIADIC(fn, ret_t, dispatch) \
  static ret_t fn(void *env, void *recv, FakeID *id, ...) { \
    (void)env; va_list va; va_start(va, id); \
    ret_t r = dispatch(recv, id, va); va_end(va); return r; } \
  static ret_t fn##V(void *env, void *recv, FakeID *id, va_list va) { \
    (void)env; return dispatch(recv, id, va); }

CALL_VARIADIC(j_CallObjectMethod, void *, dispatch_object)
CALL_VARIADIC(j_CallIntMethod, juint, dispatch_int)
CALL_VARIADIC(j_CallBooleanMethod, juint, dispatch_int)
CALL_VARIADIC(j_CallByteMethod, juint, dispatch_int)
CALL_VARIADIC(j_CallCharMethod, juint, dispatch_int)
CALL_VARIADIC(j_CallShortMethod, juint, dispatch_int)
CALL_VARIADIC(j_CallLongMethod, juint, dispatch_int)
CALL_VARIADIC(j_CallFloatMethod, float, dispatch_float)
CALL_VARIADIC(j_CallDoubleMethod, double, dispatch_double)

static void j_CallVoidMethod(void *env, void *recv, FakeID *id, ...) {
  (void)env; va_list va; va_start(va, id); dispatch_void(recv, id, va); va_end(va);
}
static void j_CallVoidMethodV(void *env, void *recv, FakeID *id, va_list va) {
  (void)env; dispatch_void(recv, id, va);
}

#define j_CallStaticObjectMethod   j_CallObjectMethod
#define j_CallStaticObjectMethodV  j_CallObjectMethodV
#define j_CallStaticIntMethod      j_CallIntMethod
#define j_CallStaticIntMethodV     j_CallIntMethodV
#define j_CallStaticBooleanMethod  j_CallBooleanMethod
#define j_CallStaticBooleanMethodV j_CallBooleanMethodV
#define j_CallStaticByteMethod     j_CallByteMethod
#define j_CallStaticByteMethodV    j_CallByteMethodV
#define j_CallStaticCharMethod     j_CallCharMethod
#define j_CallStaticCharMethodV    j_CallCharMethodV
#define j_CallStaticShortMethod    j_CallShortMethod
#define j_CallStaticShortMethodV   j_CallShortMethodV
#define j_CallStaticLongMethod     j_CallLongMethod
#define j_CallStaticLongMethodV    j_CallLongMethodV
#define j_CallStaticFloatMethod    j_CallFloatMethod
#define j_CallStaticFloatMethodV   j_CallFloatMethodV
#define j_CallStaticDoubleMethod   j_CallDoubleMethod
#define j_CallStaticDoubleMethodV  j_CallDoubleMethodV
#define j_CallStaticVoidMethod     j_CallVoidMethod
#define j_CallStaticVoidMethodV    j_CallVoidMethodV

/*
 * jvalue[] JNI variants used by SWIG and AndroidJavaObject.
 *
 * A jvalue is not a variadic argument image.  In particular, float must be
 * promoted to double and AArch64 keeps integer and FP varargs in distinct save
 * areas.  Force a synthetic va_list down its stack path, with one ABI stack
 * slot per decoded descriptor argument.  This preserves arbitrary GP/FP mixes
 * and is not limited to the eight argument registers.
 */
typedef union {
  uint8_t z;
  int8_t b;
  uint16_t c;
  int16_t s;
  int32_t i;
  int64_t j;
  float f;
  double d;
  void *l;
} FakeJValue;

typedef struct {
  void *stack;
  void *gr_top;
  void *vr_top;
  int gr_offs;
  int vr_offs;
} Aarch64VaListLayout;

_Static_assert(sizeof(FakeJValue) == 8, "JNI jvalue must occupy eight bytes");
_Static_assert(sizeof(va_list) == sizeof(Aarch64VaListLayout),
               "unexpected AArch64 va_list layout");

/* Return the next descriptor and advance *cursor past the complete type. */
static char jni_next_sig_arg(const char **cursor) {
  const char *p = *cursor;
  if (!p || !*p || *p == ')') return '\0';
  char kind = *p;
  if (kind == '[') {
    do { ++p; } while (*p == '[');
    if (*p == 'L') {
      while (*p && *p != ';') ++p;
      if (*p == ';') ++p;
    } else if (*p) {
      ++p;
    }
    kind = 'L';
  } else if (kind == 'L') {
    while (*p && *p != ';') ++p;
    if (*p == ';') ++p;
  } else {
    ++p;
  }
  *cursor = p;
  return kind;
}

static size_t jni_sig_arg_count(const char *sig) {
  const char *p = sig ? strchr(sig, '(') : NULL;
  size_t count = 0;
  if (!p) return 0;
  ++p;
  while (*p && *p != ')') {
    if (!jni_next_sig_arg(&p)) break;
    ++count;
  }
  return count;
}

static int jni_marshal_jvalues(const FakeID *id, const void *args,
                               va_list *out, uint64_t **storage_out) {
  if (!id || !storage_out) return 0;
  const size_t count = jni_sig_arg_count(id->sig);
  if (count > (SIZE_MAX / sizeof(uint64_t)) - 1) return 0;

  /* Keep a valid, aligned stack pointer even for zero-argument dispatchers. */
  uint64_t *slots = calloc(count ? count : 1, sizeof(*slots));
  if (!slots) return 0;

  const FakeJValue *values = args;
  const char *p = strchr(id->sig, '(');
  if (p) ++p;
  for (size_t i = 0; i < count; ++i) {
    const char kind = jni_next_sig_arg(&p);
    if (!values) continue; /* Invalid JNI input: expose zeroes, never overread. */
    switch (kind) {
      case 'Z': { int value = values[i].z; memcpy(&slots[i], &value, sizeof(value)); break; }
      case 'B': { int value = values[i].b; memcpy(&slots[i], &value, sizeof(value)); break; }
      case 'C': { int value = values[i].c; memcpy(&slots[i], &value, sizeof(value)); break; }
      case 'S': { int value = values[i].s; memcpy(&slots[i], &value, sizeof(value)); break; }
      case 'I': { int value = values[i].i; memcpy(&slots[i], &value, sizeof(value)); break; }
      case 'J': memcpy(&slots[i], &values[i].j, sizeof(values[i].j)); break;
      case 'F': {
        const double value = (double)values[i].f; /* default argument promotion */
        memcpy(&slots[i], &value, sizeof(value));
        break;
      }
      case 'D': memcpy(&slots[i], &values[i].d, sizeof(values[i].d)); break;
      case 'L': memcpy(&slots[i], &values[i].l, sizeof(values[i].l)); break;
      default: break; /* Malformed descriptor: retain a deterministic zero. */
    }
  }

  const Aarch64VaListLayout layout = {
    .stack = slots,
    .gr_top = slots,
    .vr_top = slots,
    .gr_offs = 0,
    .vr_offs = 0,
  };
  memcpy(out, &layout, sizeof(layout));
  *storage_out = slots;
  return 1;
}

#define CALL_A_RETURN(fn, ret_t, dispatch, zero) \
  static ret_t fn(void *env, void *recv, FakeID *id, const void *args) { \
    (void)env; va_list va; uint64_t *storage = NULL; \
    if (!jni_marshal_jvalues(id, args, &va, &storage)) return (zero); \
    ret_t result = dispatch(recv, id, va); free(storage); return result; \
  }

CALL_A_RETURN(j_CallObjectMethodA,  void *, dispatch_object, NULL)
CALL_A_RETURN(j_CallBooleanMethodA, juint,  dispatch_int,    0)
CALL_A_RETURN(j_CallByteMethodA,    juint,  dispatch_int,    0)
CALL_A_RETURN(j_CallCharMethodA,    juint,  dispatch_int,    0)
CALL_A_RETURN(j_CallShortMethodA,   juint,  dispatch_int,    0)
CALL_A_RETURN(j_CallIntMethodA,     juint,  dispatch_int,    0)
CALL_A_RETURN(j_CallLongMethodA,    juint,  dispatch_int,    0)
CALL_A_RETURN(j_CallFloatMethodA,   float,  dispatch_float,  0.0f)
CALL_A_RETURN(j_CallDoubleMethodA,  double, dispatch_double, 0.0)

static void j_CallVoidMethodA(void *env, void *recv, FakeID *id,
                              const void *args) {
  (void)env;
  va_list va;
  uint64_t *storage = NULL;
  if (!jni_marshal_jvalues(id, args, &va, &storage)) return;
  dispatch_void(recv, id, va);
  free(storage);
}

/* Nonvirtual calls use the same emulated dispatch but have an extra jclass. */
#define CALL_NONVIRTUAL_VARIADIC(fn, ret_t, dispatch) \
  static ret_t fn(void *env, void *recv, void *clazz, FakeID *id, ...) { \
    (void)env; (void)clazz; va_list va; va_start(va, id); \
    ret_t result = dispatch(recv, id, va); va_end(va); return result; \
  } \
  static ret_t fn##V(void *env, void *recv, void *clazz, FakeID *id, va_list va) { \
    (void)env; (void)clazz; return dispatch(recv, id, va); \
  } \
  static ret_t fn##A(void *env, void *recv, void *clazz, FakeID *id, const void *args) { \
    (void)clazz; return fn##_a_target(env, recv, id, args); \
  }

#define j_CallNonvirtualObjectMethod_a_target  j_CallObjectMethodA
#define j_CallNonvirtualBooleanMethod_a_target j_CallBooleanMethodA
#define j_CallNonvirtualByteMethod_a_target    j_CallByteMethodA
#define j_CallNonvirtualCharMethod_a_target    j_CallCharMethodA
#define j_CallNonvirtualShortMethod_a_target   j_CallShortMethodA
#define j_CallNonvirtualIntMethod_a_target     j_CallIntMethodA
#define j_CallNonvirtualLongMethod_a_target    j_CallLongMethodA
#define j_CallNonvirtualFloatMethod_a_target   j_CallFloatMethodA
#define j_CallNonvirtualDoubleMethod_a_target  j_CallDoubleMethodA

CALL_NONVIRTUAL_VARIADIC(j_CallNonvirtualObjectMethod,  void *, dispatch_object)
CALL_NONVIRTUAL_VARIADIC(j_CallNonvirtualBooleanMethod, juint,  dispatch_int)
CALL_NONVIRTUAL_VARIADIC(j_CallNonvirtualByteMethod,    juint,  dispatch_int)
CALL_NONVIRTUAL_VARIADIC(j_CallNonvirtualCharMethod,    juint,  dispatch_int)
CALL_NONVIRTUAL_VARIADIC(j_CallNonvirtualShortMethod,   juint,  dispatch_int)
CALL_NONVIRTUAL_VARIADIC(j_CallNonvirtualIntMethod,     juint,  dispatch_int)
CALL_NONVIRTUAL_VARIADIC(j_CallNonvirtualLongMethod,    juint,  dispatch_int)
CALL_NONVIRTUAL_VARIADIC(j_CallNonvirtualFloatMethod,   float,  dispatch_float)
CALL_NONVIRTUAL_VARIADIC(j_CallNonvirtualDoubleMethod,  double, dispatch_double)

static void j_CallNonvirtualVoidMethod(void *env, void *recv, void *clazz,
                                       FakeID *id, ...) {
  (void)env; (void)clazz;
  va_list va; va_start(va, id); dispatch_void(recv, id, va); va_end(va);
}
static void j_CallNonvirtualVoidMethodV(void *env, void *recv, void *clazz,
                                        FakeID *id, va_list va) {
  (void)env; (void)clazz; dispatch_void(recv, id, va);
}
static void j_CallNonvirtualVoidMethodA(void *env, void *recv, void *clazz,
                                        FakeID *id, const void *args) {
  (void)clazz; j_CallVoidMethodA(env, recv, id, args);
}
static void *j_NewObjectA        (void *e, void *cls, void *mid, const void *a){ (void)e;
  FakeID *m=mid; void *a0=NULL,*a1=NULL;
  if(a && m && strchr(m->sig,'(') && strchr(m->sig,'(')[1]!=')') a0=((void *const *)a)[0];
  if(a && constructor_has_second_object_arg(m)) a1=((void *const *)a)[1];
  return new_object_dispatch(cls,mid,a0,a1); }
#define j_CallStaticObjectMethodA  j_CallObjectMethodA
#define j_CallStaticBooleanMethodA j_CallBooleanMethodA
#define j_CallStaticByteMethodA    j_CallByteMethodA
#define j_CallStaticCharMethodA    j_CallCharMethodA
#define j_CallStaticShortMethodA   j_CallShortMethodA
#define j_CallStaticIntMethodA     j_CallIntMethodA
#define j_CallStaticLongMethodA    j_CallLongMethodA
#define j_CallStaticFloatMethodA   j_CallFloatMethodA
#define j_CallStaticDoubleMethodA  j_CallDoubleMethodA
#define j_CallStaticVoidMethodA    j_CallVoidMethodA

static void *j_NewStringUTF(void *env, const char *utf) { (void)env; return jni_make_local_string(utf); }
static void *j_NewString(void *env, const uint16_t *u, int len) {
  (void)env;
  if (!u || len < 0) return jni_make_local_string("");
  char *tmp = malloc((size_t)len * 4 + 1);
  if (!tmp) return NULL;
  int o = 0;
  for (int i = 0; i < len; i++) {
    uint32_t c = u[i];
    if (c >= 0xd800 && c <= 0xdbff && i + 1 < len && u[i + 1] >= 0xdc00 && u[i + 1] <= 0xdfff) {
      c = 0x10000 + (((c - 0xd800) << 10) | (u[++i] - 0xdc00));
    } else if (c >= 0xd800 && c <= 0xdfff) {
      c = 0xfffd;
    }
    if (c < 0x80) tmp[o++] = (char)c;
    else if (c < 0x800) { tmp[o++] = 0xC0 | (c >> 6); tmp[o++] = 0x80 | (c & 0x3F); }
    else if (c < 0x10000) { tmp[o++] = 0xE0 | (c >> 12); tmp[o++] = 0x80 | ((c >> 6) & 0x3F); tmp[o++] = 0x80 | (c & 0x3F); }
    else { tmp[o++] = 0xf0 | (c >> 18); tmp[o++] = 0x80 | ((c >> 12) & 0x3f);
      tmp[o++] = 0x80 | ((c >> 6) & 0x3f); tmp[o++] = 0x80 | (c & 0x3f); }
  }
  tmp[o] = 0;
  void *s = jni_make_local_string(tmp);
  free(tmp);
  return s;
}
static const uint16_t *j_GetStringChars(void *env, void *jstr, uint8_t *is_copy) {
  (void)env;
  const char *s = obj_str(jstr);
  const size_t n = utf16_len(s);
  uint16_t *out = malloc((n + 1) * sizeof(*out));
  if (!out) { if (is_copy) *is_copy = 0; return NULL; }
  jni_utf8_to_utf16(s, out); out[n] = 0;
  if (is_copy) *is_copy = 1;
  return out;
}
static void j_ReleaseStringChars(void *env, void *jstr, const uint16_t *chars) {
  (void)env; (void)jstr; free((void *)chars);
}
static const char *j_GetStringUTFChars(void *env, void *jstr, uint8_t *is_copy) {
  (void)env;
  const char *s = obj_str(jstr);
  size_t units = utf16_len(s), bytes = 0;
  uint16_t *u = malloc((units ? units : 1) * sizeof(*u));
  if (!u) { if (is_copy) *is_copy = 0; return NULL; }
  jni_utf8_to_utf16(s, u);
  char *out = utf16_to_mutf8(u, units, &bytes);
  free(u);
  if (is_copy) *is_copy = out ? 1 : 0;
  return out;
}
static void j_ReleaseStringUTFChars(void *env, void *jstr, const char *utf) { (void)env; (void)jstr; free((void *)utf); }
static juint j_GetStringUTFLength(void *env, void *jstr) {
  (void)env;
  const char *s = obj_str(jstr); size_t units = utf16_len(s), bytes = 0;
  uint16_t *u = malloc((units ? units : 1) * sizeof(*u));
  if (!u) return 0;
  jni_utf8_to_utf16(s, u); char *tmp = utf16_to_mutf8(u, units, &bytes);
  free(u); free(tmp); return (juint)bytes;
}

static void j_GetStringUTFRegion(void *env, void *jstr, int start, int len, char *buf) {
  (void)env;
  if (!buf) return;
  const char *s = obj_str(jstr);
  const int slen = (int)utf16_len(s);
  if (start < 0) start = 0;
  if (start > slen) start = slen;
  if (len < 0) len = 0;
  if (start + len > slen) len = slen - start;
  uint16_t *u = malloc((size_t)(slen ? slen : 1) * sizeof(*u));
  if (!u) return;
  jni_utf8_to_utf16(s, u);
  size_t bytes = 0; char *tmp = utf16_to_mutf8(u + start, (size_t)len, &bytes);
  if (tmp) memcpy(buf, tmp, bytes); /* JNI regions are deliberately not NUL terminated. */
  free(tmp); free(u);
}
static void j_GetStringRegion(void *env, void *jstr, int start, int len, uint16_t *buf) {
  (void)env;
  if (!buf) return;
  const char *s = obj_str(jstr);
  const int slen = (int)utf16_len(s);
  if (start < 0) start = 0;
  if (start > slen) start = slen;
  if (len < 0) len = 0;
  if (start + len > slen) len = slen - start;
  uint16_t *u = malloc((size_t)(slen ? slen : 1) * sizeof(*u));
  if (!u) return;
  jni_utf8_to_utf16(s, u); memcpy(buf, u + start, (size_t)len * sizeof(*u)); free(u);
}
static juint j_GetStringLength(void *env, void *jstr) {
  (void)env;
  return utf16_len(obj_str(jstr));
}

static juint j_GetArrayLength(void *env, void *arr) {
  (void)env;
  FakeObjArray *a = arr;
  if (a && (a->tag == TAG_PRIARR || a->tag == TAG_OBJARR))
    return a->len;
  return 0;
}

static void *new_pri_array(int len, int elem_size) {
  if (len < 0 || elem_size <= 0 || (size_t)len > SIZE_MAX / (size_t)elem_size) return NULL;
  void *data = calloc(len ? len : 1, elem_size);
  if (!data) return NULL;
  return make_pri_array_adopt(data, len, elem_size);
}
static void *j_NewBooleanArray(void *env, int len) { (void)env; return new_pri_array(len, 1); }
static void *j_NewByteArray(void *env, int len) { (void)env; return new_pri_array(len, 1); }
static void *j_NewCharArray(void *env, int len) { (void)env; return new_pri_array(len, 2); }
static void *j_NewShortArray(void *env, int len) { (void)env; return new_pri_array(len, 2); }
static void *j_NewIntArray(void *env, int len) { (void)env; return new_pri_array(len, 4); }
static void *j_NewLongArray(void *env, int len) { (void)env; return new_pri_array(len, 8); }
static void *j_NewFloatArray(void *env, int len) { (void)env; return new_pri_array(len, 4); }
static void *j_NewDoubleArray(void *env, int len) { (void)env; return new_pri_array(len, 8); }

static void *j_NewObjectArray(void *env, int len, void *cls, void *init) {
  (void)env; (void)cls;
  if (len < 0 || (size_t)len > SIZE_MAX / sizeof(void *)) return NULL;
  FakeObjArray *a = calloc(1, sizeof(*a));
  if (!a) return NULL;
  a->tag = TAG_OBJARR;
  a->refs = 1;
  a->len = len;
  a->items = calloc(len ? len : 1, sizeof(void *));
  if (!a->items) { free(a); return NULL; }
  for (int i = 0; i < len; i++) {
    if (!retain_ref(init)) { free_ref(a); return NULL; }
    a->items[i] = init;
  }
  return reg_local(a);
}
void *jni_make_object_array(int length, void *const *items) {
  if (length < 0 || (size_t)length > SIZE_MAX / sizeof(void *)) return NULL;
  FakeObjArray *a = calloc(1, sizeof(*a)); if (!a) return NULL;
  a->tag = TAG_OBJARR; a->refs = 1; a->len = length;
  a->items = calloc(length ? (size_t)length : 1, sizeof(void *));
  if (!a->items) { free(a); return NULL; }
  if (items && length) {
    for (int i = 0; i < length; ++i) {
      if (!retain_ref(items[i])) { free_ref(a); return NULL; }
      a->items[i] = items[i];
    }
  }
  return reg_local(a);
}
static void *j_GetObjectArrayElement(void *env, void *arr, int i) {
  (void)env;
  FakeObjArray *a = arr;
  if (!a || a->tag != TAG_OBJARR || i < 0 || i >= a->len) return NULL;
  mutexLock(&locals_lock);
  void *item = a->items[i];
  const int retained = retain_ref(item);
  mutexUnlock(&locals_lock);
  return retained ? reg_local(item) : NULL;
}
static void j_SetObjectArrayElement(void *env, void *arr, int i, void *val) {
  (void)env;
  FakeObjArray *a = arr;
  if (!a || a->tag != TAG_OBJARR || i < 0 || i >= a->len) return;
  mutexLock(&locals_lock);
  if (a->items[i] == val) { mutexUnlock(&locals_lock); return; }
  if (!retain_ref(val)) { mutexUnlock(&locals_lock); return; }
  void *old = a->items[i];
  a->items[i] = val;
  mutexUnlock(&locals_lock);
  free_ref(old);
}

static void *j_GetPriArrayElements(void *env, void *arr, uint8_t *is_copy) {
  (void)env; if (is_copy) *is_copy = 0;
  FakePriArray *a = arr;
  return (a && a->tag == TAG_PRIARR) ? a->data : NULL;
}
static void j_ReleasePriArrayElements(void *env, void *arr, void *elems, int mode) {
  (void)env; (void)arr; (void)elems; (void)mode;
}
static void j_GetPriArrayRegion(void *env, void *arr, int start, int len, void *buf) {
  (void)env;
  FakePriArray *a = arr;
  if (a && a->tag == TAG_PRIARR && buf && start >= 0 && len >= 0 && start <= a->len && len <= a->len - start)
    memcpy(buf, (char *)a->data + (size_t)start * a->elem_size, (size_t)len * a->elem_size);
}
static void j_SetPriArrayRegion(void *env, void *arr, int start, int len, const void *buf) {
  (void)env;
  FakePriArray *a = arr;
  if (a && a->tag == TAG_PRIARR && buf && start >= 0 && len >= 0 && start <= a->len && len <= a->len - start)
    memcpy((char *)a->data + (size_t)start * a->elem_size, buf, (size_t)len * a->elem_size);
}

#define APP_VERSION_NAME SS_VERSION_NAME
#define APP_VERSION_CODE SS_VERSION_CODE
#define NX_SDK_INT       33

static void *field_object(const FakeID *id) {
  const char *n = id->name, *c = id->cls;
  if (!strcmp(n, "versionName")) return jni_make_string(APP_VERSION_NAME);
  if (!strcmp(n, "packageName")) return jni_make_string(SS_PACKAGE);
  if (!strcmp(n, "sourceDir") || !strcmp(n, "publicSourceDir"))
    return jni_make_string(managed_path(GAME_HOME));
  if (!strcmp(n, "nativeLibraryDir")) return jni_make_string(managed_path(GAME_HOME "/lib/arm64-v8a"));
  if (!strcmp(n, "dataDir") || !strcmp(n, "deviceProtectedDataDir") || !strcmp(n, "credentialProtectedDataDir"))
    return jni_make_string(managed_path(unity_internal_data_path()));
  if (!strcmp(n, "SUPPORTED_ABIS")) {
    void *items[] = { jni_make_string("arm64-v8a") }; return jni_make_object_array(1,items);
  }
  if (name_has(c, "provider/Settings$Secure") && !strcmp(n, "ANDROID_ID"))
    return jni_make_string("android_id");
  /* UnityPlayer.currentActivity must be non-null. */
  if (name_has(c, "unity3d/player/UnityPlayer")) {
    if (!strcmp(n, "currentActivity")) return jni_make_object("android/app/Activity");
    if (!strcmp(n, "MANUFACTURER"))    return jni_make_string("Nintendo");
  }
  /* AudioManager output properties consumed by CRIWARE. */
  if (name_has(c, "media/AudioManager")) {
    if (!strcmp(n, "PROPERTY_OUTPUT_FRAMES_PER_BUFFER")) { g_last_output_prop = 2; return jni_make_string("android.media.property.OUTPUT_FRAMES_PER_BUFFER"); }
    if (!strcmp(n, "PROPERTY_OUTPUT_SAMPLE_RATE"))       { g_last_output_prop = 1; return jni_make_string("android.media.property.OUTPUT_SAMPLE_RATE"); }
  }
  if (name_has(c, "content/Context")) {
    if (!strcmp(n, "AUDIO_SERVICE"))        return jni_make_string("audio");
    if (!strcmp(n, "DISPLAY_SERVICE"))      return jni_make_string("display");
    if (!strcmp(n, "WINDOW_SERVICE"))       return jni_make_string("window");
    if (!strcmp(n, "LOCATION_SERVICE"))     return jni_make_string("location");
    if (!strcmp(n, "CONNECTIVITY_SERVICE")) return jni_make_string("connectivity");
    if (!strcmp(n, "MEDIA_ROUTER_SERVICE")) return jni_make_string("media_router");
    if (!strcmp(n, "VIBRATOR_SERVICE"))     return jni_make_string("vibrator");
  }
  /* Match getExternalStorageState(). */
  if (name_has(c, "os/Environment")) {
    if (!strcmp(n, "MEDIA_MOUNTED"))           return jni_make_string("mounted");
    if (!strcmp(n, "MEDIA_MOUNTED_READ_ONLY")) return jni_make_string("mounted_ro");
  }
  if (name_has(c, "pm/PackageManager")) {
    if (!strcmp(n, "FEATURE_AUDIO_LOW_LATENCY")) return jni_make_string("android.hardware.audio.low_latency");
    if (!strcmp(n, "FEATURE_AUDIO_PRO"))         return jni_make_string("android.hardware.audio.pro");
  }
  if (name_has(c, "os/Build")) {
    if (!strcmp(n, "MODEL"))        return jni_make_string("Switch");
    if (!strcmp(n, "MANUFACTURER")) return jni_make_string("Nintendo");
    if (!strcmp(n, "BRAND"))        return jni_make_string("Nintendo");
    if (!strcmp(n, "DEVICE"))       return jni_make_string("Switch");
    if (!strcmp(n, "PRODUCT"))      return jni_make_string("Switch");
    if (!strcmp(n, "HARDWARE"))     return jni_make_string("nx");
    if (!strcmp(n, "BOARD"))        return jni_make_string("nx");
    if (!strcmp(n, "DISPLAY"))      return jni_make_string("nx");
    if (!strcmp(n, "ID"))           return jni_make_string("REL");
    if (!strcmp(n, "TYPE"))         return jni_make_string("user");
    if (!strcmp(n, "TAGS"))         return jni_make_string("release-keys");
    if (!strcmp(n, "FINGERPRINT"))  return jni_make_string("Nintendo/Switch/Switch:13/REL/10007:user/release-keys");
    if (!strcmp(n, "BOOTLOADER"))   return jni_make_string("unknown");
    if (!strcmp(n, "HOST"))         return jni_make_string("localhost");
    if (!strcmp(n, "USER"))         return jni_make_string("nx");
    if (!strcmp(n, "SERIAL"))       return jni_make_string("unknown");
    if (!strcmp(n, "RELEASE"))      return jni_make_string("13");        /* Build.VERSION.* */
    if (!strcmp(n, "CODENAME"))     return jni_make_string("REL");
    if (!strcmp(n, "INCREMENTAL"))  return jni_make_string("10007");
    if (!strcmp(n, "SECURITY_PATCH")) return jni_make_string("2023-01-01");
    if (!strcmp(n, "BASE_OS"))      return jni_make_string("");
  }
  if (name_has(c,"pm/PackageInfo") && !strcmp(n,"applicationInfo"))
    return jni_make_object("android/content/pm/ApplicationInfo");
  if (name_has(c,"pm/PackageInfo") && !strcmp(n,"signatures"))
    return make_apk_signature_array();
  if (name_has(c,"pm/PackageInfo") && !strcmp(n,"signingInfo"))
    return jni_make_object("android/content/pm/SigningInfo");
  if (name_has(c,"pm/PackageInfo") && !strcmp(n,"splitNames"))
    return jni_make_object_array(0,NULL);
  if (name_has(c,"pm/ApplicationInfo") && !strcmp(n,"className"))
    return jni_make_string("com.miHoYo.GetMobileInfo.MainApplication");
  /* Path fields feed Unity's persistent-data path. */
  if (sig_returns(id->sig, "Ljava/lang/String;") &&
      (name_has(n, "Dir") || name_has(n, "Path") || name_has(n, "path"))) {
    return jni_make_string(managed_path(GAME_HOME));
  }
  if (sig_returns(id->sig, "Ljava/lang/String;")) {
    return jni_make_string("");
  }
  return NULL;
}

static juint field_int(const FakeID *id) {
  const char *n = id->name, *c = id->cls;
  if (!strcmp(n, "versionCode")) return APP_VERSION_CODE;
  if (!strcmp(n, "longVersionCode")) return APP_VERSION_CODE;
  if (name_has(c, "unity3d/player/UnityPlayer")) {
    if (!strcmp(n, "SDK_INT"))     return NX_SDK_INT;
    if (!strcmp(n, "densityDpi"))  return 320;
    if (!strcmp(n, "widthPixels")) return (juint)screen_width;
    if (!strcmp(n, "heightPixels"))return (juint)screen_height;
    if (!strcmp(n, "STREAM_MUSIC"))return 3;   /* AudioManager.STREAM_MUSIC      */
    if (!strcmp(n, "GET_DEVICES_OUTPUTS")) return 2; /* AudioManager.GET_DEVICES_OUTPUTS */
    if (!strcmp(n, "ROUTE_TYPE_LIVE_VIDEO")) return 1;
    if (!strcmp(n, "SCREEN_ORIENTATION_UNSPECIFIED"))       return -1;
    if (!strcmp(n, "SCREEN_ORIENTATION_LANDSCAPE"))         return 0;
    if (!strcmp(n, "SCREEN_ORIENTATION_PORTRAIT"))          return 1;
    if (!strcmp(n, "SCREEN_ORIENTATION_REVERSE_LANDSCAPE")) return 8;
    if (!strcmp(n, "SCREEN_ORIENTATION_REVERSE_PORTRAIT"))  return 9;
    if (!strcmp(n, "SCREEN_ORIENTATION_FULL_USER"))         return 13;
    if (!strcmp(n, "SCREEN_ORIENTATION_FULL_SENSOR"))       return 10;
  }
  if (name_has(c, "content/Context") && !strcmp(n, "MODE_PRIVATE")) return 0;
  if (name_has(c, "pm/PackageManager")) {
    if (!strcmp(n, "PERMISSION_GRANTED")) return 0;   /* == granted              */
    if (!strcmp(n, "PERMISSION_DENIED"))  return (juint)-1;
    if (!strcmp(n, "GET_SIGNATURES")) return 0x00000040u;
    if (!strcmp(n, "GET_SIGNING_CERTIFICATES")) return 0x08000000u;
    if (!strcmp(n, "CERT_INPUT_RAW_X509")) return 0;
    if (!strcmp(n, "CERT_INPUT_SHA256")) return 1;
  }
  if (name_has(c, "os/Build")) {
    if (!strcmp(n, "SDK_INT"))          return NX_SDK_INT;
    if (!strcmp(n, "PREVIEW_SDK_INT"))  return 0;
  }
  if (name_has(c, "DisplayMetrics")) {
    if (!strcmp(n, "widthPixels"))  return (juint)screen_width;
    if (!strcmp(n, "heightPixels")) return (juint)screen_height;
    if (!strcmp(n, "densityDpi"))   return 320;    /* xhdpi bucket                */
  }
  return 0;
}

static float field_float(const FakeID *id) {
  const char *n = id->name;
  if (name_has(id->cls, "DisplayMetrics")) {
    if (!strcmp(n, "density") || !strcmp(n, "scaledDensity")) return 2.0f;
    if (!strcmp(n, "xdpi") || !strcmp(n, "ydpi"))             return 320.0f;
  }
  return 0.0f;
}

static void *j_GetObjectField(void *env, void *obj, void *fid) {
  (void)env; (void)obj; if (!fid) return NULL;
  return field_object((const FakeID *)fid); }
static juint j_GetIntField(void *env, void *obj, void *fid) {
  (void)env; (void)obj; if (!fid) return 0;
  return field_int((const FakeID *)fid); }
static juint j_GetLongField(void *env, void *obj, void *fid) {
  (void)env; (void)obj; if (!fid) return 0; return (juint)field_int((const FakeID *)fid); }
static juint j_GetBooleanField(void *env, void *obj, void *fid) {
  (void)env; (void)obj; if (!fid) return 0; return field_int((const FakeID *)fid) ? 1 : 0; }
static float j_GetFloatField(void *env, void *obj, void *fid) {
  (void)env; (void)obj; if (!fid) return 0.0f; return field_float((const FakeID *)fid); }
static double j_GetDoubleField(void *env, void *obj, void *fid) {
  (void)env; (void)obj; if (!fid) return 0.0; return (double)field_float((const FakeID *)fid); }

/* FakeID is both the direct JNI member ID and the opaque reflected member.
 * Preserve it across Unity's ReflectionHelper -> FromReflected* round trip.
 * Keep the historical generic fallbacks for unrelated proxy objects that do
 * not carry member metadata yet. */
static void *j_FromReflectedMethod(void *env, void *m) {
  (void)env;
  if (m && *(const uint32_t *)m == TAG_ID) return m;
  return get_id("java/lang/reflect/Method", "invoke", "()V");
}
static void *j_FromReflectedField(void *env, void *f) {
  (void)env;
  if (f && *(const uint32_t *)f == TAG_ID) return f;
  return get_id("java/lang/reflect/Field", "field", "()V");
}
static void *j_ToReflectedMethod(void *env, void *cls, void *mid, juint isStatic) {
  (void)env; (void)cls; (void)isStatic; return mid ? mid : jni_make_object("java/lang/reflect/Method"); }
static void *j_ToReflectedField(void *env, void *cls, void *fid, juint isStatic) {
  (void)env; (void)cls; (void)isStatic; return fid ? fid : jni_make_object("java/lang/reflect/Field"); }

typedef struct { const char *name, *signature; void *function; } NativeMethodIn;
typedef struct { char cls[256], name[128], signature[512]; void *function; } RegisteredNative;
#define MAX_REGISTERED_NATIVES 2048
static RegisteredNative registered_natives[MAX_REGISTERED_NATIVES];
static int registered_native_count;

static juint j_RegisterNatives(void *env, void *cls, void *methods_, int n) {
  (void)env;
  if (!cls || !methods_ || n < 0) return (juint)JNI_ERR;
  const NativeMethodIn *methods = methods_;
  const char *cn = class_name_of(cls);
  mutexLock(&locals_lock);
  for (int i = 0; i < n; ++i) {
    if (!methods[i].name || !methods[i].signature || !methods[i].function ||
        registered_native_count >= MAX_REGISTERED_NATIVES) {
      mutexUnlock(&locals_lock); return (juint)JNI_ERR;
    }
    RegisteredNative *r = NULL;
    for (int k = 0; k < registered_native_count; ++k)
      if (!strcmp(registered_natives[k].cls, cn) && !strcmp(registered_natives[k].name, methods[i].name) &&
          !strcmp(registered_natives[k].signature, methods[i].signature)) { r = &registered_natives[k]; break; }
    if (!r) r = &registered_natives[registered_native_count++];
    snprintf(r->cls, sizeof(r->cls), "%s", cn);
    snprintf(r->name, sizeof(r->name), "%s", methods[i].name);
    snprintf(r->signature, sizeof(r->signature), "%s", methods[i].signature);
    r->function = methods[i].function;
  }
  mutexUnlock(&locals_lock);
  return JNI_OK;
}

int jni_register_native(const char *class_name, const char *method_name,
                        const char *signature, void *function) {
  if (!class_name || !*class_name || !method_name || !*method_name ||
      !signature || !*signature || !function)
    return JNI_ERR;
  NativeMethodIn method = { method_name, signature, function };
  return (int)j_RegisterNatives(fake_env, intern_class(class_name), &method, 1);
}

static juint j_UnregisterNatives(void *env, void *cls) {
  (void)env; const char *cn = class_name_of(cls);
  mutexLock(&locals_lock);
  for (int i = 0; i < registered_native_count; ) {
    if (!strcmp(registered_natives[i].cls, cn)) registered_natives[i] = registered_natives[--registered_native_count];
    else ++i;
  }
  mutexUnlock(&locals_lock); return JNI_OK;
}

void *jni_find_registered_native(const char *class_name, const char *method_name, const char *signature) {
  void *out = NULL; mutexLock(&locals_lock);
  for (int i = 0; i < registered_native_count; ++i) {
    RegisteredNative *r = &registered_natives[i];
    if ((!class_name || !strcmp(r->cls, class_name)) && (!method_name || !strcmp(r->name, method_name)) &&
        (!signature || !strcmp(r->signature, signature))) { out = r->function; break; }
  }
  mutexUnlock(&locals_lock); return out;
}
int jni_registered_native_count(void) { int n; mutexLock(&locals_lock); n = registered_native_count; mutexUnlock(&locals_lock); return n; }

static juint j_GetJavaVM(void *env, void **vm) { (void)env; if (!vm) return (juint)JNI_ERR; *vm = fake_vm; return JNI_OK; }
static juint j_Throw(void *env, void *throwable) {
  (void)env;
  return set_pending_exception(throwable) ? JNI_OK : (juint)JNI_ERR;
}
static juint j_ThrowNew(void *env, void *clazz, const char *message) {
  (void)env; (void)clazz;
  return set_pending_message(message) ? JNI_OK : (juint)JNI_ERR;
}
static juint j_ExceptionCheck(void *env) { (void)env; return pending_exception != NULL; }
static void *j_ExceptionOccurred(void *env) { return j_NewLocalRef(env, pending_exception); }
static void j_ExceptionDescribe(void *env) { (void)env; if (pending_exception) fprintf(stderr, "JNI exception: %s\n", jni_string_utf(pending_exception)); }
static void j_ExceptionClear(void *env) { (void)env; set_pending_exception(NULL); }
int jni_exception_pending(void) { return pending_exception != NULL; }
void jni_exception_clear(void) { set_pending_exception(NULL); }
static void j_FatalError(void *env, const char *message) {
  (void)env; fprintf(stderr, "JNI fatal error: %s\n", message ? message : "(no message)"); jni_quit_requested = 1;
}
static juint j_MonitorEnter(void *env, void *obj) { (void)env; return obj ? JNI_OK : (juint)JNI_ERR; }
static juint j_MonitorExit(void *env, void *obj) { (void)env; return obj ? JNI_OK : (juint)JNI_ERR; }
static void *j_NewDirectByteBuffer(void *env, void *address, int64_t capacity) {
  (void)env; if (!address || capacity < 0) return NULL;
  FakeDirectBuffer *b = calloc(1, sizeof(*b)); if (!b) return NULL;
  b->tag = TAG_DIRECT; b->refs = 1; b->address = address; b->capacity = capacity; return reg_local(b);
}
static void *j_GetDirectBufferAddress(void *env, void *buffer) {
  (void)env; FakeDirectBuffer *b = buffer; return b && b->tag == TAG_DIRECT ? b->address : NULL;
}
static int64_t j_GetDirectBufferCapacity(void *env, void *buffer) {
  (void)env; FakeDirectBuffer *b = buffer; return b && b->tag == TAG_DIRECT ? b->capacity : -1;
}
static juint j_GetObjectRefType(void *env, void *obj) {
  (void)env;if(!obj)return 0;juint type=1;mutexLock(&locals_lock);
  for(int i=0;i<global_ref_count;++i)if(global_refs[i].obj==obj){type=2;break;}
  mutexUnlock(&locals_lock);return type;
}

static void *env_table[233];
static void **env_table_ptr = env_table;
/* Unused JNI slots return zero. */
static uint64_t j_unimplemented(void) { return 0; }
static void jni_fill_unimpl(void **table) {
  for (int i = 0; i < 233; ++i) table[i] = (void *)j_unimplemented;
}
void *jni_bytearray_data(void *arr, int *len_out) {
  FakePriArray *a = arr;
  if (a && a->tag == TAG_PRIARR) { if (len_out) *len_out = a->len; return a->data; }
  if (len_out) *len_out = 0;
  return NULL;
}
const char *jni_string_utf(void *jstr) {
  FakeString *s = jstr;
  return (s && s->tag == TAG_STRING) ? s->utf : "";
}

void *fake_env = &env_table_ptr;

static juint vm_DestroyJavaVM(void *vm) { (void)vm; return JNI_OK; }
static juint vm_AttachCurrentThread(void *vm, void **env, void *args) {
  (void)vm; (void)args;
  if (!env) return (juint)JNI_ERR;
  vm_attached = 1;
  *env = fake_env;
  return JNI_OK;
}
void jni_thread_exit_cleanup(void) {
  LocalState *s = &local_state;
  for (int i = 0; i < s->top; ++i) free_ref(s->refs[i]);
  free(s->refs);
  memset(s, 0, sizeof(*s));
  set_pending_exception(NULL);
  vm_attached = 0;
}

static int jni_bridge_selftest_calls;
static void *jni_bridge_selftest_invoke(void *env, void *bridge_class,
                                        int64_t native_handle,
                                        void *declaring_class, void *method_,
                                        void *arguments_) {
  (void)env;
  const FakeID *method = method_;
  const FakeObjArray *arguments = arguments_;
  if (class_name_of(bridge_class)[0] &&
      !strcmp(class_name_of(bridge_class), JNI_BRIDGE_CLASS) &&
      native_handle == INT64_C(0x1122334455667788) &&
      !strcmp(class_name_of(declaring_class),
              "android/os/Handler$Callback") &&
      method && method->tag == TAG_ID &&
      !strcmp(method->cls, "android/os/Handler$Callback") &&
      !strcmp(method->name, "handleMessage") &&
      !strcmp(method->sig, "(Landroid/os/Message;)Z") &&
      arguments && arguments->tag == TAG_OBJARR && arguments->len == 1 &&
      arguments->items[0] &&
      *(const uint32_t *)arguments->items[0] == TAG_MESSAGE)
    ++jni_bridge_selftest_calls;
  return jni_make_object("java/lang/Boolean");
}

int jni_reference_self_test(void) {
  LocalState *state = &local_state;
  if (state->top != 0 || state->frame_top != 0 || global_ref_count != 0 ||
      pending_exception != NULL)
    return 0;

  void *outer = jni_make_local_string("jni-selftest-outer");
  if (!outer || j_PushLocalFrame(NULL, 8) != JNI_OK) return 0;
  void *inner = jni_make_local_string("jni-selftest-inner");
  if (!inner || state->top != 2 || state->frames[0] != 1) return 0;

  void *global = j_NewGlobalRef(NULL, outer);
  if (global != outer || state->top != 2 || state->frames[0] != 1) return 0;
  j_DeleteLocalRef(NULL, outer);
  if (state->top != 1 || state->frames[0] != 0 ||
      strcmp(jni_string_utf(global), "jni-selftest-outer"))
    return 0;

  void *alias = j_NewLocalRef(NULL, global);
  j_DeleteGlobalRef(NULL, global);
  if (!alias || strcmp(jni_string_utf(alias), "jni-selftest-outer")) return 0;

  void *array = j_NewObjectArray(NULL, 1, NULL, alias);
  if (!array) return 0;
  j_DeleteLocalRef(NULL, alias); /* the array must keep the element alive */
  array = j_PopLocalFrame(NULL, array);
  if (!array || state->frame_top != 0 || state->top != 1) return 0;
  void *element = j_GetObjectArrayElement(NULL, array, 0);
  if (!element || strcmp(jni_string_utf(element), "jni-selftest-outer")) return 0;
  j_DeleteLocalRef(NULL, element);
  j_DeleteLocalRef(NULL, array);
  if (state->top != 0 || global_ref_count != 0) return 0;

  void *throwable = jni_make_local_string("jni-selftest-exception");
  if (!throwable || j_Throw(NULL, throwable) != JNI_OK) return 0;
  j_DeleteLocalRef(NULL, throwable);
  void *occurred = j_ExceptionOccurred(NULL);
  if (!occurred || strcmp(jni_string_utf(occurred), "jni-selftest-exception"))
    return 0;
  j_ExceptionClear(NULL);
  if (pending_exception != NULL ||
      strcmp(jni_string_utf(occurred), "jni-selftest-exception"))
    return 0;
  j_DeleteLocalRef(NULL, occurred);
  if (state->top != 0 || state->frame_top != 0 || global_ref_count != 0)
    return 0;

  /* Class-valued JNI functions have the same local/global ownership rules as
   * ordinary jobject returns, even though their backing objects are pooled. */
  void *local_class = j_FindClass(NULL, "jni/selftest/Class");
  if (!local_class || state->top != 1) return 0;
  void *global_class = j_NewGlobalRef(NULL, local_class);
  if (global_class != local_class || state->top != 1 || global_ref_count != 1)
    return 0;
  j_DeleteLocalRef(NULL, local_class);
  if (state->top != 0 || global_ref_count != 1) return 0;
  void *class_of_class = j_GetObjectClass(NULL, global_class);
  if (!class_of_class || strcmp(class_name_of(class_of_class), "java/lang/Class") ||
      state->top != 1)
    return 0;
  j_DeleteLocalRef(NULL, class_of_class);
  j_DeleteGlobalRef(NULL, global_class);
  if (state->top != 0 || state->frame_top != 0 || global_ref_count != 0)
    return 0;

  /* Reproduce Unity's exact Android input bootstrap without ART: the Java
   * interface proxy carries a native handle, Handler retains the proxy,
   * Message retains its target, and sendToTarget invokes the registered
   * JNIBridge callback with reflected handleMessage metadata. */
  jni_bridge_selftest_calls = 0;
  if (jni_register_native(JNI_BRIDGE_CLASS, "invoke",
                          JNI_BRIDGE_INVOKE_SIGNATURE,
                          (void *)jni_bridge_selftest_invoke) != JNI_OK)
    return 0;
  void *interface_class = intern_class("android/os/Handler$Callback");
  void *interfaces = jni_make_object_array(1, &interface_class);
  void *proxy = make_interface_proxy(INT64_C(0x1122334455667788), interfaces);
  void *handler = make_handler(proxy);
  void *message = make_message(handler, 0);
  const int dispatched = dispatch_handler_message(message);
  (void)j_UnregisterNatives(NULL, intern_class(JNI_BRIDGE_CLASS));
  j_DeleteLocalRef(NULL, message);
  j_DeleteLocalRef(NULL, handler);
  j_DeleteLocalRef(NULL, proxy);
  j_DeleteLocalRef(NULL, interfaces);
  return dispatched && jni_bridge_selftest_calls == 1 &&
         registered_native_count == 0 && state->top == 0 &&
         state->frame_top == 0 && global_ref_count == 0;
}
static juint vm_DetachCurrentThread(void *vm) {
  (void)vm;
  jni_thread_exit_cleanup();
  return JNI_OK;
}
static juint vm_GetEnv(void *vm, void **env, int version) {
  (void)vm;
  if (!env) return (juint)JNI_ERR;
  if (version != 0x00010001 && version != 0x00010002 && version != 0x00010004 && version != JNI_VERSION_1_6) {
    *env = NULL; return (juint)JNI_EVERSION;
  }
  if (!vm_attached) {
    *env = NULL;
    return (juint)JNI_EDETACHED;
  }
  *env = fake_env; return JNI_OK;
}
static void *vm_table[8];
static void **vm_table_ptr = vm_table;
void *fake_vm = &vm_table_ptr;

void jni_init(void) {
  mutexInit(&locals_lock);
  mutexInit(&handler_dispatch_lock);
  condvarInit(&handler_dispatch_cond);
  handler_dispatch_head = handler_dispatch_tail = NULL;
  handler_dispatch_active = 0;
  android_identity_init(GAME_HOME "/no_backup");
  init_apk_signature();
  local_state.top = local_state.frame_top = 0;
  istr_count = iobj_count = class_count = id_count = 0;
  registered_native_count = 0;
  global_ref_count = 0;
  set_pending_exception(NULL);
  vm_attached = 1; /* The launcher thread owns the process Java entrypoint. */

  jni_fill_unimpl(env_table);

  env_table[4]   = (void *)j_GetVersion;
  env_table[6]   = (void *)j_FindClass;
  env_table[7]   = (void *)j_FromReflectedMethod;    // was UNIMPL (proxy bind)
  env_table[8]   = (void *)j_FromReflectedField;
  env_table[9]   = (void *)j_ToReflectedMethod;
  env_table[10]  = (void *)j_GetSuperclass;
  env_table[11]  = (void *)j_IsAssignableFrom;
  env_table[12]  = (void *)j_ToReflectedField;
  env_table[13]  = (void *)j_Throw;
  env_table[14]  = (void *)j_ThrowNew;
  env_table[15]  = (void *)j_ExceptionOccurred;
  env_table[16]  = (void *)j_ExceptionDescribe;
  env_table[17]  = (void *)j_ExceptionClear;
  env_table[18]  = (void *)j_FatalError;
  env_table[19]  = (void *)j_PushLocalFrame;
  env_table[20]  = (void *)j_PopLocalFrame;
  env_table[21]  = (void *)j_NewGlobalRef;
  env_table[22]  = (void *)j_DeleteGlobalRef;
  env_table[23]  = (void *)j_DeleteLocalRef;
  env_table[24]  = (void *)j_IsSameObject;
  env_table[25]  = (void *)j_NewLocalRef;
  env_table[26]  = (void *)j_EnsureLocalCapacity;
  env_table[27]  = (void *)j_AllocObject;
  env_table[28]  = (void *)j_NewObject;
  env_table[29]  = (void *)j_NewObjectV;
  env_table[31]  = (void *)j_GetObjectClass;
  env_table[32]  = (void *)j_IsInstanceOf;
  env_table[33]  = (void *)j_GetMethodID;
  env_table[34]  = (void *)j_CallObjectMethod;
  env_table[35]  = (void *)j_CallObjectMethodV;
  env_table[37]  = (void *)j_CallBooleanMethod;
  env_table[38]  = (void *)j_CallBooleanMethodV;
  env_table[40]  = (void *)j_CallByteMethod;
  env_table[41]  = (void *)j_CallByteMethodV;
  env_table[43]  = (void *)j_CallCharMethod;
  env_table[44]  = (void *)j_CallCharMethodV;
  env_table[46]  = (void *)j_CallShortMethod;
  env_table[47]  = (void *)j_CallShortMethodV;
  env_table[49]  = (void *)j_CallIntMethod;
  env_table[50]  = (void *)j_CallIntMethodV;
  env_table[52]  = (void *)j_CallLongMethod;
  env_table[53]  = (void *)j_CallLongMethodV;
  env_table[55]  = (void *)j_CallFloatMethod;
  env_table[56]  = (void *)j_CallFloatMethodV;
  env_table[58]  = (void *)j_CallDoubleMethod;
  env_table[59]  = (void *)j_CallDoubleMethodV;
  env_table[61]  = (void *)j_CallVoidMethod;
  env_table[62]  = (void *)j_CallVoidMethodV;
  env_table[30]  = (void *)j_NewObjectA;
  env_table[36]  = (void *)j_CallObjectMethodA;
  env_table[39]  = (void *)j_CallBooleanMethodA;
  env_table[42]  = (void *)j_CallByteMethodA;
  env_table[45]  = (void *)j_CallCharMethodA;
  env_table[48]  = (void *)j_CallShortMethodA;
  env_table[51]  = (void *)j_CallIntMethodA;
  env_table[54]  = (void *)j_CallLongMethodA;
  env_table[57]  = (void *)j_CallFloatMethodA;
  env_table[60]  = (void *)j_CallDoubleMethodA;
  env_table[63]  = (void *)j_CallVoidMethodA;
  env_table[64]  = (void *)j_CallNonvirtualObjectMethod;
  env_table[65]  = (void *)j_CallNonvirtualObjectMethodV;
  env_table[66]  = (void *)j_CallNonvirtualObjectMethodA;
  env_table[67]  = (void *)j_CallNonvirtualBooleanMethod;
  env_table[68]  = (void *)j_CallNonvirtualBooleanMethodV;
  env_table[69]  = (void *)j_CallNonvirtualBooleanMethodA;
  env_table[70]  = (void *)j_CallNonvirtualByteMethod;
  env_table[71]  = (void *)j_CallNonvirtualByteMethodV;
  env_table[72]  = (void *)j_CallNonvirtualByteMethodA;
  env_table[73]  = (void *)j_CallNonvirtualCharMethod;
  env_table[74]  = (void *)j_CallNonvirtualCharMethodV;
  env_table[75]  = (void *)j_CallNonvirtualCharMethodA;
  env_table[76]  = (void *)j_CallNonvirtualShortMethod;
  env_table[77]  = (void *)j_CallNonvirtualShortMethodV;
  env_table[78]  = (void *)j_CallNonvirtualShortMethodA;
  env_table[79]  = (void *)j_CallNonvirtualIntMethod;
  env_table[80]  = (void *)j_CallNonvirtualIntMethodV;
  env_table[81]  = (void *)j_CallNonvirtualIntMethodA;
  env_table[82]  = (void *)j_CallNonvirtualLongMethod;
  env_table[83]  = (void *)j_CallNonvirtualLongMethodV;
  env_table[84]  = (void *)j_CallNonvirtualLongMethodA;
  env_table[85]  = (void *)j_CallNonvirtualFloatMethod;
  env_table[86]  = (void *)j_CallNonvirtualFloatMethodV;
  env_table[87]  = (void *)j_CallNonvirtualFloatMethodA;
  env_table[88]  = (void *)j_CallNonvirtualDoubleMethod;
  env_table[89]  = (void *)j_CallNonvirtualDoubleMethodV;
  env_table[90]  = (void *)j_CallNonvirtualDoubleMethodA;
  env_table[91]  = (void *)j_CallNonvirtualVoidMethod;
  env_table[92]  = (void *)j_CallNonvirtualVoidMethodV;
  env_table[93]  = (void *)j_CallNonvirtualVoidMethodA;
  env_table[94]  = (void *)j_GetFieldID;
  env_table[95]  = (void *)j_GetObjectField;
  env_table[96]  = (void *)j_GetBooleanField;        // GetBooleanField
  env_table[97]  = (void *)j_GetIntField;            // GetByteField
  env_table[98]  = (void *)j_GetIntField;            // GetCharField
  env_table[99]  = (void *)j_GetIntField;            // GetShortField
  env_table[100] = (void *)j_GetIntField;
  env_table[101] = (void *)j_GetLongField;           // GetLongField
  env_table[102] = (void *)j_GetFloatField;          // GetFloatField
  env_table[103] = (void *)j_GetDoubleField;
  env_table[113] = (void *)j_GetMethodID;            // GetStaticMethodID
  env_table[114] = (void *)j_CallStaticObjectMethod;
  env_table[115] = (void *)j_CallStaticObjectMethodV;
  env_table[117] = (void *)j_CallStaticBooleanMethod;
  env_table[118] = (void *)j_CallStaticBooleanMethodV;
  env_table[120] = (void *)j_CallStaticByteMethod;
  env_table[121] = (void *)j_CallStaticByteMethodV;
  env_table[123] = (void *)j_CallStaticCharMethod;
  env_table[124] = (void *)j_CallStaticCharMethodV;
  env_table[126] = (void *)j_CallStaticShortMethod;
  env_table[127] = (void *)j_CallStaticShortMethodV;
  env_table[129] = (void *)j_CallStaticIntMethod;
  env_table[130] = (void *)j_CallStaticIntMethodV;
  env_table[132] = (void *)j_CallStaticLongMethod;
  env_table[133] = (void *)j_CallStaticLongMethodV;
  env_table[135] = (void *)j_CallStaticFloatMethod;
  env_table[136] = (void *)j_CallStaticFloatMethodV;
  env_table[138] = (void *)j_CallStaticDoubleMethod;
  env_table[139] = (void *)j_CallStaticDoubleMethodV;
  env_table[141] = (void *)j_CallStaticVoidMethod;
  env_table[142] = (void *)j_CallStaticVoidMethodV;
  env_table[116] = (void *)j_CallStaticObjectMethodA;
  env_table[119] = (void *)j_CallStaticBooleanMethodA;
  env_table[122] = (void *)j_CallStaticByteMethodA;
  env_table[125] = (void *)j_CallStaticCharMethodA;
  env_table[128] = (void *)j_CallStaticShortMethodA;
  env_table[131] = (void *)j_CallStaticIntMethodA;
  env_table[134] = (void *)j_CallStaticLongMethodA;
  env_table[137] = (void *)j_CallStaticFloatMethodA;
  env_table[140] = (void *)j_CallStaticDoubleMethodA;
  env_table[143] = (void *)j_CallStaticVoidMethodA;
  env_table[144] = (void *)j_GetFieldID;             // GetStaticFieldID
  env_table[145] = (void *)j_GetObjectField;         // GetStaticObjectField
  env_table[146] = (void *)j_GetBooleanField;        // GetStaticBooleanField
  env_table[147] = (void *)j_GetIntField;
  env_table[148] = (void *)j_GetIntField;
  env_table[149] = (void *)j_GetIntField;
  env_table[150] = (void *)j_GetIntField;            // GetStaticIntField
  env_table[151] = (void *)j_GetLongField;           // GetStaticLongField
  env_table[152] = (void *)j_GetFloatField;          // GetStaticFloatField
  env_table[153] = (void *)j_GetDoubleField;
  env_table[163] = (void *)j_NewString;
  env_table[164] = (void *)j_GetStringLength;
  env_table[165] = (void *)j_GetStringChars;
  env_table[166] = (void *)j_ReleaseStringChars;
  env_table[167] = (void *)j_NewStringUTF;
  env_table[168] = (void *)j_GetStringUTFLength;
  env_table[169] = (void *)j_GetStringUTFChars;
  env_table[170] = (void *)j_ReleaseStringUTFChars;
  env_table[171] = (void *)j_GetArrayLength;
  env_table[172] = (void *)j_NewObjectArray;
  env_table[173] = (void *)j_GetObjectArrayElement;
  env_table[174] = (void *)j_SetObjectArrayElement;
  env_table[175] = (void *)j_NewBooleanArray;
  env_table[176] = (void *)j_NewByteArray;
  env_table[177] = (void *)j_NewCharArray;
  env_table[178] = (void *)j_NewShortArray;
  env_table[179] = (void *)j_NewIntArray;
  env_table[180] = (void *)j_NewLongArray;
  env_table[181] = (void *)j_NewFloatArray;
  env_table[182] = (void *)j_NewDoubleArray;
  for (int i = 183; i <= 190; i++) env_table[i] = (void *)j_GetPriArrayElements;
  for (int i = 191; i <= 198; i++) env_table[i] = (void *)j_ReleasePriArrayElements;
  for (int i = 199; i <= 206; i++) env_table[i] = (void *)j_GetPriArrayRegion;
  for (int i = 207; i <= 214; i++) env_table[i] = (void *)j_SetPriArrayRegion;
  env_table[215] = (void *)j_RegisterNatives;
  env_table[216] = (void *)j_UnregisterNatives;
  env_table[217] = (void *)j_MonitorEnter;
  env_table[218] = (void *)j_MonitorExit;
  env_table[219] = (void *)j_GetJavaVM;
  env_table[220] = (void *)j_GetStringRegion;
  env_table[221] = (void *)j_GetStringUTFRegion; // engine reads every string via this
  env_table[222] = (void *)j_GetPriArrayElements;     // GetPrimitiveArrayCritical
  env_table[223] = (void *)j_ReleasePriArrayElements; // ReleasePrimitiveArrayCritical
  env_table[224] = (void *)j_GetStringChars;
  env_table[225] = (void *)j_ReleaseStringChars;
  env_table[226] = (void *)j_NewGlobalRef;            // NewWeakGlobalRef
  env_table[227] = (void *)j_DeleteGlobalRef;         // DeleteWeakGlobalRef
  env_table[228] = (void *)j_ExceptionCheck;
  env_table[229] = (void *)j_NewDirectByteBuffer;
  env_table[230] = (void *)j_GetDirectBufferAddress;
  env_table[231] = (void *)j_GetDirectBufferCapacity;
  env_table[232] = (void *)j_GetObjectRefType;

  vm_table[3] = (void *)vm_DestroyJavaVM;
  vm_table[4] = (void *)vm_AttachCurrentThread;
  vm_table[5] = (void *)vm_DetachCurrentThread;
  vm_table[6] = (void *)vm_GetEnv;
  vm_table[7] = (void *)vm_AttachCurrentThread; // AttachCurrentThreadAsDaemon
}
