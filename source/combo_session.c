#include "combo_session.h"

#include <switch.h>

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "android_identity.h"
#include "config.h"
#include "json_min.h"

#define SESSION_DIR "sdmc:/config/genshinimpact_nx"
#define SESSION_PATH SESSION_DIR "/combo_session.bin"
#define SESSION_TEMP_PATH SESSION_DIR "/.combo_session.tmp"
#define LEGACY_SESSION_PATH GAME_HOME "/no_backup/combo_session.bin"
#define LEGACY_SESSION_TEMP_PATH GAME_HOME "/no_backup/.combo_session.tmp"
#define SESSION_HEADER_SIZE 32u
#define SESSION_MAC_SIZE SHA256_HASH_SIZE
#define SESSION_NONCE_SIZE 16u
#define SESSION_MAX_INNER (256u * 1024u)
#define SESSION_MAX_NAME 255u
#define SESSION_MAX_PLAINTEXT (8u + SESSION_MAX_NAME + SESSION_MAX_INNER)
#define SESSION_MAX_FILE (SESSION_HEADER_SIZE + SESSION_MAX_PLAINTEXT + \
                          SESSION_MAC_SIZE)
#define SESSION_VERSION 1u

static const uint8_t g_session_magic[8] = {
  'G', 'N', 'X', 'S', 'E', 'S', 'S', '1'
};

static void erase_bytes(void *memory, size_t size) {
  volatile uint8_t *cursor = memory;
  while (cursor && size--) *cursor++ = 0;
}

static uint32_t load_u32_le(const uint8_t *bytes) {
  return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) |
         ((uint32_t)bytes[2] << 16) | ((uint32_t)bytes[3] << 24);
}

static void store_u32_le(uint8_t *bytes, uint32_t value) {
  bytes[0] = (uint8_t)value;
  bytes[1] = (uint8_t)(value >> 8);
  bytes[2] = (uint8_t)(value >> 16);
  bytes[3] = (uint8_t)(value >> 24);
}

static void store_u64_le(uint8_t *bytes, uint64_t value) {
  for (unsigned index = 0; index < 8; ++index)
    bytes[index] = (uint8_t)(value >> (index * 8));
}

static int constant_time_equal(const uint8_t *left, const uint8_t *right,
                               size_t size) {
  uint8_t difference = 0;
  for (size_t index = 0; index < size; ++index)
    difference |= left[index] ^ right[index];
  return difference == 0;
}

static int ensure_session_directory(void) {
  if (mkdir("sdmc:/config", 0777) != 0 && errno != EEXIST) return -1;
  if (mkdir(SESSION_DIR, 0700) != 0 && errno != EEXIST) return -1;
  return 0;
}

/* Derive independent encryption and MAC keys from a domain-separated console
 * identifier.  This protects a session copied off the SD card, while keeping
 * the limitation explicit: code running with access to this console can ask
 * SPL for the same identifier. */
static int derive_session_keys(uint8_t encryption_key[32],
                               uint8_t mac_key[32]) {
  static const char domain[] = "genshinimpact-nx/combo-session/v1";
  uint64_t device_id = 0;
  if (R_FAILED(splGetConfig(SplConfigItem_DeviceId, &device_id)) || !device_id)
    return -1;

  uint8_t material[sizeof(domain) - 1 + 8];
  memcpy(material, domain, sizeof(domain) - 1);
  store_u64_le(material + sizeof(domain) - 1, device_id);
  uint8_t seed[32];
  sha256CalculateHash(seed, material, sizeof material);

  uint8_t derivation[36];
  memcpy(derivation, seed, sizeof seed);
  memcpy(derivation + sizeof seed, "ENC1", 4);
  sha256CalculateHash(encryption_key, derivation, sizeof derivation);
  memcpy(derivation + sizeof seed, "MAC1", 4);
  sha256CalculateHash(mac_key, derivation, sizeof derivation);

  erase_bytes(derivation, sizeof derivation);
  erase_bytes(seed, sizeof seed);
  erase_bytes(material, sizeof material);
  device_id = 0;
  return 0;
}

static int nonempty_string_member(const JsonMinValue *object,
                                  const char *name) {
  JsonMinValue value;
  return json_min_object_get(object, name, &value) == 0 &&
         value.type == JSON_MIN_STRING && value.length > 2;
}

/* Reject anything except the exact successful Android SDK response contract
 * produced by combo_auth.c, and pin it to this install's persistent Android
 * ID before it can be delivered back to managed code. */
static int validate_inner_json(const char *json, size_t size) {
  JsonMinValue root, data, value;
  int64_t number = -1;
  if (!json || !size || size >= SESSION_MAX_INNER ||
      json_min_parse(json, size, &root) != 0 ||
      root.type != JSON_MIN_OBJECT ||
      json_min_object_get(&root, "ret", &value) != 0 ||
      json_min_int64(&value, &number) != 0 || number != 0 ||
      json_min_object_get(&root, "data", &data) != 0 ||
      data.type != JSON_MIN_OBJECT)
    return -1;

  if (json_min_object_get(&data, "app_id", &value) != 0 ||
      json_min_int64(&value, &number) != 0 || number != 4 ||
      json_min_object_get(&data, "channel_id", &value) != 0 ||
      json_min_int64(&value, &number) != 0 || number != 1 ||
      !nonempty_string_member(&data, "channel_token") ||
      !nonempty_string_member(&data, "open_id") ||
      !nonempty_string_member(&data, "combo_token") ||
      json_min_object_get(&data, "account_type", &value) != 0 ||
      json_min_int64(&value, &number) != 0)
    return -1;

  const char *android_id = android_identity_android_id();
  char stored_android_id[32];
  if (!android_id || !*android_id ||
      json_min_object_get(&data, "device_id", &value) != 0 ||
      json_min_string(&value, stored_android_id,
                      sizeof stored_android_id) != 0)
    return -1;
  const int matches = strcmp(stored_android_id, android_id) == 0;
  erase_bytes(stored_android_id, sizeof stored_android_id);
  return matches ? 0 : -1;
}

static int write_atomic(const uint8_t *bytes, size_t size) {
  if (ensure_session_directory() != 0) return -1;
  FILE *file = fopen(SESSION_TEMP_PATH, "wb");
  if (!file) return -1;
  int ok = fwrite(bytes, 1, size, file) == size;
  if (ok && fflush(file) != 0) ok = 0;
  if (ok && fsync(fileno(file)) != 0) ok = 0;
  if (fclose(file) != 0) ok = 0;
  if (ok && rename(SESSION_TEMP_PATH, SESSION_PATH) == 0) return 0;

  /* FAT does not consistently replace an existing destination atomically.
   * The temporary file is already durable before the narrow replacement. */
  if (ok && unlink(SESSION_PATH) == 0 &&
      rename(SESSION_TEMP_PATH, SESSION_PATH) == 0)
    return 0;
  unlink(SESSION_TEMP_PATH);
  return -1;
}

int combo_session_store(const char *inner_json, size_t inner_size,
                        const char *asterisk_name) {
  const size_t name_size = asterisk_name ? strlen(asterisk_name) : 0;
  if (validate_inner_json(inner_json, inner_size) != 0 ||
      name_size > SESSION_MAX_NAME ||
      inner_size > UINT32_MAX || name_size > UINT32_MAX)
    return -1;

  const size_t plaintext_size = 8 + name_size + inner_size;
  const size_t file_size = SESSION_HEADER_SIZE + plaintext_size +
                           SESSION_MAC_SIZE;
  if (plaintext_size > SESSION_MAX_PLAINTEXT || file_size > SESSION_MAX_FILE)
    return -1;

  uint8_t encryption_key[32], mac_key[32];
  if (derive_session_keys(encryption_key, mac_key) != 0) return -2;
  uint8_t *file_bytes = calloc(1, file_size);
  if (!file_bytes) {
    erase_bytes(encryption_key, sizeof encryption_key);
    erase_bytes(mac_key, sizeof mac_key);
    return -1;
  }

  memcpy(file_bytes, g_session_magic, sizeof g_session_magic);
  store_u32_le(file_bytes + 8, SESSION_VERSION);
  store_u32_le(file_bytes + 12, (uint32_t)plaintext_size);
  randomGet(file_bytes + 16, SESSION_NONCE_SIZE);

  uint8_t *plaintext = malloc(plaintext_size);
  if (!plaintext) {
    erase_bytes(file_bytes, file_size);
    free(file_bytes);
    erase_bytes(encryption_key, sizeof encryption_key);
    erase_bytes(mac_key, sizeof mac_key);
    return -1;
  }
  store_u32_le(plaintext, (uint32_t)name_size);
  store_u32_le(plaintext + 4, (uint32_t)inner_size);
  if (name_size) memcpy(plaintext + 8, asterisk_name, name_size);
  memcpy(plaintext + 8 + name_size, inner_json, inner_size);

  Aes256CtrContext aes;
  aes256CtrContextCreate(&aes, encryption_key, file_bytes + 16);
  aes256CtrCrypt(&aes, file_bytes + SESSION_HEADER_SIZE, plaintext,
                 plaintext_size);
  hmacSha256CalculateMac(file_bytes + SESSION_HEADER_SIZE + plaintext_size,
                         mac_key, sizeof mac_key, file_bytes,
                         SESSION_HEADER_SIZE + plaintext_size);
  const int result = write_atomic(file_bytes, file_size);

  erase_bytes(&aes, sizeof aes);
  erase_bytes(plaintext, plaintext_size);
  free(plaintext);
  erase_bytes(file_bytes, file_size);
  free(file_bytes);
  erase_bytes(encryption_key, sizeof encryption_key);
  erase_bytes(mac_key, sizeof mac_key);
  return result;
}

ComboSessionLoadResult combo_session_load(char **inner_json_out,
                                          size_t *inner_size_out,
                                          char *asterisk_name_out,
                                          size_t asterisk_name_capacity) {
  if (!inner_json_out || !inner_size_out || !asterisk_name_out ||
      !asterisk_name_capacity)
    return COMBO_SESSION_LOAD_INVALID;
  *inner_json_out = NULL;
  *inner_size_out = 0;
  asterisk_name_out[0] = 0;

  const char *source_path = SESSION_PATH;
  FILE *file = fopen(source_path, "rb");
  if (!file && errno == ENOENT) {
    source_path = LEGACY_SESSION_PATH;
    file = fopen(source_path, "rb");
  }
  if (!file) return errno == ENOENT ? COMBO_SESSION_LOAD_MISSING
                                    : COMBO_SESSION_LOAD_IO_ERROR;
  if (fseek(file, 0, SEEK_END) != 0) {
    fclose(file);
    return COMBO_SESSION_LOAD_IO_ERROR;
  }
  const long length = ftell(file);
  if (length < (long)(SESSION_HEADER_SIZE + SESSION_MAC_SIZE) ||
      length > (long)SESSION_MAX_FILE || fseek(file, 0, SEEK_SET) != 0) {
    fclose(file);
    return COMBO_SESSION_LOAD_INVALID;
  }
  const size_t file_size = (size_t)length;
  uint8_t *file_bytes = malloc(file_size);
  if (!file_bytes) {
    fclose(file);
    return COMBO_SESSION_LOAD_IO_ERROR;
  }
  const int read_ok = fread(file_bytes, 1, file_size, file) == file_size &&
                      fgetc(file) == EOF;
  fclose(file);
  if (!read_ok) {
    erase_bytes(file_bytes, file_size);
    free(file_bytes);
    return COMBO_SESSION_LOAD_IO_ERROR;
  }

  ComboSessionLoadResult result = COMBO_SESSION_LOAD_INVALID;
  const uint32_t plaintext_size = load_u32_le(file_bytes + 12);
  if (memcmp(file_bytes, g_session_magic, sizeof g_session_magic) != 0 ||
      load_u32_le(file_bytes + 8) != SESSION_VERSION ||
      plaintext_size < 8 || plaintext_size > SESSION_MAX_PLAINTEXT ||
      file_size != SESSION_HEADER_SIZE + (size_t)plaintext_size +
                   SESSION_MAC_SIZE)
    goto out;

  uint8_t encryption_key[32], mac_key[32], expected_mac[32];
  if (derive_session_keys(encryption_key, mac_key) != 0) {
    result = COMBO_SESSION_LOAD_KEY_UNAVAILABLE;
    goto out;
  }
  hmacSha256CalculateMac(expected_mac, mac_key, sizeof mac_key, file_bytes,
                         SESSION_HEADER_SIZE + plaintext_size);
  if (!constant_time_equal(expected_mac,
                           file_bytes + SESSION_HEADER_SIZE + plaintext_size,
                           sizeof expected_mac)) {
    erase_bytes(expected_mac, sizeof expected_mac);
    erase_bytes(encryption_key, sizeof encryption_key);
    erase_bytes(mac_key, sizeof mac_key);
    goto out;
  }
  erase_bytes(expected_mac, sizeof expected_mac);

  uint8_t *plaintext = malloc(plaintext_size);
  if (!plaintext) {
    erase_bytes(encryption_key, sizeof encryption_key);
    erase_bytes(mac_key, sizeof mac_key);
    result = COMBO_SESSION_LOAD_IO_ERROR;
    goto out;
  }
  Aes256CtrContext aes;
  aes256CtrContextCreate(&aes, encryption_key, file_bytes + 16);
  aes256CtrCrypt(&aes, plaintext, file_bytes + SESSION_HEADER_SIZE,
                 plaintext_size);
  erase_bytes(&aes, sizeof aes);
  erase_bytes(encryption_key, sizeof encryption_key);
  erase_bytes(mac_key, sizeof mac_key);

  const uint32_t name_size = load_u32_le(plaintext);
  const uint32_t inner_size = load_u32_le(plaintext + 4);
  if (name_size > SESSION_MAX_NAME ||
      name_size + 1 > asterisk_name_capacity ||
      inner_size == 0 || inner_size >= SESSION_MAX_INNER ||
      (size_t)name_size + (size_t)inner_size + 8 != plaintext_size ||
      memchr(plaintext + 8, 0, name_size) != NULL) {
    erase_bytes(plaintext, plaintext_size);
    free(plaintext);
    goto out;
  }

  char *inner = malloc((size_t)inner_size + 1);
  if (!inner) {
    erase_bytes(plaintext, plaintext_size);
    free(plaintext);
    result = COMBO_SESSION_LOAD_IO_ERROR;
    goto out;
  }
  memcpy(inner, plaintext + 8 + name_size, inner_size);
  inner[inner_size] = 0;
  if (validate_inner_json(inner, inner_size) != 0) {
    erase_bytes(inner, (size_t)inner_size + 1);
    free(inner);
    erase_bytes(plaintext, plaintext_size);
    free(plaintext);
    goto out;
  }

  if (name_size) memcpy(asterisk_name_out, plaintext + 8, name_size);
  asterisk_name_out[name_size] = 0;
  *inner_json_out = inner;
  *inner_size_out = inner_size;
  result = COMBO_SESSION_LOAD_OK;
  if (strcmp(source_path, LEGACY_SESSION_PATH) == 0 &&
      combo_session_store(inner, inner_size, asterisk_name_out) == 0) {
    unlink(LEGACY_SESSION_PATH);
    unlink(LEGACY_SESSION_TEMP_PATH);
  }
  erase_bytes(plaintext, plaintext_size);
  free(plaintext);

out:
  erase_bytes(file_bytes, file_size);
  free(file_bytes);
  return result;
}

void combo_session_invalidate(void) {
  unlink(SESSION_PATH);
  unlink(SESSION_TEMP_PATH);
  unlink(LEGACY_SESSION_PATH);
  unlink(LEGACY_SESSION_TEMP_PATH);
}
