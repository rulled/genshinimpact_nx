/* Exact HoYo Combo native-crypto bridge. */
#ifndef GENSHIN_COMBO_CRYPTO_H
#define GENSHIN_COMBO_CRYPTO_H

#include <stddef.h>

typedef enum {
  COMBO_CRYPTO_AES_ENCRYPT,
  COMBO_CRYPTO_AES_DECRYPT,
  COMBO_CRYPTO_RC4_ENCRYPT,
  COMBO_CRYPTO_RC4_DECRYPT,
} ComboCryptoOperation;

/* Initialize the exact staged library and run non-sensitive round trips. */
int combo_crypto_init(void);
int combo_crypto_ready(void);
const char *combo_crypto_error(void);

/* Main/attached guest threads only.  The caller owns and must erase `output`.
 * Returns zero on a complete, non-truncated JNI string result. */
int combo_crypto_transform(ComboCryptoOperation operation, const char *input,
                           char *output, size_t output_capacity);

#endif
