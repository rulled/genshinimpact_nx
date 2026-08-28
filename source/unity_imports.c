/* Supplemental Unity and IL2CPP imports. */
#include <stdint.h>
#include <stddef.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <wchar.h>
#include <wctype.h>
#include <ctype.h>
#include <errno.h>
#include <time.h>
#include <signal.h>
#include <fnmatch.h>
#include <arpa/inet.h>
#include <EGL/egl.h>
#include <zlib.h>
#include <switch.h>
#include <unistd.h>
#include "imports.h"
#include "libc_shim.h"
#include "android_native_unity.h"
#include "asset_pack.h"
#include "so_util.h"
#include "vulkan_egl_stubs.h"

/* Bionic exports _ctype_ as a pointer object, not as the table itself.  The
 * table has a leading EOF slot and callers index character c at c + 1. */
static char z_ctype_table[257];
static const char *z_ctype_ptr = z_ctype_table;
static void z_ctype_init(void) __attribute__((constructor));
static void z_ctype_init(void){
  for(int c=0;c<256;c++){
    unsigned char f=0;
    if(isupper(c))  f|=0x01;
    if(islower(c))  f|=0x02;
    if(isdigit(c))  f|=0x04;
    if(isspace(c))  f|=0x08;
    if(ispunct(c))  f|=0x10;
    if(iscntrl(c))  f|=0x20;
    if(isxdigit(c)) f|=0x40;
    if(c==' ')      f|=0x80;
    z_ctype_table[c+1]=(char)f;
  }
}

/* Unavailable optional APIs. */
static long z_stub0(void){ return 0; }
static int z_vprintf_noop(const char *fmt, va_list va){ (void)fmt; (void)va; return 0; }

/* POSIX drand48 state transition, used only for Unity's non-cryptographic
 * random calls.  Keep it independent from the entropy-backed Android APIs. */
static Mutex z_rand48_lock;
static uint64_t z_rand48_state = UINT64_C(0x1234abcd330e);
static void z_srand48(long seed) {
  mutexLock(&z_rand48_lock);
  z_rand48_state = (((uint64_t)(uint32_t)seed << 16) | UINT64_C(0x330e)) &
                   UINT64_C(0xffffffffffff);
  mutexUnlock(&z_rand48_lock);
}
static long z_lrand48(void) {
  mutexLock(&z_rand48_lock);
  z_rand48_state = (z_rand48_state * UINT64_C(0x5deece66d) + UINT64_C(0xb)) &
                   UINT64_C(0xffffffffffff);
  const long value = (long)(z_rand48_state >> 17);
  mutexUnlock(&z_rand48_lock);
  return value;
}

#define MEDIA_KEY(symbol, value) static const char *z_##symbol = value
MEDIA_KEY(AMEDIAFORMAT_KEY_CHANNEL_COUNT, "channel-count");
MEDIA_KEY(AMEDIAFORMAT_KEY_COLOR_FORMAT, "color-format");
MEDIA_KEY(AMEDIAFORMAT_KEY_COLOR_RANGE, "color-range");
MEDIA_KEY(AMEDIAFORMAT_KEY_COLOR_STANDARD, "color-standard");
MEDIA_KEY(AMEDIAFORMAT_KEY_DURATION, "durationUs");
MEDIA_KEY(AMEDIAFORMAT_KEY_ENCODER_DELAY, "encoder-delay");
MEDIA_KEY(AMEDIAFORMAT_KEY_FRAME_RATE, "frame-rate");
MEDIA_KEY(AMEDIAFORMAT_KEY_HEIGHT, "height");
MEDIA_KEY(AMEDIAFORMAT_KEY_IS_ADTS, "is-adts");
MEDIA_KEY(AMEDIAFORMAT_KEY_LANGUAGE, "language");
MEDIA_KEY(AMEDIAFORMAT_KEY_MIME, "mime");
MEDIA_KEY(AMEDIAFORMAT_KEY_ROTATION, "rotation-degrees");
MEDIA_KEY(AMEDIAFORMAT_KEY_SAMPLE_RATE, "sample-rate");
MEDIA_KEY(AMEDIAFORMAT_KEY_SLICE_HEIGHT, "slice-height");
MEDIA_KEY(AMEDIAFORMAT_KEY_STRIDE, "stride");
MEDIA_KEY(AMEDIAFORMAT_KEY_WIDTH, "width");
#undef MEDIA_KEY

enum { Z_MEDIA_STRING = 1, Z_MEDIA_BUFFER = 2, Z_MEDIA_INT32 = 3 };
typedef struct {
  char *key;
  int kind;
  void *value;
  size_t size;
  int64_t integer;
} ZMediaEntry;
typedef struct {
  uint32_t tag;
  size_t count, capacity;
  ZMediaEntry *entries;
} ZMediaFormat;
#define Z_MEDIA_FORMAT_TAG 0x4e584d46u /* NXMF */

static ZMediaEntry *z_media_entry(ZMediaFormat *format, const char *key) {
  if (!format || format->tag != Z_MEDIA_FORMAT_TAG || !key || !key[0]) return NULL;
  for (size_t i=0;i<format->count;++i) if(!strcmp(format->entries[i].key,key)) return &format->entries[i];
  if (format->count == format->capacity) {
    size_t next=format->capacity ? format->capacity*2 : 8;
    if(next>128)return NULL;
    ZMediaEntry *entries=realloc(format->entries,next*sizeof(*entries)); if(!entries)return NULL;
    memset(entries+format->capacity,0,(next-format->capacity)*sizeof(*entries));
    format->entries=entries;format->capacity=next;
  }
  ZMediaEntry *entry=&format->entries[format->count++];
  entry->key=strdup(key); if(!entry->key){--format->count;return NULL;} return entry;
}
static void *z_AMediaFormat_new(void) {
  ZMediaFormat *format=calloc(1,sizeof(*format)); if(format)format->tag=Z_MEDIA_FORMAT_TAG; return format;
}
static void z_AMediaFormat_delete(void *format_) {
  ZMediaFormat *format=format_; if(!format||format->tag!=Z_MEDIA_FORMAT_TAG)return;
  format->tag=0;
  for(size_t i=0;i<format->count;++i){free(format->entries[i].key);free(format->entries[i].value);}
  free(format->entries);free(format);
}
static void z_AMediaFormat_setString(void *format_, const char *key, const char *value) {
  if(!value)return;
  size_t size=strnlen(value,1024*1024);if(size==1024*1024)return;
  ZMediaEntry *entry=z_media_entry(format_,key);if(!entry)return;
  char *copy=malloc(size+1);if(!copy)return;memcpy(copy,value,size);copy[size]='\0';
  free(entry->value);entry->value=copy;entry->size=size;entry->kind=Z_MEDIA_STRING;
}
static void z_AMediaFormat_setBuffer(void *format_, const char *key, const void *data, size_t size) {
  if((!data&&size)||size>64u*1024u*1024u)return;
  ZMediaEntry *entry=z_media_entry(format_,key);if(!entry)return;
  void *copy=malloc(size?size:1);if(!copy)return;if(size)memcpy(copy,data,size);
  free(entry->value);entry->value=copy;entry->size=size;entry->kind=Z_MEDIA_BUFFER;
}
static void z_AMediaFormat_setInt32(void *format_, const char *key, int32_t value) {
  ZMediaEntry *entry=z_media_entry(format_,key);if(!entry)return;
  free(entry->value);entry->value=NULL;entry->size=sizeof(value);
  entry->integer=value;entry->kind=Z_MEDIA_INT32;
}
static int z_AMediaFormat_getInt32(void *format_, const char *key, int32_t *value) {
  ZMediaFormat *format=format_;
  if(!format||format->tag!=Z_MEDIA_FORMAT_TAG||!key||!value)return 0;
  for(size_t i=0;i<format->count;++i)if(!strcmp(format->entries[i].key,key)){
    if(format->entries[i].kind!=Z_MEDIA_INT32)return 0;
    *value=(int32_t)format->entries[i].integer;return 1;
  }
  return 0;
}

/* Horizon exposes no Android MediaCodec service.  Report a coherent hardware
 * decoder failure so CRI can select its bundled software path; returning zero
 * for every signature falsely meant both a NULL codec and AMEDIA_OK. */
#define Z_AMEDIA_ERROR_UNKNOWN (-10000)
#define Z_AMEDIA_ERROR_UNSUPPORTED (-10002)
#define Z_AMEDIACODEC_INFO_TRY_AGAIN_LATER (-1)
static void *z_AMediaCodec_createDecoderByType(const char *mime){(void)mime;return NULL;}
static int z_AMediaCodec_configure(void *codec,void *format,void *surface,
                                   void *crypto,uint32_t flags){
  (void)codec;(void)format;(void)surface;(void)crypto;(void)flags;
  return Z_AMEDIA_ERROR_UNKNOWN;
}
static int z_AMediaCodec_status_fail(void *codec){(void)codec;return Z_AMEDIA_ERROR_UNKNOWN;}
static long z_AMediaCodec_dequeueInputBuffer(void *codec,int64_t timeout_us){
  (void)codec;(void)timeout_us;return Z_AMEDIACODEC_INFO_TRY_AGAIN_LATER;
}
static long z_AMediaCodec_dequeueOutputBuffer(void *codec,void *info,int64_t timeout_us){
  (void)codec;(void)info;(void)timeout_us;
  return Z_AMEDIACODEC_INFO_TRY_AGAIN_LATER;
}
static void *z_AMediaCodec_getBuffer(void *codec,size_t index,size_t *size){
  (void)codec;(void)index;if(size)*size=0;return NULL;
}
static void *z_AMediaCodec_getOutputFormat(void *codec){(void)codec;return NULL;}
static int z_AMediaCodec_queueInputBuffer(void *codec,size_t index,size_t offset,
                                          size_t size,uint64_t time,uint32_t flags){
  (void)codec;(void)index;(void)offset;(void)size;(void)time;(void)flags;
  return Z_AMEDIA_ERROR_UNKNOWN;
}
static int z_AMediaCodec_releaseOutputBuffer(void *codec,size_t index,int render){
  (void)codec;(void)index;(void)render;return Z_AMEDIA_ERROR_UNKNOWN;
}
static int z_AMediaCodec_setOutputSurface(void *codec,void *surface){
  (void)codec;(void)surface;return Z_AMEDIA_ERROR_UNKNOWN;
}

/* Horizon has no Android GraphicBuffer, ImageReader, or MediaExtractor
 * services.  Preserve their NDK signatures while failing deterministically:
 * status APIs never report AMEDIA_OK, pointer outputs are cleared, and
 * extractor cursors report end-of-stream instead of a fabricated sample. */
typedef struct {
  uint32_t width;
  uint32_t height;
  uint32_t layers;
  uint32_t format;
  uint64_t usage;
  uint32_t stride;
  uint32_t rfu0;
  uint64_t rfu1;
} ZAHardwareBufferDesc;
_Static_assert(sizeof(ZAHardwareBufferDesc) == 40, "AHardwareBuffer_Desc arm64 size");
_Static_assert(offsetof(ZAHardwareBufferDesc, usage) == 16, "AHardwareBuffer_Desc usage offset");
_Static_assert(offsetof(ZAHardwareBufferDesc, stride) == 24, "AHardwareBuffer_Desc stride offset");
_Static_assert(offsetof(ZAHardwareBufferDesc, rfu1) == 32, "AHardwareBuffer_Desc rfu1 offset");

static void z_AHardwareBuffer_acquire(void *buffer){(void)buffer;}
static void z_AHardwareBuffer_describe(const void *buffer,ZAHardwareBufferDesc *out_desc){
  (void)buffer;if(out_desc)memset(out_desc,0,sizeof(*out_desc));
}
static void z_AHardwareBuffer_release(void *buffer){(void)buffer;}

static void z_AImage_delete(void *image){(void)image;}
static void z_AImage_deleteAsync(void *image,int release_fence_fd){
  (void)image;(void)release_fence_fd;
}
static int z_AImage_getHardwareBuffer(const void *image,void **buffer){
  (void)image;if(buffer)*buffer=NULL;return Z_AMEDIA_ERROR_UNSUPPORTED;
}
static int z_AImage_getTimestamp(const void *image,int64_t *timestamp_ns){
  (void)image;if(timestamp_ns)*timestamp_ns=0;return Z_AMEDIA_ERROR_UNSUPPORTED;
}
static int z_AImage_getWidth(const void *image,int32_t *width){
  (void)image;if(width)*width=0;return Z_AMEDIA_ERROR_UNSUPPORTED;
}

static int z_AImageReader_acquireLatestImage(void *reader,void **image){
  (void)reader;if(image)*image=NULL;return Z_AMEDIA_ERROR_UNSUPPORTED;
}
static void z_AImageReader_delete(void *reader){(void)reader;}
static int z_AImageReader_getWindow(void *reader,void **window){
  (void)reader;if(window)*window=NULL;return Z_AMEDIA_ERROR_UNSUPPORTED;
}
static int z_AImageReader_newWithUsage(int32_t width,int32_t height,int32_t format,
                                       uint64_t usage,int32_t max_images,void **reader){
  (void)width;(void)height;(void)format;(void)usage;(void)max_images;
  if(reader)*reader=NULL;
  return Z_AMEDIA_ERROR_UNSUPPORTED;
}
static int z_AImageReader_setBufferRemovedListener(void *reader,const void *listener){
  (void)reader;(void)listener;return Z_AMEDIA_ERROR_UNSUPPORTED;
}
static int z_AImageReader_setImageListener(void *reader,const void *listener){
  (void)reader;(void)listener;return Z_AMEDIA_ERROR_UNSUPPORTED;
}

static _Bool z_AMediaExtractor_advance(void *extractor){(void)extractor;return 0;}
static int z_AMediaExtractor_delete(void *extractor){
  (void)extractor;return Z_AMEDIA_ERROR_UNSUPPORTED;
}
static int64_t z_AMediaExtractor_getSampleTime(void *extractor){(void)extractor;return -1;}
static int z_AMediaExtractor_getSampleTrackIndex(void *extractor){(void)extractor;return -1;}
static size_t z_AMediaExtractor_getTrackCount(void *extractor){(void)extractor;return 0;}
static void *z_AMediaExtractor_getTrackFormat(void *extractor,size_t index){
  (void)extractor;(void)index;return NULL;
}
static void *z_AMediaExtractor_new(void){return NULL;}
static ssize_t z_AMediaExtractor_readSampleData(void *extractor,uint8_t *buffer,size_t capacity){
  (void)extractor;(void)buffer;(void)capacity;return -1;
}
static int z_AMediaExtractor_seekTo(void *extractor,int64_t seek_us,int mode){
  (void)extractor;(void)seek_us;(void)mode;return Z_AMEDIA_ERROR_UNSUPPORTED;
}
static int z_AMediaExtractor_selectTrack(void *extractor,size_t index){
  (void)extractor;(void)index;return Z_AMEDIA_ERROR_UNSUPPORTED;
}
static int z_AMediaExtractor_setDataSource(void *extractor,const char *location){
  (void)extractor;(void)location;return Z_AMEDIA_ERROR_UNSUPPORTED;
}
static int z_AMediaExtractor_setDataSourceCustom(void *extractor,void *source){
  (void)extractor;(void)source;return Z_AMEDIA_ERROR_UNSUPPORTED;
}
static int z_AMediaExtractor_setDataSourceFd(void *extractor,int fd,int64_t offset,int64_t length){
  (void)extractor;(void)fd;(void)offset;(void)length;return Z_AMEDIA_ERROR_UNSUPPORTED;
}

/* Android Dynamic Performance Framework thermal surface used by
 * libmiHoYoAndroidADPF.so.  Zero means no thermal throttling/headroom use. */
typedef struct { uint32_t tag; } ZThermalManager;
static ZThermalManager z_thermal_manager={0x4e585448u}; /* NXTH */
static int z_android_get_device_api_level(void){return 33;}
static void *z_AThermal_acquireManager(void){return &z_thermal_manager;}
static void z_AThermal_releaseManager(void *manager){(void)manager;}
static int z_AThermal_getCurrentThermalStatus(void *manager){(void)manager;return 0;}
static float z_AThermal_getThermalHeadroom(void *manager,int forecast_seconds){
  (void)manager;(void)forecast_seconds;return 0.0f;
}

enum {
  Z_PR_SET_PDEATHSIG = 1,
  Z_PR_GET_PDEATHSIG = 2,
  Z_PR_GET_DUMPABLE = 3,
  Z_PR_SET_DUMPABLE = 4,
  Z_PR_SET_NAME = 15,
  Z_PR_GET_NAME = 16,
  Z_PR_GET_SECCOMP = 21,
  Z_PR_SET_TIMERSLACK = 29,
  Z_PR_GET_TIMERSLACK = 30,
  Z_PR_SET_NO_NEW_PRIVS = 38,
  Z_PR_GET_NO_NEW_PRIVS = 39,
};
#define Z_PR_SET_PTRACER 0x59616d61
static _Thread_local char z_prctl_thread_name[16] = "Unity";
static _Thread_local unsigned long z_prctl_timer_slack_ns = 50000;
static _Thread_local int z_prctl_no_new_privs;
static int z_prctl_dumpable = 1;

static long z_prctl(int option, unsigned long a2, unsigned long a3,
                    unsigned long a4, unsigned long a5){
  switch (option) {
    case Z_PR_SET_PDEATHSIG:
      /* Horizon has no guest parent process to notify. */
      return 0;
    case Z_PR_GET_PDEATHSIG:
      if (!a2) { errno = EFAULT; return -1; }
      *(int *)(uintptr_t)a2 = 0;
      return 0;
    case Z_PR_GET_DUMPABLE:
      return __atomic_load_n(&z_prctl_dumpable, __ATOMIC_RELAXED);
    case Z_PR_SET_DUMPABLE:
      if (a2 > 1) { errno = EINVAL; return -1; }
      __atomic_store_n(&z_prctl_dumpable, (int)a2, __ATOMIC_RELAXED);
      return 0;
    case Z_PR_SET_NAME: {
      if (!a2) { errno = EFAULT; return -1; }
      const char *name = (const char *)(uintptr_t)a2;
      size_t length = strnlen(name, sizeof(z_prctl_thread_name) - 1u);
      memcpy(z_prctl_thread_name, name, length);
      z_prctl_thread_name[length] = '\0';
      memset(z_prctl_thread_name + length + 1, 0,
             sizeof(z_prctl_thread_name) - length - 1u);
      return 0;
    }
    case Z_PR_GET_NAME:
      if (!a2) { errno = EFAULT; return -1; }
      memcpy((void *)(uintptr_t)a2, z_prctl_thread_name,
             sizeof(z_prctl_thread_name));
      return 0;
    case Z_PR_GET_SECCOMP:
      return 0;
    case Z_PR_SET_TIMERSLACK:
      z_prctl_timer_slack_ns = a2 ? a2 : 50000;
      return 0;
    case Z_PR_GET_TIMERSLACK:
      return (long)z_prctl_timer_slack_ns;
    case Z_PR_SET_NO_NEW_PRIVS:
      if (a2 != 1 || a3 || a4 || a5) { errno = EINVAL; return -1; }
      z_prctl_no_new_privs = 1;
      return 0;
    case Z_PR_GET_NO_NEW_PRIVS:
      if (a2 || a3 || a4 || a5) { errno = EINVAL; return -1; }
      return z_prctl_no_new_privs;
    case Z_PR_SET_PTRACER:
      /* Crash reporters may nominate a dumper which cannot exist on Horizon. */
      return 0;
    default:
      errno = ENOSYS;
      return -1;
  }
}
static int z_pthread_setname_np(void *thread, const char *name){
  if (!thread || !name) return EINVAL;
  if (strnlen(name, sizeof(z_prctl_thread_name)) >=
      sizeof(z_prctl_thread_name)) return ERANGE;
  const pthread_t target = (pthread_t)thread;
  if (pthread_equal(target, pthread_self())) {
    snprintf(z_prctl_thread_name, sizeof(z_prctl_thread_name), "%s", name);
    return 0;
  }
  /* Horizon/libnx has no public thread-name setter.  Validate that a
   * non-current guest handle is still live, then accept the diagnostic hint;
   * PR_GET_NAME is defined only for the calling thread and remains exact. */
  NxGuestThreadRef *ref = nx_guest_thread_acquire(target);
  if (!ref) return ESRCH;
  nx_guest_thread_release(ref);
  return 0;
}

/* Crash reporters probe ptrace before entering their helper-process path.
 * Horizon has no Android process relationship or ptrace target here, so fail
 * coherently instead of reporting success and letting the caller attempt
 * waitpid/peek operations against a process that cannot exist. */
static long z_ptrace(int request, long pid, void *address, void *data) {
  (void)request;
  (void)pid;
  (void)address;
  (void)data;
  errno = 38; /* arm64 Bionic ENOSYS */
  return -1;
}

/* Keep affinity queries consistent with sysconf_fake(), which exposes the
 * three application CPU cores.  Thread placement remains controlled by
 * libnx, but callers receive initialized masks and invalid requests fail. */
static int z_sched_getaffinity(int pid, size_t bytes, void *mask) {
  (void)pid;
  if (!mask) { errno = EFAULT; return -1; }
  if (!bytes) { errno = EINVAL; return -1; }
  memset(mask, 0, bytes);
  ((unsigned char *)mask)[0] = 0x07u;
  return 0;
}

static int z_sched_setaffinity(int pid, size_t bytes, const void *mask) {
  (void)pid;
  if (!mask) { errno = EFAULT; return -1; }
  if (!bytes || !(((const unsigned char *)mask)[0] & 0x07u)) {
    errno = EINVAL;
    return -1;
  }
  return 0;
}

/* Report the current thread's actual libnx stack mirror to IL2CPP.  A raw
 * svcQueryMemory() result describes a Horizon virtual-memory region, which is
 * not necessarily the usable stack interval recorded by threadCreate().
 * Unity's conservative collector trusts this pair as its root-scan bounds, so
 * prefer the exact Thread metadata and retain the query only for host-owned
 * threads whose runtime descriptor has no usable stack information. */
int z_pthread_attr_getstack(const void *attr, void **stackaddr, size_t *stacksize){
  (void)attr;
  uintptr_t sp; __asm__ volatile("mov %0, sp" : "=r"(sp));
  void *base = NULL; size_t sz = 0;
  Thread *self = threadGetSelf();
  if (self && self->stack_mirror && self->stack_sz) {
    const uintptr_t start = (uintptr_t)self->stack_mirror;
    if (start <= UINTPTR_MAX - self->stack_sz &&
        sp >= start && sp <= start + self->stack_sz) {
      base = self->stack_mirror;
      sz = self->stack_sz;
    }
  }
  MemoryInfo mi; u32 pi;
  if (!base && R_SUCCEEDED(svcQueryMemory(&mi, &pi, (u64)sp)) && mi.size){
    base = (void *)(uintptr_t)mi.addr;
    sz   = (size_t)mi.size;
  } else if (!base) {
    base = (void *)(sp & ~0xFFFFFull);
    sz   = 0x100000;
  }
  if (stackaddr) *stackaddr = base;
  if (stacksize) *stacksize = sz;
  return 0;
}
int z_pthread_getattr_np(void *thread, void *attr){
  if (!thread || !attr) return EINVAL;
  if (!pthread_equal((pthread_t)thread, pthread_self())) return ENOTSUP;
  return pthread_attr_init_fake(attr);
}

int z_getpagesize(void){ return 0x1000; }
int z_pthread_equal(unsigned long a, unsigned long b){ return a==b; }
int z_gettid(void){ return 1; }
int z_dup(int fd){
  return dup_fake(fd);
}
/* JNI device properties may be null. */
int z_strcasecmp(const char *a, const char *b){
  if (a == b) return 0;
  if (!a) return -1;
  if (!b) return 1;
  return strcasecmp(a, b);
}
char *z_basename(const char *path){
  if (!path || !*path) return (char *)".";
  const char *s = strrchr(path, '/');
  return (char *)(s ? s + 1 : path);
}
/* Locale objects are ignored by newlib's character classifiers. */
int z_isdigit_l (int c, void *l){ (void)l; return isdigit(c); }
int z_islower_l (int c, void *l){ (void)l; return islower(c); }
int z_isupper_l (int c, void *l){ (void)l; return isupper(c); }
int z_isxdigit_l(int c, void *l){ (void)l; return isxdigit(c); }
int z_tolower_l (int c, void *l){ (void)l; return tolower(c); }
int z_toupper_l (int c, void *l){ (void)l; return toupper(c); }
void*z_memrchr(const void*s,int c,unsigned long n){ const unsigned char*p=(const unsigned char*)s+n; while(n--){ if(*--p==(unsigned char)c) return (void*)p; } return 0; }

DynLibFunction unity_dynlib_functions[] = {
  { "ALooper_acquire", (uintptr_t)&ALooper_acquire },
  { "ALooper_forThread", (uintptr_t)&ALooper_forThread },
  { "ALooper_release", (uintptr_t)&ALooper_release },
  { "ALooper_wake", (uintptr_t)&ALooper_wake },
  { "ANativeWindow_acquire", (uintptr_t)&ANativeWindow_acquire },
  { "ANativeWindow_fromSurface", (uintptr_t)&ANativeWindow_fromSurface },
  { "ANativeWindow_getHeight", (uintptr_t)&ANativeWindow_getHeight },
  { "ANativeWindow_getWidth", (uintptr_t)&ANativeWindow_getWidth },
  { "ANativeWindow_release", (uintptr_t)&ANativeWindow_release },
#define OPTIONAL_MEDIA_FN(symbol) { #symbol, (uintptr_t)&z_stub0 }
  { "AHardwareBuffer_acquire", (uintptr_t)&z_AHardwareBuffer_acquire },
  { "AHardwareBuffer_describe", (uintptr_t)&z_AHardwareBuffer_describe },
  { "AHardwareBuffer_release", (uintptr_t)&z_AHardwareBuffer_release },
  { "AImage_delete", (uintptr_t)&z_AImage_delete },
  { "AImage_deleteAsync", (uintptr_t)&z_AImage_deleteAsync },
  { "AImage_getHardwareBuffer", (uintptr_t)&z_AImage_getHardwareBuffer },
  { "AImage_getTimestamp", (uintptr_t)&z_AImage_getTimestamp },
  { "AImage_getWidth", (uintptr_t)&z_AImage_getWidth },
  { "AImageReader_acquireLatestImage", (uintptr_t)&z_AImageReader_acquireLatestImage },
  { "AImageReader_delete", (uintptr_t)&z_AImageReader_delete },
  { "AImageReader_getWindow", (uintptr_t)&z_AImageReader_getWindow },
  { "AImageReader_newWithUsage", (uintptr_t)&z_AImageReader_newWithUsage },
  { "AImageReader_setBufferRemovedListener", (uintptr_t)&z_AImageReader_setBufferRemovedListener },
  { "AImageReader_setImageListener", (uintptr_t)&z_AImageReader_setImageListener },
  { "AMediaCodec_configure", (uintptr_t)&z_AMediaCodec_configure },
  { "AMediaCodec_createDecoderByType", (uintptr_t)&z_AMediaCodec_createDecoderByType },
  { "AMediaCodec_delete", (uintptr_t)&z_AMediaCodec_status_fail },
  { "AMediaCodec_dequeueInputBuffer", (uintptr_t)&z_AMediaCodec_dequeueInputBuffer },
  { "AMediaCodec_dequeueOutputBuffer", (uintptr_t)&z_AMediaCodec_dequeueOutputBuffer },
  { "AMediaCodec_flush", (uintptr_t)&z_AMediaCodec_status_fail },
  { "AMediaCodec_getInputBuffer", (uintptr_t)&z_AMediaCodec_getBuffer },
  { "AMediaCodec_getOutputBuffer", (uintptr_t)&z_AMediaCodec_getBuffer },
  { "AMediaCodec_getOutputFormat", (uintptr_t)&z_AMediaCodec_getOutputFormat },
  { "AMediaCodec_queueInputBuffer", (uintptr_t)&z_AMediaCodec_queueInputBuffer },
  { "AMediaCodec_releaseOutputBuffer", (uintptr_t)&z_AMediaCodec_releaseOutputBuffer },
  { "AMediaCodec_setOutputSurface", (uintptr_t)&z_AMediaCodec_setOutputSurface },
  { "AMediaCodec_start", (uintptr_t)&z_AMediaCodec_status_fail },
  { "AMediaCodec_stop", (uintptr_t)&z_AMediaCodec_status_fail },
  OPTIONAL_MEDIA_FN(AMediaDataSource_delete),
  OPTIONAL_MEDIA_FN(AMediaDataSource_new),
  OPTIONAL_MEDIA_FN(AMediaDataSource_setClose),
  OPTIONAL_MEDIA_FN(AMediaDataSource_setGetSize),
  OPTIONAL_MEDIA_FN(AMediaDataSource_setReadAt),
  OPTIONAL_MEDIA_FN(AMediaDataSource_setUserdata),
  { "AMediaExtractor_advance", (uintptr_t)&z_AMediaExtractor_advance },
  { "AMediaExtractor_delete", (uintptr_t)&z_AMediaExtractor_delete },
  { "AMediaExtractor_getSampleTime", (uintptr_t)&z_AMediaExtractor_getSampleTime },
  { "AMediaExtractor_getSampleTrackIndex", (uintptr_t)&z_AMediaExtractor_getSampleTrackIndex },
  { "AMediaExtractor_getTrackCount", (uintptr_t)&z_AMediaExtractor_getTrackCount },
  { "AMediaExtractor_getTrackFormat", (uintptr_t)&z_AMediaExtractor_getTrackFormat },
  { "AMediaExtractor_new", (uintptr_t)&z_AMediaExtractor_new },
  { "AMediaExtractor_readSampleData", (uintptr_t)&z_AMediaExtractor_readSampleData },
  { "AMediaExtractor_seekTo", (uintptr_t)&z_AMediaExtractor_seekTo },
  { "AMediaExtractor_selectTrack", (uintptr_t)&z_AMediaExtractor_selectTrack },
  { "AMediaExtractor_setDataSource", (uintptr_t)&z_AMediaExtractor_setDataSource },
  { "AMediaExtractor_setDataSourceCustom", (uintptr_t)&z_AMediaExtractor_setDataSourceCustom },
  { "AMediaExtractor_setDataSourceFd", (uintptr_t)&z_AMediaExtractor_setDataSourceFd },
  { "AMediaFormat_delete", (uintptr_t)&z_AMediaFormat_delete },
  OPTIONAL_MEDIA_FN(AMediaFormat_getFloat),
  { "AMediaFormat_getInt32", (uintptr_t)&z_AMediaFormat_getInt32 },
  OPTIONAL_MEDIA_FN(AMediaFormat_getInt64),
  OPTIONAL_MEDIA_FN(AMediaFormat_getString),
  { "AMediaFormat_new", (uintptr_t)&z_AMediaFormat_new },
  { "AMediaFormat_setBuffer", (uintptr_t)&z_AMediaFormat_setBuffer },
  { "AMediaFormat_setInt32", (uintptr_t)&z_AMediaFormat_setInt32 },
  { "AMediaFormat_setString", (uintptr_t)&z_AMediaFormat_setString },
  OPTIONAL_MEDIA_FN(ANativeWindow_toSurface),
#undef OPTIONAL_MEDIA_FN
#define MEDIA_KEY_ENTRY(symbol) { #symbol, (uintptr_t)&z_##symbol }
  MEDIA_KEY_ENTRY(AMEDIAFORMAT_KEY_CHANNEL_COUNT),
  MEDIA_KEY_ENTRY(AMEDIAFORMAT_KEY_COLOR_FORMAT),
  MEDIA_KEY_ENTRY(AMEDIAFORMAT_KEY_COLOR_RANGE),
  MEDIA_KEY_ENTRY(AMEDIAFORMAT_KEY_COLOR_STANDARD),
  MEDIA_KEY_ENTRY(AMEDIAFORMAT_KEY_DURATION),
  MEDIA_KEY_ENTRY(AMEDIAFORMAT_KEY_ENCODER_DELAY),
  MEDIA_KEY_ENTRY(AMEDIAFORMAT_KEY_FRAME_RATE),
  MEDIA_KEY_ENTRY(AMEDIAFORMAT_KEY_HEIGHT),
  MEDIA_KEY_ENTRY(AMEDIAFORMAT_KEY_IS_ADTS),
  MEDIA_KEY_ENTRY(AMEDIAFORMAT_KEY_LANGUAGE),
  MEDIA_KEY_ENTRY(AMEDIAFORMAT_KEY_MIME),
  MEDIA_KEY_ENTRY(AMEDIAFORMAT_KEY_ROTATION),
  MEDIA_KEY_ENTRY(AMEDIAFORMAT_KEY_SAMPLE_RATE),
  MEDIA_KEY_ENTRY(AMEDIAFORMAT_KEY_SLICE_HEIGHT),
  MEDIA_KEY_ENTRY(AMEDIAFORMAT_KEY_STRIDE),
  MEDIA_KEY_ENTRY(AMEDIAFORMAT_KEY_WIDTH),
#undef MEDIA_KEY_ENTRY
  { "android_get_device_api_level", (uintptr_t)&z_android_get_device_api_level },
  { "AThermal_acquireManager", (uintptr_t)&z_AThermal_acquireManager },
  { "AThermal_releaseManager", (uintptr_t)&z_AThermal_releaseManager },
  { "AThermal_getCurrentThermalStatus", (uintptr_t)&z_AThermal_getCurrentThermalStatus },
  { "AThermal_getThermalHeadroom", (uintptr_t)&z_AThermal_getThermalHeadroom },
  { "ASensorEventQueue_hasEvents", (uintptr_t)&ASensorEventQueue_hasEvents },
  { "ASensorManager_createEventQueue", (uintptr_t)&ASensorManager_createEventQueue },
  { "ASensorManager_destroyEventQueue", (uintptr_t)&ASensorManager_destroyEventQueue },
  { "ASensorManager_getDefaultSensor", (uintptr_t)&ASensorManager_getDefaultSensor },
  { "ASensorManager_getInstance", (uintptr_t)&ASensorManager_getInstance },
  { "ASensorManager_getSensorList", (uintptr_t)&ASensorManager_getSensorList },
  { "ASensor_getMinDelay", (uintptr_t)&ASensor_getMinDelay },
  { "ASensor_getName", (uintptr_t)&ASensor_getName },
  { "ASensor_getResolution", (uintptr_t)&ASensor_getResolution },
  { "ASensor_getType", (uintptr_t)&ASensor_getType },
  { "ASensor_getVendor", (uintptr_t)&ASensor_getVendor },
  { "_ZTH15gDeferredAction", (uintptr_t)&z_stub0 },
  { "__system_property_find", (uintptr_t)&__system_property_find_fake },
  { "__system_property_read", (uintptr_t)&__system_property_read_fake },
  { "_ctype_", (uintptr_t)&z_ctype_ptr },
  { "acos", (uintptr_t)&acos },
  { "asin", (uintptr_t)&asin },
  { "atan", (uintptr_t)&atan },
  { "atan2", (uintptr_t)&atan2 },
  { "atanf", (uintptr_t)&atanf },
  { "atol", (uintptr_t)&atol },
  { "basename", (uintptr_t)&z_basename },
  { "bsearch", (uintptr_t)&bsearch },
  { "cbrtf", (uintptr_t)&cbrtf },
  { "clearerr", (uintptr_t)&clearerr_fake },
  { "clock", (uintptr_t)&clock },
  { "clock_getres", (uintptr_t)&clock_getres_fake },
  { "cos", (uintptr_t)&cos },
  { "difftime", (uintptr_t)&difftime },
  { "div", (uintptr_t)&div },
  { "dladdr", (uintptr_t)&so_dladdr },
  { "dup", (uintptr_t)&z_dup },
#ifdef VULKAN_ONLY
  { "eglChooseConfig", (uintptr_t)&nx_eglChooseConfig_stub },
  { "eglCreatePbufferSurface", (uintptr_t)&nx_eglCreatePbufferSurface_stub },
  { "eglGetCurrentContext", (uintptr_t)&nx_eglGetCurrentContext_stub },
  { "eglGetCurrentSurface", (uintptr_t)&nx_eglGetCurrentSurface_stub },
  { "eglGetError", (uintptr_t)&nx_eglGetError_stub },
  { "eglGetProcAddress", (uintptr_t)&nx_eglGetProcAddress_stub },
  { "eglQueryString", (uintptr_t)&nx_eglQueryString_stub },
  { "eglSurfaceAttrib", (uintptr_t)&nx_eglSurfaceAttrib_stub },
  { "eglSwapInterval", (uintptr_t)&nx_eglSwapInterval_stub },
#else
  { "eglChooseConfig", (uintptr_t)&eglChooseConfig },
  { "eglCreatePbufferSurface", (uintptr_t)&eglCreatePbufferSurface },
  { "eglGetCurrentContext", (uintptr_t)&eglGetCurrentContext },
  { "eglGetCurrentSurface", (uintptr_t)&eglGetCurrentSurface },
  { "eglGetError", (uintptr_t)&eglGetError },
  { "eglGetProcAddress", (uintptr_t)&eglGetProcAddress },
  { "eglQueryString", (uintptr_t)&eglQueryString },
  { "eglSurfaceAttrib", (uintptr_t)&eglSurfaceAttrib },
  { "eglSwapInterval", (uintptr_t)&eglSwapInterval },
#endif
  { "exit", (uintptr_t)&exit },
  { "exp", (uintptr_t)&exp },
  { "exp2f", (uintptr_t)&exp2f },
  { "fdopen", (uintptr_t)&fdopen_fake },
  { "flock", (uintptr_t)&flock_fake },
  { "fmod", (uintptr_t)&fmod },
  { "fnmatch", (uintptr_t)&fnmatch },
  { "fscanf", (uintptr_t)&fscanf_fake },
  { "futimens", (uintptr_t)&futimens_fake },
  { "gethostbyaddr", (uintptr_t)&gethostbyaddr_fake },
  { "gethostbyname", (uintptr_t)&gethostbyname_fake },
  { "getpagesize", (uintptr_t)&z_getpagesize },
  { "getpriority", (uintptr_t)&z_stub0 },
  { "getpwuid_r", (uintptr_t)&getpwuid_r_fake },
  { "gettid", (uintptr_t)&z_gettid },
  { "hypot", (uintptr_t)&hypot },
  { "inet_addr", (uintptr_t)&inet_addr },
  { "inet_ntop", (uintptr_t)&inet_ntop_shim },
  { "inflate", (uintptr_t)&inflate },
  { "inflateEnd", (uintptr_t)&inflateEnd },
  { "inflateInit2_", (uintptr_t)&inflateInit2_ },
  { "isdigit_l", (uintptr_t)&z_isdigit_l },
  { "islower_l", (uintptr_t)&z_islower_l },
  { "isupper_l", (uintptr_t)&z_isupper_l },
  { "isxdigit_l", (uintptr_t)&z_isxdigit_l },
  { "ldexp", (uintptr_t)&ldexp },
  { "ldexpf", (uintptr_t)&ldexpf },
  { "lldiv", (uintptr_t)&lldiv },
  { "log", (uintptr_t)&log },
  { "log10", (uintptr_t)&log10 },
  { "log10f", (uintptr_t)&log10f },
  { "log2", (uintptr_t)&log2 },
  { "log2f", (uintptr_t)&log2f },
  { "logb", (uintptr_t)&logb },
  { "lrand48", (uintptr_t)&z_lrand48 },
  { "lseek64", (uintptr_t)&z_lseek },
  { "madvise", (uintptr_t)&madvise_fake },
  { "memrchr", (uintptr_t)&z_memrchr },
  { "modf", (uintptr_t)&modf },
  { "modff", (uintptr_t)&modff },
  { "prctl", (uintptr_t)&z_prctl },
  { "pthread_atfork", (uintptr_t)&z_stub0 },
  { "pthread_attr_getstack", (uintptr_t)&z_pthread_attr_getstack },
  { "pthread_condattr_destroy", (uintptr_t)&pthread_condattr_destroy_fake },
  { "pthread_condattr_init", (uintptr_t)&pthread_condattr_init_fake },
  { "pthread_condattr_setclock", (uintptr_t)&pthread_condattr_setclock_fake },
  { "pthread_equal", (uintptr_t)&z_pthread_equal },
  { "pthread_getattr_np", (uintptr_t)&z_pthread_getattr_np },
  { "pthread_rwlock_init", (uintptr_t)&pthread_rwlock_init_fake },
  { "pthread_setname_np", (uintptr_t)&z_pthread_setname_np },
  { "ptrace", (uintptr_t)&z_ptrace },
  { "raise", (uintptr_t)&raise },
  { "recvmsg", (uintptr_t)&recvmsg_fake },
  { "scalbn", (uintptr_t)&scalbn },
  { "sched_getaffinity", (uintptr_t)&z_sched_getaffinity },
  { "sched_setaffinity", (uintptr_t)&z_sched_setaffinity },
  { "sem_getvalue", (uintptr_t)&sem_getvalue_fake },
  { "sendmsg", (uintptr_t)&sendmsg_fake },
  { "setbuf", (uintptr_t)&setbuf_fake },
  { "setenv", (uintptr_t)&setenv },
  { "setpriority", (uintptr_t)&z_stub0 },
  { "setvbuf", (uintptr_t)&setvbuf_fake },
  /* The primary Android table supplies its stateful ENOSYS shim.  Keep this
   * secondary fallback local because that implementation is intentionally
   * private to imports.c. */
  { "sigaltstack", (uintptr_t)&z_stub0 },
  { "sigdelset", (uintptr_t)&z_stub0 },
  { "sigfillset", (uintptr_t)&z_stub0 },
  { "sigsuspend", (uintptr_t)&z_stub0 },
  { "sin", (uintptr_t)&sin },
  { "sqrtf", (uintptr_t)&sqrtf },
  { "srand48", (uintptr_t)&z_srand48 },
  { "strcasecmp", (uintptr_t)&z_strcasecmp },
  { "strcspn", (uintptr_t)&strcspn },
  { "strdup", (uintptr_t)&strdup },
  { "strftime", (uintptr_t)&strftime },
  { "strlcpy", (uintptr_t)&strlcpy },
  { "strnlen", (uintptr_t)&strnlen },
  { "strspn", (uintptr_t)&strspn },
  { "strtok_r", (uintptr_t)&strtok_r },
  { "tan", (uintptr_t)&tan },
  { "tolower_l", (uintptr_t)&z_tolower_l },
  { "toupper_l", (uintptr_t)&z_toupper_l },
  { "towlower", (uintptr_t)&towlower },
  { "unsetenv", (uintptr_t)&unsetenv },
  { "utimes", (uintptr_t)&utimes_fake },
  { "vprintf", (uintptr_t)&z_vprintf_noop },
  { "wmemcpy", (uintptr_t)&wmemcpy },
  { "wmemmove", (uintptr_t)&wmemmove },
  { "wmemset", (uintptr_t)&wmemset },
};
int unity_dynlib_numfunctions = (int)(sizeof(unity_dynlib_functions)/sizeof(unity_dynlib_functions[0]));
