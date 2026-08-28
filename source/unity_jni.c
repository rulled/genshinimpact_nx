/* Unity JNI handlers for assets, preferences, display and context services. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <limits.h>

#include "unity_jni.h"
#include "android_identity.h"
#include "asset_pack.h"
#include "combo_auth.h"
#include "combo_bridge.h"
#include "config.h"
#include "libc_shim.h"

/* Must match jni_fake.c. */
struct FakeID { uint32_t tag; char cls[256]; char name[128]; char sig[512]; };

#define REFRESH_HZ        60

static char g_root[512];
static char g_assets[544];
static char g_internal_root[544];
static char g_internal_files[576];
static char g_files[544];
static char g_cache[544];
static char g_code_cache[544];
static char g_no_backup[544];
static char g_obb[544];
static char g_package_root[544];

/* The exact PlatformModule login_switch_role fallback clears its in-process
 * role selection, then calls callSwitchRoleResult(0, "switch role success")
 * synchronously.  It is navigation, not an explicit account logout, so keep
 * the wrapper's server-issued saved session for the next launch.  Reproduce
 * only the non-authenticating terminal callback; it contains no account
 * identity or credential.  ComboForUnity$1 wraps the inner result string in
 * the callback-index envelope before OnGetInvokeResponse. */
static int combo_complete_empty_switch_role(int callback_index) {
  if (callback_index < 0) return -1;
  char payload[192];
  const int length = snprintf(
    payload, sizeof payload,
    "{\"index\":%d,\"data\":\"{\\\"ret\\\":0,\\\"msg\\\":\\\"switch role success\\\"}\"}",
    callback_index);
  if (length < 0 || (size_t)length >= sizeof payload) return -1;
  return combo_bridge_enqueue_callback(COMBO_ROUTE_INVOKE_RESPONSE,
                                       callback_index, payload,
                                       (size_t)length, 1);
}

/* AccountModule.realInitForInvoke reports callbackSuccess("init success")
 * after the SDK bridge finishes initializing.  ComboForUnity$1 forwards the
 * initialization result string unchanged to MiHoYoSDK.OnInitResponse rather
 * than adding the ordinary callback-index envelope.  At this point the native
 * wrapper has initialized its callback transport, exact channel metadata,
 * crypto bridge, and network transport, so acknowledge that local bridge
 * state only.  This is not account authentication and deliberately carries no
 * identity, cookie, session, or server-issued credential. */
static int combo_complete_native_bridge_init(int callback_index) {
  if (callback_index < 0) return -1;
  static const char payload[] = "{\"ret\":0,\"msg\":\"init success\"}";
  return combo_bridge_enqueue_callback(COMBO_ROUTE_INIT_RESPONSE,
                                       callback_index, payload,
                                       sizeof payload - 1, 1);
}

/* ConsentPrivacyPresenter.onAcceptNecessaryClicked is the privacy-preserving
 * terminal path in the reviewed 6.7.0 SDK: it stores both analytics and
 * advertising as denied, then invokes callback(0, "", {shouldShowEdit:true}).
 * The Android SDK UI cannot be hosted by libnx, so reproduce only that
 * necessary-storage result for the initial "privacy" banner.  This does not
 * grant either optional purpose and carries no account or request data. */
static int combo_complete_necessary_consent(int callback_index) {
  if (callback_index < 0) return -1;
  char payload[256];
  const int length = snprintf(
    payload, sizeof payload,
    "{\"index\":%d,\"data\":\"{\\\"ret\\\":0,\\\"msg\\\":\\\"\\\","
    "\\\"data\\\":{\\\"shouldShowEdit\\\":true}}\"}",
    callback_index);
  if (length < 0 || (size_t)length >= sizeof payload) return -1;
  return combo_bridge_enqueue_callback(COMBO_ROUTE_INVOKE_RESPONSE,
                                       callback_index, payload,
                                       (size_t)length, 1);
}

/* Inspect only the allowlisted, non-sensitive consent level.  The surrounding
 * invoke JSON is still never logged, retained, or passed to diagnostics. */
static int combo_json_string_equals(const char *json, const char *key,
                                    const char *expected) {
  if (!json || !key || !expected || !*key) return 0;
  char quoted_key[64];
  const int key_length = snprintf(quoted_key, sizeof quoted_key,
                                  "\"%s\"", key);
  if (key_length < 0 || (size_t)key_length >= sizeof quoted_key) return 0;
  const char *cursor = json;
  while ((cursor = strstr(cursor, quoted_key)) != NULL) {
    cursor += (size_t)key_length;
    while (*cursor == ' ' || *cursor == '\t' ||
           *cursor == '\r' || *cursor == '\n') ++cursor;
    if (*cursor++ != ':') continue;
    while (*cursor == ' ' || *cursor == '\t' ||
           *cursor == '\r' || *cursor == '\n') ++cursor;
    if (*cursor++ != '"') continue;
    const size_t expected_length = strlen(expected);
    if (!strncmp(cursor, expected, expected_length) &&
        cursor[expected_length] == '"')
      return 1;
  }
  return 0;
}

static int  has(const char *s, const char *sub){ return strstr(s,sub)!=NULL; }

typedef struct {
  uint64_t block_size;
  uint64_t block_count;
  uint64_t free_blocks;
  uint64_t available_blocks;
  uint64_t total_bytes;
  uint64_t free_bytes;
  uint64_t available_bytes;
} UStorageStats;

static uint64_t sat_mul_u64(uint64_t a, uint64_t b) {
  return b && a > UINT64_MAX / b ? UINT64_MAX : a * b;
}

/* Java File/StatFs must report the capacity of the real staged filesystem.
 * A nonexistent child still belongs to the same SD filesystem, so paths below
 * the data root fall back to the root rather than reporting a false zero. */
static int storage_stats(const char *path, UStorageStats *out) {
  struct statvfs st;
  memset(out, 0, sizeof *out);
  if (!path || statvfs(path, &st) != 0) {
    if (!path || strncmp(path, g_root, strlen(g_root)) != 0 ||
        statvfs(g_root[0] ? g_root : GAME_HOME, &st) != 0)
      return 0;
  }
  const uint64_t block_size = st.f_frsize ? (uint64_t)st.f_frsize :
                                             (uint64_t)st.f_bsize;
  out->block_size = block_size;
  out->block_count = (uint64_t)st.f_blocks;
  out->free_blocks = (uint64_t)st.f_bfree;
  out->available_blocks = (uint64_t)st.f_bavail;
  out->total_bytes = sat_mul_u64(out->block_count, block_size);
  out->free_bytes = sat_mul_u64(out->free_blocks, block_size);
  out->available_bytes = sat_mul_u64(out->available_blocks, block_size);
  return 1;
}

static uint64_t legacy_block_count(uint64_t value) {
  return value > INT_MAX ? (uint64_t)INT_MAX : value;
}
/* Stateful stream, preference and collection handles. */
enum { UJ_TAG = 0x554a4831 /*'UJH1'*/ };
enum { UJ_INPUTSTREAM, UJ_AFD, UJ_FD, UJ_EDITOR,
       UJ_MAP, UJ_SET, UJ_ITER, UJ_ENTRY, UJ_BOXED, UJ_FILE, UJ_PREFS };

typedef struct {
  uint32_t tag; int kind;
  unsigned refs;
  FILE *fp;
  int fd;
  int packed;
  long off, len;
  int idx;
  char btype;
  long long bival;
  double bfval;
  char *text;
  void *state;
} UHandle;

static UHandle *uh_new(int kind){
  UHandle *h = calloc(1,sizeof *h);
  if (!h) return NULL;
  h->tag = UJ_TAG; h->kind = kind; h->refs=1; h->fd = -1;
  return jni_track_local(h);
}
static int is_uh(void *p,int kind){ UHandle*h=p; return h && h->tag==UJ_TAG && h->kind==kind; }

static void join_path(char *out, size_t n, const char *parent, const char *child){
  const char *p = parent ? parent : "", *c = child ? child : "";
  while (*c == '/') ++c;
  if (!n) return;
  size_t used=strnlen(p,n-1); memcpy(out,p,used); out[used]='\0';
  if (used && out[used-1]!='/' && used+1<n) { out[used++]='/'; out[used]='\0'; }
  size_t left=n-1-used, add=strnlen(c,left); memcpy(out+used,c,add); out[used+add]='\0';
}

void *unity_make_file(const char *parent, const char *child){
  char base[768], path[768];
  if (parent && parent[0] == '/') snprintf(base,sizeof(base),"sdmc:%s",parent);
  else snprintf(base,sizeof(base),"%s",parent ? parent : "");
  if (child && child[0]) join_path(path, sizeof(path), base, child);
  else snprintf(path, sizeof(path), "%s", base);
  UHandle *h = uh_new(UJ_FILE); if (!h) return NULL;
  h->text = strdup(path); return h;
}

const char *unity_class_of(void *p){
  UHandle *h = p;
  if (!h || h->tag != UJ_TAG) return NULL;
  switch (h->kind) {
    case UJ_INPUTSTREAM: return "java/io/InputStream";
    case UJ_AFD:         return "android/content/res/AssetFileDescriptor";
    case UJ_FD:          return "java/io/FileDescriptor";
    case UJ_EDITOR:      return "android/content/SharedPreferences$Editor";
    case UJ_PREFS:       return "android/content/SharedPreferences";
    case UJ_MAP:         return "java/util/Map";
    case UJ_SET:         return "java/util/Set";
    case UJ_ITER:        return "java/util/Iterator";
    case UJ_ENTRY:       return "java/util/Map$Entry";
    case UJ_FILE:        return "java/io/File";
    case UJ_BOXED:
      return h->btype == 'I' ? "java/lang/Integer" : h->btype == 'L' ? "java/lang/Long" :
             h->btype == 'F' ? "java/lang/Float" : "java/lang/Boolean";
    default: return "java/lang/Object";
  }
}

/* AssetManager paths use the staged assets tree. */
static int asset_path(char *out,size_t n,const char *name){
  while (name && (name[0]=='/' )) name++;
  if (!name || strstr(name,"../") || !strcmp(name,"..") || strchr(name,'\\')) { if(n)out[0]='\0'; return 0; }
  snprintf(out,n,"%s/%s",g_assets,name?name:"");
  return 1;
}

static int asset_fd_open(const char *path, int *packed) {
  int fd = asset_pack_active() ? asset_pack_open_path(path) : -1;
  if (fd >= 0 && asset_pack_fd_is(fd)) {
    if (packed) *packed = 1;
    return fd;
  }
  fd = open(path, O_RDONLY);
  if (packed) *packed = 0;
  return fd;
}

static long asset_fd_read(int fd, int packed, void *buffer, size_t size) {
  return packed ? asset_pack_read_fd(fd, buffer, size) :
                  (long)read(fd, buffer, size);
}

static long asset_fd_seek(int fd, int packed, long offset, int whence) {
  return packed ? asset_pack_lseek_fd(fd, offset, whence) :
                  (long)lseek(fd, (off_t)offset, whence);
}

static int asset_fd_close(int fd, int packed) {
  return packed ? asset_pack_close_fd(fd) : close(fd);
}

static int asset_fd_length(int fd, int packed, long *length) {
  if (!length) return 0;
  if (packed) {
    uint64_t size = 0, ino = 0;
    int directory = 0;
    if (!asset_pack_fstat_fd(fd, &size, &ino, &directory) || directory ||
        size > LONG_MAX)
      return 0;
    *length = (long)size;
    return 1;
  }
  struct stat st;
  if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size > LONG_MAX)
    return 0;
  *length = (long)st.st_size;
  return 1;
}

const char *unity_file_path(void *p){
  UHandle *h = p; return is_uh(h, UJ_FILE) && h->text ? h->text : g_root;
}
const char *unity_assets_path(void){ return g_assets[0] ? g_assets : GAME_HOME "/assets"; }
const char *unity_internal_data_path(void){
  return g_internal_root[0] ? g_internal_root : GAME_HOME "/android_internal";
}
const char *unity_internal_files_path(void){
  return g_internal_files[0] ? g_internal_files : GAME_HOME "/android_internal/files";
}
const char *unity_external_files_path(void){
  return g_files[0] ? g_files : GAME_HOME "/files";
}

/* Genshin's Android resource migration treats Context.getFilesDir() and
 * Context.getExternalFilesDir() as independent installations.  If a wrapper
 * aliases them, accepting the migration prompt makes its "other install"
 * cleanup delete the live AssetBundles tree.  Keep this as a startup-fatal
 * invariant so a later path refactor cannot silently reintroduce data loss. */
int unity_storage_paths_self_test(void){
  const char *internal_root = unity_internal_data_path();
  const char *internal_files = unity_internal_files_path();
  const char *external_files = unity_external_files_path();
  const size_t root_length = strlen(g_root);
  const size_t internal_length = strlen(internal_root);
  return g_root[0] && root_length > 0 &&
         internal_root[0] && internal_files[0] && external_files[0] &&
         strcmp(internal_root, g_root) != 0 &&
         strcmp(internal_files, external_files) != 0 &&
         !strncmp(internal_root, g_root, root_length) &&
         internal_root[root_length] == '/' &&
         !strncmp(internal_files, internal_root, internal_length) &&
         internal_files[internal_length] == '/';
}

static int is_context_class(const char *cls){
  return has(cls,"content/Context") || has(cls,"app/Activity") || has(cls,"app/Application") ||
         has(cls,"UnityPlayerActivity") || has(cls,"ComboSDKActivity") || has(cls,"MainActivity");
}

/* SharedPreferences are named, transactional, and serialized.  Session data
 * is never logged.  apply() is made synchronously durable because this wrapper
 * has no Android queued-work service to finish writes during process teardown. */
typedef struct { char type; char *key; char *val; } KV;
typedef struct PrefStore {
  char *name;
  char path[768];
  KV *items;
  int count, capacity, dirty;
  struct PrefStore *next;
} PrefStore;
typedef struct PrefEditorOp {
  char type;
  char *key;
  char *value;
  struct PrefEditorOp *next;
} PrefEditorOp;
typedef struct {
  PrefStore *store;
  PrefEditorOp *head, *tail;
  int clear_first;
} PrefEditor;
typedef struct { KV *items; int count; unsigned refs; } PrefSnapshot;

#define PREF_STORE_LIMIT 64
static PrefStore *g_pref_stores;
static PrefStore *g_default_prefs;
static int g_pref_store_count;
static Mutex g_prefs_lock;
static char g_shared_prefs[544];

static void pref_free_text(char *text, int sensitive) {
  if (!text) return;
  if (sensitive) {
    volatile unsigned char *p=(volatile unsigned char *)text;
    size_t remaining=strlen(text);
    while(remaining--)*p++=0;
  }
  free(text);
}

static KV *kv_find(KV *items, int count, const char *key) {
  if (!key) return NULL;
  for (int i = 0; i < count; ++i)
    if (!strcmp(items[i].key, key)) return &items[i];
  return NULL;
}

static int kv_set_store(PrefStore *store, char type, const char *key,
                        const char *value) {
  if (!store || !key || !key[0] || !value) return 0;
  KV *item = kv_find(store->items, store->count, key);
  char *copy = strdup(value);
  if (!copy) return 0;
  if (item) {
    pref_free_text(item->val,1);
    item->val = copy;
    item->type = type;
    store->dirty = 1;
    return 1;
  }
  if (store->count == store->capacity) {
    int next_capacity = store->capacity ? store->capacity * 2 : 32;
    KV *next = realloc(store->items, (size_t)next_capacity * sizeof(*next));
    if (!next) { free(copy); return 0; }
    store->items = next;
    store->capacity = next_capacity;
  }
  char *key_copy = strdup(key);
  if (!key_copy) { free(copy); return 0; }
  store->items[store->count++] = (KV){ type, key_copy, copy };
  store->dirty = 1;
  return 1;
}

static void kv_remove_store(PrefStore *store, const char *key) {
  if (!store || !key) return;
  for (int i = 0; i < store->count; ++i) if (!strcmp(store->items[i].key, key)) {
    pref_free_text(store->items[i].key,0);
    pref_free_text(store->items[i].val,1);
    store->items[i] = store->items[--store->count];
    store->dirty = 1;
    return;
  }
}

static void kv_clear_store(PrefStore *store) {
  if (!store) return;
  for (int i = 0; i < store->count; ++i) {
    pref_free_text(store->items[i].key,0);
    pref_free_text(store->items[i].val,1);
  }
  store->count = 0;
  store->dirty = 1;
}

static void esc(FILE*f,const char*s){ for(;*s;s++){ if(*s=='\\'||*s=='\t'||*s=='\n'){fputc('\\',f);
  fputc(*s=='\t'?'t':*s=='\n'?'n':'\\',f);} else fputc(*s,f);} }
static char *unesc(char*s){ char*o=s,*w=s; for(;*o;o++){ if(*o=='\\'&&o[1]){o++;
  *w++=(*o=='t')?'\t':(*o=='n')?'\n':*o;} else *w++=*o;} *w=0; return s; }

static int pref_store_load(PrefStore *store){
  FILE*f=fopen(store->path,"rb");
  if(!f) return 0;
  char line[8192];
  while (fgets(line,sizeof line,f)){
    char *nl=strchr(line,'\n'); if(nl)*nl=0;
    if(!line[0]) continue;
    char type=line[0]; char *k=line+2;            /* "T\tkey\tval"          */
    char *t1=strchr(k,'\t'); if(!t1) continue; *t1=0; char*v=t1+1;
    if (type=='S'||type=='I'||type=='L'||type=='F'||type=='B')
      (void)kv_set_store(store, type, unesc(k), unesc(v));
  }
  fclose(f); store->dirty=0; return 1;
}
static int pref_store_flush(PrefStore *store){
  if(!store || !store->dirty) return 1;
  char temp[800];
  if (snprintf(temp,sizeof temp,"%s.tmp",store->path) >= (int)sizeof temp)
    return 0;
  FILE*f=fopen(temp,"wb");
  if(!f) return 0;
  int ok=1;
  for(int i=0;i<store->count;i++){
    if(fputc(store->items[i].type,f)==EOF||fputc('\t',f)==EOF){ok=0;break;}
    esc(f,store->items[i].key); if(fputc('\t',f)==EOF){ok=0;break;}
    esc(f,store->items[i].val); if(fputc('\n',f)==EOF){ok=0;break;}
  }
  if(ok && fflush(f)!=0) ok=0;
  if(ok && fsync(fileno(f))!=0) ok=0;
  if(fclose(f)!=0) ok=0;
  if(ok && rename(temp,store->path)!=0) ok=0;
  if(!ok) (void)remove(temp);
  if(ok) store->dirty=0;
  return ok;
}

static uint64_t pref_name_hash(const char *name) {
  uint64_t hash=UINT64_C(1469598103934665603);
  for (const unsigned char *p=(const unsigned char *)name; *p; ++p) {
    hash^=*p; hash*=UINT64_C(1099511628211);
  }
  return hash;
}

static PrefStore *pref_store_get_locked(const char *name) {
  if(!name||!name[0]) name="default";
  if(strnlen(name,256)==256) return NULL;
  for(PrefStore*s=g_pref_stores;s;s=s->next) if(!strcmp(s->name,name)) return s;
  if(g_pref_store_count>=PREF_STORE_LIMIT) return NULL;
  PrefStore*s=calloc(1,sizeof(*s)); if(!s) return NULL;
  s->name=strdup(name); if(!s->name){free(s);return NULL;}
  snprintf(s->path,sizeof s->path,"%s/pref_%016llx.kv",g_shared_prefs,
           (unsigned long long)pref_name_hash(name));
  (void)pref_store_load(s);
  s->next=g_pref_stores;g_pref_stores=s;g_pref_store_count++;
  return s;
}

static PrefStore *prefs_for_handle(void *recv) {
  UHandle*h=recv;
  return is_uh(h,UJ_PREFS)&&h->state ? (PrefStore*)h->state : g_default_prefs;
}

static UHandle *pref_handle(const char *name) {
  mutexLock(&g_prefs_lock);
  PrefStore*store=pref_store_get_locked(name);
  mutexUnlock(&g_prefs_lock);
  if(!store) return NULL;
  UHandle*h=uh_new(UJ_PREFS); if(h)h->state=store; return h;
}

static PrefSnapshot *pref_snapshot(PrefStore *store) {
  PrefSnapshot*snapshot=calloc(1,sizeof(*snapshot)); if(!snapshot)return NULL;
  mutexLock(&g_prefs_lock);
  if(store&&store->count){
    snapshot->items=calloc((size_t)store->count,sizeof(*snapshot->items));
    if(snapshot->items){
      for(int i=0;i<store->count;i++){
        snapshot->items[i].type=store->items[i].type;
        snapshot->items[i].key=strdup(store->items[i].key);
        snapshot->items[i].val=strdup(store->items[i].val);
        if(!snapshot->items[i].key||!snapshot->items[i].val){
          pref_free_text(snapshot->items[i].key,0);
          pref_free_text(snapshot->items[i].val,1);
          memset(&snapshot->items[i],0,sizeof(snapshot->items[i]));
          break;
        }
        snapshot->count++;
      }
    }
  }
  mutexUnlock(&g_prefs_lock);
  return snapshot;
}

static void pref_snapshot_free(PrefSnapshot *snapshot) {
  if(!snapshot)return;
  for(int i=0;i<snapshot->count;i++){
    pref_free_text(snapshot->items[i].key,0);
    pref_free_text(snapshot->items[i].val,1);
  }
  free(snapshot->items);
  free(snapshot);
}

static void pref_snapshot_retain(PrefSnapshot *snapshot) {
  if(!snapshot)return;
  mutexLock(&g_prefs_lock);
  if(snapshot->refs<UINT_MAX)snapshot->refs++;
  mutexUnlock(&g_prefs_lock);
}

static void pref_snapshot_release(PrefSnapshot *snapshot) {
  if(!snapshot)return;
  int destroy=0;
  mutexLock(&g_prefs_lock);
  if(snapshot->refs&&--snapshot->refs==0)destroy=1;
  mutexUnlock(&g_prefs_lock);
  if(destroy)pref_snapshot_free(snapshot);
}

static UHandle *pref_child_handle(int kind, void *state) {
  UHandle*h=uh_new(kind);
  if(h){h->state=state;pref_snapshot_retain((PrefSnapshot*)state);}
  return h;
}

static PrefSnapshot *pref_snapshot_for_handle(void *recv) {
  UHandle *h=recv;
  if (!h || h->tag!=UJ_TAG) return NULL;
  if (h->kind!=UJ_MAP && h->kind!=UJ_SET && h->kind!=UJ_ITER &&
      h->kind!=UJ_ENTRY) return NULL;
  return (PrefSnapshot *)h->state;
}

static PrefEditor *pref_editor_from(void *recv) {
  UHandle*h=recv; return is_uh(h,UJ_EDITOR)?(PrefEditor*)h->state:NULL;
}

static UHandle *pref_editor_new(PrefStore *store) {
  PrefEditor*editor=calloc(1,sizeof(*editor)); if(!editor)return NULL;
  editor->store=store;
  UHandle*h=uh_new(UJ_EDITOR); if(!h){free(editor);return NULL;} h->state=editor;return h;
}

static int pref_editor_add(PrefEditor *editor,char type,const char*key,const char*value){
  if(!editor||!key||!key[0])return 0;
  PrefEditorOp*op=calloc(1,sizeof(*op));if(!op)return 0;
  op->type=type;op->key=strdup(key);op->value=value?strdup(value):NULL;
  if(!op->key||(value&&!op->value)){free(op->key);free(op->value);free(op);return 0;}
  mutexLock(&g_prefs_lock);
  if(editor->tail)editor->tail->next=op;else editor->head=op;
  editor->tail=op;
  mutexUnlock(&g_prefs_lock);
  return 1;
}

static void pref_editor_reset_locked(PrefEditor *editor){
  PrefEditorOp*op=editor?editor->head:NULL;
  while(op){PrefEditorOp*next=op->next;pref_free_text(op->key,0);
    pref_free_text(op->value,1);free(op);op=next;}
  if(editor){editor->head=editor->tail=NULL;editor->clear_first=0;}
}

static int pref_editor_commit(PrefEditor *editor){
  if(!editor||!editor->store)return 0;
  mutexLock(&g_prefs_lock);
  if(editor->clear_first)kv_clear_store(editor->store);
  int ok=1;
  for(PrefEditorOp*op=editor->head;op;op=op->next){
    if(op->type=='R')kv_remove_store(editor->store,op->key);
    else if(!kv_set_store(editor->store,op->type,op->key,op->value?op->value:""))ok=0;
  }
  if(ok)ok=pref_store_flush(editor->store);
  pref_editor_reset_locked(editor);
  mutexUnlock(&g_prefs_lock);
  return ok;
}

int unity_is_handle(void *obj) {
  UHandle *handle=obj;
  return handle&&handle->tag==UJ_TAG;
}

int unity_retain_handle(void *obj) {
  UHandle *handle=obj;
  if(!handle||handle->tag!=UJ_TAG)return 0;
  unsigned refs=__atomic_load_n(&handle->refs,__ATOMIC_ACQUIRE);
  while(refs&&refs<UINT_MAX-1){
    if(__atomic_compare_exchange_n(&handle->refs,&refs,refs+1,0,
                                   __ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE))return 1;
  }
  return 0;
}

int unity_release_handle(void *obj) {
  UHandle *handle=obj;
  if(!handle||handle->tag!=UJ_TAG)return 0;
  unsigned refs=__atomic_load_n(&handle->refs,__ATOMIC_ACQUIRE);
  while(refs){
    if(__atomic_compare_exchange_n(&handle->refs,&refs,refs-1,0,
                                   __ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE))break;
  }
  if(!refs)return 1;
  if(refs>1)return 1;
  handle->tag=0; /* make duplicate/free races fail closed */
  switch(handle->kind){
    case UJ_INPUTSTREAM:
      if(handle->fd>=0)asset_fd_close(handle->fd,handle->packed);
      break;
    case UJ_AFD:
      if(handle->fd>=0)asset_fd_close(handle->fd,handle->packed);
      break;
    case UJ_EDITOR: {
      PrefEditor *editor=handle->state;
      if(editor){mutexLock(&g_prefs_lock);pref_editor_reset_locked(editor);
        mutexUnlock(&g_prefs_lock);free(editor);}
      break;
    }
    case UJ_MAP: case UJ_SET: case UJ_ITER: case UJ_ENTRY:
      pref_snapshot_release((PrefSnapshot*)handle->state);
      break;
    case UJ_FILE:
      free(handle->text);
      break;
    default:
      break;
  }
  free(handle);
  return 1;
}

/* Box primitive preference values for getAll(). */
static void *uh_box_from_kv(const KV *kv){
  if (!kv) return jni_make_string("");
  switch (kv->type){
    case 'I': case 'L': case 'B': {
      UHandle *h = uh_new(UJ_BOXED);
      if (!h) return NULL;
      h->btype = kv->type;
      h->bival = strtoll(kv->val, NULL, 10);
      if (kv->type=='B') h->bival = (kv->val[0]=='1'||kv->val[0]=='t'||kv->val[0]=='T') ? 1 : 0;
      return h;
    }
    case 'F': {
      UHandle *h = uh_new(UJ_BOXED);
      if (!h) return NULL;
      h->btype = 'F'; h->bfval = strtod(kv->val, NULL);
      return h;
    }
    default: /* 'S' and anything else -> string */
      return jni_make_local_string(kv->val);
  }
}

int unity_is_boxed(void *p){
  UHandle *h = p; return (h && h->tag==UJ_TAG && h->kind==UJ_BOXED) ? 1 : 0;
}
uint64_t unity_boxed_int(void *p){
  UHandle *h = p; if (!unity_is_boxed(h)) return 0;
  if (h->btype=='F') return (uint64_t)(long long)h->bfval;
  return (uint64_t)h->bival;
}
float unity_boxed_float(void *p){
  UHandle *h = p; if (!unity_is_boxed(h)) return 0.0f;
  return (h->btype=='F') ? (float)h->bfval : (float)h->bival;
}
/* Return -1 for objects outside this handler. */
int unity_isinstance(void *p, const char *clazz){
  UHandle *h = p; if (!h || h->tag!=UJ_TAG || h->kind!=UJ_BOXED) return -1;
  if (!clazz) return 0;
  switch (h->btype){
    case 'I': return strstr(clazz,"Integer") ? 1 : 0;
    case 'L': return strstr(clazz,"Long")    ? 1 : 0;
    case 'F': return strstr(clazz,"Float")   ? 1 : 0;
    case 'B': return strstr(clazz,"Boolean") ? 1 : 0;
  }
  return 0;
}

int unity_owns_class(const char *cls){
  return has(cls,"AssetManager") || has(cls,"java/io/InputStream") ||
         has(cls,"AssetFileDescriptor") || has(cls,"java/io/FileDescriptor") ||
         has(cls,"java/io/File") ||
         has(cls,"SharedPreferences") || has(cls,"SharedPreferences$Editor") ||
         has(cls,"android/os/StatFs") ||
         has(cls,"java/util/Map") || has(cls,"java/util/Set") ||
         has(cls,"java/util/Iterator") || has(cls,"java/util/HashMap") ||
         has(cls,"view/Display") || has(cls,"WindowManager") || has(cls,"DisplayManager") ||
         has(cls,"res/Configuration") || has(cls,"res/Resources") ||
         has(cls,"DisplayMetrics") || has(cls,"content/Context") ||
         has(cls,"app/Activity") || has(cls,"app/Application") || has(cls,"UnityPlayerActivity") ||
         has(cls,"ComboSDKActivity") || has(cls,"ComboForUnity") || has(cls,"MainActivity") ||
         has(cls,"pm/PackageManager") || has(cls,"net/ConnectivityManager") ||
         has(cls,"net/NetworkInfo") || has(cls,"net/NetworkCapabilities") ||
         has(cls,"unity3d/player/UnityPlayer");
}

void *unity_dispatch_object(void *recv, const void *id_, va_list va){ const struct FakeID *id = id_;
  const char *cls=id->cls, *m=id->name;

  if (has(cls,"ComboForUnity") && has(m,"invokeReturn")) {
    const char *invoke_name = jni_string_utf(va_arg(va,void*));
    (void)va_arg(va,void*); /* sensitive JSON payload: never log it */

    /* DEX contract: each module Dispatcher strips the module prefix.  The
     * reviewed SDK returns SDKInfo channel/subchannel/device metadata and its
     * redacted account label synchronously.  These getters run immediately
     * after OnSDKLoginCB, so they must agree with the genuine Combo success
     * envelope or managed login never advances to game/resource login. */
    if (invoke_name &&
        (!strcmp(invoke_name, "info_get_channel_id") ||
         !strcmp(invoke_name, "get_channel_id"))) {
      return jni_make_string(GENSHIN_CHANNEL_ID_TEXT);
    }
    if (invoke_name &&
        (!strcmp(invoke_name, "info_get_sub_channel_id") ||
         !strcmp(invoke_name, "get_sub_channel_id"))) {
      return jni_make_string(GENSHIN_SUB_CHANNEL_ID_TEXT);
    }
    if (invoke_name &&
        (!strcmp(invoke_name, "info_get_device_id") ||
         !strcmp(invoke_name, "get_device_id"))) {
      return jni_make_string(android_identity_android_id());
    }
    if (invoke_name &&
        (!strcmp(invoke_name, "login_get_asterisk_name") ||
         !strcmp(invoke_name, "get_asterisk_name"))) {
      return jni_make_string(combo_auth_asterisk_name());
    }

    return jni_make_string("");
  }

  if (has(cls,"AssetManager")){
    if (has(m,"openFd") || has(m,"openNonAssetFd")){
      const char *name = jni_string_utf(va_arg(va,void*));
      char path[1024]; if(!asset_path(path,sizeof path,name))return NULL;
      int packed=0; int fd=asset_fd_open(path,&packed);
      if(fd<0) return NULL;
      long length=0; if(!asset_fd_length(fd,packed,&length)){asset_fd_close(fd,packed);return NULL;}
      UHandle*h=uh_new(UJ_AFD); if(!h){asset_fd_close(fd,packed);return NULL;}
      h->fd=fd; h->packed=packed; h->off=0; h->len=length; return h;
    }
    if (has(m,"open")){
      const char *name = jni_string_utf(va_arg(va,void*));
      char path[1024]; if(!asset_path(path,sizeof path,name))return NULL;
      int packed=0; int fd=asset_fd_open(path,&packed);
      if(fd<0) return NULL;
      long length=0; if(!asset_fd_length(fd,packed,&length)){asset_fd_close(fd,packed);return NULL;}
      UHandle*h=uh_new(UJ_INPUTSTREAM); if(!h){asset_fd_close(fd,packed);return NULL;}
      h->fd=fd; h->packed=packed; h->len=length; return h;
    }
    if (has(m,"list")) {
      const char *name = jni_string_utf(va_arg(va,void*));
      char path[1024]; if(!asset_path(path,sizeof path,name))return jni_make_object_array(0,NULL);
      void *dir = asset_pack_active() ? asset_pack_opendir_path(path) : NULL;
      int packed = dir != NULL;
      if (!dir) dir=opendir(path);
      if (!dir) return jni_make_object_array(0, NULL);
      void **items = NULL; int count = 0, cap = 0;
      for (;;) {
        const char *entry_name=NULL;
        if (packed) entry_name=asset_pack_readdir_path(dir,NULL,NULL);
        else { struct dirent *de=readdir((DIR *)dir); entry_name=de?de->d_name:NULL; }
        if (!entry_name) break;
        if (!strcmp(entry_name,".") || !strcmp(entry_name,"..")) continue;
        if (count == cap) { int next = cap ? cap * 2 : 16; void **p = realloc(items,(size_t)next*sizeof(*p));
          if (!p) break;
          items=p; cap=next; }
        items[count++] = jni_make_local_string(entry_name);
      }
      if(packed)asset_pack_closedir_path(dir);else closedir((DIR *)dir);
      void *out = jni_make_object_array(count,items); free(items); return out;
    }
    return jni_make_object("AssetManager");
  }

  if (has(cls,"AssetFileDescriptor")){
    if (has(m,"getFileDescriptor") || has(m,"getParcelFileDescriptor")){
      UHandle*a=recv; UHandle*fd=uh_new(UJ_FD); if(!fd)return NULL;
      fd->fd = is_uh(a,UJ_AFD)?a->fd:-1; return fd;
    }
    return jni_make_object("AssetFileDescriptor");
  }

  if (has(cls,"SharedPreferences") && !has(cls,"Editor")){
    PrefStore *store=prefs_for_handle(recv);
    if (has(m,"edit")) return pref_editor_new(store);
    if (has(m,"getString")){
      const char *key = jni_string_utf(va_arg(va,void*));
      void *def_obj = va_arg(va,void*);
      mutexLock(&g_prefs_lock);
      KV*kv=store?kv_find(store->items,store->count,key):NULL;
      void *result=(kv&&kv->type=='S')?jni_make_local_string(kv->val):def_obj;
      mutexUnlock(&g_prefs_lock);
      return result;
    }
    if (has(m,"getAll")) {
      PrefSnapshot *snapshot=pref_snapshot(store);
      if(!snapshot)return NULL;
      UHandle *map=pref_child_handle(UJ_MAP,snapshot);
      if(!map)pref_snapshot_free(snapshot);
      return map;
    }
    if (has(m,"getStringSet")) {
      (void)jni_string_utf(va_arg(va,void*));
      return va_arg(va,void*); /* string-set persistence is not synthesized */
    }
    return recv;
  }

  /* getAll() collections retain an immutable snapshot so SDK worker threads
   * cannot observe an editor commit half way through an iteration. */
  if (has(cls,"java/util/Map") && !has(cls,"Entry")){
    PrefSnapshot *snapshot=pref_snapshot_for_handle(recv);
    if (has(m,"entrySet") || has(m,"keySet")) {
      UHandle *set=pref_child_handle(UJ_SET,snapshot);
      if(set)set->btype=has(m,"keySet")?'K':'E';
      return set;
    }
    if (has(m,"get")){
      const char*k=jni_string_utf(va_arg(va,void*));
      KV *kv=snapshot?kv_find(snapshot->items,snapshot->count,k):NULL;
      return kv?uh_box_from_kv(kv):NULL;
    }
    return recv;
  }
  if (has(cls,"java/util/Set")){
    if (has(m,"iterator")){
      UHandle *set=recv;
      UHandle*it=pref_child_handle(UJ_ITER,pref_snapshot_for_handle(recv));
      if(it){it->idx=0;it->btype=is_uh(set,UJ_SET)?set->btype:'E';}
      return it;
    }
    return recv;
  }
  if (has(cls,"java/util/Iterator")){
    if (has(m,"next")){                   /* return current entry, advance cursor */
      UHandle*it=recv;
      int i = is_uh(it,UJ_ITER) ? it->idx : 0;
      if (is_uh(it,UJ_ITER)) it->idx++;
      PrefSnapshot *snapshot=pref_snapshot_for_handle(recv);
      if(!snapshot||i<0||i>=snapshot->count)return NULL;
      if(is_uh(it,UJ_ITER)&&it->btype=='K')return jni_make_local_string(snapshot->items[i].key);
      UHandle*e=pref_child_handle(UJ_ENTRY,snapshot); if(e)e->idx=i; return e;
    }
    return jni_make_object("java/util/Iterator");
  }
  if (has(cls,"java/util/Map") && has(cls,"Entry")){   /* java/util/Map$Entry */
    UHandle*e=recv; int i = is_uh(e,UJ_ENTRY) ? e->idx : -1;
    PrefSnapshot *snapshot=pref_snapshot_for_handle(recv);
    if (!snapshot||i<0||i>=snapshot->count) return NULL;
    if (has(m,"getKey"))   return jni_make_local_string(snapshot->items[i].key);
    if (has(m,"getValue")) return uh_box_from_kv(&snapshot->items[i]);
    return NULL;
  }
  if (has(cls,"SharedPreferences$Editor")){
    /* putX returns the Editor for chained calls. */
    PrefEditor *editor=pref_editor_from(recv);
    if (has(m,"putString")){ const char*k=jni_string_utf(va_arg(va,void*));
      void *value_obj=va_arg(va,void*);
      const char*v=jni_string_utf(value_obj);
      if(!k[0]) return recv;
      (void)pref_editor_add(editor,value_obj?'S':'R',k,value_obj?v:NULL); return recv; }
    if (has(m,"putInt")){ const char*k=jni_string_utf(va_arg(va,void*));
      int v=va_arg(va,int); char b[32]; snprintf(b,sizeof b,"%d",v);
      if(!k[0]) return recv;
      (void)pref_editor_add(editor,'I',k,b); return recv; }
    if (has(m,"putLong")){ const char*k=jni_string_utf(va_arg(va,void*));
      long long v=va_arg(va,long long); char b[32]; snprintf(b,sizeof b,"%lld",v);
      if(!k[0]) return recv;
      (void)pref_editor_add(editor,'L',k,b); return recv; }
    if (has(m,"putFloat")){ const char*k=jni_string_utf(va_arg(va,void*));
      double v=va_arg(va,double); char b[32]; snprintf(b,sizeof b,"%.9g",v);
      if(!k[0]) return recv;
      (void)pref_editor_add(editor,'F',k,b); return recv; }
    if (has(m,"putBoolean")){ const char*k=jni_string_utf(va_arg(va,void*));
      int v=va_arg(va,int);
      if(!k[0]) return recv;
      (void)pref_editor_add(editor,'B',k,v?"1":"0"); return recv; }
    if (has(m,"remove")){ const char*k=jni_string_utf(va_arg(va,void*));
      (void)pref_editor_add(editor,'R',k,NULL); return recv; }
    if (has(m,"clear")){ if(editor){mutexLock(&g_prefs_lock);editor->clear_first=1;mutexUnlock(&g_prefs_lock);} return recv; }
    return recv;
  }

  if (has(cls,"WindowManager") && has(m,"getDefaultDisplay"))
    return jni_make_object("android/view/Display");
  if (has(cls,"DisplayManager") && has(m,"getDisplay"))
    return jni_make_object("android/view/Display");
  if (has(cls,"res/Resources")){
    if (has(m,"getConfiguration")) return jni_make_object("Configuration");
    if (has(m,"getDisplayMetrics")) return jni_make_object("DisplayMetrics");
    return jni_make_object("Resources");
  }
  if (is_context_class(cls)){
    if (has(m,"getFilesDir")) return unity_make_file(g_internal_files,NULL);
    if (has(m,"getCacheDir")||has(m,"getExternalCacheDir")) return unity_make_file(g_cache,NULL);
    if (has(m,"getCodeCacheDir")) return unity_make_file(g_code_cache,NULL);
    if (has(m,"getNoBackupFilesDir")) return unity_make_file(g_no_backup,NULL);
    if (has(m,"getDataDir")) return unity_make_file(g_internal_root,NULL);
    if (has(m,"getObbDir")) return unity_make_file(g_obb,NULL);
    if (has(m,"getExternalFilesDir")) {
      const char *type = jni_string_utf(va_arg(va,void*)); return unity_make_file(g_files,type && type[0] ? type : NULL);
    }
    if (has(m,"getPackageName")) return jni_make_string(SS_PACKAGE);
    if (has(m,"getPackageCodePath")||has(m,"getPackageResourcePath")) return jni_make_local_string(managed_path(g_package_root));
    if (has(m,"getAssets")) return jni_make_object("AssetManager");
    if (has(m,"getResources")) return jni_make_object("Resources");
    if (has(m,"getClassLoader")) return jni_make_object("java/lang/ClassLoader");
    if (has(m,"getApplicationContext")||has(m,"getBaseContext")) return jni_make_object("android/content/Context");
    if (has(m,"getPackageManager")) return jni_make_object("android/content/pm/PackageManager");
    if (has(m,"getApplicationInfo")) return jni_make_object("android/content/pm/ApplicationInfo");
    if (has(m,"getSharedPreferences")) {
      const char *name=jni_string_utf(va_arg(va,void*));
      (void)va_arg(va,int);
      return pref_handle(name&&name[0]?name:"default");
    }
    if (has(m,"getSystemService")) {
      const char *service = jni_string_utf(va_arg(va,void*));
      if (strstr(service,"audio")) return jni_make_object("android/media/AudioManager");
      if (strstr(service,"connect")) return jni_make_object("android/net/ConnectivityManager");
      if (strstr(service,"window")) return jni_make_object("android/view/WindowManager");
      if (strstr(service,"display")) return jni_make_object("android/hardware/display/DisplayManager");
      if (strstr(service,"vibrator")) return jni_make_object("android/os/Vibrator");
      return jni_make_object("java/lang/Object");
    }
    return jni_make_object("android/content/Context");
  }
  if (has(cls,"pm/PackageManager")) {
    if (has(m,"getPackageInfo")) return jni_make_object("android/content/pm/PackageInfo");
    if (has(m,"getApplicationInfo")) return jni_make_object("android/content/pm/ApplicationInfo");
    if (has(m,"getApplicationLabel")) return jni_make_string("Genshin Impact");
    if (has(m,"getInstallerPackageName")) return jni_make_string("");
  }
  if (has(cls,"net/ConnectivityManager")) {
    if (has(m,"getActiveNetworkInfo")) return jni_make_object("android/net/NetworkInfo");
    if (has(m,"getActiveNetwork")) return jni_make_object("android/net/Network");
    if (has(m,"getNetworkCapabilities")) return jni_make_object("android/net/NetworkCapabilities");
  }
  if (has(cls,"java/io/File")) {
    const char *path = unity_file_path(recv);
    if (has(m,"getAbsolutePath")||has(m,"getCanonicalPath")||has(m,"getPath")||has(m,"toString"))
      return jni_make_local_string(managed_path(path));
    if (has(m,"getName")) { const char *slash=strrchr(path,'/'); return jni_make_local_string(slash?slash+1:path); }
    if (has(m,"getParentFile")) { char parent[768]; snprintf(parent,sizeof parent,"%s",path); char *slash=strrchr(parent,'/');
      if (slash && slash!=parent) *slash='\0';
      return unity_make_file(parent,NULL); }
    if (has(m,"getParent")) { char parent[768]; snprintf(parent,sizeof parent,"%s",path); char *slash=strrchr(parent,'/');
      if (slash && slash!=parent) *slash='\0';
      return jni_make_local_string(managed_path(parent)); }
  }

  if (has(cls,"UnityPlayer")) return jni_make_object("UnityPlayer");

  return jni_make_object(cls); /* default: opaque handle, never NULL */
}

uint64_t unity_dispatch_int(void *recv, const void *id_, va_list va){ const struct FakeID *id = id_;
  const char *cls=id->cls, *m=id->name;

  if (has(cls,"java/util/Iterator") && has(m,"hasNext")){
    UHandle*it=recv; PrefSnapshot *snapshot=pref_snapshot_for_handle(recv);
    return (uint64_t)((is_uh(it,UJ_ITER) && snapshot && it->idx < snapshot->count) ? 1 : 0);
  }
  if (has(cls,"java/util/Map") && !has(cls,"Entry")){
    PrefSnapshot *snapshot=pref_snapshot_for_handle(recv);
    if (has(m,"size"))    return (uint64_t)(snapshot?snapshot->count:0);
    if (has(m,"isEmpty")) return (uint64_t)(!snapshot||snapshot->count==0);
    if (has(m,"containsKey")){ const char*k=jni_string_utf(va_arg(va,void*));
      return (uint64_t)(snapshot&&kv_find(snapshot->items,snapshot->count,k)?1:0); }
  }
  if (has(cls,"java/util/Set")) {
    PrefSnapshot *snapshot=pref_snapshot_for_handle(recv);
    if (has(m,"size")) return (uint64_t)(snapshot?snapshot->count:0);
    if (has(m,"isEmpty")) return (uint64_t)(!snapshot||snapshot->count==0);
    if (has(m,"contains")) { const char *k=jni_string_utf(va_arg(va,void*));
      return (uint64_t)(snapshot&&kv_find(snapshot->items,snapshot->count,k)?1:0); }
  }

  if (has(cls,"java/io/InputStream")){
    UHandle*h=recv; if(!is_uh(h,UJ_INPUTSTREAM)||h->fd<0) return (uint64_t)-1;
    if (has(m,"available")){ long cur=asset_fd_seek(h->fd,h->packed,0,SEEK_CUR);
      return (uint64_t)(cur>=0&&cur<h->len?h->len-cur:0); }
    if (has(m,"skip")){ long nskip=(long)va_arg(va,long long); if(nskip<0)return 0;
      long before=asset_fd_seek(h->fd,h->packed,0,SEEK_CUR); if(before<0)return 0;
      if(nskip>h->len-before)nskip=h->len-before;
      long after=asset_fd_seek(h->fd,h->packed,nskip,SEEK_CUR);
      return (uint64_t)(after>before?after-before:0); }
    if (has(m,"close")){ asset_fd_close(h->fd,h->packed); h->fd=-1; return 0; }
    if (has(m,"read")){
      if (strstr(id->sig,"([B")){                     /* read(byte[][,off,len]) */
        void *arr = va_arg(va,void*);
        int alen=0; char *buf = jni_bytearray_data(arr,&alen);
        int off=0, len=alen;
        if (strstr(id->sig,"([BII)")){ off=va_arg(va,int); len=va_arg(va,int); }
        if (!buf || off < 0 || len < 0 || off > alen || len > alen - off) return (uint64_t)-1;
        long got=asset_fd_read(h->fd,h->packed,buf+off,(size_t)len);
        return got>0? (uint64_t)got : (uint64_t)-1;   /* -1 == EOF, per InputStream */
      }
      unsigned char c=0; long got=asset_fd_read(h->fd,h->packed,&c,1);
      return (uint64_t)(got==1?c:-1);                  /* read() one byte */
    }
    return 0;
  }

  if (has(cls,"SharedPreferences") && !has(cls,"Editor")){
    PrefStore *store=prefs_for_handle(recv);
    if (has(m,"contains")){ const char*k=jni_string_utf(va_arg(va,void*));
      mutexLock(&g_prefs_lock); int found=store&&kv_find(store->items,store->count,k)!=NULL;
      mutexUnlock(&g_prefs_lock); return (uint64_t)found; }
    if (has(m,"getInt")){ const char*k=jni_string_utf(va_arg(va,void*));
      int def=va_arg(va,int); mutexLock(&g_prefs_lock);
      KV*kv=store?kv_find(store->items,store->count,k):NULL;
      long long value=(kv&&kv->type=='I')?strtoll(kv->val,NULL,10):def;
      mutexUnlock(&g_prefs_lock); return (uint64_t)value; }
    if (has(m,"getLong")){ const char*k=jni_string_utf(va_arg(va,void*));
      long long def=va_arg(va,long long); mutexLock(&g_prefs_lock);
      KV*kv=store?kv_find(store->items,store->count,k):NULL;
      long long value=(kv&&kv->type=='L')?strtoll(kv->val,NULL,10):def;
      mutexUnlock(&g_prefs_lock); return (uint64_t)value; }
    if (has(m,"getBoolean")){ const char*k=jni_string_utf(va_arg(va,void*));
      int def=va_arg(va,int); mutexLock(&g_prefs_lock);
      KV*kv=store?kv_find(store->items,store->count,k):NULL;
      int value=(kv&&kv->type=='B')?(kv->val[0]=='1'||kv->val[0]=='t'||kv->val[0]=='T'):def;
      mutexUnlock(&g_prefs_lock); return (uint64_t)value; }
    return 0;
  }
  if (has(cls,"SharedPreferences$Editor")){
    PrefEditor *editor=pref_editor_from(recv);
    if (has(m,"commit")) return (uint64_t)pref_editor_commit(editor);
    if (has(m,"putInt")){ const char*k=jni_string_utf(va_arg(va,void*));
      int v=va_arg(va,int); char b[32]; snprintf(b,sizeof b,"%d",v);
      (void)pref_editor_add(editor,'I',k,b); return (uint64_t)(uintptr_t)recv; }
    if (has(m,"putLong")){ const char*k=jni_string_utf(va_arg(va,void*));
      long long v=va_arg(va,long long); char b[32]; snprintf(b,sizeof b,"%lld",v);
      (void)pref_editor_add(editor,'L',k,b); return (uint64_t)(uintptr_t)recv; }
    if (has(m,"putBoolean")){ const char*k=jni_string_utf(va_arg(va,void*));
      int v=va_arg(va,int); (void)pref_editor_add(editor,'B',k,v?"1":"0"); return (uint64_t)(uintptr_t)recv; }
    return (uint64_t)(uintptr_t)recv;
  }

  if (has(cls,"AssetFileDescriptor")){
    UHandle*a=recv;
    if (has(m,"getStartOffset")) return (uint64_t)(is_uh(a,UJ_AFD)?a->off:0);
    if (has(m,"getLength")||has(m,"getDeclaredLength")) return (uint64_t)(is_uh(a,UJ_AFD)?a->len:0);
    return 0;
  }
  if (has(cls,"java/io/FileDescriptor")){ UHandle*f=recv; return (uint64_t)(is_uh(f,UJ_FD)?(unsigned)f->fd:0); }

  if (has(cls,"java/io/File")) {
    const char *path=unity_file_path(recv); struct stat st;
    if (has(m,"exists")) return stat(path,&st)==0;
    if (has(m,"isDirectory")) return stat(path,&st)==0 && S_ISDIR(st.st_mode);
    if (has(m,"isFile")) return stat(path,&st)==0 && S_ISREG(st.st_mode);
    if (has(m,"length")) return stat(path,&st)==0 ? (uint64_t)st.st_size : 0;
    if (has(m,"lastModified")) return stat(path,&st)==0 ? (uint64_t)st.st_mtime * 1000u : 0;
    if (!strcmp(m,"getFreeSpace") || !strcmp(m,"getUsableSpace") ||
        !strcmp(m,"getTotalSpace")) {
      UStorageStats fs;
      if (!storage_stats(path, &fs)) return 0;
      if (!strcmp(m,"getFreeSpace")) return fs.free_bytes;
      if (!strcmp(m,"getUsableSpace")) return fs.available_bytes;
      return fs.total_bytes;
    }
    if (has(m,"canRead")||has(m,"canWrite")) return stat(path,&st)==0;
    if (has(m,"mkdir")||has(m,"mkdirs")) {
      if (strncmp(path,g_root,strlen(g_root)) != 0) return 0;
      char tmp[768]; snprintf(tmp,sizeof tmp,"%s",path);
      for(char *p=tmp+strlen(g_root)+1;*p;++p) if(*p=='/'){*p='\0';mkdir(tmp,0777);*p='/';}
      return mkdir(tmp,0777)==0 || errno==EEXIST;
    }
  }

  if (has(cls,"android/os/StatFs")) {
    UStorageStats fs;
    if (!storage_stats(g_root, &fs)) return 0;
    if (!strcmp(m,"getAvailableBytes"))      return fs.available_bytes;
    if (!strcmp(m,"getFreeBytes"))           return fs.free_bytes;
    if (!strcmp(m,"getTotalBytes"))          return fs.total_bytes;
    if (!strcmp(m,"getBlockSizeLong"))       return fs.block_size;
    if (!strcmp(m,"getAvailableBlocksLong")) return fs.available_blocks;
    if (!strcmp(m,"getBlockCountLong"))      return fs.block_count;
    if (!strcmp(m,"getBlockSize"))           return legacy_block_count(fs.block_size);
    if (!strcmp(m,"getAvailableBlocks"))     return legacy_block_count(fs.available_blocks);
    if (!strcmp(m,"getBlockCount"))          return legacy_block_count(fs.block_count);
  }

  if (is_context_class(cls)) {
    if (has(m,"checkSelfPermission")||has(m,"checkCallingPermission")||has(m,"checkCallingOrSelfPermission")) return 0;
  }
  if (has(cls,"pm/PackageManager")) {
    if (has(m,"checkPermission")) return 0;
    if (has(m,"hasSystemFeature")) return 1;
  }
  if (has(cls,"net/NetworkInfo")) {
    if (has(m,"isConnected")||has(m,"isConnectedOrConnecting")||has(m,"isAvailable")) return g_net_on != 0;
    if (has(m,"getType")) return 1; /* TYPE_WIFI */
  }
  if (has(cls,"net/NetworkCapabilities") && has(m,"has")) return g_net_on != 0;

  if (has(cls,"view/Display")||has(cls,"DisplayMetrics")||has(cls,"DisplayManager")){
    if (has(m,"getWidth")||has(m,"WidthPixels")||has(m,"getRawWidth"))  return screen_width;
    if (has(m,"getHeight")||has(m,"HeightPixels")||has(m,"getRawHeight"))return screen_height;
    if (has(m,"getRotation")) return 0;
    if (has(m,"getDisplayId")) return 0;
    return 0;
  }
  return 0;
}

void unity_dispatch_void(void *recv, const void *id_, va_list va){ const struct FakeID *id = id_;
  const char *cls=id->cls, *m=id->name;
  if (has(cls,"ComboForUnity")) {
    if (!strcmp(m,"invoke")) {
      const char *invoke_name = jni_string_utf(va_arg(va,void*));
      const char *invoke_payload = jni_string_utf(va_arg(va,void*));
      const int callback_index = va_arg(va,int);
      const int observed = combo_bridge_observe_invoke(invoke_name,
                                                        callback_index);

      if (observed == 0 && invoke_name &&
          !strcmp(invoke_name, "login_login"))
        (void)combo_auth_request_login(callback_index);
      if (observed == 0 && invoke_name &&
          !strcmp(invoke_name, "login_logout"))
        combo_auth_invalidate_session();
      if (observed == 0 && invoke_name &&
          (!strcmp(invoke_name, "login_init") ||
           !strcmp(invoke_name, "login_switch_role") ||
           (!strcmp(invoke_name, "launch_show_consent_banner") &&
            combo_json_string_equals(invoke_payload, "level", "privacy")))) {
        const int completed = !strcmp(invoke_name, "login_init")
          ? combo_complete_native_bridge_init(callback_index)
          : !strcmp(invoke_name, "login_switch_role")
            ? combo_complete_empty_switch_role(callback_index)
            : combo_complete_necessary_consent(callback_index);

        /* Android's switch-role fallback invokes the managed result before
         * returning from JNI.  Waiting for main's post-render drain here can
         * deadlock when nativeRender synchronously waits for that result. */
        if (completed == 0 &&
            !strcmp(invoke_name, "login_switch_role"))
          combo_bridge_after_render();
      }
    }
    return;
  }
  if (has(cls,"java/io/InputStream") && has(m,"close")){ UHandle*h=recv;
    if(is_uh(h,UJ_INPUTSTREAM)&&h->fd>=0){asset_fd_close(h->fd,h->packed);h->fd=-1;} return; }
  if (has(cls,"AssetFileDescriptor") && has(m,"close")){ UHandle*a=recv;
    if(is_uh(a,UJ_AFD)&&a->fd>=0){asset_fd_close(a->fd,a->packed);a->fd=-1;} return; }
  if (has(cls,"SharedPreferences$Editor") && has(m,"apply")){
    (void)pref_editor_commit(pref_editor_from(recv)); return; }
  if (has(cls,"SharedPreferences$Editor") && has(m,"putString")){ /* if routed here as void */
    const char*k=jni_string_utf(va_arg(va,void*)); void *value_obj=va_arg(va,void*);
    const char*v=jni_string_utf(value_obj);
    (void)pref_editor_add(pref_editor_from(recv),value_obj?'S':'R',k,value_obj?v:NULL); return; }
  if (has(cls,"UnityPlayer")){
    return;
  }
  (void)recv;(void)va;
}

float unity_dispatch_float(void *recv, const void *id_, va_list va){ const struct FakeID *id=id_;
  if (has(id->cls,"view/Display") && has(id->name,"getRefreshRate"))
    return (float)REFRESH_HZ;
  if (has(id->cls,"SharedPreferences") && !has(id->cls,"Editor") && has(id->name,"getFloat")) {
    const char *key=jni_string_utf(va_arg(va,void*)); double def=va_arg(va,double);
    PrefStore *store=prefs_for_handle(recv); mutexLock(&g_prefs_lock);
    KV *kv=store?kv_find(store->items,store->count,key):NULL;
    float value=(kv&&kv->type=='F')?(float)strtod(kv->val,NULL):(float)def;
    mutexUnlock(&g_prefs_lock); return value;
  }
  (void)recv; return 0.0f;
}

typedef union { uint8_t z; int8_t b; uint16_t c; int16_t s; int32_t i; int64_t j;
                float f; double d; void *l; } UJValue;
float unity_dispatch_float_a(void *recv, const void *id_, const void *args_){ const struct FakeID *id=id_;
  const UJValue *args=args_;
  if (has(id->cls,"view/Display") && has(id->name,"getRefreshRate"))
    return (float)REFRESH_HZ;
  if (args && has(id->cls,"SharedPreferences") && !has(id->cls,"Editor") && has(id->name,"getFloat")) {
    const char *key=jni_string_utf(args[0].l); PrefStore *store=prefs_for_handle(recv);
    mutexLock(&g_prefs_lock); KV *kv=store?kv_find(store->items,store->count,key):NULL;
    float value=(kv&&kv->type=='F')?(float)strtod(kv->val,NULL):args[1].f;
    mutexUnlock(&g_prefs_lock); return value;
  }
  (void)recv; return 0.0f;
}
void *unity_dispatch_editor_float_a(void *recv, const void *id_, const void *args_){ const struct FakeID *id=id_;
  const UJValue *args=args_;
  if (args && has(id->cls,"SharedPreferences$Editor") && has(id->name,"putFloat")) {
    const char *key=jni_string_utf(args[0].l); char value[32]; snprintf(value,sizeof value,"%.9g",(double)args[1].f);
    if (key[0]) (void)pref_editor_add(pref_editor_from(recv),'F',key,value);
    return recv;
  }
  return recv;
}

void unity_jni_init(const char *data_root){
  snprintf(g_root,sizeof g_root,"%s",data_root && *data_root ? data_root : GAME_HOME);
  snprintf(g_assets,sizeof g_assets,"%s/assets",g_root);
  snprintf(g_internal_root,sizeof g_internal_root,"%s/android_internal",g_root);
  snprintf(g_internal_files,sizeof g_internal_files,"%s/files",g_internal_root);
  snprintf(g_files,sizeof g_files,"%s/files",g_root);
  snprintf(g_cache,sizeof g_cache,"%s/cache",g_root);
  snprintf(g_code_cache,sizeof g_code_cache,"%s/code_cache",g_root);
  snprintf(g_no_backup,sizeof g_no_backup,"%s/no_backup",g_root);
  snprintf(g_obb,sizeof g_obb,"%s/obb",g_root);
  /* Unity receives the extracted package root. Its ordinary "/assets/..."
   * suffix is then served by the loose or first-boot optimized asset backend. */
  snprintf(g_package_root,sizeof g_package_root,"%s",g_root);
  snprintf(g_shared_prefs,sizeof g_shared_prefs,"%s/shared_prefs",g_root);
  mkdir(g_root,0777); mkdir(g_internal_root,0777); mkdir(g_internal_files,0777);
  mkdir(g_files,0777); mkdir(g_cache,0777); mkdir(g_code_cache,0777);
  mkdir(g_no_backup,0777); mkdir(g_obb,0777); mkdir(g_shared_prefs,0777);
  mutexLock(&g_prefs_lock);
  g_default_prefs=pref_store_get_locked("default");
  mutexUnlock(&g_prefs_lock);
}
