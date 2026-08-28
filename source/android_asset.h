/* Staged-files implementation of Android NDK's asset manager API. */
#ifndef GENSHIN_ANDROID_ASSET_H
#define GENSHIN_ANDROID_ASSET_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

typedef struct NxAAssetManager AAssetManager;
typedef struct NxAAsset AAsset;
typedef struct NxAAssetDir AAssetDir;

AAssetManager *AAssetManager_fromJava(void *env, void *asset_manager);
AAsset *AAssetManager_open(AAssetManager *manager, const char *filename, int mode);
AAssetDir *AAssetManager_openDir(AAssetManager *manager, const char *dirname);

int AAsset_read(AAsset *asset, void *buffer, size_t count);
off_t AAsset_seek(AAsset *asset, off_t offset, int whence);
int64_t AAsset_seek64(AAsset *asset, int64_t offset, int whence);
void AAsset_close(AAsset *asset);
const void *AAsset_getBuffer(AAsset *asset);
off_t AAsset_getLength(AAsset *asset);
int64_t AAsset_getLength64(AAsset *asset);
off_t AAsset_getRemainingLength(AAsset *asset);
int64_t AAsset_getRemainingLength64(AAsset *asset);
int AAsset_openFileDescriptor(AAsset *asset, off_t *start, off_t *length);
int AAsset_openFileDescriptor64(AAsset *asset, int64_t *start, int64_t *length);
int AAsset_isAllocated(AAsset *asset);

const char *AAssetDir_getNextFileName(AAssetDir *asset_dir);
void AAssetDir_rewind(AAssetDir *asset_dir);
void AAssetDir_close(AAssetDir *asset_dir);

#endif
