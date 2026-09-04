#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "device_profile.h"

#define PROFILE_PATH "sdmc:/config/genshinimpact_nx/device_profile.ini"
#define PROFILE_MAX_ENTRIES 32u
#define PROFILE_KEY_MAX 31u
#define PROFILE_VALUE_MAX 127u
#define ANDROID_PROPERTY_VALUE_MAX 91u

typedef struct {
  char key[PROFILE_KEY_MAX + 1];
  char value[PROFILE_VALUE_MAX + 1];
} ProfileEntry;

static ProfileEntry profile_entries[PROFILE_MAX_ENTRIES];
static int profile_loaded;

static char *trim(char *text) {
  while (*text && isspace((unsigned char)*text)) ++text;
  char *end = text + strlen(text);
  while (end > text && isspace((unsigned char)end[-1])) --end;
  *end = '\0';
  return text;
}

static int profile_key_is_android_property(const char *key) {
  static const char *const keys[] = {
    "model", "device_name", "manufacturer", "brand", "product", "device",
    "board", "hardware", "platform", "fingerprint", "build_id",
    "display_id", "build_host", "build_user", "characteristics",
    "version_release", "version_sdk", "security_patch", "incremental",
  };
  for (size_t i = 0; i < sizeof keys / sizeof keys[0]; ++i)
    if (!strcmp(key, keys[i])) return 1;
  return 0;
}

static int profile_key_valid(const char *key) {
  return profile_key_is_android_property(key) ||
         !strcmp(key, "android_id") || !strcmp(key, "device_fp");
}

static int profile_value_valid(const char *key, const char *value) {
  if (!*key || !*value || !profile_key_valid(key)) return 0;
  if (profile_key_is_android_property(key) &&
      strlen(value) > ANDROID_PROPERTY_VALUE_MAX)
    return 0;
  if (!strcmp(key, "version_sdk")) {
    char *end = NULL;
    errno = 0;
    const long sdk = strtol(value, &end, 10);
    if (errno == ERANGE || end == value || *end || sdk <= 0 || sdk > INT_MAX)
      return 0;
  }
  if (!strcmp(key, "android_id")) {
    if (strlen(value) != 16) return 0;
    for (const char *c = value; *c; ++c) {
      const char *hex = strchr("0123456789abcdef", tolower((unsigned char)*c));
      if (!hex) return 0;
    }
  }
  for (const char *c = value; *c; ++c) {
    const unsigned char byte = (unsigned char)*c;
    if (byte < 0x20 || byte == 0x7f) return 0;
  }
  return 1;
}

static void store_entry(const char *key, const char *value) {
  for (unsigned i = 0; i < PROFILE_MAX_ENTRIES; ++i) {
    if (!profile_entries[i].key[0]) {
      strncpy(profile_entries[i].key, key, PROFILE_KEY_MAX);
      strncpy(profile_entries[i].value, value, PROFILE_VALUE_MAX);
      return;
    }
    if (!strcmp(profile_entries[i].key, key)) {
      strncpy(profile_entries[i].value, value, PROFILE_VALUE_MAX);
      return;
    }
  }
}

void device_profile_init(void) {
  profile_loaded = 1;
  FILE *file = fopen(PROFILE_PATH, "rb");
  if (!file) return;
  char line[256];
  while (fgets(line, sizeof line, file)) {
    char *comment = strchr(line, '#');
    if (comment) *comment = '\0';
    char *separator = strchr(line, '=');
    if (!separator) continue;
    *separator = '\0';
    char *key = trim(line);
    char *value = trim(separator + 1);
    if (strlen(key) > PROFILE_KEY_MAX ||
        strlen(value) > PROFILE_VALUE_MAX ||
        !profile_value_valid(key, value))
      continue;
    store_entry(key, value);
  }
  fclose(file);
}

const char *device_profile_get(const char *key) {
  if (!profile_loaded || !key || !*key) return NULL;
  for (unsigned i = 0; i < PROFILE_MAX_ENTRIES; ++i) {
    if (profile_entries[i].key[0] &&
        !strcmp(profile_entries[i].key, key))
      return profile_entries[i].value;
  }
  return NULL;
}

const char *device_profile_or(const char *key, const char *fallback) {
  const char *value = device_profile_get(key);
  return value ? value : fallback;
}
