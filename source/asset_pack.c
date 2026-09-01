#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <switch.h>

#include "asset_pack.h"
#include "error.h"

/* Version 3 binds a structurally valid pack to the exact Android client.  This
 * prevents an upgrade from pairing a new libyuanshen.so with an old asset pack. */
#define PACK_VERSION 3u
#define PACK_HANDLES 512
#define PACK_CACHE_SIZE (64u * 1024u)

typedef struct {
  char magic[8];
  uint32_t version;
  uint32_t client_version;
  uint64_t pack_id;
  uint64_t file_size;
  uint64_t data_checksum;
} PackHeader;

typedef struct {
  char magic[8];
  uint32_t version;
  uint32_t count;
  uint32_t client_version;
  uint32_t reserved;
  uint64_t pack_id;
  uint64_t pack_size;
  uint64_t paths_size;
  uint64_t checksum;
  uint64_t data_checksum;
} IndexHeader;

_Static_assert(sizeof(PackHeader) == 40u, "asset pack header ABI");
_Static_assert(sizeof(IndexHeader) == 64u, "asset index header ABI");

typedef struct {
  uint64_t offset;
  uint64_t size;
  uint32_t path_offset;
  uint32_t path_length;
} DiskEntry;

typedef struct {
  char *path;
  uint64_t size;
  uint64_t offset;
} BuildEntry;

typedef struct {
  unsigned references;
  uint32_t entry;
  uint64_t directory_ino;
  uint64_t position;
  uint64_t cache_offset;
  size_t cache_size;
  unsigned char *cache;
  Mutex lock;
} PackOpenFile;

typedef struct {
  int state;
  int fd;
  unsigned active_operations;
  PackOpenFile *open_file;
} PackHandle;

typedef struct {
  uint64_t magic;
  size_t cursor;
  unsigned dots;
  char prefix[768];
  char name[256];
  char last[256];
} PackDir;

#define PACK_DIR_MAGIC 0x53534e5844495232ULL
#define PACK_DIRECTORY_ENTRY UINT32_MAX
#define PACK_HANDLE_FREE 0
#define PACK_HANDLE_ACTIVE 1
#define PACK_HANDLE_CLOSING 2

static int g_pack_fd = -1;
static DiskEntry *g_entries;
static char *g_paths;
static size_t g_entry_count;
static char g_pack_path[768];
static PackHandle g_handles[PACK_HANDLES];
static Mutex g_handle_lock;
static CondVar g_handle_cond;
static Mutex g_dup_lock;
static Mutex g_pack_io_lock;
static char g_error[192];

static void set_error(const char *message) {
  snprintf(g_error, sizeof g_error, "%s", message ? message : "Unknown error");
}

const char *asset_pack_error(void) {
  return g_error[0] ? g_error : "Asset pack is unavailable";
}

static uint64_t fnv_bytes(uint64_t hash, const void *data, size_t size) {
  const unsigned char *bytes = data;
  for (size_t i = 0; i < size; i++) {
    hash ^= bytes[i];
    hash *= 1099511628211ULL;
  }
  return hash;
}

static int read_at(int fd, void *buffer, size_t size, uint64_t offset) {
  if (lseek(fd, (off_t)offset, SEEK_SET) < 0) return 0;
  size_t done = 0;
  while (done < size) {
    ssize_t got = read(fd, (char *)buffer + done, size - done);
    if (got <= 0) return 0;
    done += (size_t)got;
  }
  return 1;
}

static int write_all(int fd, const void *buffer, size_t size) {
  size_t done = 0;
  while (done < size) {
    ssize_t put = write(fd, (const char *)buffer + done, size - done);
    if (put <= 0) return 0;
    done += (size_t)put;
  }
  return 1;
}

static int valid_relative(const char *path, size_t length) {
  if (!path || !length || path[0] == '/' || path[length - 1] == '/') return 0;
  size_t part = 0;
  for (size_t i = 0; i <= length; i++) {
    if (i == length || path[i] == '/') {
      if (!part || (part == 1 && path[i - 1] == '.') ||
          (part == 2 && path[i - 2] == '.' && path[i - 1] == '.')) return 0;
      part = 0;
    } else {
      if (path[i] == '\\' || path[i] == ':') return 0;
      part++;
    }
  }
  return 1;
}

static int normalize_asset_path(const char *input, char *output, size_t capacity,
                                int allow_empty) {
  if (!input || !output || capacity == 0) return 0;
  char temp[768];
  size_t length = strlen(input);
  if (length >= sizeof temp) return 0;
  for (size_t i = 0; i <= length; i++) temp[i] = input[i] == '\\' ? '/' : input[i];

  const char *relative = NULL;
  if (!strcmp(temp, "assets")) relative = temp + 6;
  else if (!strncmp(temp, "assets/", 7)) relative = temp + 7;
  for (const char *p = temp; (p = strstr(p, "/assets")) != NULL; p++) {
    if (p[7] == 0) relative = p + 7;
    else if (p[7] == '/') relative = p + 8;
  }
  if (!relative) return 0;

  size_t used = 0;
  const char *p = relative;
  while (*p) {
    while (*p == '/') p++;
    const char *start = p;
    while (*p && *p != '/') p++;
    size_t part = (size_t)(p - start);
    if (!part || (part == 1 && start[0] == '.')) continue;
    if (part == 2 && start[0] == '.' && start[1] == '.') return 0;
    if (used && used + 1 >= capacity) return 0;
    if (used) output[used++] = '/';
    if (used + part >= capacity) return 0;
    memcpy(output + used, start, part);
    used += part;
  }
  output[used] = 0;
  return allow_empty || used != 0;
}

static int normalize_relative(const char *input, char *output, size_t capacity) {
  return normalize_asset_path(input, output, capacity, 0);
}

static int entry_compare_path(const char *path, size_t *result) {
  size_t low = 0, high = g_entry_count;
  while (low < high) {
    size_t mid = low + (high - low) / 2;
    const char *candidate = g_paths + g_entries[mid].path_offset;
    int cmp = strcmp(path, candidate);
    if (cmp == 0) { *result = mid; return 1; }
    if (cmp < 0) high = mid;
    else low = mid + 1;
  }
  *result = low;
  return 0;
}

static int find_relative(const char *path, size_t *result) {
  if (!g_entries || !path) return 0;
  size_t length = strlen(path);
  if (!valid_relative(path, length)) return 0;
  return entry_compare_path(path, result);
}

static int find_directory_relative(const char *path, uint64_t *ino) {
  if (!g_entries || !path) return 0;
  size_t length = strlen(path);
  if (length && !valid_relative(path, length)) return 0;
  if (!length) {
    if (!g_entry_count) return 0;
  } else {
    char prefix[768];
    if (length + 2 > sizeof prefix) return 0;
    memcpy(prefix, path, length);
    prefix[length++] = '/';
    prefix[length] = 0;
    size_t index;
    entry_compare_path(prefix, &index);
    if (index >= g_entry_count ||
        strncmp(g_paths + g_entries[index].path_offset, prefix, length)) return 0;
  }
  uint64_t hash = fnv_bytes(1469598103934665603ULL, path, strlen(path));
  if (ino) *ino = hash ? hash : 1;
  return 1;
}

static void free_loaded(DiskEntry *entries, char *paths, int fd) {
  if (fd >= 0) close(fd);
  free(entries);
  free(paths);
}

static int load_pair(const char *pack_path, const char *index_path,
                     uint32_t client_version, DiskEntry **entries_out,
                     char **paths_out, size_t *count_out, int *fd_out) {
  int pack_fd = -1, index_fd = -1;
  DiskEntry *entries = NULL;
  char *paths = NULL;
  struct stat pack_stat, index_stat;
  PackHeader pack_header;
  IndexHeader index_header;

  pack_fd = open(pack_path, O_RDONLY);
  index_fd = open(index_path, O_RDONLY);
  if (pack_fd < 0 || index_fd < 0 || fstat(pack_fd, &pack_stat) != 0 ||
      fstat(index_fd, &index_stat) != 0) {
    set_error("No complete asset pack was found");
    goto failed;
  }
  if (!read_at(pack_fd, &pack_header, sizeof pack_header, 0) ||
      !read_at(index_fd, &index_header, sizeof index_header, 0) ||
      memcmp(pack_header.magic, "GINXPAK1", 8) ||
      memcmp(index_header.magic, "GINXIDX1", 8) ||
      pack_header.version != PACK_VERSION || index_header.version != PACK_VERSION ||
      pack_header.client_version != client_version ||
      index_header.client_version != client_version ||
      pack_header.pack_id != index_header.pack_id ||
      pack_header.file_size != (uint64_t)pack_stat.st_size ||
      pack_header.data_checksum != index_header.data_checksum ||
      index_header.pack_size != (uint64_t)pack_stat.st_size ||
      index_header.count == 0 || index_header.count > 100000 ||
      index_header.paths_size == 0 || index_header.paths_size > (64u << 20)) {
    set_error("The asset pack header is invalid");
    goto failed;
  }

  uint64_t entries_size = (uint64_t)index_header.count * sizeof(DiskEntry);
  uint64_t expected_index = sizeof(IndexHeader) + entries_size + index_header.paths_size;
  if (expected_index != (uint64_t)index_stat.st_size || expected_index > SIZE_MAX) {
    set_error("The asset index size is invalid");
    goto failed;
  }
  entries = malloc((size_t)entries_size);
  paths = malloc((size_t)index_header.paths_size);
  if (!entries || !paths ||
      !read_at(index_fd, entries, (size_t)entries_size, sizeof(IndexHeader)) ||
      !read_at(index_fd, paths, (size_t)index_header.paths_size,
               sizeof(IndexHeader) + entries_size)) {
    set_error("The asset index could not be read");
    goto failed;
  }
  uint64_t checksum = fnv_bytes(1469598103934665603ULL, entries, (size_t)entries_size);
  checksum = fnv_bytes(checksum, paths, (size_t)index_header.paths_size);
  if (checksum != index_header.checksum) {
    set_error("The asset index checksum is invalid");
    goto failed;
  }
  for (uint32_t i = 0; i < index_header.count; i++) {
    DiskEntry *entry = &entries[i];
    uint64_t path_end = (uint64_t)entry->path_offset + entry->path_length + 1;
    if (entry->path_length == 0 || path_end > index_header.paths_size ||
        paths[entry->path_offset + entry->path_length] != 0 ||
        !valid_relative(paths + entry->path_offset, entry->path_length) ||
        entry->offset < sizeof(PackHeader) || entry->offset > index_header.pack_size ||
        entry->size > index_header.pack_size - entry->offset ||
        (i && strcmp(paths + entries[i - 1].path_offset,
                     paths + entry->path_offset) >= 0)) {
      set_error("The asset index contains an invalid entry");
      goto failed;
    }
  }

  close(index_fd);
  *entries_out = entries;
  *paths_out = paths;
  *count_out = index_header.count;
  *fd_out = pack_fd;
  return 1;

failed:
  if (index_fd >= 0) close(index_fd);
  free_loaded(entries, paths, pack_fd);
  return 0;
}

int asset_pack_open_existing(const char *root, uint32_t client_version) {
  if (g_pack_fd >= 0) return 1;
  char pack_path[768], index_path[768];
  snprintf(pack_path, sizeof pack_path, "%s/assets.nxpack", root);
  snprintf(index_path, sizeof index_path, "%s/assets.nxidx", root);
  DiskEntry *entries = NULL;
  char *paths = NULL;
  size_t count = 0;
  int fd = -1;
  if (!load_pair(pack_path, index_path, client_version, &entries, &paths,
                 &count, &fd))
    return 0;
  g_entries = entries;
  g_paths = paths;
  g_entry_count = count;
  g_pack_fd = fd;
  snprintf(g_pack_path, sizeof g_pack_path, "%s", pack_path);
  g_error[0] = 0;
  return 1;
}

int asset_pack_active(void) {
  return g_pack_fd >= 0;
}

static int build_compare(const void *left, const void *right) {
  const BuildEntry *a = left, *b = right;
  return strcmp(a->path, b->path);
}

static int collect_files(const char *root, const char *relative,
                         BuildEntry **items, size_t *count, size_t *capacity) {
  char directory[1024];
  if (relative[0]) snprintf(directory, sizeof directory, "%s/%s", root, relative);
  else snprintf(directory, sizeof directory, "%s", root);
  DIR *dir = opendir(directory);
  if (!dir) return 0;
  int ok = 1;
  struct dirent *entry;
  while (ok && (entry = readdir(dir)) != NULL) {
    if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
    char child_relative[768], child_path[1024];
    if (snprintf(child_relative, sizeof child_relative, "%s%s%s", relative,
                 relative[0] ? "/" : "", entry->d_name) >= (int)sizeof child_relative ||
        snprintf(child_path, sizeof child_path, "%s/%s", root, child_relative) >=
          (int)sizeof child_path) {
      ok = 0;
      break;
    }
    struct stat st;
    if (stat(child_path, &st) != 0) { ok = 0; break; }
    if (S_ISDIR(st.st_mode)) {
      ok = collect_files(root, child_relative, items, count, capacity);
    } else if (S_ISREG(st.st_mode)) {
      if (*count == *capacity) {
        size_t next = *capacity ? *capacity * 2 : 256;
        BuildEntry *grown = realloc(*items, next * sizeof(**items));
        if (!grown) { ok = 0; break; }
        *items = grown;
        *capacity = next;
      }
      (*items)[*count].path = strdup(child_relative);
      if (!(*items)[*count].path) { ok = 0; break; }
      (*items)[*count].size = (uint64_t)st.st_size;
      (*items)[*count].offset = 0;
      (*count)++;
    }
  }
  closedir(dir);
  return ok;
}

static void free_build_entries(BuildEntry *items, size_t count) {
  for (size_t i = 0; i < count; i++) free(items[i].path);
  free(items);
}

static uint64_t align16(uint64_t value) {
  return (value + 15u) & ~15ULL;
}

static int verify_pack_data(int fd, const DiskEntry *entries, size_t count,
                            uint64_t expected) {
  void *buffer = malloc(1u << 20);
  if (!buffer) return 0;
  uint64_t total = 0, checked = 0;
  for (size_t i = 0; i < count; i++) total += entries[i].size;
  uint64_t checksum = 1469598103934665603ULL;
  unsigned last_percent = 101;
  for (size_t i = 0; i < count; i++) {
    uint64_t offset = 0;
    while (offset < entries[i].size) {
      size_t chunk = entries[i].size - offset > (1u << 20)
                       ? (1u << 20) : (size_t)(entries[i].size - offset);
      if (!read_at(fd, buffer, chunk, entries[i].offset + offset)) {
        free(buffer);
        return 0;
      }
      checksum = fnv_bytes(checksum, buffer, chunk);
      offset += chunk;
      checked += chunk;
      unsigned percent = total ? (unsigned)((checked * 100) / total) : 100;
      if (percent != last_percent) {
        char status[96];
        snprintf(status, sizeof status, "Verifying optimized assets\n\n  %u%%", percent);
        startup_status_update(status);
        last_percent = percent;
      }
    }
  }
  free(buffer);
  return checksum == expected;
}

int asset_pack_build(const char *assets_root, const char *root,
                     uint32_t client_version) {
  BuildEntry *items = NULL;
  size_t count = 0, capacity = 0;
  int pack_fd = -1, index_fd = -1, source_fd = -1;
  void *buffer = NULL;
  DiskEntry *disk_entries = NULL;
  char *paths = NULL;
  char pack_path[768], index_path[768], temp_pack[768], temp_index[768];
  snprintf(pack_path, sizeof pack_path, "%s/assets.nxpack", root);
  snprintf(index_path, sizeof index_path, "%s/assets.nxidx", root);
  snprintf(temp_pack, sizeof temp_pack, "%s/assets.nxpack.tmp", root);
  snprintf(temp_index, sizeof temp_index, "%s/assets.nxidx.tmp", root);
  unlink(temp_pack);
  unlink(temp_index);

  if (!collect_files(assets_root, "", &items, &count, &capacity) ||
      count == 0 || count > 100000) {
    set_error("The extracted assets could not be enumerated");
    goto failed;
  }
  qsort(items, count, sizeof(*items), build_compare);
  uint64_t paths_size = 0, total_data = 0, pack_size = sizeof(PackHeader);
  for (size_t i = 0; i < count; i++) {
    size_t path_length = strlen(items[i].path);
    if (path_length > UINT32_MAX || paths_size + path_length + 1 > UINT32_MAX) {
      set_error("The extracted asset paths are too large");
      goto failed;
    }
    pack_size = align16(pack_size);
    items[i].offset = pack_size;
    if (UINT64_MAX - pack_size < items[i].size) {
      set_error("The extracted assets are too large");
      goto failed;
    }
    pack_size += items[i].size;
    paths_size += path_length + 1;
    total_data += items[i].size;
  }

  uint64_t pack_id = 1469598103934665603ULL;
  for (size_t i = 0; i < count; i++) {
    pack_id = fnv_bytes(pack_id, items[i].path, strlen(items[i].path));
    pack_id = fnv_bytes(pack_id, &items[i].size, sizeof items[i].size);
  }
  uint64_t nonce = ((uint64_t)time(NULL) << 32) ^ armGetSystemTick();
  pack_id = fnv_bytes(pack_id, &nonce, sizeof nonce);
  if (!pack_id) pack_id = 1;

  if (!client_version) {
    set_error("The asset pack client version is invalid");
    goto failed;
  }

  PackHeader pack_header = {0};
  memcpy(pack_header.magic, "GINXPAK1", 8);
  pack_header.version = PACK_VERSION;
  pack_header.client_version = client_version;
  pack_header.pack_id = pack_id;
  pack_header.file_size = pack_size;
  pack_fd = open(temp_pack, O_WRONLY | O_CREAT | O_TRUNC, 0666);
  if (pack_fd < 0 || !write_all(pack_fd, &pack_header, sizeof pack_header)) {
    set_error("The temporary asset pack could not be created");
    goto failed;
  }
  buffer = malloc(1u << 20);
  if (!buffer) { set_error("Not enough memory to build the asset pack"); goto failed; }
  uint64_t written_data = 0, output_position = sizeof(PackHeader);
  uint64_t data_checksum = 1469598103934665603ULL;
  unsigned last_percent = 101;
  static const unsigned char padding[16] = {0};
  for (size_t i = 0; i < count; i++) {
    if (items[i].offset > output_position &&
        !write_all(pack_fd, padding, (size_t)(items[i].offset - output_position))) {
      set_error("The asset pack could not be written");
      goto failed;
    }
    output_position = items[i].offset;
    char source_path[1024];
    snprintf(source_path, sizeof source_path, "%s/%s", assets_root, items[i].path);
    source_fd = open(source_path, O_RDONLY);
    if (source_fd < 0) { set_error("An extracted asset disappeared while packing"); goto failed; }
    uint64_t remaining = items[i].size;
    while (remaining) {
      size_t chunk = remaining > (1u << 20) ? (1u << 20) : (size_t)remaining;
      ssize_t got = read(source_fd, buffer, chunk);
      if (got <= 0 || !write_all(pack_fd, buffer, (size_t)got)) {
        set_error("An extracted asset could not be packed");
        goto failed;
      }
      remaining -= (size_t)got;
      output_position += (size_t)got;
      written_data += (size_t)got;
      data_checksum = fnv_bytes(data_checksum, buffer, (size_t)got);
      unsigned percent = total_data ? (unsigned)((written_data * 100) / total_data) : 100;
      if (percent != last_percent) {
        char status[96];
        snprintf(status, sizeof status, "Optimizing game assets (first boot)\n\n  %u%%", percent);
        startup_status_update(status);
        last_percent = percent;
      }
    }
    close(source_fd);
    source_fd = -1;
  }
  pack_header.data_checksum = data_checksum;
  if (lseek(pack_fd, 0, SEEK_SET) < 0 ||
      !write_all(pack_fd, &pack_header, sizeof pack_header)) {
    set_error("The asset pack checksum could not be written");
    goto failed;
  }
  int pack_ok = fsync(pack_fd) == 0;
  if (close(pack_fd) != 0) pack_ok = 0;
  pack_fd = -1;
  if (!pack_ok) {
    set_error("The asset pack could not be finalized");
    goto failed;
  }

  disk_entries = malloc(count * sizeof(*disk_entries));
  paths = malloc((size_t)paths_size);
  if (!disk_entries || !paths) { set_error("Not enough memory to create the asset index"); goto failed; }
  uint32_t path_offset = 0;
  for (size_t i = 0; i < count; i++) {
    size_t path_length = strlen(items[i].path);
    disk_entries[i].offset = items[i].offset;
    disk_entries[i].size = items[i].size;
    disk_entries[i].path_offset = path_offset;
    disk_entries[i].path_length = (uint32_t)path_length;
    memcpy(paths + path_offset, items[i].path, path_length + 1);
    path_offset += (uint32_t)path_length + 1;
  }
  IndexHeader index_header = {0};
  memcpy(index_header.magic, "GINXIDX1", 8);
  index_header.version = PACK_VERSION;
  index_header.count = (uint32_t)count;
  index_header.client_version = client_version;
  index_header.pack_id = pack_id;
  index_header.pack_size = pack_size;
  index_header.paths_size = paths_size;
  index_header.checksum = fnv_bytes(1469598103934665603ULL, disk_entries,
                                    count * sizeof(*disk_entries));
  index_header.checksum = fnv_bytes(index_header.checksum, paths, (size_t)paths_size);
  index_header.data_checksum = data_checksum;
  index_fd = open(temp_index, O_WRONLY | O_CREAT | O_TRUNC, 0666);
  int index_ok = index_fd >= 0 &&
                 write_all(index_fd, &index_header, sizeof index_header) &&
                 write_all(index_fd, disk_entries, count * sizeof(*disk_entries)) &&
                 write_all(index_fd, paths, (size_t)paths_size);
  if (index_fd >= 0 && index_ok && fsync(index_fd) != 0) index_ok = 0;
  if (index_fd >= 0 && close(index_fd) != 0) index_ok = 0;
  index_fd = -1;
  if (!index_ok) {
    set_error("The asset index could not be finalized");
    goto failed;
  }

  DiskEntry *test_entries = NULL;
  char *test_paths = NULL;
  size_t test_count = 0;
  int test_fd = -1;
  if (!load_pair(temp_pack, temp_index, client_version, &test_entries,
                 &test_paths, &test_count, &test_fd))
    goto failed;
  if (!verify_pack_data(test_fd, test_entries, test_count, data_checksum)) {
    set_error("The written asset data did not pass verification");
    free_loaded(test_entries, test_paths, test_fd);
    goto failed;
  }
  free_loaded(test_entries, test_paths, test_fd);

  unlink(pack_path);
  unlink(index_path);
  if (rename(temp_pack, pack_path) != 0 || rename(temp_index, index_path) != 0 ||
      !asset_pack_open_existing(root, client_version)) {
    set_error("The validated asset pack could not be installed");
    goto failed;
  }
  free(buffer);
  free(disk_entries);
  free(paths);
  free_build_entries(items, count);
  return 1;

failed:
  if (source_fd >= 0) close(source_fd);
  if (pack_fd >= 0) close(pack_fd);
  if (index_fd >= 0) close(index_fd);
  unlink(temp_pack);
  unlink(temp_index);
  free(buffer);
  free(disk_entries);
  free(paths);
  free_build_entries(items, count);
  return 0;
}

int asset_pack_stat_relative(const char *path, uint64_t *size, uint64_t *ino) {
  size_t index;
  if (!find_relative(path, &index)) {
    return 0;
  }
  if (size) *size = g_entries[index].size;
  if (ino) *ino = 0x5353000000000000ULL | (uint64_t)(index + 1);
  return 1;
}

int asset_pack_stat_path(const char *path, uint64_t *size, uint64_t *ino) {
  char relative[768];
  if (!normalize_relative(path, relative, sizeof relative)) return 0;
  return asset_pack_stat_relative(relative, size, ino);
}

int asset_pack_stat_path_info(const char *path, uint64_t *size, uint64_t *ino,
                              int *directory) {
  char relative[768];
  size_t index;
  if (!normalize_asset_path(path, relative, sizeof relative, 1)) return 0;
  if (relative[0] && find_relative(relative, &index)) {
    if (size) *size = g_entries[index].size;
    if (ino) *ino = 0x5353000000000000ULL | (uint64_t)(index + 1);
    if (directory) *directory = 0;
    return 1;
  }
  if (find_directory_relative(relative, ino)) {
    if (size) *size = 0;
    if (directory) *directory = 1;
    return 1;
  }
  return 0;
}

static PackOpenFile *create_open_file(uint32_t entry, uint64_t directory_ino) {
  PackOpenFile *open_file = calloc(1, sizeof(*open_file));
  if (!open_file) return NULL;
  open_file->entry = entry;
  open_file->directory_ino = directory_ino;
  mutexInit(&open_file->lock);
  return open_file;
}

static void destroy_open_file(PackOpenFile *open_file) {
  if (!open_file) return;
  free(open_file->cache);
  free(open_file);
}

static PackHandle *find_handle_locked(int fd, int include_closing) {
  if (fd < 0) return NULL;
  for (int i = 0; i < PACK_HANDLES; i++) {
    PackHandle *handle = &g_handles[i];
    if (handle->fd == fd &&
        (handle->state == PACK_HANDLE_ACTIVE ||
         (include_closing && handle->state == PACK_HANDLE_CLOSING)))
      return handle;
  }
  return NULL;
}

static PackHandle *find_free_handle_locked(void) {
  for (int i = 0; i < PACK_HANDLES; i++)
    if (g_handles[i].state == PACK_HANDLE_FREE) return &g_handles[i];
  return NULL;
}

static PackHandle *acquire_handle(int fd) {
  PackHandle *handle;
  mutexLock(&g_handle_lock);
  handle = find_handle_locked(fd, 0);
  if (handle) handle->active_operations++;
  mutexUnlock(&g_handle_lock);
  return handle;
}

static void release_handle(PackHandle *handle) {
  mutexLock(&g_handle_lock);
  if (handle->active_operations) handle->active_operations--;
  if (!handle->active_operations) condvarWakeAll(&g_handle_cond);
  mutexUnlock(&g_handle_lock);
}

int asset_pack_operation_acquire(int fd, AssetPackOperation *operation) {
  if (!operation) { errno = EINVAL; return 0; }
  operation->handle = acquire_handle(fd);
  return operation->handle != NULL;
}

void asset_pack_operation_release(AssetPackOperation *operation) {
  if (!operation || !operation->handle) return;
  PackHandle *handle = operation->handle;
  operation->handle = NULL;
  release_handle(handle);
}

/* Installs one native backing descriptor and takes one open-file reference. */
static int install_handle(int fd, PackOpenFile *open_file) {
  mutexLock(&g_handle_lock);
  PackHandle *handle = find_free_handle_locked();
  if (!handle || find_handle_locked(fd, 1)) {
    mutexUnlock(&g_handle_lock);
    close(fd);
    errno = handle ? EBUSY : EMFILE;
    return -1;
  }
  open_file->references++;
  handle->fd = fd;
  handle->active_operations = 0;
  handle->open_file = open_file;
  handle->state = PACK_HANDLE_ACTIVE;
  mutexUnlock(&g_handle_lock);
  return fd;
}

int asset_pack_open_path(const char *path) {
  char relative[768];
  size_t index;
  uint64_t directory_ino = 0;
  if (!normalize_asset_path(path, relative, sizeof relative, 1)) return -1;
  int directory = !relative[0] || !find_relative(relative, &index);
  if (directory && !find_directory_relative(relative, &directory_ino)) {
    return -1;
  }
  int fd = open(g_pack_path, O_RDONLY);
  if (fd < 0) {
    return -1;
  }
  uint32_t entry = directory ? PACK_DIRECTORY_ENTRY : (uint32_t)index;
  PackOpenFile *open_file = create_open_file(entry, directory_ino);
  if (!open_file) {
    int saved = errno;
    close(fd);
    errno = saved ? saved : ENOMEM;
    return -1;
  }
  int result = install_handle(fd, open_file);
  if (result < 0) destroy_open_file(open_file);
  return result;
}

int asset_pack_fd_is(int fd) {
  mutexLock(&g_handle_lock);
  /* A draining handle still owns its native fd; keep callers out of libc. */
  int result = find_handle_locked(fd, 1) != NULL;
  mutexUnlock(&g_handle_lock);
  return result;
}

int asset_pack_dup_fd_min(int fd, int minimum, int cloexec) {
  if (minimum < 0) { errno = EINVAL; return -1; }
  PackHandle *source = acquire_handle(fd);
  if (!source) { errno = EBADF; return -1; }
  PackOpenFile *open_file = source->open_file;
  int temporary = open(g_pack_path, O_RDONLY);
  if (temporary < 0) {
    release_handle(source);
    return -1;
  }
  int duplicate = temporary;
  if (temporary < minimum) {
    duplicate = fcntl(temporary, cloexec ? F_DUPFD_CLOEXEC : F_DUPFD, minimum);
    int saved = errno;
    close(temporary);
    if (duplicate < 0) {
      release_handle(source);
      errno = saved;
      return -1;
    }
  } else if (cloexec && fcntl(duplicate, F_SETFD, FD_CLOEXEC) < 0) {
    int saved = errno;
    close(duplicate);
    release_handle(source);
    errno = saved;
    return -1;
  }
  int result = install_handle(duplicate, open_file);
  release_handle(source);
  return result;
}

int asset_pack_dup_fd(int fd) {
  return asset_pack_dup_fd_min(fd, 0, 0);
}

int asset_pack_dup2_fd(int fd, int target) {
  if (target < 0) { errno = EBADF; return -1; }
  if (fd == target) {
    PackHandle *same = acquire_handle(fd);
    if (!same) { errno = EBADF; return -1; }
    release_handle(same);
    return fd;
  }
  /* The pack index owns this private descriptor.  Returning a different fd
     would violate dup2's exact-target contract, so fail without corrupting it. */
  if (target == g_pack_fd) { errno = EBUSY; return -1; }
  /* Avoid cross-dup deadlocks where each operation drains the other's source. */
  mutexLock(&g_dup_lock);
  PackHandle *source = acquire_handle(fd);
  if (!source) {
    mutexUnlock(&g_dup_lock);
    errno = EBADF;
    return -1;
  }
  PackOpenFile *source_open_file = source->open_file;
  int temporary = open(g_pack_path, O_RDONLY);
  if (temporary < 0) {
    release_handle(source);
    mutexUnlock(&g_dup_lock);
    return -1;
  }

  PackOpenFile *retired = NULL;
  mutexLock(&g_handle_lock);
  PackHandle *target_handle = find_handle_locked(target, 1);
  while (target_handle && target_handle->state == PACK_HANDLE_CLOSING) {
    condvarWait(&g_handle_cond, &g_handle_lock);
    target_handle = find_handle_locked(target, 1);
  }
  PackHandle *slot = target_handle ? target_handle : find_free_handle_locked();
  if (!slot) {
    mutexUnlock(&g_handle_lock);
    close(temporary);
    release_handle(source);
    mutexUnlock(&g_dup_lock);
    errno = EMFILE;
    return -1;
  }
  if (target_handle) {
    target_handle->state = PACK_HANDLE_CLOSING;
    while (target_handle->active_operations)
      condvarWait(&g_handle_cond, &g_handle_lock);
  }

  int duplicate = temporary == target ? temporary : dup2(temporary, target);
  int saved = errno;
  if (temporary != target) close(temporary);
  if (duplicate < 0) {
    if (target_handle) target_handle->state = PACK_HANDLE_ACTIVE;
    condvarWakeAll(&g_handle_cond);
    mutexUnlock(&g_handle_lock);
    release_handle(source);
    mutexUnlock(&g_dup_lock);
    errno = saved;
    return -1;
  }

  if (target_handle && target_handle->open_file != source_open_file) {
    PackOpenFile *old_open_file = target_handle->open_file;
    if (old_open_file->references && --old_open_file->references == 0)
      retired = old_open_file;
    source_open_file->references++;
  } else if (!target_handle) {
    source_open_file->references++;
  }
  slot->fd = target;
  slot->active_operations = 0;
  slot->open_file = source_open_file;
  slot->state = PACK_HANDLE_ACTIVE;
  condvarWakeAll(&g_handle_cond);
  mutexUnlock(&g_handle_lock);
  destroy_open_file(retired);
  release_handle(source);
  mutexUnlock(&g_dup_lock);
  return target;
}

static long pread_entry(uint32_t entry_index, void *buffer, size_t count, uint64_t offset) {
  if (entry_index >= g_entry_count) { errno = EBADF; return -1; }
  DiskEntry *entry = &g_entries[entry_index];
  if (offset >= entry->size) return 0;
  uint64_t available = entry->size - offset;
  if ((uint64_t)count > available) count = (size_t)available;
  mutexLock(&g_pack_io_lock);
  int ok = read_at(g_pack_fd, buffer, count, entry->offset + offset);
  mutexUnlock(&g_pack_io_lock);
  return ok ? (long)count : -1;
}

static long read_handle_at(PackHandle *handle, PackOpenFile *open_file,
                           void *buffer, size_t count, uint64_t offset) {
  if (open_file->entry == PACK_DIRECTORY_ENTRY) { errno = EISDIR; return -1; }
  DiskEntry *entry = &g_entries[open_file->entry];
  if (offset >= entry->size) return 0;
  uint64_t available = entry->size - offset;
  if ((uint64_t)count > available) count = (size_t)available;
  return read_at(handle->fd, buffer, count, entry->offset + offset) ? (long)count : -1;
}

long asset_pack_read_fd(int fd, void *buffer, size_t count) {
  PackHandle *handle = acquire_handle(fd);
  if (!handle) { errno = EBADF; return -1; }
  PackOpenFile *open_file = handle->open_file;
  mutexLock(&open_file->lock);
  uint32_t entry = open_file->entry;
  long result = 0;
  if (entry == PACK_DIRECTORY_ENTRY) {
    errno = EISDIR;
    result = -1;
  } else if (open_file->position >= g_entries[entry].size) {
    result = 0;
  } else if (count > PACK_CACHE_SIZE / 2) {
    result = read_handle_at(handle, open_file, buffer, count, open_file->position);
    if (result > 0) open_file->position += (uint64_t)result;
  } else {
    size_t done = 0;
    while (done < count) {
      if (!open_file->cache || open_file->position < open_file->cache_offset ||
          open_file->position >= open_file->cache_offset + open_file->cache_size) {
        if (!open_file->cache) open_file->cache = malloc(PACK_CACHE_SIZE);
        if (!open_file->cache) {
          if (!done) errno = ENOMEM;
          result = done ? (long)done : -1;
          break;
        }
        open_file->cache_offset = open_file->position;
        open_file->cache_size = 0;
        long got = read_handle_at(handle, open_file, open_file->cache,
                                  PACK_CACHE_SIZE, open_file->cache_offset);
        if (got <= 0) { result = done ? (long)done : got; break; }
        open_file->cache_size = (size_t)got;
      }
      size_t inside = (size_t)(open_file->position - open_file->cache_offset);
      size_t available = open_file->cache_size - inside;
      size_t take = count - done < available ? count - done : available;
      memcpy((char *)buffer + done, open_file->cache + inside, take);
      open_file->position += take;
      done += take;
      result = (long)done;
    }
  }
  mutexUnlock(&open_file->lock);
  release_handle(handle);
  return result;
}

long asset_pack_operation_read(AssetPackOperation *operation, void *buffer,
                               size_t count) {
  PackHandle *handle = operation ? operation->handle : NULL;
  if (!handle) { errno = EBADF; return -1; }
  PackOpenFile *open_file = handle->open_file;
  mutexLock(&open_file->lock);
  uint32_t entry = open_file->entry;
  long result = 0;
  if (entry == PACK_DIRECTORY_ENTRY) {
    errno = EISDIR;
    result = -1;
  } else if (open_file->position >= g_entries[entry].size) {
    result = 0;
  } else if (count > PACK_CACHE_SIZE / 2) {
    result = read_handle_at(handle, open_file, buffer, count,
                            open_file->position);
    if (result > 0) open_file->position += (uint64_t)result;
  } else {
    size_t done = 0;
    while (done < count) {
      if (!open_file->cache || open_file->position < open_file->cache_offset ||
          open_file->position >= open_file->cache_offset +
                                      open_file->cache_size) {
        if (!open_file->cache) open_file->cache = malloc(PACK_CACHE_SIZE);
        if (!open_file->cache) {
          if (!done) errno = ENOMEM;
          result = done ? (long)done : -1;
          break;
        }
        open_file->cache_offset = open_file->position;
        open_file->cache_size = 0;
        long got = read_handle_at(handle, open_file, open_file->cache,
                                  PACK_CACHE_SIZE, open_file->cache_offset);
        if (got <= 0) { result = done ? (long)done : got; break; }
        open_file->cache_size = (size_t)got;
      }
      size_t inside = (size_t)(open_file->position - open_file->cache_offset);
      size_t available = open_file->cache_size - inside;
      size_t take = count - done < available ? count - done : available;
      memcpy((char *)buffer + done, open_file->cache + inside, take);
      open_file->position += take;
      done += take;
      result = (long)done;
    }
  }
  mutexUnlock(&open_file->lock);
  return result;
}

long asset_pack_pread_fd(int fd, void *buffer, size_t count, long offset) {
  PackHandle *handle = acquire_handle(fd);
  if (!handle) { errno = EBADF; return -1; }
  if (offset < 0) {
    release_handle(handle);
    errno = EINVAL;
    return -1;
  }
  PackOpenFile *open_file = handle->open_file;
  mutexLock(&open_file->lock);
  uint32_t entry = open_file->entry;
  long result = entry == PACK_DIRECTORY_ENTRY ? (errno = EISDIR, -1) :
                read_handle_at(handle, open_file, buffer, count, (uint64_t)offset);
  mutexUnlock(&open_file->lock);
  release_handle(handle);
  return result;
}

long asset_pack_operation_pread(AssetPackOperation *operation, void *buffer,
                                size_t count, long offset) {
  PackHandle *handle = operation ? operation->handle : NULL;
  if (!handle) { errno = EBADF; return -1; }
  if (offset < 0) { errno = EINVAL; return -1; }
  PackOpenFile *open_file = handle->open_file;
  mutexLock(&open_file->lock);
  uint32_t entry = open_file->entry;
  long result = entry == PACK_DIRECTORY_ENTRY ? (errno = EISDIR, -1) :
                read_handle_at(handle, open_file, buffer, count,
                               (uint64_t)offset);
  mutexUnlock(&open_file->lock);
  return result;
}

long asset_pack_lseek_fd(int fd, long offset, int whence) {
  PackHandle *handle = acquire_handle(fd);
  if (!handle) { errno = EBADF; return -1; }
  PackOpenFile *open_file = handle->open_file;
  mutexLock(&open_file->lock);
  if (open_file->entry == PACK_DIRECTORY_ENTRY) {
    mutexUnlock(&open_file->lock);
    release_handle(handle);
    errno = EISDIR;
    return -1;
  }
  uint64_t base;
  if (whence == SEEK_SET) base = 0;
  else if (whence == SEEK_CUR) base = open_file->position;
  else if (whence == SEEK_END) base = g_entries[open_file->entry].size;
  else {
    mutexUnlock(&open_file->lock);
    release_handle(handle);
    errno = EINVAL;
    return -1;
  }
  uint64_t result_position;
  if (base > (uint64_t)LONG_MAX) {
    mutexUnlock(&open_file->lock);
    release_handle(handle);
    errno = EOVERFLOW;
    return -1;
  }
  if (offset < 0) {
    uint64_t magnitude = (uint64_t)(-(offset + 1)) + 1;
    if (magnitude > base) {
      mutexUnlock(&open_file->lock);
      release_handle(handle);
      errno = EINVAL;
      return -1;
    }
    result_position = base - magnitude;
  } else {
    uint64_t positive = (uint64_t)offset;
    if (positive > (uint64_t)LONG_MAX - base) {
      mutexUnlock(&open_file->lock);
      release_handle(handle);
      errno = EOVERFLOW;
      return -1;
    }
    result_position = base + positive;
  }
  open_file->position = result_position;
  long result = (long)result_position;
  mutexUnlock(&open_file->lock);
  release_handle(handle);
  return result;
}

long asset_pack_operation_lseek(AssetPackOperation *operation, long offset,
                                int whence) {
  PackHandle *handle = operation ? operation->handle : NULL;
  if (!handle) { errno = EBADF; return -1; }
  PackOpenFile *open_file = handle->open_file;
  mutexLock(&open_file->lock);
  if (open_file->entry == PACK_DIRECTORY_ENTRY) {
    mutexUnlock(&open_file->lock);
    errno = EISDIR;
    return -1;
  }
  uint64_t base;
  if (whence == SEEK_SET) base = 0;
  else if (whence == SEEK_CUR) base = open_file->position;
  else if (whence == SEEK_END) base = g_entries[open_file->entry].size;
  else { mutexUnlock(&open_file->lock); errno = EINVAL; return -1; }
  uint64_t result_position;
  if (base > (uint64_t)LONG_MAX) {
    mutexUnlock(&open_file->lock); errno = EOVERFLOW; return -1;
  }
  if (offset < 0) {
    uint64_t magnitude = (uint64_t)(-(offset + 1)) + 1;
    if (magnitude > base) {
      mutexUnlock(&open_file->lock); errno = EINVAL; return -1;
    }
    result_position = base - magnitude;
  } else {
    uint64_t positive = (uint64_t)offset;
    if (positive > (uint64_t)LONG_MAX - base) {
      mutexUnlock(&open_file->lock); errno = EOVERFLOW; return -1;
    }
    result_position = base + positive;
  }
  open_file->position = result_position;
  long result = (long)result_position;
  mutexUnlock(&open_file->lock);
  return result;
}

int asset_pack_fstat_fd(int fd, uint64_t *size, uint64_t *ino, int *directory) {
  PackHandle *handle = acquire_handle(fd);
  if (!handle) return 0;
  PackOpenFile *open_file = handle->open_file;
  mutexLock(&open_file->lock);
  if (open_file->entry == PACK_DIRECTORY_ENTRY) {
    if (size) *size = 0;
    if (ino) *ino = open_file->directory_ino;
    if (directory) *directory = 1;
    mutexUnlock(&open_file->lock);
    release_handle(handle);
    return 1;
  }
  if (directory) *directory = 0;
  if (size) *size = g_entries[open_file->entry].size;
  if (ino) *ino = 0x5353000000000000ULL | (uint64_t)(open_file->entry + 1);
  mutexUnlock(&open_file->lock);
  release_handle(handle);
  return 1;
}

int asset_pack_close_fd(int fd) {
  PackOpenFile *retired = NULL;
  mutexLock(&g_handle_lock);
  PackHandle *handle = find_handle_locked(fd, 0);
  if (!handle) {
    mutexUnlock(&g_handle_lock);
    errno = EBADF;
    return -1;
  }
  handle->state = PACK_HANDLE_CLOSING;
  while (handle->active_operations)
    condvarWait(&g_handle_cond, &g_handle_lock);
  int result = close(handle->fd);
  PackOpenFile *open_file = handle->open_file;
  if (open_file->references && --open_file->references == 0) retired = open_file;
  handle->fd = -1;
  handle->open_file = NULL;
  handle->state = PACK_HANDLE_FREE;
  condvarWakeAll(&g_handle_cond);
  mutexUnlock(&g_handle_lock);
  destroy_open_file(retired);
  return result;
}

int asset_pack_read_all_relative(const char *path, void **data, size_t *size) {
  size_t index;
  if (!data || !size) return 0;
  if (!find_relative(path, &index)) {
    return 0;
  }
  if (g_entries[index].size > SIZE_MAX)
    return 0;
  size_t length = (size_t)g_entries[index].size;
  void *buffer = malloc(length ? length : 1);
  if (!buffer || (length && pread_entry((uint32_t)index, buffer, length, 0) != (long)length)) {
    free(buffer);
    return 0;
  }
  *data = buffer;
  *size = length;
  return 1;
}

int asset_pack_read_all_path(const char *path, void **data, size_t *size) {
  char relative[768];
  return normalize_relative(path, relative, sizeof relative) &&
         asset_pack_read_all_relative(relative, data, size);
}

size_t asset_pack_entry_count(void) {
  return g_entry_count;
}

const char *asset_pack_entry_path(size_t index) {
  return index < g_entry_count ? g_paths + g_entries[index].path_offset : NULL;
}

void *asset_pack_opendir_path(const char *path) {
  char relative[768];
  if (!normalize_asset_path(path, relative, sizeof relative, 1)) return NULL;
  size_t prefix = strlen(relative);
  int found = 0;
  for (size_t i = 0; i < g_entry_count; i++) {
    const char *entry = g_paths + g_entries[i].path_offset;
    if ((!prefix && entry[0]) ||
        (prefix && !strncmp(entry, relative, prefix) && entry[prefix] == '/')) {
      found = 1;
      break;
    }
  }
  if (!found) {
    return NULL;
  }
  PackDir *dir = calloc(1, sizeof(*dir));
  if (!dir) return NULL;
  dir->magic = PACK_DIR_MAGIC;
  snprintf(dir->prefix, sizeof dir->prefix, "%s", relative);
  return dir;
}

int asset_pack_dir_is(const void *dir) {
  return dir && ((const PackDir *)dir)->magic == PACK_DIR_MAGIC;
}

const char *asset_pack_readdir_path(void *opaque, uint8_t *type, uint64_t *ino) {
  PackDir *dir = opaque;
  if (!asset_pack_dir_is(dir)) return NULL;
  if (dir->dots < 2) {
    snprintf(dir->name, sizeof dir->name, "%s", dir->dots++ ? ".." : ".");
    if (type) *type = DT_DIR;
    if (ino) *ino = dir->dots;
    return dir->name;
  }
  size_t prefix = strlen(dir->prefix);
  while (dir->cursor < g_entry_count) {
    size_t index = dir->cursor++;
    const char *entry = g_paths + g_entries[index].path_offset;
    const char *tail = entry;
    if (prefix) {
      if (strncmp(entry, dir->prefix, prefix) || entry[prefix] != '/') continue;
      tail = entry + prefix + 1;
    }
    const char *slash = strchr(tail, '/');
    size_t length = slash ? (size_t)(slash - tail) : strlen(tail);
    if (!length || length >= sizeof dir->name) continue;
    if (strlen(dir->last) == length && !memcmp(dir->last, tail, length)) continue;
    memcpy(dir->name, tail, length);
    dir->name[length] = 0;
    snprintf(dir->last, sizeof dir->last, "%s", dir->name);
    if (type) *type = slash ? DT_DIR : DT_REG;
    if (ino) {
      uint64_t hash = fnv_bytes(1469598103934665603ULL, dir->name, length);
      *ino = hash ? hash : 1;
    }
    return dir->name;
  }
  return NULL;
}

int asset_pack_closedir_path(void *opaque) {
  PackDir *dir = opaque;
  if (!asset_pack_dir_is(dir)) return -1;
  dir->magic = 0;
  free(dir);
  return 0;
}
