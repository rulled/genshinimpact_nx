/* Native bridge for the libMHYComboCrypto.so shipped by Genshin 7.0.1. */

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "combo_crypto.h"
#include "jni_fake.h"
#include "plugin_loader.h"

#define CRYPTO_CLASS "com/combosdk/support/base/utils/CryptoUtils"
#define CRYPTO_SIGNATURE "(Ljava/lang/String;)Ljava/lang/String;"

typedef void *(*ComboCryptoFn)(void *env, void *clazz, void *input);

typedef struct {
  const char *java_name;
  const char *symbol;
  ComboCryptoFn function;
} ComboCryptoExport;

static ComboCryptoExport g_exports[] = {
  { "AESEncryptNative", "Java_com_combosdk_support_base_utils_CryptoUtils_AESEncryptNative", NULL },
  { "AESDecryptNative", "Java_com_combosdk_support_base_utils_CryptoUtils_AESDecryptNative", NULL },
  { "RC4EncryptNative", "Java_com_combosdk_support_base_utils_CryptoUtils_RC4EncryptNative", NULL },
  { "RC4DecryptNative", "Java_com_combosdk_support_base_utils_CryptoUtils_RC4DecryptNative", NULL },
};

static int g_ready;
static char g_error[192];

static void set_error(const char *format, ...) {
  va_list ap;
  va_start(ap, format);
  vsnprintf(g_error, sizeof(g_error), format, ap);
  va_end(ap);
}

static void erase_bytes(void *memory, size_t size) {
  volatile unsigned char *p = memory;
  while (p && size--) *p++ = 0;
}

int combo_crypto_ready(void) { return g_ready; }
const char *combo_crypto_error(void) { return g_error[0] ? g_error : NULL; }

int combo_crypto_transform(ComboCryptoOperation operation, const char *input,
                           char *output, size_t output_capacity) {
  if (output && output_capacity) output[0] = 0;
  if (!g_ready || operation < COMBO_CRYPTO_AES_ENCRYPT ||
      operation > COMBO_CRYPTO_RC4_DECRYPT || !input || !output ||
      output_capacity == 0) {
    set_error("invalid native crypto transform request");
    return -1;
  }
  if (jni_push_local_frame(4) != 0) {
    set_error("could not allocate crypto JNI local frame");
    return -1;
  }

  int result = -1;
  void *jni_input = jni_make_local_string(input);
  void *jni_output = jni_input
    ? g_exports[(int)operation].function(fake_env, NULL, jni_input)
    : NULL;
  const char *text = jni_output ? jni_string_utf(jni_output) : NULL;
  if (!text) {
    set_error("native crypto returned null");
  } else if (jni_exception_pending()) {
    set_error("native crypto left a JNI exception pending");
    jni_exception_clear();
  } else {
    const size_t size = strnlen(text, output_capacity);
    if (size == output_capacity) {
      set_error("native crypto output exceeds bridge capacity");
    } else {
      memcpy(output, text, size + 1);
      result = 0;
    }
  }

  jni_pop_local_frame(NULL);
  return result;
}

static void strip_ascii_newlines(char *text) {
  char *out = text;
  for (char *in = text; *in; ++in)
    if (*in != '\n' && *in != '\r') *out++ = *in;
  *out = 0;
}

static int round_trip(ComboCryptoOperation encrypt,
                      ComboCryptoOperation decrypt) {
  static const char fixture[] = "Z2Vuc2hpbi1ueC1jcnlwdG8tc2VsZi10ZXN0";
  char cipher[512] = {0};
  char plain[512] = {0};
  int result = -1;
  if (combo_crypto_transform(encrypt, fixture, cipher, sizeof(cipher)) != 0)
    goto done;
  strip_ascii_newlines(cipher);
  if (!cipher[0] || !strcmp(cipher, fixture)) {
    set_error("native crypto encryption self-test did not transform input");
    goto done;
  }
  if (combo_crypto_transform(decrypt, cipher, plain, sizeof(plain)) != 0)
    goto done;
  strip_ascii_newlines(plain);
  if (strcmp(plain, fixture)) {
    set_error("native crypto round-trip mismatch");
    goto done;
  }
  result = 0;

done:
  erase_bytes(cipher, sizeof(cipher));
  erase_bytes(plain, sizeof(plain));
  return result;
}

int combo_crypto_init(void) {
  if (g_ready) return 0;
  g_error[0] = 0;

  void *handle = plugin_loader_dlopen("libMHYComboCrypto.so", 0);
  if (!handle) {
    const char *detail = plugin_loader_dlerror();
    set_error("could not load native Combo crypto%s%s",
              detail ? ": " : "", detail ? detail : "");
    return -1;
  }
  if (plugin_loader_jni_onload(handle, fake_vm) < 0) {
    const char *detail = plugin_loader_dlerror();
    set_error("native Combo crypto initialization failed%s%s",
              detail ? ": " : "", detail ? detail : "");
    return -1;
  }

  for (size_t i = 0; i < sizeof(g_exports) / sizeof(g_exports[0]); ++i) {
    g_exports[i].function = (ComboCryptoFn)plugin_loader_dlsym(handle,
                                                               g_exports[i].symbol);
    if (!g_exports[i].function) {
      set_error("native Combo crypto export %s is absent", g_exports[i].java_name);
      return -1;
    }
    if (jni_register_native(CRYPTO_CLASS, g_exports[i].java_name,
                            CRYPTO_SIGNATURE,
                            (void *)g_exports[i].function) != 0 ||
        jni_find_registered_native(CRYPTO_CLASS, g_exports[i].java_name,
                                   CRYPTO_SIGNATURE) !=
          (void *)g_exports[i].function) {
      set_error("could not register native Combo crypto method %s",
                g_exports[i].java_name);
      return -1;
    }
  }

  g_ready = 1;
  if (round_trip(COMBO_CRYPTO_AES_ENCRYPT, COMBO_CRYPTO_AES_DECRYPT) != 0 ||
      round_trip(COMBO_CRYPTO_RC4_ENCRYPT, COMBO_CRYPTO_RC4_DECRYPT) != 0) {
    g_ready = 0;
    return -1;
  }
  return 0;
}
