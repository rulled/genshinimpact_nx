/* Android NDK asset API backed by either first-boot packed or loose assets. */

#include "android_asset.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

#include "config.h"
#include "asset_pack.h"
#include "unity_jni.h"

#define ASSET_MANAGER_TAG 0x4e58414du /* NXAM */
#define ASSET_TAG         0x4e584153u /* NXAS */
#define ASSET_DIR_TAG     0x4e584144u /* NXAD */

struct NxAAssetManager { uint32_t tag; };
struct NxAAsset {
  uint32_t tag;
  int fd;
  int packed;
  int64_t length;
  void *buffer;
};
struct NxAAssetDir {
  uint32_t tag;
  void *dir;
  int packed;
  char path[1024];
  char name[NAME_MAX + 1];
};

static struct NxAAssetManager g_manager = { ASSET_MANAGER_TAG };

static int safe_asset_path(char *out, size_t out_size, const char *relative) {
  if (!out || !out_size || !relative) return 0;
  while (*relative == '/') ++relative;
  if (strchr(relative, '\\') || strchr(relative, ':')) return 0;
  const char *part = relative;
  for (const char *p = relative;; ++p) {
    if (*p == '/' || *p == '\0') {
      const size_t len = (size_t)(p - part);
      if ((len == 1 && part[0] == '.') || (len == 2 && part[0] == '.' && part[1] == '.')) return 0;
      if (!*p) break;
      part = p + 1;
    }
  }
  const char *root = unity_assets_path();
  const int n = snprintf(out, out_size, "%s%s%s", root, root[0] && root[strlen(root)-1] == '/' ? "" : "/", relative);
  return n >= 0 && (size_t)n < out_size;
}

AAssetManager *AAssetManager_fromJava(void *env, void *asset_manager) {
  (void)env; (void)asset_manager; return &g_manager;
}

AAsset *AAssetManager_open(AAssetManager *manager, const char *filename, int mode) {
  (void)mode;
  if (manager != &g_manager || manager->tag != ASSET_MANAGER_TAG) return NULL;
  char path[1024]; if (!safe_asset_path(path,sizeof(path),filename)) return NULL;
  int fd = asset_pack_active() ? asset_pack_open_path(path) : -1;
  int packed = fd >= 0 && asset_pack_fd_is(fd);
  uint64_t length = 0, ino = 0;
  int directory = 0;
  if (packed) {
    if (!asset_pack_fstat_fd(fd, &length, &ino, &directory) || directory) {
      asset_pack_close_fd(fd);
      return NULL;
    }
  } else {
    fd = open(path,O_RDONLY); if (fd < 0) return NULL;
    struct stat st;
    if (fstat(fd,&st) != 0 || !S_ISREG(st.st_mode)) { close(fd); return NULL; }
    length = (uint64_t)st.st_size;
  }
  if (length > INT64_MAX) {
    if (packed) asset_pack_close_fd(fd); else close(fd);
    errno = EOVERFLOW;
    return NULL;
  }
  AAsset *asset=calloc(1,sizeof(*asset));
  if (!asset) { if (packed) asset_pack_close_fd(fd); else close(fd); return NULL; }
  asset->tag=ASSET_TAG; asset->fd=fd; asset->packed=packed;
  asset->length=(int64_t)length; return asset;
}

AAssetDir *AAssetManager_openDir(AAssetManager *manager, const char *dirname) {
  if (manager != &g_manager || manager->tag != ASSET_MANAGER_TAG) return NULL;
  char path[1024]; if (!safe_asset_path(path,sizeof(path),dirname ? dirname : "")) return NULL;
  void *dir = asset_pack_active() ? asset_pack_opendir_path(path) : NULL;
  int packed = dir != NULL;
  if (!dir) dir=opendir(path);
  if (!dir) return NULL;
  AAssetDir *asset_dir=calloc(1,sizeof(*asset_dir));
  if (!asset_dir) {
    if (packed) asset_pack_closedir_path(dir); else closedir((DIR *)dir);
    return NULL;
  }
  asset_dir->tag=ASSET_DIR_TAG; asset_dir->dir=dir; asset_dir->packed=packed;
  snprintf(asset_dir->path,sizeof(asset_dir->path),"%s",path);
  return asset_dir;
}

static int valid_asset(const AAsset *asset) { return asset && asset->tag == ASSET_TAG && asset->fd >= 0; }

int AAsset_read(AAsset *asset, void *buffer, size_t count) {
  if (!valid_asset(asset) || (!buffer && count)) { errno=EINVAL; return -1; }
  if (count > INT_MAX) count=INT_MAX;
  long got;
  do {
    got = asset->packed ? asset_pack_read_fd(asset->fd,buffer,count) :
                          (long)read(asset->fd,buffer,count);
  } while (got < 0 && errno == EINTR);
  return got < 0 ? -1 : (int)got;
}

int64_t AAsset_seek64(AAsset *asset, int64_t offset, int whence) {
  if (!valid_asset(asset)) { errno=EINVAL; return -1; }
  return asset->packed ? (int64_t)asset_pack_lseek_fd(asset->fd,(long)offset,whence) :
                         (int64_t)lseek(asset->fd,(off_t)offset,whence);
}
off_t AAsset_seek(AAsset *asset, off_t offset, int whence) { return (off_t)AAsset_seek64(asset,(int64_t)offset,whence); }

void AAsset_close(AAsset *asset) {
  if (!asset || asset->tag != ASSET_TAG) return;
  asset->tag=0;
  if(asset->fd>=0) {
    if (asset->packed) asset_pack_close_fd(asset->fd); else close(asset->fd);
  }
  free(asset->buffer); free(asset);
}

const void *AAsset_getBuffer(AAsset *asset) {
  if (!valid_asset(asset)) return NULL;
  if (asset->buffer) return asset->buffer;
  if ((uint64_t)asset->length > SIZE_MAX) return NULL;
  void *buffer=malloc(asset->length ? (size_t)asset->length : 1); if(!buffer)return NULL;
  int64_t saved=AAsset_seek64(asset,0,SEEK_CUR); if(saved<0){free(buffer);return NULL;}
  if(AAsset_seek64(asset,0,SEEK_SET)<0){free(buffer);return NULL;}
  size_t done=0;
  while(done<(size_t)asset->length){int n=AAsset_read(asset,(char*)buffer+done,(size_t)asset->length-done);
    if(n<=0)break;
    done+=(size_t)n;}
  (void)AAsset_seek64(asset,saved,SEEK_SET);
  if(done!=(size_t)asset->length){free(buffer);return NULL;}
  asset->buffer=buffer; return buffer;
}

int64_t AAsset_getLength64(AAsset *asset) { return valid_asset(asset) ? asset->length : 0; }
off_t AAsset_getLength(AAsset *asset) { return (off_t)AAsset_getLength64(asset); }
int64_t AAsset_getRemainingLength64(AAsset *asset) {
  if(!valid_asset(asset))return 0;
  int64_t pos=AAsset_seek64(asset,0,SEEK_CUR);
  return pos<0||pos>=asset->length?0:asset->length-(int64_t)pos;
}
off_t AAsset_getRemainingLength(AAsset *asset) { return (off_t)AAsset_getRemainingLength64(asset); }

int AAsset_openFileDescriptor64(AAsset *asset, int64_t *start, int64_t *length) {
  if (!valid_asset(asset)) return -1;
  int fd=asset->packed ? asset_pack_dup_fd(asset->fd) : dup(asset->fd);
  if(fd<0)return -1;
  if(start)*start=0;
  if(length)*length=asset->length;
  return fd;
}
int AAsset_openFileDescriptor(AAsset *asset, off_t *start, off_t *length) {
  int64_t s=0,n=0;int fd=AAsset_openFileDescriptor64(asset,&s,&n);
  if(fd>=0){if(start)*start=(off_t)s;if(length)*length=(off_t)n;}return fd;
}
int AAsset_isAllocated(AAsset *asset) { return valid_asset(asset) && asset->buffer != NULL; }

const char *AAssetDir_getNextFileName(AAssetDir *asset_dir) {
  if(!asset_dir||asset_dir->tag!=ASSET_DIR_TAG||!asset_dir->dir)return NULL;
  if (asset_dir->packed) {
    const char *name;
    while ((name=asset_pack_readdir_path(asset_dir->dir,NULL,NULL))!=NULL) {
      if(!strcmp(name,".")||!strcmp(name,".."))continue;
      snprintf(asset_dir->name,sizeof(asset_dir->name),"%s",name);return asset_dir->name;
    }
  } else {
    struct dirent *entry;
    while((entry=readdir((DIR *)asset_dir->dir))!=NULL){
      if(!strcmp(entry->d_name,".")||!strcmp(entry->d_name,".."))continue;
      snprintf(asset_dir->name,sizeof(asset_dir->name),"%s",entry->d_name);return asset_dir->name;
    }
  }
  return NULL;
}
void AAssetDir_rewind(AAssetDir *asset_dir) {
  if(!asset_dir||asset_dir->tag!=ASSET_DIR_TAG||!asset_dir->dir)return;
  if (asset_dir->packed) {
    asset_pack_closedir_path(asset_dir->dir);
    asset_dir->dir=asset_pack_opendir_path(asset_dir->path);
  } else rewinddir((DIR *)asset_dir->dir);
}
void AAssetDir_close(AAssetDir *asset_dir) {
  if(!asset_dir||asset_dir->tag!=ASSET_DIR_TAG)return;
  asset_dir->tag=0;
  if(asset_dir->dir) {
    if(asset_dir->packed)asset_pack_closedir_path(asset_dir->dir);
    else closedir((DIR *)asset_dir->dir);
  }
  free(asset_dir);
}
