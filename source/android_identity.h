/* App-scoped Android identity and exact APK signer metadata. */

#ifndef GENSHIN_NX_ANDROID_IDENTITY_H
#define GENSHIN_NX_ANDROID_IDENTITY_H

#include <stddef.h>
#include <stdint.h>

/* Publishes the pinned signer for the supported official package and
 * creates/loads the private, random per-install ANDROID_ID. */
void android_identity_init(const char *no_backup_dir);

const char *android_identity_android_id(void);
const uint8_t *android_identity_signer(size_t *size_out);
const uint8_t *android_identity_signer_sha256(size_t *size_out);

/* Implements PackageManager.hasSigningCertificate for this package only.
 * input_type follows Android: 0 = raw X.509, 1 = SHA-256 digest. */
int android_identity_has_signing_certificate(const char *package_name,
                                             const void *certificate,
                                             size_t certificate_size,
                                             int input_type);

#endif
