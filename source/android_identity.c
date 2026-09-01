/*
 * Identity services which Android would normally provide through the package
 * manager and Settings.Secure.  The supported package's public signer
 * certificate is pinned in the compatibility layer, so a redundant APK ZIP
 * does not have to remain on the SD card after its files are extracted.
 * ANDROID_ID is an app-local random value persisted in stable private config
 * storage and is deliberately not exposed as OAID/ad ID.
 */

#include <errno.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <switch.h>

#include "android_identity.h"
#include "config.h"

#define IDENTITY_DIR "sdmc:/config/genshinimpact_nx"
#define ANDROID_ID_PATH IDENTITY_DIR "/android_id"
#define ANDROID_ID_TEMP_PATH IDENTITY_DIR "/.android_id.tmp"

/* SHA-256 of the sole v2/v3 signer certificate in the supported base APK.
 * Subject/issuer: CN=Android, OU=Android, O=Google Inc., L=Mountain View,
 * S=California, C=US. */
static const uint8_t exact_signer_sha256[32] = {
  0x68,0x84,0x25,0x30,0xe0,0xc7,0xf9,0xd4,
  0x2b,0x02,0x1e,0x14,0x8b,0xee,0x24,0x7f,
  0xa7,0x41,0xa0,0x63,0xbe,0x39,0x44,0x0c,
  0xc8,0xda,0xec,0xc9,0xa0,0x5f,0x66,0x9c,
};

static const uint8_t exact_signer_certificate[] = {
#include "android_signer_cert.inc"
};
_Static_assert(sizeof(exact_signer_certificate) == 1420,
               "exact signer certificate size changed");

static size_t signer_cert_size;
static char install_android_id[17];

static int hex_digit(int c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

static int load_android_id_file(const char *path) {
  char bytes[18];
  FILE *file = fopen(path, "rb");
  if (!file) return 0;
  const size_t got = fread(bytes, 1, sizeof(bytes), file);
  const int extra = fgetc(file);
  fclose(file);
  if ((got != 16 && !(got == 17 && bytes[16] == '\n')) || extra != EOF)
    return 0;
  for (size_t i = 0; i < 16; ++i) {
    const int digit = hex_digit((unsigned char)bytes[i]);
    if (digit < 0) return 0;
    install_android_id[i] = "0123456789abcdef"[digit];
  }
  install_android_id[16] = '\0';
  return strcmp(install_android_id, "0000000000000000") != 0;
}

static void generate_android_id(void) {
  uint8_t random[8];
  do {
    randomGet(random, sizeof(random));
  } while (!memcmp(random, "\0\0\0\0\0\0\0\0", sizeof(random)));
  for (size_t i = 0; i < sizeof(random); ++i)
    snprintf(install_android_id + i * 2, 3, "%02x", random[i]);
  install_android_id[16] = '\0';
}

static int ensure_identity_directory(void) {
  if (mkdir("sdmc:/config", 0777) != 0 && errno != EEXIST) return 0;
  if (mkdir(IDENTITY_DIR, 0700) != 0 && errno != EEXIST) return 0;
  return 1;
}

static int persist_android_id(void) {
  if (!install_android_id[0] || !ensure_identity_directory()) return 0;
  FILE *file = fopen(ANDROID_ID_TEMP_PATH, "wb");
  if (!file) return 0;
  const int written = fprintf(file, "%s\n", install_android_id) == 17;
  const int flushed = written && fflush(file) == 0;
  const int synchronized = flushed && fsync(fileno(file)) == 0;
  const int closed = fclose(file) == 0;
  if (synchronized && closed) {
    if (rename(ANDROID_ID_TEMP_PATH, ANDROID_ID_PATH) == 0) return 1;
    /* Some FAT implementations do not replace an invalid destination. */
    if (unlink(ANDROID_ID_PATH) == 0 &&
        rename(ANDROID_ID_TEMP_PATH, ANDROID_ID_PATH) == 0)
      return 1;
  }
  unlink(ANDROID_ID_TEMP_PATH);
  return 0;
}

static void load_or_create_android_id(const char *legacy_no_backup_dir) {
  char legacy_path[768], legacy_temporary[768];
  install_android_id[0] = '\0';
  if (load_android_id_file(ANDROID_ID_PATH)) return;
  if (!legacy_no_backup_dir) {
    generate_android_id();
    (void)persist_android_id();
    return;
  }
  const int path_length =
    snprintf(legacy_path, sizeof(legacy_path), "%s/android_id",
             legacy_no_backup_dir);
  const int temporary_length =
    snprintf(legacy_temporary, sizeof(legacy_temporary), "%s/.android_id.tmp",
             legacy_no_backup_dir);
  if (path_length > 0 && (size_t)path_length < sizeof(legacy_path) &&
      temporary_length > 0 &&
      (size_t)temporary_length < sizeof(legacy_temporary) &&
      load_android_id_file(legacy_path)) {
    if (persist_android_id()) {
      unlink(legacy_path);
      unlink(legacy_temporary);
    }
    return;
  }

  generate_android_id();
  (void)persist_android_id();
}

void android_identity_init(const char *no_backup_dir) {
  load_or_create_android_id(no_backup_dir);
  signer_cert_size = sizeof(exact_signer_certificate);
}

const char *android_identity_android_id(void) {
  return install_android_id;
}

const uint8_t *android_identity_signer(size_t *size_out) {
  if (size_out) *size_out = signer_cert_size;
  return signer_cert_size ? exact_signer_certificate : NULL;
}

const uint8_t *android_identity_signer_sha256(size_t *size_out) {
  if (size_out) *size_out = signer_cert_size ? sizeof(exact_signer_sha256) : 0;
  return signer_cert_size ? exact_signer_sha256 : NULL;
}

int android_identity_has_signing_certificate(const char *package_name,
                                             const void *certificate,
                                             size_t certificate_size,
                                             int input_type) {
  if (!package_name || strcmp(package_name, SS_PACKAGE) || !certificate ||
      !signer_cert_size)
    return 0;
  if (input_type == 0)
    return certificate_size == signer_cert_size &&
           !memcmp(certificate, exact_signer_certificate, signer_cert_size);
  if (input_type == 1)
    return certificate_size == sizeof(exact_signer_sha256) &&
           !memcmp(certificate, exact_signer_sha256,
                   sizeof(exact_signer_sha256));
  return 0;
}
