#pragma once

#include <stddef.h>
#include <stdint.h>

int asset_pack_open_existing(const char *root);
int asset_pack_build(const char *assets_root, const char *root);
int asset_pack_active(void);
const char *asset_pack_error(void);

typedef struct {
  void *handle;
} AssetPackOperation;

/* Pin a numeric packed handle across a route-generation validation.  Public
 * fd operations may safely re-acquire the handle while this pin is held. */
int asset_pack_operation_acquire(int fd, AssetPackOperation *operation);
void asset_pack_operation_release(AssetPackOperation *operation);
long asset_pack_operation_read(AssetPackOperation *operation, void *buffer,
                               size_t count);
long asset_pack_operation_pread(AssetPackOperation *operation, void *buffer,
                                size_t count, long offset);
long asset_pack_operation_lseek(AssetPackOperation *operation, long offset,
                                int whence);

int asset_pack_stat_path(const char *path, uint64_t *size, uint64_t *ino);
int asset_pack_stat_path_info(const char *path, uint64_t *size, uint64_t *ino,
                              int *directory);
int asset_pack_stat_relative(const char *path, uint64_t *size, uint64_t *ino);
int asset_pack_open_path(const char *path);
int asset_pack_dup_fd(int fd);
int asset_pack_dup_fd_min(int fd, int minimum, int cloexec);
int asset_pack_dup2_fd(int fd, int target);
int asset_pack_fd_is(int fd);
long asset_pack_read_fd(int fd, void *buffer, size_t count);
long asset_pack_pread_fd(int fd, void *buffer, size_t count, long offset);
long asset_pack_lseek_fd(int fd, long offset, int whence);
int asset_pack_fstat_fd(int fd, uint64_t *size, uint64_t *ino, int *directory);
int asset_pack_close_fd(int fd);

int asset_pack_read_all_path(const char *path, void **data, size_t *size);
int asset_pack_read_all_relative(const char *path, void **data, size_t *size);
size_t asset_pack_entry_count(void);
const char *asset_pack_entry_path(size_t index);

void *asset_pack_opendir_path(const char *path);
int asset_pack_dir_is(const void *dir);
const char *asset_pack_readdir_path(void *dir, uint8_t *type, uint64_t *ino);
int asset_pack_closedir_path(void *dir);
