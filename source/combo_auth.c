#include "combo_auth.h"

#include <switch.h>
#include <curl/curl.h>

#include <arpa/inet.h>
#include <errno.h>
#include <limits.h>
#include <netinet/in.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

#include "android_identity.h"
#include "combo_bridge.h"
#include "combo_session.h"
#include "config.h"
#include "json_min.h"

/* These production endpoints and client identifiers are copied from the
 * reviewed 6.7.0 assets/server_env_config.json entry (area=os, env=2). */
#define PASSPORT_TLS_PREFLIGHT_URL \
  "https://hk4e-sdk-os.hoyoverse.com/hk4e_global/account/ma-passport/api/getConfig"
#define PASSPORT_PASSWORD_URL \
  "https://hk4e-sdk-os.hoyoverse.com/hk4e_global/account/ma-passport/api/appLoginByPassword"
#define COMBO_LOGIN_URL \
  "https://hk4e-sdk-os.hoyoverse.com/hk4e_global/combo/granter/login/v2/login"
#define VERIFIER_INFO_URL \
  "https://passport-api-sg.hoyoverse.com/account/ma-verifier/api/getActionTicketInfo"
#define VERIFIER_METHOD_URL \
  "https://passport-api-sg.hoyoverse.com/account/ma-verifier/api/modifyActionTicketVerifyMethod"
#define VERIFIER_EMAIL_CAPTCHA_URL \
  "https://passport-api-sg.hoyoverse.com/account/ma-verifier/api/createEmailCaptchaByActionTicket"
#define VERIFIER_MOBILE_CAPTCHA_URL \
  "https://passport-api-sg.hoyoverse.com/account/ma-verifier/api/createMobileCaptchaByActionTicket"
#define VERIFIER_PARTIAL_URL \
  "https://passport-api-sg.hoyoverse.com/account/ma-verifier/api/verifyActionTicketPartly"
#define PASSPORT_APP_ID "c77bxgx7ljb4"
#define COMBO_APP_KEY "6a4c78fe0356ba4673b8071127b28123"
#define COMBO_APP_ID 4
#define COMBO_CHANNEL_ID 1

#define RSA_BYTES SPL_RSA_BUFFER_SIZE
#define RSA_PLAINTEXT_MAX (RSA_BYTES - 11u)
#define RSA_BASE64_CAPACITY 345u
#define INPUT_CAPACITY 246u
#define TOKEN_CAPACITY 8192u
#define ID_CAPACITY 256u
#define COUNTRY_CAPACITY 64u
#define MESSAGE_CAPACITY 1024u
#define EXTENSION_CAPACITY (64u * 1024u)
#define HTTP_RESPONSE_CAPACITY (512u * 1024u)
#define AUTH_CALLBACK_CAPACITY (256u * 1024u)
#define VERIFY_HEADER_CAPACITY (64u * 1024u)
#define AIGIS_HEADER_CAPACITY (16u * 1024u)
#define AIGIS_RESULT_CAPACITY (16u * 1024u)
#define AIGIS_FIELD_CAPACITY 4096u
#define GEETEST_HTML_CAPACITY (32u * 1024u)
#define GEETEST_REQUEST_CAPACITY (32u * 1024u)
#define GEETEST_STACK_SIZE (128u * 1024u)
#define GEETEST_PRIMARY_PORT 80u
#define GEETEST_FALLBACK_PORT 46321u
#define ACTION_TICKET_CAPACITY 8192u
#define VERIFY_FACTOR_CAPACITY RSA_BASE64_CAPACITY
#define VERIFY_METHOD_CAPACITY 8u
#define VERIFY_COMBINATION_CAPACITY 8u
#define UI_DISPLAY_MESSAGE_CAPACITY 256u
#define ASTERISK_NAME_CAPACITY 256u

#define VERIFY_METHOD_MOBILE 1
#define VERIFY_METHOD_EMAIL 2
#define VERIFY_METHOD_PASSWORD 32
#define VERIFY_TYPE_GEETEST 1
#define VERIFY_TYPE_IDENTITY 2
#define VERIFY_STATUS_VERIFIED "StatusVerified"
#define VERIFY_ACTION_TYPE "verify_for_component"

/* RSA/None/PKCS1Padding public key used by Porte OS 2.3.0 in this exact APK. */
static const uint8_t g_passport_rsa_modulus[RSA_BYTES] = {
  0xe0,0xf3,0x12,0xd8,0x95,0x4c,0xc0,0x1b,0x0e,0x22,0xb6,0x16,0x46,0x5b,0x98,0xc0,
  0x48,0x85,0x64,0xbe,0xc0,0xa6,0x1b,0x66,0xf7,0x3e,0x44,0xbb,0xf6,0xa7,0xcc,0x9d,
  0x3d,0x9c,0x1d,0x34,0xba,0x15,0xbe,0x49,0xca,0xc3,0x58,0x51,0x02,0x3f,0x0a,0x50,
  0xb6,0x8f,0xc6,0x94,0xe2,0x58,0x09,0x4c,0x15,0x33,0xfa,0xda,0x42,0x90,0x23,0xff,
  0x1b,0xdc,0x8c,0xae,0x6d,0x35,0x5c,0xf3,0x2b,0xde,0x26,0x86,0x4f,0xa3,0xf5,0x18,
  0x90,0xf9,0xa5,0x87,0x69,0x35,0xe6,0x3a,0x0b,0xab,0x87,0x14,0xa9,0xdd,0x86,0xf0,
  0x95,0x10,0xef,0x3a,0x07,0x2a,0xe7,0x23,0xe2,0x4f,0x3d,0x3e,0x91,0x27,0x8a,0x64,
  0x97,0x06,0x78,0x9f,0x79,0x6e,0xf2,0x7e,0x84,0x9f,0xd4,0x30,0xfd,0x19,0xc7,0x6a,
  0x81,0xe4,0xf1,0x25,0x66,0x68,0x8a,0xbd,0xf8,0x8d,0xa9,0x55,0x24,0xf0,0x0f,0x28,
  0xc4,0xb2,0x5d,0x04,0x1e,0x5e,0x0a,0x9a,0x96,0x57,0xa0,0x94,0xa8,0x32,0x90,0x29,
  0x24,0x0f,0xe2,0xc3,0x34,0x99,0x9b,0xd0,0xd5,0x2a,0xf8,0x51,0xc0,0x02,0x55,0xc6,
  0x20,0x62,0x70,0x9b,0x4d,0x93,0x16,0x84,0x94,0xd6,0x92,0xb3,0x57,0xa8,0x8f,0x13,
  0x32,0x89,0x9e,0x92,0x09,0xf2,0xdf,0x2f,0x9d,0xe8,0xd5,0xff,0x70,0x56,0xba,0xea,
  0x3a,0x33,0x14,0x81,0x78,0x1c,0x25,0xf9,0x69,0x6f,0x1b,0xa7,0xc0,0xbc,0xbe,0xad,
  0xad,0x16,0xf1,0x5b,0x95,0xe4,0x3f,0x43,0x70,0x94,0xe7,0xa1,0x16,0x58,0xa3,0x2b,
  0x39,0xca,0x66,0x16,0xd4,0x66,0x0b,0x5f,0x2c,0xf5,0xb8,0x99,0x05,0x54,0x7f,0xf1,
};
/* The SPL secure monitor rejects exponent buffers not aligned to a 32-bit
 * word (0x41A / SecureMonitorInvalidArgument). 65537 is therefore encoded as
 * the canonical four-byte big-endian integer, not the three-byte DER value. */
static const uint8_t g_rsa_exponent[] = { 0x00, 0x01, 0x00, 0x01 };
_Static_assert(sizeof g_rsa_exponent % sizeof(uint32_t) == 0,
               "SPL RSA exponent must be word-aligned");

typedef struct {
  char *data;
  size_t capacity;
  size_t length;
  int failed;
} StringBuilder;

typedef struct {
  char *data;
  size_t size;
  size_t capacity;
  int overflow;
} HttpResponse;

typedef struct {
  char verify[VERIFY_HEADER_CAPACITY];
  size_t verify_size;
  uint32_t verify_seen_sources;
  uint32_t verify_captured_sources;
  int verify_invalid;
  char aigis[AIGIS_HEADER_CAPACITY];
  size_t aigis_size;
  uint32_t aigis_seen_sources;
  uint32_t aigis_captured_sources;
  int aigis_invalid;
  uint32_t callback_lines;
  uint32_t debug_lines;
} HttpHeaders;

typedef struct {
  int listen_fd;
  const char *path;
  const char *complete_path;
  const char *html;
  size_t html_size;
  volatile int stop;
  volatile int result_ready;
  volatile int finished;
} GeetestServer;

#define HEADER_SOURCE_CALLBACK 0x1u
#define HEADER_SOURCE_DEBUG    0x2u

static int g_initialized;
static int g_spl_initialized;
static int g_tls_verified;
static uint64_t g_login_requests;
static int g_callback_index = -1;
static int g_preflight_state = COMBO_AUTH_PREFLIGHT_IDLE;
static int g_phase = COMBO_AUTH_PHASE_IDLE;
static int g_curl_code = CURLE_OK;
static long g_http_status;
static int g_service_result;
static int64_t g_server_retcode;
static int g_challenge_code;
static int g_retry_pending;
static char g_lifecycle_id[37];
static int g_passport_verify_retry;
static int g_verify_type;
static int g_verify_method;
static int g_verify_header_state;
static uint32_t g_verify_header_sources;
static uint64_t g_verify_header_bytes;
static uint64_t g_verify_header_lines;
static int g_aigis_header_state;
static uint32_t g_aigis_header_sources;
static uint64_t g_aigis_header_bytes;
static uint64_t g_geetest_pages_served;
static uint64_t g_geetest_results_received;
static uint64_t g_geetest_requests_rejected;
static int g_passport_aigis_retry;
static int g_geetest_retry_pending;
static int g_verifier_retry_pending;
static int g_verifier_steps;
static int g_selected_methods[VERIFY_METHOD_CAPACITY];
static size_t g_selected_method_count;
static int g_verify_combinations[VERIFY_COMBINATION_CAPACITY]
                                [VERIFY_METHOD_CAPACITY];
static size_t g_verify_combination_sizes[VERIFY_COMBINATION_CAPACITY];
static size_t g_verify_combination_original_indices
               [VERIFY_COMBINATION_CAPACITY];
static size_t g_verify_combination_count;
static int g_captcha_sent;
static int g_connection_ui_mode;
static int g_connection_ui_state;
static int g_connection_ui_result;
static uint64_t g_connection_ui_messages_sent;
static uint64_t g_connection_ui_messages_received;
static uint32_t g_connection_ui_pages_served;
static char g_connection_ui_message[UI_DISPLAY_MESSAGE_CAPACITY];
static int g_session_state = COMBO_AUTH_SESSION_UNCHECKED;
static uint64_t g_session_restores;
static uint64_t g_session_saves;
static uint64_t g_session_invalidations;
static int g_session_checked;
static int g_session_replay_pending;
/* This is the only account-derived display state retained after Passport.
 * It is redacted before the account plaintext is RSA-encrypted, never logged
 * or placed in the callback, and is persisted only inside combo_session's
 * authenticated encrypted envelope. */
static char g_asterisk_name[ASTERISK_NAME_CAPACITY];

static int supported_verify_method(int method);

/* Only RSA ciphertext survives the keyboard calls. Plaintext is stack-local,
 * encrypted immediately, and erased before returning to Unity. */
static char g_account_cipher[RSA_BASE64_CAPACITY];
static char g_password_cipher[RSA_BASE64_CAPACITY];
static char g_passport_token[TOKEN_CAPACITY];
static char g_passport_uid[ID_CAPACITY];
static char g_passport_country[COUNTRY_CAPACITY];
/* These values are opaque server-issued capabilities. They are never logged,
 * persisted, or exposed to Unity. */
static char g_verify_header[VERIFY_HEADER_CAPACITY];
static char g_aigis_result[AIGIS_RESULT_CAPACITY];
static char g_aigis_gt[AIGIS_FIELD_CAPACITY];
static char g_aigis_session[AIGIS_FIELD_CAPACITY];
static char g_aigis_risk_type[AIGIS_FIELD_CAPACITY];
static char g_action_ticket[ACTION_TICKET_CAPACITY];
static char g_risk_ticket[ACTION_TICKET_CAPACITY];
/* OTP plaintext lives here only between the blocking keyboard and the next
 * network tick. Password factors are RSA ciphertext before they reach here. */
static char g_verifier_factor[VERIFY_FACTOR_CAPACITY];

static void erase_bytes(void *memory, size_t size) {
  volatile unsigned char *cursor = memory;
  while (cursor && size--) *cursor++ = 0;
}

static void geetest_short_token(char output[9], const uint8_t random_bytes[5]) {
  static const char alphabet[] = "23456789ABCDEFGHJKLMNPQRSTUVWXYZ";
  uint64_t value = 0;
  for (unsigned index = 0; index < 5; ++index)
    value = (value << 8) | random_bytes[index];
  for (unsigned index = 0; index < 8; ++index) {
    output[7 - index] = alphabet[value & 31u];
    value >>= 5;
  }
  output[8] = 0;
}

static void set_connection_ui_message(const char *message) {
  erase_bytes(g_connection_ui_message, sizeof g_connection_ui_message);
  snprintf(g_connection_ui_message, sizeof g_connection_ui_message, "%s",
           message && *message ? message : "Login failed");
}

/* PlatformTools.getAsteriskName returns a display-only account label.  The
 * exact Android implementation reads the original account back from SDK
 * storage, which this wrapper intentionally does not create.  Produce the
 * same privacy property at the one point where plaintext exists: retain at
 * most the first and final character of the local part, plus the public mail
 * domain.  Non-mail account names retain only their endpoints. */
static void capture_asterisk_name(const char *account) {
  erase_bytes(g_asterisk_name, sizeof g_asterisk_name);
  if (!account || !*account) return;

  const size_t length = strnlen(account, INPUT_CAPACITY);
  if (!length || length >= INPUT_CAPACITY) return;
  const char *at = strchr(account, '@');
  if (at && at > account && at[1]) {
    const size_t local_length = (size_t)(at - account);
    if (local_length > 1) {
      (void)snprintf(g_asterisk_name, sizeof g_asterisk_name,
                     "%c****%c%s", account[0], account[local_length - 1], at);
    } else {
      (void)snprintf(g_asterisk_name, sizeof g_asterisk_name,
                     "%c****%s", account[0], at);
    }
    return;
  }

  if (length > 2) {
    (void)snprintf(g_asterisk_name, sizeof g_asterisk_name,
                   "%c****%c", account[0], account[length - 1]);
  } else {
    (void)snprintf(g_asterisk_name, sizeof g_asterisk_name, "****");
  }
}

static void erase_attempt_secrets(void) {
  erase_bytes(g_account_cipher, sizeof g_account_cipher);
  erase_bytes(g_password_cipher, sizeof g_password_cipher);
  erase_bytes(g_passport_token, sizeof g_passport_token);
  erase_bytes(g_passport_uid, sizeof g_passport_uid);
  erase_bytes(g_passport_country, sizeof g_passport_country);
  erase_bytes(g_verify_header, sizeof g_verify_header);
  erase_bytes(g_aigis_result, sizeof g_aigis_result);
  erase_bytes(g_aigis_gt, sizeof g_aigis_gt);
  erase_bytes(g_aigis_session, sizeof g_aigis_session);
  erase_bytes(g_aigis_risk_type, sizeof g_aigis_risk_type);
  erase_bytes(g_action_ticket, sizeof g_action_ticket);
  erase_bytes(g_risk_ticket, sizeof g_risk_ticket);
  erase_bytes(g_verifier_factor, sizeof g_verifier_factor);
  erase_bytes(g_selected_methods, sizeof g_selected_methods);
  erase_bytes(g_verify_combinations, sizeof g_verify_combinations);
  erase_bytes(g_verify_combination_sizes, sizeof g_verify_combination_sizes);
  erase_bytes(g_verify_combination_original_indices,
              sizeof g_verify_combination_original_indices);
  g_selected_method_count = 0;
  g_verify_combination_count = 0;
  g_captcha_sent = 0;
  g_passport_verify_retry = 0;
  g_passport_aigis_retry = 0;
  g_geetest_retry_pending = 0;
  g_verifier_retry_pending = 0;
  g_verifier_steps = 0;
}

static void erase_login_ciphertext(void) {
  erase_bytes(g_account_cipher, sizeof g_account_cipher);
  erase_bytes(g_password_cipher, sizeof g_password_cipher);
}

static void erase_verifier_secrets(void) {
  erase_bytes(g_verify_header, sizeof g_verify_header);
  erase_bytes(g_aigis_result, sizeof g_aigis_result);
  erase_bytes(g_aigis_gt, sizeof g_aigis_gt);
  erase_bytes(g_aigis_session, sizeof g_aigis_session);
  erase_bytes(g_aigis_risk_type, sizeof g_aigis_risk_type);
  erase_bytes(g_action_ticket, sizeof g_action_ticket);
  erase_bytes(g_risk_ticket, sizeof g_risk_ticket);
  erase_bytes(g_verifier_factor, sizeof g_verifier_factor);
  erase_bytes(g_selected_methods, sizeof g_selected_methods);
  erase_bytes(g_verify_combinations, sizeof g_verify_combinations);
  erase_bytes(g_verify_combination_sizes, sizeof g_verify_combination_sizes);
  erase_bytes(g_verify_combination_original_indices,
              sizeof g_verify_combination_original_indices);
  g_selected_method_count = 0;
  g_verify_combination_count = 0;
  g_captcha_sent = 0;
  g_passport_verify_retry = 0;
  g_passport_aigis_retry = 0;
  g_geetest_retry_pending = 0;
  g_verifier_retry_pending = 0;
  g_verifier_steps = 0;
}

static void builder_raw_n(StringBuilder *builder, const char *text,
                          size_t size) {
  if (!builder || builder->failed || !text ||
      size > builder->capacity - builder->length - 1) {
    if (builder) builder->failed = 1;
    return;
  }
  memcpy(builder->data + builder->length, text, size);
  builder->length += size;
  builder->data[builder->length] = 0;
}

static void builder_raw(StringBuilder *builder, const char *text) {
  builder_raw_n(builder, text, text ? strlen(text) : 0);
}

static void builder_format(StringBuilder *builder, const char *format, ...) {
  if (!builder || builder->failed || !format) return;
  va_list args;
  va_start(args, format);
  const size_t remaining = builder->capacity - builder->length;
  const int written = vsnprintf(builder->data + builder->length,
                                remaining, format, args);
  va_end(args);
  if (written < 0 || (size_t)written >= remaining) {
    builder->failed = 1;
    return;
  }
  builder->length += (size_t)written;
}

static void builder_json_string(StringBuilder *builder, const char *text) {
  if (!text) text = "";
  builder_raw(builder, "\"");
  for (const unsigned char *cursor = (const unsigned char *)text;
       *cursor && !builder->failed; ++cursor) {
    char escaped[7];
    switch (*cursor) {
      case '"': builder_raw(builder, "\\\""); break;
      case '\\': builder_raw(builder, "\\\\"); break;
      case '\b': builder_raw(builder, "\\b"); break;
      case '\f': builder_raw(builder, "\\f"); break;
      case '\n': builder_raw(builder, "\\n"); break;
      case '\r': builder_raw(builder, "\\r"); break;
      case '\t': builder_raw(builder, "\\t"); break;
      default:
        if (*cursor < 0x20) {
          snprintf(escaped, sizeof escaped, "\\u%04x", *cursor);
          builder_raw(builder, escaped);
        } else {
          builder_raw_n(builder, (const char *)cursor, 1);
        }
        break;
    }
  }
  builder_raw(builder, "\"");
}

static size_t curl_write_response(char *data, size_t size, size_t count,
                                  void *userdata) {
  HttpResponse *response = userdata;
  if (!response || (count && size > SIZE_MAX / count)) return 0;
  const size_t bytes = size * count;
  if (bytes > response->capacity - response->size - 1) {
    response->overflow = 1;
    return 0;
  }
  memcpy(response->data + response->size, data, bytes);
  response->size += bytes;
  response->data[response->size] = 0;
  return bytes;
}

static int ascii_equal_nocase(const char *left, const char *right,
                              size_t size) {
  for (size_t index = 0; index < size; ++index) {
    unsigned char a = (unsigned char)left[index];
    unsigned char b = (unsigned char)right[index];
    if (a >= 'A' && a <= 'Z') a = (unsigned char)(a - 'A' + 'a');
    if (b >= 'A' && b <= 'Z') b = (unsigned char)(b - 'A' + 'a');
    if (a != b) return 0;
  }
  return 1;
}

static void capture_verify_header_value(HttpHeaders *headers,
                                        const char *start, const char *end,
                                        uint32_t source) {
  if (!headers || !start || !end || end < start) return;
  headers->verify_seen_sources |= source;
  while (start < end && (*start == ' ' || *start == '\t')) ++start;
  while (end > start && (end[-1] == ' ' || end[-1] == '\t')) --end;
  const size_t value_size = (size_t)(end - start);
  if (!value_size || value_size >= sizeof headers->verify) {
    headers->verify_invalid = 1;
    return;
  }
  for (size_t index = 0; index < value_size; ++index) {
    const unsigned char value = (unsigned char)start[index];
    /* Horizontal tab is legal HTTP optional whitespace and legal JSON
     * whitespace. Other controls could make the capability ambiguous. */
    if ((value < 0x20 && value != '\t') || value == 0x7f) {
      headers->verify_invalid = 1;
      return;
    }
  }

  if (headers->verify_captured_sources & source) {
    /* The capability is defined as a single response header. */
    headers->verify_invalid = 1;
    return;
  }
  if (headers->verify_size) {
    /* HEADERFUNCTION and DEBUGFUNCTION independently report the same wire
     * header. Accept that corroboration only when the values are identical. */
    if (headers->verify_size != value_size ||
      memcmp(headers->verify, start, value_size) != 0) {
      headers->verify_invalid = 1;
      return;
    }
    headers->verify_captured_sources |= source;
    return;
  }
  memcpy(headers->verify, start, value_size);
  headers->verify[value_size] = 0;
  headers->verify_size = value_size;
  headers->verify_captured_sources |= source;
}

static void capture_aigis_header_value(HttpHeaders *headers,
                                       const char *start, const char *end,
                                       uint32_t source) {
  if (!headers || !start || !end || end < start) return;
  headers->aigis_seen_sources |= source;
  while (start < end && (*start == ' ' || *start == '\t')) ++start;
  while (end > start && (end[-1] == ' ' || end[-1] == '\t')) --end;
  const size_t value_size = (size_t)(end - start);
  if (!value_size || value_size >= sizeof headers->aigis) {
    headers->aigis_invalid = 1;
    return;
  }
  for (size_t index = 0; index < value_size; ++index) {
    const unsigned char value = (unsigned char)start[index];
    if ((value < 0x20 && value != '\t') || value == 0x7f) {
      headers->aigis_invalid = 1;
      return;
    }
  }
  if (headers->aigis_captured_sources & source) {
    headers->aigis_invalid = 1;
    return;
  }
  if (headers->aigis_size) {
    if (headers->aigis_size != value_size ||
        memcmp(headers->aigis, start, value_size) != 0) {
      headers->aigis_invalid = 1;
      return;
    }
    headers->aigis_captured_sources |= source;
    return;
  }
  memcpy(headers->aigis, start, value_size);
  headers->aigis[value_size] = 0;
  headers->aigis_size = value_size;
  headers->aigis_captured_sources |= source;
}

static void capture_header_lines(HttpHeaders *headers, const char *data,
                                 size_t bytes, uint32_t source) {
  static const char verify_name[] = "x-rpc-verify";
  static const char aigis_name[] = "x-rpc-aigis";
  if (!headers || !data || !bytes) return;
  if (source == HEADER_SOURCE_CALLBACK) ++headers->callback_lines;
  else if (source == HEADER_SOURCE_DEBUG) ++headers->debug_lines;

  const char *cursor = data;
  const char *limit = data + bytes;
  while (cursor < limit) {
    const char *line_end = memchr(cursor, '\n', (size_t)(limit - cursor));
    if (line_end) ++line_end;
    else line_end = limit;
    const char *start = cursor;
    const char *end = line_end;
    while (end > start && (end[-1] == '\r' || end[-1] == '\n')) --end;
    while (start < end && (*start == ' ' || *start == '\t')) ++start;
    const char *colon = memchr(start, ':', (size_t)(end - start));
    if (colon) {
      const char *name_end = colon;
      while (name_end > start &&
             (name_end[-1] == ' ' || name_end[-1] == '\t'))
        --name_end;
      const size_t name_size = (size_t)(name_end - start);
      if (name_size == sizeof verify_name - 1 &&
          ascii_equal_nocase(start, verify_name, name_size))
        capture_verify_header_value(headers, colon + 1, end, source);
      else if (name_size == sizeof aigis_name - 1 &&
               ascii_equal_nocase(start, aigis_name, name_size))
        capture_aigis_header_value(headers, colon + 1, end, source);
    }
    cursor = line_end;
  }
}

static size_t curl_capture_headers(char *data, size_t size, size_t count,
                                   void *userdata) {
  HttpHeaders *headers = userdata;
  if (!headers || (count && size > SIZE_MAX / count)) return 0;
  const size_t bytes = size * count;
  capture_header_lines(headers, data, bytes, HEADER_SOURCE_CALLBACK);
  return bytes;
}

static int curl_capture_debug(CURL *curl, curl_infotype type, char *data,
                              size_t size, void *userdata) {
  (void)curl;
  if (type == CURLINFO_HEADER_IN)
    capture_header_lines(userdata, data, size, HEADER_SOURCE_DEBUG);
  /* This replaces libcurl's default verbose sink. Request credentials and
   * response tickets are deliberately never emitted. */
  return 0;
}

static int append_header(struct curl_slist **headers, const char *format, ...) {
  char line[512];
  va_list args;
  va_start(args, format);
  const int written = vsnprintf(line, sizeof line, format, args);
  va_end(args);
  if (written < 0 || (size_t)written >= sizeof line) return -1;
  struct curl_slist *updated = curl_slist_append(*headers, line);
  if (!updated) return -1;
  *headers = updated;
  return 0;
}

static int append_secret_header(struct curl_slist **headers, const char *name,
                                const char *value) {
  if (!headers || !name || !*name || !value || !*value) return -1;
  const size_t name_size = strlen(name);
  const size_t value_size = strlen(value);
  if (name_size > SIZE_MAX - value_size - 3) return -1;
  const size_t line_size = name_size + value_size + 3;
  char *line = malloc(line_size);
  if (!line) return -1;
  const int written = snprintf(line, line_size, "%s: %s", name, value);
  struct curl_slist *updated = NULL;
  if (written >= 0 && (size_t)written < line_size)
    updated = curl_slist_append(*headers, line);
  erase_bytes(line, line_size);
  free(line);
  if (!updated) return -1;
  *headers = updated;
  return 0;
}

static void secure_slist_free_all(struct curl_slist *headers) {
  for (struct curl_slist *cursor = headers; cursor; cursor = cursor->next) {
    if (cursor->data) erase_bytes(cursor->data, strlen(cursor->data));
  }
  curl_slist_free_all(headers);
}

static struct curl_slist *passport_headers(const char *verify_header,
                                           const char *aigis_header) {
  const char *device_id = android_identity_android_id();
  struct curl_slist *headers = NULL;
  if (append_header(&headers, "Accept: application/json") ||
      append_header(&headers, "Content-Type: application/json; charset=UTF-8") ||
      append_header(&headers, "x-rpc-game_biz: hk4e_global") ||
      append_header(&headers, "x-rpc-app_id: %s", PASSPORT_APP_ID) ||
      append_header(&headers, "x-rpc-app_version: %s", SS_VERSION_NAME) ||
      append_header(&headers, "x-rpc-device_id: %s", device_id) ||
      append_header(&headers, "x-rpc-device_fp;") ||
      append_header(&headers, "x-rpc-device_model: Nintendo+Switch") ||
      append_header(&headers, "x-rpc-device_os: Android+12") ||
      append_header(&headers, "x-rpc-device_name: Nintendo+Switch") ||
      append_header(&headers, "x-rpc-client_type: 2") ||
      append_header(&headers, "x-rpc-sdk_version: 2.3.0") ||
      append_header(&headers, "x-rpc-language: en-us") ||
      append_header(&headers, "x-rpc-package_name: " SS_PACKAGE) ||
      append_header(&headers, "x-rpc-lifecycle_id: %s", g_lifecycle_id) ||
      append_header(&headers, "x-rpc-age_gate: true") ||
      append_header(&headers, "x-rpc-age_gate_eu: true") ||
      append_header(&headers, "x-rpc-age_gate_br: true") ||
      append_header(&headers, "x-rpc-aigis_v4: true") ||
      (verify_header && *verify_header &&
       append_secret_header(&headers, "x-rpc-verify", verify_header)) ||
      (aigis_header && *aigis_header &&
       append_secret_header(&headers, "x-rpc-aigis", aigis_header))) {
    secure_slist_free_all(headers);
    return NULL;
  }
  return headers;
}

static struct curl_slist *verifier_headers(void) {
  struct curl_slist *headers = passport_headers(NULL, NULL);
  if (!headers ||
      append_header(&headers, "Origin: https://account.hoyoverse.com") ||
      append_header(&headers, "Referer: https://account.hoyoverse.com/") ||
      append_header(&headers, "x-rpc-domain_redirect: true") ||
      append_header(&headers,
                    "x-rpc-referrer: https://account.hoyoverse.com/login-platform/mobile.html#/security-verification")) {
    secure_slist_free_all(headers);
    return NULL;
  }
  return headers;
}

static struct curl_slist *combo_headers(void) {
  const char *device_id = android_identity_android_id();
  struct curl_slist *headers = NULL;
  if (append_header(&headers, "Accept: application/json") ||
      append_header(&headers, "Content-Type: application/json; charset=UTF-8") ||
      append_header(&headers, "x-rpc-sys_version: Android+12") ||
      append_header(&headers, "x-rpc-device_id: %s", device_id) ||
      append_header(&headers, "x-rpc-device_model: Nintendo+Switch") ||
      append_header(&headers, "x-rpc-client_type: 2") ||
      append_header(&headers, "x-rpc-language: en-us") ||
      append_header(&headers, "x-rpc-channel_version: 2.52.0") ||
      append_header(&headers, "x-rpc-mdk_version: 2.52.0") ||
      append_header(&headers, "x-rpc-game_biz: hk4e_global") ||
      append_header(&headers, "x-rpc-channel_id: 1") ||
      append_header(&headers, "x-rpc-device_fp;") ||
      append_header(&headers, "x-rpc-lifecycle_id: %s", g_lifecycle_id) ||
      append_header(&headers, "x-rpc-app_id: %s", PASSPORT_APP_ID) ||
      append_header(&headers, "x-rpc-combo_version: 2.52.0") ||
      append_header(&headers, "x-rpc-account_version: 2.3.0") ||
      append_header(&headers, "x-rpc-age_gate: true") ||
      append_header(&headers, "x-rpc-age_gate_eu: true") ||
      append_header(&headers, "x-rpc-age_gate_br: true")) {
    secure_slist_free_all(headers);
    return NULL;
  }
  return headers;
}

static CURLcode http_post_json(const char *url, struct curl_slist *headers,
                               const char *body, size_t body_size,
                               HttpResponse *response,
                               HttpHeaders *response_headers,
                               long *http_status) {
  if (!url || !headers || !body || !response || !http_status)
    return CURLE_BAD_FUNCTION_ARGUMENT;
  CURL *curl = curl_easy_init();
  if (!curl) return CURLE_FAILED_INIT;
  curl_easy_setopt(curl, CURLOPT_URL, url);
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
  curl_easy_setopt(curl, CURLOPT_POST, 1L);
  curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);
  curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE_LARGE, (curl_off_t)body_size);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_response);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, response);
  if (response_headers) {
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, curl_capture_headers);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, response_headers);
    /* Switch portlibs currently ships libcurl 7.69, before
     * curl_easy_header(). The debug callback is an independent incoming
     * header path if this backend skips HEADERFUNCTION. */
    curl_easy_setopt(curl, CURLOPT_DEBUGFUNCTION, curl_capture_debug);
    curl_easy_setopt(curl, CURLOPT_DEBUGDATA, response_headers);
    curl_easy_setopt(curl, CURLOPT_VERBOSE, 1L);
  }
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
  curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
  curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
  curl_easy_setopt(curl, CURLOPT_CAINFO, CA_BUNDLE_PATH);
  curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
  curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
  curl_easy_setopt(curl, CURLOPT_USERAGENT, "GenshinImpact/6.7.0 Android");
  const CURLcode result = curl_easy_perform(curl);
  if (result == CURLE_OK)
    (void)curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, http_status);
  curl_easy_cleanup(curl);
  return result;
}

static int base64_encode(const uint8_t *input, size_t size,
                         char *output, size_t capacity) {
  static const char alphabet[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  if (!input || !output || size > (SIZE_MAX - 2) / 3) return -1;
  const size_t needed = ((size + 2) / 3) * 4;
  if (capacity <= needed) return -1;
  size_t source = 0;
  size_t target = 0;
  while (source + 3 <= size) {
    const uint32_t value = ((uint32_t)input[source] << 16) |
                           ((uint32_t)input[source + 1] << 8) |
                           input[source + 2];
    output[target++] = alphabet[(value >> 18) & 63];
    output[target++] = alphabet[(value >> 12) & 63];
    output[target++] = alphabet[(value >> 6) & 63];
    output[target++] = alphabet[value & 63];
    source += 3;
  }
  if (source < size) {
    uint32_t value = (uint32_t)input[source] << 16;
    const int has_second = source + 1 < size;
    if (has_second) value |= (uint32_t)input[source + 1] << 8;
    output[target++] = alphabet[(value >> 18) & 63];
    output[target++] = alphabet[(value >> 12) & 63];
    output[target++] = has_second ? alphabet[(value >> 6) & 63] : '=';
    output[target++] = '=';
  }
  output[target] = 0;
  return target == needed ? 0 : -1;
}

static int rsa_encrypt_text(const char *plaintext, char *output,
                            size_t output_capacity) {
  if (!plaintext || !output) return -1;
  const size_t plaintext_size = strlen(plaintext);
  if (!plaintext_size || plaintext_size > RSA_PLAINTEXT_MAX) return -1;
  uint8_t encoded[RSA_BYTES];
  uint8_t ciphertext[RSA_BYTES];
  memset(encoded, 0, sizeof encoded);
  memset(ciphertext, 0, sizeof ciphertext);
  encoded[0] = 0;
  encoded[1] = 2;
  const size_t padding_size = RSA_BYTES - plaintext_size - 3;
  for (size_t index = 0; index < padding_size; ++index) {
    do randomGet(&encoded[2 + index], 1); while (!encoded[2 + index]);
  }
  encoded[2 + padding_size] = 0;
  memcpy(encoded + 3 + padding_size, plaintext, plaintext_size);
  const Result result = splUserExpMod(encoded, g_passport_rsa_modulus,
                                      g_rsa_exponent, sizeof g_rsa_exponent,
                                      ciphertext);
  g_service_result = (int)result;
  int encoded_ok = -1;
  if (R_SUCCEEDED(result))
    encoded_ok = base64_encode(ciphertext, sizeof ciphertext,
                               output, output_capacity);
  erase_bytes(encoded, sizeof encoded);
  erase_bytes(ciphertext, sizeof ciphertext);
  return encoded_ok;
}

/* Return 0 after immediate encryption/queuing, 1 for user-correctable input,
 * or -1 when the secure encryption operation itself fails. */
#if 0 /* Retired custom UI plaintext adapters; swkbd encrypts in-place below. */
static int queue_plain_credentials(const char *account, const char *password) {
  if (!account || !password) return 1;
  const size_t account_size = strnlen(account, INPUT_CAPACITY);
  const size_t password_size = strnlen(password, INPUT_CAPACITY);
  if (!account_size || account_size > 128 ||
      !password_size || password_size > 128)
    return 1;
  erase_login_ciphertext();
  if (rsa_encrypt_text(account, g_account_cipher, sizeof g_account_cipher) ||
      rsa_encrypt_text(password, g_password_cipher,
                       sizeof g_password_cipher)) {
    erase_login_ciphertext();
    return -1;
  }
  g_retry_pending = 0;
  set_connection_ui_message("Signing in securely.");
  __atomic_store_n(&g_phase, COMBO_AUTH_PHASE_PASSPORT_QUEUED,
                   __ATOMIC_RELEASE);
  return 0;
}

static int queue_plain_verifier_factor(const char *value) {
  if (!value || !supported_verify_method(g_verify_method)) return 1;
  const size_t length = strnlen(value, INPUT_CAPACITY);
  if (!length || length > 128) return 1;
  erase_bytes(g_verifier_factor, sizeof g_verifier_factor);
  if (g_verify_method == VERIFY_METHOD_PASSWORD) {
    if (rsa_encrypt_text(value, g_verifier_factor,
                         sizeof g_verifier_factor))
      return -1;
  } else {
    int valid = length == 6;
    for (size_t index = 0; index < length; ++index)
      if (value[index] < '0' || value[index] > '9') valid = 0;
    if (!valid) return 1;
    memcpy(g_verifier_factor, value, length + 1);
  }
  g_verifier_retry_pending = 0;
  set_connection_ui_message("Checking your verification response.");
  __atomic_store_n(&g_phase, COMBO_AUTH_PHASE_VERIFIER_VERIFY_QUEUED,
                   __ATOMIC_RELEASE);
  return 0;
}
#endif

static int is_keyboard_cancel(Result result) {
  return R_VALUE(result) ==
    R_VALUE(MAKERESULT(Module_Libnx, LibnxError_LibAppletBadExit));
}

static Result show_keyboard(int password, char *output, size_t capacity) {
  SwkbdConfig keyboard;
  const Result create_result = swkbdCreate(&keyboard, 0);
  if (R_FAILED(create_result)) return create_result;
  if (password) swkbdConfigMakePresetPassword(&keyboard);
  else swkbdConfigMakePresetDefault(&keyboard);
  swkbdConfigSetHeaderText(&keyboard,
    password ? "HoYoverse Account Password" :
    g_retry_pending ? "HoYoverse Login Rejected - Try Again" :
                      "HoYoverse Account");
  swkbdConfigSetSubText(&keyboard,
    "Credentials are encrypted locally and sent only over verified HTTPS.");
  swkbdConfigSetGuideText(&keyboard,
    password ? "Password" : "Email or username");
  swkbdConfigSetStringLenMin(&keyboard, 1);
  swkbdConfigSetStringLenMax(&keyboard, 128);
  const Result show_result = swkbdShow(&keyboard, output, capacity);
  swkbdClose(&keyboard);
  return show_result;
}

static Result show_verifier_keyboard(int method, char *output,
                                     size_t capacity) {
  SwkbdConfig keyboard;
  const Result create_result = swkbdCreate(&keyboard, 0);
  if (R_FAILED(create_result)) return create_result;
  const int password = method == VERIFY_METHOD_PASSWORD;
  if (password) {
    swkbdConfigMakePresetPassword(&keyboard);
  } else {
    swkbdConfigMakePresetDefault(&keyboard);
    swkbdConfigSetType(&keyboard, SwkbdType_NumPad);
  }
  swkbdConfigSetHeaderText(&keyboard,
    g_verifier_retry_pending ? "Verification Rejected - Try Again" :
                               "HoYoverse Security Verification");
  swkbdConfigSetSubText(&keyboard,
    password ? "Enter the account password requested by HoYoverse." :
               method == VERIFY_METHOD_EMAIL ?
                 "Enter the 6-digit code sent to the account email." :
                 "Enter the 6-digit code sent to the account mobile number.");
  swkbdConfigSetGuideText(&keyboard,
    password ? "Password" :
    method == VERIFY_METHOD_EMAIL ? "Email verification code" :
                                    "Mobile verification code");
  swkbdConfigSetStringLenMin(&keyboard, password ? 1 : 6);
  swkbdConfigSetStringLenMax(&keyboard, password ? 128 : 6);
  const Result show_result = swkbdShow(&keyboard, output, capacity);
  swkbdClose(&keyboard);
  return show_result;
}

static Result show_geetest_keyboard(const char *url, char *output,
                                    size_t capacity) {
  SwkbdConfig keyboard;
  const Result create_result = swkbdCreate(&keyboard, 0);
  if (R_FAILED(create_result)) return create_result;
  swkbdConfigMakePresetDefault(&keyboard);
  swkbdConfigSetHeaderText(&keyboard, url ? url : "HoYoverse Captcha");
  swkbdConfigSetSubText(&keyboard,
    "Open this URL on a same-Wi-Fi phone. If it keeps loading, press Continue, then finish there within 3 minutes.");
  swkbdConfigSetGuideText(&keyboard, "Captcha page says Complete");
  swkbdConfigSetOkButtonText(&keyboard, "Continue");
  swkbdConfigSetInitialText(&keyboard, "DONE");
  swkbdConfigSetStringLenMin(&keyboard, 4);
  swkbdConfigSetStringLenMax(&keyboard, 4);
  const Result show_result = swkbdShow(&keyboard, output, capacity);
  swkbdClose(&keyboard);
  return show_result;
}

static int enqueue_inner_callback(const char *inner, size_t inner_size) {
  if (!inner || inner_size >= AUTH_CALLBACK_CAPACITY) return -1;
  char *outer = malloc(AUTH_CALLBACK_CAPACITY);
  if (!outer) return -1;
  StringBuilder builder = { outer, AUTH_CALLBACK_CAPACITY, 0, 0 };
  outer[0] = 0;
  builder_raw(&builder, "{\"index\":");
  builder_format(&builder, "%d", g_callback_index);
  builder_raw(&builder, ",\"data\":");
  builder_json_string(&builder, inner);
  builder_raw(&builder, "}");
  int result = -1;
  if (!builder.failed)
    result = combo_bridge_enqueue_callback(COMBO_ROUTE_INVOKE_RESPONSE,
                                           g_callback_index, outer,
                                           builder.length, 1);
  erase_bytes(outer, AUTH_CALLBACK_CAPACITY);
  free(outer);
  return result;
}

static void finish_error(const char *message, ComboAuthPhase terminal_phase) {
  set_connection_ui_message(message && *message ? message : "Login failed");
  char inner[4096];
  StringBuilder builder = { inner, sizeof inner, 0, 0 };
  inner[0] = 0;
  builder_raw(&builder, "{\"ret\":-101,\"msg\":");
  builder_json_string(&builder, message && *message ? message : "Login failed");
  builder_raw(&builder, "}");
  const int queued = !builder.failed
    ? enqueue_inner_callback(inner, builder.length) : -1;
  erase_bytes(inner, sizeof inner);
  erase_attempt_secrets();
  erase_bytes(g_asterisk_name, sizeof g_asterisk_name);
  g_retry_pending = 0;
  __atomic_store_n(&g_phase,
                   queued == 0 ? terminal_phase : COMBO_AUTH_PHASE_FAILED,
                   __ATOMIC_RELEASE);
}

static void finish_cancel(void) {
  static const char inner[] =
    "{\"ret\":-102,\"msg\":\"mdk-os login cancel\"}";
  set_connection_ui_message("Login canceled.");
  const int queued = enqueue_inner_callback(inner, sizeof inner - 1);
  erase_attempt_secrets();
  erase_bytes(g_asterisk_name, sizeof g_asterisk_name);
  g_retry_pending = 0;
  __atomic_store_n(&g_phase,
                   queued == 0 ? COMBO_AUTH_PHASE_CANCELED
                               : COMBO_AUTH_PHASE_FAILED,
                   __ATOMIC_RELEASE);
}

static int json_object_member(const JsonMinValue *object, const char *name,
                              JsonMinValue *value) {
  return json_min_object_get(object, name, value) == 0;
}

static int json_int_member(const JsonMinValue *object, const char *name,
                           int64_t *value) {
  JsonMinValue member;
  return json_object_member(object, name, &member) &&
         json_min_int64(&member, value) == 0;
}

static int json_string_member(const JsonMinValue *object, const char *name,
                              char *output, size_t capacity) {
  JsonMinValue member;
  return json_object_member(object, name, &member) &&
         json_min_string(&member, output, capacity) == 0;
}

static int json_bool_member(const JsonMinValue *object, const char *name,
                            int *value) {
  JsonMinValue member;
  return json_object_member(object, name, &member) &&
         json_min_bool(&member, value) == 0;
}

static void response_message(const JsonMinValue *root, char *output,
                             size_t capacity) {
  output[0] = 0;
  if (!json_string_member(root, "message", output, capacity) &&
      !json_string_member(root, "msg", output, capacity) &&
      !json_string_member(root, "describe", output, capacity))
    snprintf(output, capacity, "%s", "Login failed");
}

static int challenge_class(int64_t retcode) {
  if (retcode == -3101) return 1; /* Geetest/Aigis captcha. */
  if (retcode == -3104 || retcode == -3239 || retcode == -3240) return 2;
  if (retcode == -3105 || retcode == -3233 || retcode == -3228) return 3;
  if (retcode == -3264) return 4;
  if (retcode == -4212 || retcode == -4211 || retcode == -4213 ||
      retcode == -4209 || retcode == -4200 || retcode == -4225) return 5;
  return 0;
}

static int json_integer_member_relaxed(const JsonMinValue *object,
                                       const char *name, int64_t *value) {
  JsonMinValue member;
  if (!object || !name || !value ||
      json_min_object_get(object, name, &member))
    return 0;
  if (json_min_int64(&member, value) == 0) return 1;
  if (member.type != JSON_MIN_STRING) return 0;
  char number[32];
  if (json_min_string(&member, number, sizeof number)) return 0;
  char *end = NULL;
  const long long parsed = strtoll(number, &end, 10);
  if (!end || end == number || *end) {
    erase_bytes(number, sizeof number);
    return 0;
  }
  *value = (int64_t)parsed;
  erase_bytes(number, sizeof number);
  return 1;
}

static int parse_verify_header(const char *header, size_t size) {
  if (!header || !size || size >= sizeof g_verify_header) {
    g_verify_header_state = COMBO_AUTH_VERIFY_HEADER_CAPTURE_INVALID;
    return -1;
  }
  JsonMinValue root;
  if (json_min_parse(header, size, &root) || root.type != JSON_MIN_OBJECT) {
    g_verify_header_state = COMBO_AUTH_VERIFY_HEADER_OUTER_JSON_INVALID;
    return -1;
  }

  JsonMinValue payload = root;
  JsonMinValue verify_str;
  char decoded[VERIFY_HEADER_CAPACITY];
  memset(decoded, 0, sizeof decoded);
  const int has_verify_str =
    json_min_object_get(&root, "verify_str", &verify_str) == 0;
  if (has_verify_str) {
    if (verify_str.type == JSON_MIN_OBJECT) {
      payload = verify_str;
    } else if (verify_str.type != JSON_MIN_STRING ||
               json_min_string(&verify_str, decoded, sizeof decoded) ||
               json_min_parse(decoded, strlen(decoded), &payload) ||
               payload.type != JSON_MIN_OBJECT) {
      erase_bytes(decoded, sizeof decoded);
      g_verify_header_state = COMBO_AUTH_VERIFY_HEADER_NESTED_JSON_INVALID;
      return -1;
    }
  }

  char action_ticket[ACTION_TICKET_CAPACITY];
  char risk_ticket[ACTION_TICKET_CAPACITY];
  memset(action_ticket, 0, sizeof action_ticket);
  memset(risk_ticket, 0, sizeof risk_ticket);
  int64_t verify_type = 0;
  const int valid =
    json_string_member(&payload, "ticket", action_ticket,
                       sizeof action_ticket) &&
    json_integer_member_relaxed(&payload, "verify_type", &verify_type) &&
    action_ticket[0] &&
    (verify_type == VERIFY_TYPE_GEETEST ||
     verify_type == VERIFY_TYPE_IDENTITY);
  (void)json_string_member(&root, "risk_ticket", risk_ticket,
                           sizeof risk_ticket);
  if (!valid) {
    erase_bytes(decoded, sizeof decoded);
    erase_bytes(action_ticket, sizeof action_ticket);
    erase_bytes(risk_ticket, sizeof risk_ticket);
    g_verify_header_state = COMBO_AUTH_VERIFY_HEADER_FIELDS_INVALID;
    return -1;
  }

  erase_verifier_secrets();
  memcpy(g_verify_header, header, size);
  g_verify_header[size] = 0;
  snprintf(g_action_ticket, sizeof g_action_ticket, "%s", action_ticket);
  snprintf(g_risk_ticket, sizeof g_risk_ticket, "%s", risk_ticket);
  g_verify_type = (int)verify_type;
  g_verify_header_state = COMBO_AUTH_VERIFY_HEADER_READY;
  erase_bytes(decoded, sizeof decoded);
  erase_bytes(action_ticket, sizeof action_ticket);
  erase_bytes(risk_ticket, sizeof risk_ticket);
  return 0;
}

static int parse_aigis_header(const char *header, size_t size) {
  if (!header || !size || size >= AIGIS_HEADER_CAPACITY) {
    g_aigis_header_state = COMBO_AUTH_AIGIS_HEADER_CAPTURE_INVALID;
    return -1;
  }
  JsonMinValue root;
  JsonMinValue data;
  JsonMinValue data_member;
  char decoded_data[AIGIS_HEADER_CAPACITY];
  memset(decoded_data, 0, sizeof decoded_data);
  if (json_min_parse(header, size, &root) || root.type != JSON_MIN_OBJECT) {
    g_aigis_header_state = COMBO_AUTH_AIGIS_HEADER_JSON_INVALID;
    return -1;
  }
  int64_t mmt_type = 0;
  int use_v4 = 0;
  char gt[AIGIS_FIELD_CAPACITY];
  char session[AIGIS_FIELD_CAPACITY];
  char risk_type[AIGIS_FIELD_CAPACITY];
  memset(gt, 0, sizeof gt);
  memset(session, 0, sizeof session);
  memset(risk_type, 0, sizeof risk_type);
  /* Porte OS models AigisData with a custom Gson deserializer: the outer
   * `data` member is normally a JSON-encoded string, and getAsString() is fed
   * to a second Gson.fromJson() call.  Some service variants return the same
   * object inline.  Accept precisely those two SDK-defined representations;
   * do not loosen any of the v4 capability fields below. */
  const int has_data = json_object_member(&root, "data", &data_member);
  int decoded_data_valid = 0;
  if (has_data && data_member.type == JSON_MIN_OBJECT) {
    data = data_member;
    decoded_data_valid = 1;
  } else if (has_data && data_member.type == JSON_MIN_STRING &&
             json_min_string(&data_member, decoded_data,
                             sizeof decoded_data) == 0 &&
             json_min_parse(decoded_data, strlen(decoded_data), &data) == 0 &&
             data.type == JSON_MIN_OBJECT) {
    decoded_data_valid = 1;
  }
  const int valid =
    json_integer_member_relaxed(&root, "mmt_type", &mmt_type) &&
    mmt_type == 1 &&
    json_string_member(&root, "session_id", session, sizeof session) &&
    decoded_data_valid &&
    json_string_member(&data, "gt", gt, sizeof gt) &&
    json_bool_member(&data, "use_v4", &use_v4) &&
    session[0] && gt[0];
  if (!valid) {
    erase_bytes(decoded_data, sizeof decoded_data);
    erase_bytes(gt, sizeof gt);
    erase_bytes(session, sizeof session);
    erase_bytes(risk_type, sizeof risk_type);
    g_aigis_header_state = COMBO_AUTH_AIGIS_HEADER_FIELDS_INVALID;
    return -1;
  }
  if (!use_v4) {
    erase_bytes(decoded_data, sizeof decoded_data);
    erase_bytes(gt, sizeof gt);
    erase_bytes(session, sizeof session);
    erase_bytes(risk_type, sizeof risk_type);
    g_aigis_header_state = COMBO_AUTH_AIGIS_HEADER_V3_UNSUPPORTED;
    return 1;
  }
  if (!json_string_member(&data, "risk_type", risk_type,
                          sizeof risk_type) || !risk_type[0]) {
    erase_bytes(decoded_data, sizeof decoded_data);
    erase_bytes(gt, sizeof gt);
    erase_bytes(session, sizeof session);
    erase_bytes(risk_type, sizeof risk_type);
    g_aigis_header_state = COMBO_AUTH_AIGIS_HEADER_FIELDS_INVALID;
    return -1;
  }

  erase_verifier_secrets();
  snprintf(g_aigis_gt, sizeof g_aigis_gt, "%s", gt);
  snprintf(g_aigis_session, sizeof g_aigis_session, "%s", session);
  snprintf(g_aigis_risk_type, sizeof g_aigis_risk_type, "%s", risk_type);
  g_verify_type = VERIFY_TYPE_GEETEST;
  g_aigis_header_state = COMBO_AUTH_AIGIS_HEADER_V4_READY;
  erase_bytes(decoded_data, sizeof decoded_data);
  erase_bytes(gt, sizeof gt);
  erase_bytes(session, sizeof session);
  erase_bytes(risk_type, sizeof risk_type);
  return 0;
}

static int validate_geetest_result(const char *json, size_t size) {
  if (!json || !size || size >= AIGIS_RESULT_CAPACITY) return -1;
  JsonMinValue root;
  if (json_min_parse(json, size, &root) || root.type != JSON_MIN_OBJECT)
    return -1;
  char lot_number[AIGIS_FIELD_CAPACITY];
  char captcha_output[AIGIS_FIELD_CAPACITY];
  char pass_token[AIGIS_FIELD_CAPACITY];
  char gen_time[AIGIS_FIELD_CAPACITY];
  memset(lot_number, 0, sizeof lot_number);
  memset(captcha_output, 0, sizeof captcha_output);
  memset(pass_token, 0, sizeof pass_token);
  memset(gen_time, 0, sizeof gen_time);
  const int valid =
    json_string_member(&root, "lot_number", lot_number,
                       sizeof lot_number) &&
    json_string_member(&root, "captcha_output", captcha_output,
                       sizeof captcha_output) &&
    json_string_member(&root, "pass_token", pass_token,
                       sizeof pass_token) &&
    json_string_member(&root, "gen_time", gen_time,
                       sizeof gen_time) &&
    lot_number[0] && captcha_output[0] && pass_token[0] && gen_time[0];
  /* Porte OS does not send the Geetest JSON directly.  The exact staged
   * GeeTestUtils.startGeeTest$gtCallback$1 first Base64-encodes the UTF-8
   * result and, for startGeeTestStandard, prefixes the Aigis session as
   * `<session_id>;<base64-result>`.  Sending the raw JSON is accepted by the
   * local bridge but rejected by Passport as -3102. */
  char canonical[AIGIS_RESULT_CAPACITY];
  memset(canonical, 0, sizeof canonical);
  StringBuilder builder = {
    canonical, sizeof canonical, 0, valid ? 0 : 1
  };
  builder_raw(&builder, "{\"lot_number\":");
  builder_json_string(&builder, lot_number);
  builder_raw(&builder, ",\"captcha_output\":");
  builder_json_string(&builder, captcha_output);
  builder_raw(&builder, ",\"pass_token\":");
  builder_json_string(&builder, pass_token);
  builder_raw(&builder, ",\"gen_time\":");
  builder_json_string(&builder, gen_time);
  builder_raw(&builder, "}");
  erase_bytes(lot_number, sizeof lot_number);
  erase_bytes(captcha_output, sizeof captcha_output);
  erase_bytes(pass_token, sizeof pass_token);
  erase_bytes(gen_time, sizeof gen_time);
  const size_t session_size = strnlen(g_aigis_session,
                                      sizeof g_aigis_session);
  if (builder.failed || !session_size ||
      session_size >= sizeof g_aigis_session ||
      session_size + 2 >= sizeof g_aigis_result) {
    erase_bytes(canonical, sizeof canonical);
    erase_bytes(g_aigis_result, sizeof g_aigis_result);
    return -1;
  }
  erase_bytes(g_aigis_result, sizeof g_aigis_result);
  memcpy(g_aigis_result, g_aigis_session, session_size);
  g_aigis_result[session_size] = ';';
  if (base64_encode((const uint8_t *)canonical, builder.length,
                    g_aigis_result + session_size + 1,
                    sizeof g_aigis_result - session_size - 1)) {
    erase_bytes(canonical, sizeof canonical);
    erase_bytes(g_aigis_result, sizeof g_aigis_result);
    return -1;
  }
  erase_bytes(canonical, sizeof canonical);
  return 0;
}

static int socket_send_all(int fd, const char *data, size_t size) {
  size_t sent = 0;
  while (sent < size) {
    const ssize_t result = send(fd, data + sent, size - sent, 0);
    if (result > 0) {
      sent += (size_t)result;
      continue;
    }
    if (result < 0 && errno == EINTR) continue;
    return -1;
  }
  return 0;
}

static int geetest_open_listener(uint16_t port) {
  const int listen_fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  int reuse = 1;
  struct sockaddr_in local;
  memset(&local, 0, sizeof local);
  local.sin_family = AF_INET;
  local.sin_addr.s_addr = htonl(INADDR_ANY);
  local.sin_port = htons(port);
  if (listen_fd < 0) return -1;
  if (setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR,
                 &reuse, sizeof reuse) < 0 ||
      bind(listen_fd, (const struct sockaddr *)&local, sizeof local) < 0 ||
      listen(listen_fd, 4) < 0) {
    const int saved = errno;
    close(listen_fd);
    errno = saved;
    return -1;
  }
  return listen_fd;
}

static void geetest_reply(int fd, int status, const char *content_type,
                          const char *body, size_t body_size) {
  char header[512];
  const char *reason = status == 200 ? "OK" :
                       status == 404 ? "Not Found" : "Bad Request";
  const int header_size = snprintf(
    header, sizeof header,
    "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\n"
    "Cache-Control: no-store\r\nX-Content-Type-Options: nosniff\r\n"
    "Referrer-Policy: no-referrer\r\nX-Frame-Options: DENY\r\n"
    "Connection: close\r\n\r\n",
    status, reason, content_type, body_size);
  if (header_size > 0 && (size_t)header_size < sizeof header) {
    (void)socket_send_all(fd, header, (size_t)header_size);
    (void)socket_send_all(fd, body, body_size);
  }
  erase_bytes(header, sizeof header);
}

static const char *find_http_header_end(const char *request, size_t size) {
  if (!request || size < 4) return NULL;
  for (size_t index = 0; index + 4 <= size; ++index)
    if (memcmp(request + index, "\r\n\r\n", 4) == 0)
      return request + index + 4;
  return NULL;
}

static int parse_content_length(const char *request, size_t header_size,
                                size_t *length_out) {
  static const char name[] = "content-length:";
  if (!request || !length_out) return -1;
  const char *cursor = request;
  const char *end = request + header_size;
  while (cursor < end) {
    const char *line_end = cursor;
    while (line_end < end && *line_end != '\n') ++line_end;
    if ((size_t)(line_end - cursor) >= sizeof name - 1 &&
        ascii_equal_nocase(cursor, name, sizeof name - 1)) {
      const char *value = cursor + sizeof name - 1;
      while (value < line_end && (*value == ' ' || *value == '\t')) ++value;
      size_t length = 0;
      if (value == line_end) return -1;
      while (value < line_end && *value >= '0' && *value <= '9') {
        if (length > (GEETEST_REQUEST_CAPACITY - (size_t)(*value - '0')) / 10)
          return -1;
        length = length * 10 + (size_t)(*value++ - '0');
      }
      while (value < line_end && (*value == '\r' || *value == ' ' ||
                                  *value == '\t')) ++value;
      if (value != line_end) return -1;
      *length_out = length;
      return 0;
    }
    cursor = line_end < end ? line_end + 1 : end;
  }
  return -1;
}

static void geetest_handle_client(GeetestServer *server, int client) {
  char *request = malloc(GEETEST_REQUEST_CAPACITY);
  if (!request) return;
  size_t received = 0;
  size_t expected = 0;
  int have_expected = 0;
  while (received + 1 < GEETEST_REQUEST_CAPACITY) {
    const ssize_t count = recv(client, request + received,
                               GEETEST_REQUEST_CAPACITY - received - 1, 0);
    if (count > 0) {
      received += (size_t)count;
      request[received] = 0;
      const char *body = find_http_header_end(request, received);
      if (body && !have_expected) {
        const size_t header_size = (size_t)(body - request);
        if (memcmp(request, "POST ", 5) == 0) {
          size_t content_length = 0;
          if (parse_content_length(request, header_size, &content_length) ||
              content_length >= AIGIS_RESULT_CAPACITY ||
              header_size > GEETEST_REQUEST_CAPACITY - content_length) {
            break;
          }
          expected = header_size + content_length;
        } else {
          expected = header_size;
        }
        have_expected = 1;
      }
      if (have_expected && received >= expected) break;
      continue;
    }
    if (count < 0 && errno == EINTR) continue;
    break;
  }

  const char *header_end = find_http_header_end(request, received);
  const char *first_space = received ? memchr(request, ' ', received) : NULL;
  const char *second_space = first_space ?
    memchr(first_space + 1, ' ', received - (size_t)(first_space + 1 - request))
    : NULL;
  const size_t path_size = first_space && second_space ?
    (size_t)(second_space - first_space - 1) : 0;
  if (header_end && memcmp(request, "GET ", 4) == 0 &&
      strlen(server->path) == path_size &&
      memcmp(first_space + 1, server->path, path_size) == 0) {
    geetest_reply(client, 200, "text/html; charset=utf-8",
                  server->html, server->html_size);
    __atomic_add_fetch(&g_geetest_pages_served, 1, __ATOMIC_RELAXED);
  } else if (header_end && memcmp(request, "POST ", 5) == 0 &&
             strlen(server->complete_path) == path_size &&
             memcmp(first_space + 1, server->complete_path, path_size) == 0) {
    size_t content_length = 0;
    const size_t header_size = (size_t)(header_end - request);
    if (!parse_content_length(request, header_size, &content_length) &&
        content_length == received - header_size &&
        validate_geetest_result(header_end, content_length) == 0) {
      static const char ok[] = "{\"ok\":true}";
      geetest_reply(client, 200, "application/json", ok, sizeof ok - 1);
      __atomic_add_fetch(&g_geetest_results_received, 1, __ATOMIC_RELAXED);
      __atomic_store_n(&server->result_ready, 1, __ATOMIC_RELEASE);
    } else {
      static const char bad[] = "{\"ok\":false}";
      geetest_reply(client, 400, "application/json", bad, sizeof bad - 1);
      __atomic_add_fetch(&g_geetest_requests_rejected, 1, __ATOMIC_RELAXED);
    }
  } else {
    static const char missing[] = "Not found";
    geetest_reply(client, 404, "text/plain", missing, sizeof missing - 1);
  }
  erase_bytes(request, GEETEST_REQUEST_CAPACITY);
  free(request);
}

static void geetest_server_thread(void *argument) {
  GeetestServer *server = argument;
  while (!__atomic_load_n(&server->stop, __ATOMIC_ACQUIRE) &&
         !__atomic_load_n(&server->result_ready, __ATOMIC_ACQUIRE)) {
    fd_set read_set;
    FD_ZERO(&read_set);
    FD_SET(server->listen_fd, &read_set);
    struct timeval timeout = { .tv_sec = 0, .tv_usec = 250000 };
    const int ready = select(server->listen_fd + 1, &read_set, NULL, NULL,
                             &timeout);
    if (ready < 0 && errno == EINTR) continue;
    if (ready <= 0) continue;
    struct sockaddr_in peer;
    socklen_t peer_size = sizeof peer;
    const int client = accept(server->listen_fd,
                              (struct sockaddr *)&peer, &peer_size);
    if (client < 0) continue;
    struct timeval io_timeout = { .tv_sec = 5, .tv_usec = 0 };
    (void)setsockopt(client, SOL_SOCKET, SO_RCVTIMEO,
                     &io_timeout, sizeof io_timeout);
    (void)setsockopt(client, SOL_SOCKET, SO_SNDTIMEO,
                     &io_timeout, sizeof io_timeout);
    geetest_handle_client(server, client);
    close(client);
  }
  __atomic_store_n(&server->finished, 1, __ATOMIC_RELEASE);
}

static int build_geetest_html(char *html, size_t capacity, size_t *size_out,
                              const char *complete_path) {
  if (!html || !size_out || !complete_path) return -1;
  StringBuilder builder = { html, capacity, 0, 0 };
  html[0] = 0;
  builder_raw(&builder,
    "<!doctype html><html><head><meta charset=utf-8><meta name=referrer content=no-referrer><meta name=viewport "
    "content=\"width=device-width,initial-scale=1\"><title>HoYoverse "
    "verification</title><style>body{font:16px system-ui;background:#f5f5f5;"
    "color:#222;margin:0;padding:28px}.card{max-width:560px;margin:auto;"
    "background:white;border-radius:16px;padding:24px;box-shadow:0 4px 24px "
    "#0002}button{font-size:18px;padding:12px 18px}#status{margin-top:16px}"
    "</style><script src=\"https://static.geetest.com/v4/gt4.js\"></script>"
    "</head><body><div class=card><h2>HoYoverse security check</h2>"
    "<p>This page contains only the short-lived captcha challenge. It never "
    "receives your account or password.</p><button id=start disabled>Loading "
    "official captcha...</button><p id=status></p></div><script>const cfg={"
    "captchaId:");
  builder_json_string(&builder, g_aigis_gt);
  builder_raw(&builder, ",product:\"bind\",language:\"eng\",protocol:\"https://\",riskType:");
  builder_json_string(&builder, g_aigis_risk_type);
  builder_raw(&builder, ",userInfo:JSON.stringify({session_id:");
  builder_json_string(&builder, g_aigis_session);
  builder_raw(&builder,
    "}),loading:\"\",hideSuccess:true,apiServers:[\"gcaptcha4.captchami.com\","
    "\"gcaptcha4.geetest.com\"],staticServers:[\"static.captchami.com\","
    "\"static.geetest.com\"]};const endpoint=");
  builder_json_string(&builder, complete_path);
  builder_raw(&builder,
    ";const b=document.getElementById('start'),s=document.getElementById('status');"
    "function fail(m){s.textContent=m;b.disabled=false;}"
    "if(typeof initGeetest4!=='function')fail('Captcha library did not load. Check the phone connection and reload.');"
    "else initGeetest4(cfg,function(c){b.disabled=false;b.textContent='Start captcha';"
    "b.onclick=()=>c.showCaptcha();c.onReady(()=>c.showCaptcha());"
    "c.onError(()=>fail('Captcha error. Tap Start captcha to retry.'));"
    "c.onSuccess(async()=>{try{const v=c.getValidate();if(!v)throw Error();"
    "const r=await fetch(endpoint,{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(v)});"
    "if(!r.ok)throw Error();b.disabled=true;b.textContent='Complete';"
    "s.textContent='Complete. Return to the Switch and press Continue.';}"
    "catch(e){fail('Could not return the result to the Switch. Keep this page open and retry.');}});});"
    "</script></body></html>");
  if (builder.failed) return -1;
  *size_out = builder.length;
  return 0;
}

static int parse_service_response(const char *json, size_t size,
                                  JsonMinValue *data,
                                  char *message, size_t message_capacity) {
  JsonMinValue root;
  if (!json || !data || !message || !message_capacity ||
      json_min_parse(json, size, &root) || root.type != JSON_MIN_OBJECT)
    return -1;
  int64_t retcode = 0;
  if (!json_int_member(&root, "retcode", &retcode)) return -1;
  g_server_retcode = retcode;
  response_message(&root, message, message_capacity);
  if (retcode != 0) return 1;
  memset(data, 0, sizeof *data);
  (void)json_min_object_get(&root, "data", data);
  return 0;
}

static int supported_verify_method(int method) {
  return method == VERIFY_METHOD_EMAIL ||
         method == VERIFY_METHOD_MOBILE ||
         method == VERIFY_METHOD_PASSWORD;
}

static int parse_method_array(const JsonMinValue *array, int *methods,
                              size_t capacity, size_t *count_out) {
  if (!array || array->type != JSON_MIN_ARRAY || !methods || !capacity ||
      !count_out)
    return -1;
  size_t count = 0;
  if (json_min_array_count(array, &count) || count > capacity) return -1;
  for (size_t index = 0; index < count; ++index) {
    JsonMinValue value;
    int64_t method = 0;
    if (json_min_array_get(array, index, &value) ||
        json_min_int64(&value, &method) ||
        method < INT_MIN || method > INT_MAX)
      return -1;
    methods[index] = (int)method;
  }
  *count_out = count;
  return 0;
}

static int method_in_array(int method, const int *methods, size_t count) {
  for (size_t index = 0; index < count; ++index)
    if (methods[index] == method) return 1;
  return 0;
}

static int make_passport_body(char *body, size_t capacity, size_t *size_out) {
  StringBuilder builder = { body, capacity, 0, 0 };
  body[0] = 0;
  builder_raw(&builder, "{\"account\":");
  builder_json_string(&builder, g_account_cipher);
  builder_raw(&builder, ",\"password\":");
  builder_json_string(&builder, g_password_cipher);
  builder_raw(&builder, "}");
  if (builder.failed) return -1;
  *size_out = builder.length;
  return 0;
}

static int make_action_ticket_body(char *body, size_t capacity,
                                   size_t *size_out) {
  StringBuilder builder = { body, capacity, 0, 0 };
  body[0] = 0;
  builder_raw(&builder, "{\"action_type\":\"");
  builder_raw(&builder, VERIFY_ACTION_TYPE);
  builder_raw(&builder, "\",\"action_ticket\":");
  builder_json_string(&builder, g_action_ticket);
  builder_raw(&builder, "}");
  if (builder.failed) return -1;
  *size_out = builder.length;
  return 0;
}

static int make_verifier_method_body(char *body, size_t capacity,
                                     size_t *size_out) {
  if (!g_selected_method_count) return -1;
  StringBuilder builder = { body, capacity, 0, 0 };
  body[0] = 0;
  builder_raw(&builder, "{\"action_type\":\"");
  builder_raw(&builder, VERIFY_ACTION_TYPE);
  builder_raw(&builder, "\",\"action_ticket\":");
  builder_json_string(&builder, g_action_ticket);
  builder_raw(&builder,
              ",\"verify_method_combination\":{\"verify_methods\":[");
  for (size_t index = 0; index < g_selected_method_count; ++index) {
    if (index) builder_raw(&builder, ",");
    builder_format(&builder, "%d", g_selected_methods[index]);
  }
  builder_raw(&builder, "]}}");
  if (builder.failed) return -1;
  *size_out = builder.length;
  return 0;
}

static int make_verifier_factor_body(char *body, size_t capacity,
                                     size_t *size_out) {
  if (!supported_verify_method(g_verify_method) || !g_verifier_factor[0])
    return -1;
  StringBuilder builder = { body, capacity, 0, 0 };
  body[0] = 0;
  builder_raw(&builder, "{\"action_type\":\"");
  builder_raw(&builder, VERIFY_ACTION_TYPE);
  builder_raw(&builder, "\",\"action_ticket\":");
  builder_json_string(&builder, g_action_ticket);
  if (g_verify_method == VERIFY_METHOD_EMAIL)
    builder_raw(&builder, ",\"email_captcha\":");
  else if (g_verify_method == VERIFY_METHOD_MOBILE)
    builder_raw(&builder, ",\"mobile_captcha\":");
  else
    builder_raw(&builder, ",\"password\":");
  builder_json_string(&builder, g_verifier_factor);
  builder_raw(&builder, ",\"verify_method\":");
  builder_format(&builder, "%d", g_verify_method);
  builder_raw(&builder, "}");
  if (builder.failed) return -1;
  *size_out = builder.length;
  return 0;
}

static void queue_verifier_factor(int captcha_sent) {
  if (++g_verifier_steps > 16) {
    finish_error("HoYoverse verification did not converge",
                 COMBO_AUTH_PHASE_FAILED);
    return;
  }
  g_verifier_retry_pending = 0;
  if ((g_verify_method == VERIFY_METHOD_EMAIL ||
       g_verify_method == VERIFY_METHOD_MOBILE) && !captcha_sent) {
    __atomic_store_n(&g_phase, COMBO_AUTH_PHASE_VERIFIER_CAPTCHA_QUEUED,
                     __ATOMIC_RELEASE);
  } else {
    __atomic_store_n(&g_phase, COMBO_AUTH_PHASE_VERIFIER_INPUT_QUEUED,
                     __ATOMIC_RELEASE);
  }
}

static int select_verifier_combination(size_t selection) {
  if (selection >= g_verify_combination_count ||
      !g_verify_combination_sizes[selection])
    return -1;
  erase_bytes(g_selected_methods, sizeof g_selected_methods);
  const size_t method_count = g_verify_combination_sizes[selection];
  memcpy(g_selected_methods, g_verify_combinations[selection],
         method_count * sizeof g_selected_methods[0]);
  g_selected_method_count = method_count;
  g_verify_method = g_selected_methods[0];
  const size_t original_index =
    g_verify_combination_original_indices[selection];
  if (original_index) {
    __atomic_store_n(&g_phase, COMBO_AUTH_PHASE_VERIFIER_METHOD_QUEUED,
                     __ATOMIC_RELEASE);
  } else {
    queue_verifier_factor(g_captcha_sent);
  }
  return 0;
}

/* Return 0 after queuing the next genuine step, 1 when no native method can
 * satisfy the server-provided combinations, or -1 for a malformed response. */
static int process_verifier_info(const JsonMinValue *data) {
  if (!data || data->type != JSON_MIN_OBJECT) return -1;
  char refreshed_ticket[ACTION_TICKET_CAPACITY];
  memset(refreshed_ticket, 0, sizeof refreshed_ticket);
  if (json_string_member(data, "action_ticket", refreshed_ticket,
                         sizeof refreshed_ticket)) {
    if (!refreshed_ticket[0]) {
      erase_bytes(refreshed_ticket, sizeof refreshed_ticket);
      return -1;
    }
    erase_bytes(g_action_ticket, sizeof g_action_ticket);
    snprintf(g_action_ticket, sizeof g_action_ticket, "%s", refreshed_ticket);
  }
  erase_bytes(refreshed_ticket, sizeof refreshed_ticket);

  JsonMinValue verify_info;
  JsonMinValue combinations;
  if (!json_object_member(data, "verify_info", &verify_info) ||
      verify_info.type != JSON_MIN_OBJECT ||
      !json_object_member(&verify_info, "verify_method_combinations",
                          &combinations) ||
      combinations.type != JSON_MIN_ARRAY)
    return -1;
  char status[64];
  status[0] = 0;
  (void)json_string_member(&verify_info, "status", status, sizeof status);
  size_t combination_count = 0;
  if (json_min_array_count(&combinations, &combination_count) ||
      combination_count > VERIFY_COMBINATION_CAPACITY)
    return -1;
  if (!strcmp(status, VERIFY_STATUS_VERIFIED) || !combination_count) {
    erase_bytes(status, sizeof status);
    g_passport_verify_retry = 1;
    g_verifier_retry_pending = 0;
    __atomic_store_n(&g_phase, COMBO_AUTH_PHASE_PASSPORT_QUEUED,
                     __ATOMIC_RELEASE);
    return 0;
  }
  erase_bytes(status, sizeof status);

  int chosen[VERIFY_METHOD_CAPACITY];
  int partly[VERIFY_METHOD_CAPACITY];
  memset(chosen, 0, sizeof chosen);
  memset(partly, 0, sizeof partly);
  size_t chosen_count = 0;
  size_t partly_count = 0;
  JsonMinValue array;
  if (json_min_object_get(&verify_info, "chosen_methods", &array) == 0 &&
      parse_method_array(&array, chosen, VERIFY_METHOD_CAPACITY,
                         &chosen_count))
    return -1;
  if (json_min_object_get(&verify_info, "partly_verified_methods", &array) == 0 &&
      parse_method_array(&array, partly, VERIFY_METHOD_CAPACITY,
                         &partly_count))
    return -1;

  int captcha_sent = 0;
  (void)json_bool_member(data, "captcha_sent", &captcha_sent);
  g_captcha_sent = captcha_sent;
  if (chosen_count) {
    for (size_t index = 0; index < chosen_count; ++index) {
      const int method = chosen[index];
      if (method_in_array(method, partly, partly_count)) continue;
      if (!supported_verify_method(method)) {
        erase_bytes(chosen, sizeof chosen);
        erase_bytes(partly, sizeof partly);
        return 1;
      }
      g_verify_method = method;
      erase_bytes(chosen, sizeof chosen);
      erase_bytes(partly, sizeof partly);
      queue_verifier_factor(captcha_sent);
      return 0;
    }
    erase_bytes(chosen, sizeof chosen);
    erase_bytes(partly, sizeof partly);
    return -1;
  }
  erase_bytes(chosen, sizeof chosen);
  erase_bytes(partly, sizeof partly);

  erase_bytes(g_verify_combinations, sizeof g_verify_combinations);
  erase_bytes(g_verify_combination_sizes, sizeof g_verify_combination_sizes);
  erase_bytes(g_verify_combination_original_indices,
              sizeof g_verify_combination_original_indices);
  g_verify_combination_count = 0;
  for (size_t combination_index = 0;
       combination_index < combination_count; ++combination_index) {
    JsonMinValue combination;
    JsonMinValue methods_value;
    int methods[VERIFY_METHOD_CAPACITY];
    size_t method_count = 0;
    memset(methods, 0, sizeof methods);
    if (json_min_array_get(&combinations, combination_index, &combination) ||
        combination.type != JSON_MIN_OBJECT ||
        !json_object_member(&combination, "verify_methods", &methods_value) ||
        parse_method_array(&methods_value, methods, VERIFY_METHOD_CAPACITY,
                           &method_count) || !method_count) {
      erase_bytes(methods, sizeof methods);
      return -1;
    }
    int supported = 1;
    for (size_t index = 0; index < method_count; ++index)
      if (!supported_verify_method(methods[index])) supported = 0;
    if (!supported) {
      erase_bytes(methods, sizeof methods);
      continue;
    }
    const size_t selection = g_verify_combination_count++;
    memcpy(g_verify_combinations[selection], methods,
           method_count * sizeof methods[0]);
    g_verify_combination_sizes[selection] = method_count;
    g_verify_combination_original_indices[selection] = combination_index;
    erase_bytes(methods, sizeof methods);
  }
  if (!g_verify_combination_count) return 1;
  /* The keyboard-only frontend has no method picker. Select the first complete
   * combination offered by HoYoverse so verification cannot stall waiting for
   * the retired custom window. */
  return select_verifier_combination(0);
}

static int parse_passport_success(const char *json, size_t size,
                                  char *message, size_t message_capacity) {
  JsonMinValue root;
  if (json_min_parse(json, size, &root) || root.type != JSON_MIN_OBJECT)
    return -1;
  int64_t retcode = 0;
  if (!json_int_member(&root, "retcode", &retcode)) return -1;
  g_server_retcode = retcode;
  response_message(&root, message, message_capacity);
  if (retcode != 0) {
    g_challenge_code = challenge_class(retcode);
    return 1;
  }
  JsonMinValue data;
  JsonMinValue token;
  JsonMinValue user;
  if (!json_object_member(&root, "data", &data) ||
      data.type != JSON_MIN_OBJECT ||
      !json_object_member(&data, "token", &token) ||
      token.type != JSON_MIN_OBJECT ||
      !json_object_member(&data, "user_info", &user) ||
      user.type != JSON_MIN_OBJECT ||
      !json_string_member(&token, "token", g_passport_token,
                          sizeof g_passport_token) ||
      !json_string_member(&user, "aid", g_passport_uid,
                          sizeof g_passport_uid) ||
      !g_passport_token[0] || !g_passport_uid[0])
    return -1;
  g_passport_country[0] = 0;
  (void)json_string_member(&user, "country", g_passport_country,
                           sizeof g_passport_country);
  return 0;
}

static int build_passport_data(char *output, size_t capacity,
                               size_t *size_out) {
  StringBuilder builder = { output, capacity, 0, 0 };
  output[0] = 0;
  builder_raw(&builder, "{\"uid\":");
  builder_json_string(&builder, g_passport_uid);
  builder_raw(&builder, ",\"token\":");
  builder_json_string(&builder, g_passport_token);
  builder_raw(&builder, ",\"guest\":false,\"country_code\":");
  builder_json_string(&builder, g_passport_country);
  builder_raw(&builder, "}");
  if (builder.failed) return -1;
  *size_out = builder.length;
  return 0;
}

static void hmac_hex(const char *canonical, size_t canonical_size,
                     char output[65]) {
  uint8_t mac[SHA256_HASH_SIZE];
  hmacSha256CalculateMac(mac, COMBO_APP_KEY, strlen(COMBO_APP_KEY),
                         canonical, canonical_size);
  for (size_t index = 0; index < sizeof mac; ++index)
    snprintf(output + index * 2, 3, "%02x", mac[index]);
  output[64] = 0;
  erase_bytes(mac, sizeof mac);
}

static int make_combo_body(char *body, size_t body_capacity,
                           size_t *body_size) {
  const char *device_id = android_identity_android_id();
  char passport_data[TOKEN_CAPACITY + ID_CAPACITY + COUNTRY_CAPACITY + 128];
  size_t passport_data_size = 0;
  if (build_passport_data(passport_data, sizeof passport_data,
                          &passport_data_size))
    return -1;
  char canonical[TOKEN_CAPACITY + ID_CAPACITY + COUNTRY_CAPACITY + 256];
  StringBuilder signing = { canonical, sizeof canonical, 0, 0 };
  canonical[0] = 0;
  builder_raw(&signing, "app_id=4&channel_id=1&data=");
  builder_raw_n(&signing, passport_data, passport_data_size);
  builder_raw(&signing, "&device=");
  builder_raw(&signing, device_id);
  char signature[65];
  signature[0] = 0;
  if (!signing.failed) hmac_hex(canonical, signing.length, signature);

  StringBuilder builder = { body, body_capacity, 0, 0 };
  body[0] = 0;
  builder_raw(&builder, "{\"app_id\":4,\"channel_id\":1,\"device\":");
  builder_json_string(&builder, device_id);
  builder_raw(&builder, ",\"data\":");
  builder_json_string(&builder, passport_data);
  builder_raw(&builder, ",\"sign\":");
  builder_json_string(&builder, signature);
  builder_raw(&builder, "}");
  erase_bytes(passport_data, sizeof passport_data);
  erase_bytes(canonical, sizeof canonical);
  erase_bytes(signature, sizeof signature);
  if (signing.failed || builder.failed) return -1;
  *body_size = builder.length;
  return 0;
}

static int object_has_members(const JsonMinValue *object) {
  if (!object || object->type != JSON_MIN_OBJECT || object->length < 2)
    return 0;
  const char *cursor = object->start + 1;
  const char *end = object->start + object->length - 1;
  while (cursor < end && (*cursor == ' ' || *cursor == '\t' ||
                          *cursor == '\r' || *cursor == '\n'))
    ++cursor;
  return cursor < end;
}

static int finish_combo_success(const JsonMinValue *data) {
  char open_id[TOKEN_CAPACITY];
  char combo_token[TOKEN_CAPACITY];
  char *extension = NULL;
  JsonMinValue extension_object = {0};
  int64_t account_type = 0;
  if (!json_string_member(data, "open_id", open_id, sizeof open_id) ||
      !json_string_member(data, "combo_token", combo_token,
                          sizeof combo_token) ||
      !json_int_member(data, "account_type", &account_type) ||
      account_type < INT_MIN || account_type > INT_MAX ||
      !open_id[0] || !combo_token[0]) {
    erase_bytes(open_id, sizeof open_id);
    erase_bytes(combo_token, sizeof combo_token);
    return -1;
  }

  JsonMinValue extension_value;
  if (json_object_member(data, "data", &extension_value) &&
      extension_value.type == JSON_MIN_STRING) {
    extension = malloc(EXTENSION_CAPACITY);
    if (extension && !json_min_string(&extension_value, extension,
                                      EXTENSION_CAPACITY) &&
        !json_min_parse(extension, strlen(extension), &extension_object) &&
        extension_object.type == JSON_MIN_OBJECT) {
      /* Valid extension data is merged below like LoginVerifyEntity.toMap. */
    } else {
      if (extension) erase_bytes(extension, EXTENSION_CAPACITY);
      free(extension);
      extension = NULL;
      memset(&extension_object, 0, sizeof extension_object);
    }
  }

  char *inner = malloc(AUTH_CALLBACK_CAPACITY);
  if (!inner) {
    erase_bytes(open_id, sizeof open_id);
    erase_bytes(combo_token, sizeof combo_token);
    if (extension) erase_bytes(extension, EXTENSION_CAPACITY);
    free(extension);
    return -1;
  }
  StringBuilder builder = { inner, AUTH_CALLBACK_CAPACITY, 0, 0 };
  inner[0] = 0;
  builder_raw(&builder,
    "{\"ret\":0,\"msg\":\"\",\"data\":{\"app_id\":4,\"channel_id\":1,\"channel_token\":");
  builder_json_string(&builder, g_passport_token);
  builder_raw(&builder, ",\"account_type\":");
  builder_format(&builder, "%lld", (long long)account_type);
  builder_raw(&builder, ",\"open_id\":");
  builder_json_string(&builder, open_id);
  builder_raw(&builder, ",\"combo_token\":");
  builder_json_string(&builder, combo_token);
  builder_raw(&builder, ",\"device_id\":");
  builder_json_string(&builder, android_identity_android_id());
  builder_raw(&builder,
              ",\"guest\":false,\"is_new_register\":false,\"login_method\":\"1\"");
  if (extension && object_has_members(&extension_object)) {
    builder_raw(&builder, ",");
    builder_raw_n(&builder, extension_object.start + 1,
                  extension_object.length - 2);
  }
  builder_raw(&builder, "}}");

  const int queued = !builder.failed
    ? enqueue_inner_callback(inner, builder.length) : -1;
  if (queued == 0) {
    const int stored = combo_session_store(inner, builder.length,
                                            g_asterisk_name);
    if (stored == 0) {
      g_session_state = COMBO_AUTH_SESSION_SAVED;
      ++g_session_saves;
      g_session_checked = 1;
      g_session_replay_pending = 0;
    } else {
      g_session_state = stored == -2
        ? COMBO_AUTH_SESSION_KEY_UNAVAILABLE
        : COMBO_AUTH_SESSION_IO_ERROR;
    }
  }
  erase_bytes(inner, AUTH_CALLBACK_CAPACITY);
  free(inner);
  erase_bytes(open_id, sizeof open_id);
  erase_bytes(combo_token, sizeof combo_token);
  if (extension) erase_bytes(extension, EXTENSION_CAPACITY);
  free(extension);
  erase_attempt_secrets();
  g_retry_pending = 0;
  if (queued != 0) return -1;
  set_connection_ui_message("Login successful.");
  __atomic_store_n(&g_phase, COMBO_AUTH_PHASE_SUCCEEDED, __ATOMIC_RELEASE);
  return 0;
}

static int parse_combo_success(const char *json, size_t size,
                               char *message, size_t message_capacity) {
  JsonMinValue root;
  if (json_min_parse(json, size, &root) || root.type != JSON_MIN_OBJECT)
    return -1;
  int64_t retcode = INT64_MIN;
  int64_t code = INT64_MIN;
  const int has_retcode = json_int_member(&root, "retcode", &retcode);
  const int has_code = json_int_member(&root, "code", &code);
  if (!has_retcode && !has_code) return -1;
  g_server_retcode = has_retcode ? retcode : code;
  response_message(&root, message, message_capacity);
  if (!((has_retcode && retcode == 0) || (has_code && code == 200))) return 1;
  JsonMinValue data;
  if (!json_object_member(&root, "data", &data) ||
      data.type != JSON_MIN_OBJECT)
    return -1;
  return finish_combo_success(&data) == 0 ? 0 : -1;
}

static int verifier_post(const char *url, const char *body, size_t body_size,
                         HttpResponse *response) {
  if (!url || !body || !response) return -1;
  memset(response, 0, sizeof *response);
  response->capacity = HTTP_RESPONSE_CAPACITY;
  response->data = malloc(response->capacity);
  struct curl_slist *headers = verifier_headers();
  if (!response->data || !headers) {
    if (response->data) {
      erase_bytes(response->data, response->capacity);
      free(response->data);
    }
    response->data = NULL;
    secure_slist_free_all(headers);
    return -1;
  }
  response->data[0] = 0;
  long http_status = 0;
  const CURLcode result = http_post_json(url, headers, body, body_size,
                                          response, NULL, &http_status);
  secure_slist_free_all(headers);
  g_curl_code = (int)result;
  g_http_status = http_status;
  if (result != CURLE_OK || response->overflow) {
    erase_bytes(response->data, response->capacity);
    free(response->data);
    response->data = NULL;
    return -1;
  }
  return 0;
}

static void release_http_response(HttpResponse *response) {
  if (!response || !response->data) return;
  erase_bytes(response->data, response->capacity);
  free(response->data);
  memset(response, 0, sizeof *response);
}

static void run_verifier_info(void) {
  __atomic_store_n(&g_phase, COMBO_AUTH_PHASE_VERIFIER_INFO_RUNNING,
                   __ATOMIC_RELEASE);
  char body[ACTION_TICKET_CAPACITY + 256];
  size_t body_size = 0;
  if (make_action_ticket_body(body, sizeof body, &body_size)) {
    finish_error("Could not prepare HoYoverse verification",
                 COMBO_AUTH_PHASE_FAILED);
    return;
  }
  HttpResponse response;
  const int posted = verifier_post(VERIFIER_INFO_URL, body, body_size,
                                   &response);
  erase_bytes(body, sizeof body);
  if (posted) {
    finish_error("HoYoverse verification connection failed",
                 COMBO_AUTH_PHASE_FAILED);
    return;
  }
  JsonMinValue data;
  char message[MESSAGE_CAPACITY];
  const int parsed = parse_service_response(response.data, response.size,
                                             &data, message,
                                             sizeof message);
  int processed = -1;
  if (parsed == 0) processed = process_verifier_info(&data);
  release_http_response(&response);
  if (parsed == 1) {
    finish_error(message, COMBO_AUTH_PHASE_PASSPORT_REJECTED);
  } else if (parsed < 0 || processed < 0) {
    finish_error("Invalid HoYoverse verification response",
                 COMBO_AUTH_PHASE_FAILED);
  } else if (processed == 1) {
    finish_error("This account requires a HoYoverse verification method that is not available on Switch",
                 COMBO_AUTH_PHASE_VERIFIER_UNSUPPORTED);
  }
  erase_bytes(message, sizeof message);
}

static void run_verifier_method(void) {
  __atomic_store_n(&g_phase, COMBO_AUTH_PHASE_VERIFIER_METHOD_RUNNING,
                   __ATOMIC_RELEASE);
  char body[ACTION_TICKET_CAPACITY + 512];
  size_t body_size = 0;
  if (make_verifier_method_body(body, sizeof body, &body_size)) {
    finish_error("Could not select HoYoverse verification method",
                 COMBO_AUTH_PHASE_FAILED);
    return;
  }
  HttpResponse response;
  const int posted = verifier_post(VERIFIER_METHOD_URL, body, body_size,
                                   &response);
  erase_bytes(body, sizeof body);
  if (posted) {
    finish_error("HoYoverse verification connection failed",
                 COMBO_AUTH_PHASE_FAILED);
    return;
  }
  JsonMinValue data;
  char message[MESSAGE_CAPACITY];
  const int parsed = parse_service_response(response.data, response.size,
                                             &data, message,
                                             sizeof message);
  release_http_response(&response);
  if (parsed == 0) {
    __atomic_store_n(&g_phase, COMBO_AUTH_PHASE_VERIFIER_INFO_QUEUED,
                     __ATOMIC_RELEASE);
  } else if (parsed == 1) {
    finish_error(message, COMBO_AUTH_PHASE_PASSPORT_REJECTED);
  } else {
    finish_error("Invalid HoYoverse verification response",
                 COMBO_AUTH_PHASE_FAILED);
  }
  erase_bytes(message, sizeof message);
}

static void run_verifier_captcha(void) {
  __atomic_store_n(&g_phase, COMBO_AUTH_PHASE_VERIFIER_CAPTCHA_RUNNING,
                   __ATOMIC_RELEASE);
  const char *url = g_verify_method == VERIFY_METHOD_EMAIL ?
                      VERIFIER_EMAIL_CAPTCHA_URL :
                    g_verify_method == VERIFY_METHOD_MOBILE ?
                      VERIFIER_MOBILE_CAPTCHA_URL : NULL;
  char body[ACTION_TICKET_CAPACITY + 256];
  size_t body_size = 0;
  if (!url || make_action_ticket_body(body, sizeof body, &body_size)) {
    finish_error("Could not prepare HoYoverse verification code request",
                 COMBO_AUTH_PHASE_FAILED);
    return;
  }
  HttpResponse response;
  const int posted = verifier_post(url, body, body_size, &response);
  erase_bytes(body, sizeof body);
  if (posted) {
    finish_error("HoYoverse verification connection failed",
                 COMBO_AUTH_PHASE_FAILED);
    return;
  }
  JsonMinValue data;
  char message[MESSAGE_CAPACITY];
  const int parsed = parse_service_response(response.data, response.size,
                                             &data, message,
                                             sizeof message);
  release_http_response(&response);
  if (parsed == 0) {
    g_verifier_retry_pending = 0;
    set_connection_ui_message("A verification code was sent. Enter the latest code.");
    __atomic_store_n(&g_phase, COMBO_AUTH_PHASE_VERIFIER_INPUT_QUEUED,
                     __ATOMIC_RELEASE);
  } else if (parsed == 1) {
    finish_error(message, COMBO_AUTH_PHASE_PASSPORT_REJECTED);
  } else {
    finish_error("Invalid HoYoverse verification response",
                 COMBO_AUTH_PHASE_FAILED);
  }
  erase_bytes(message, sizeof message);
}

static void run_verifier_verify(void) {
  __atomic_store_n(&g_phase, COMBO_AUTH_PHASE_VERIFIER_VERIFY_RUNNING,
                   __ATOMIC_RELEASE);
  char body[ACTION_TICKET_CAPACITY + VERIFY_FACTOR_CAPACITY + 512];
  size_t body_size = 0;
  if (make_verifier_factor_body(body, sizeof body, &body_size)) {
    erase_bytes(g_verifier_factor, sizeof g_verifier_factor);
    finish_error("Could not prepare HoYoverse verification response",
                 COMBO_AUTH_PHASE_FAILED);
    return;
  }
  erase_bytes(g_verifier_factor, sizeof g_verifier_factor);
  HttpResponse response;
  const int posted = verifier_post(VERIFIER_PARTIAL_URL, body, body_size,
                                   &response);
  erase_bytes(body, sizeof body);
  if (posted) {
    finish_error("HoYoverse verification connection failed",
                 COMBO_AUTH_PHASE_FAILED);
    return;
  }
  JsonMinValue data;
  char message[MESSAGE_CAPACITY];
  const int parsed = parse_service_response(response.data, response.size,
                                             &data, message,
                                             sizeof message);
  release_http_response(&response);
  if (parsed == 0) {
    g_verifier_retry_pending = 0;
    __atomic_store_n(&g_phase, COMBO_AUTH_PHASE_VERIFIER_INFO_QUEUED,
                     __ATOMIC_RELEASE);
  } else if (parsed == 1 && g_server_retcode != -3003 &&
             g_server_retcode != -100) {
    /* Match the official page: an incorrect factor clears only the input and
     * leaves the action ticket alive so the user can try again. */
    g_verifier_retry_pending = 1;
    set_connection_ui_message("That verification response was not accepted. Try again.");
    __atomic_store_n(&g_phase, COMBO_AUTH_PHASE_VERIFIER_INPUT_QUEUED,
                     __ATOMIC_RELEASE);
  } else if (parsed == 1) {
    finish_error(message, COMBO_AUTH_PHASE_PASSPORT_REJECTED);
  } else {
    finish_error("Invalid HoYoverse verification response",
                 COMBO_AUTH_PHASE_FAILED);
  }
  erase_bytes(message, sizeof message);
}

static void run_preflight(void) {
  __atomic_store_n(&g_phase, COMBO_AUTH_PHASE_PREFLIGHT_RUNNING,
                   __ATOMIC_RELEASE);
  __atomic_store_n(&g_preflight_state, COMBO_AUTH_PREFLIGHT_RUNNING,
                   __ATOMIC_RELEASE);
  CURL *curl = curl_easy_init();
  CURLcode result = curl ? CURLE_OK : CURLE_FAILED_INIT;
  long http_status = 0;
  if (curl) {
    /* A HEAD 405 is valid here: TLS peer and hostname verification, not the
     * application response code, is the credential-input gate. */
    curl_easy_setopt(curl, CURLOPT_URL, PASSPORT_TLS_PREFLIGHT_URL);
    curl_easy_setopt(curl, CURLOPT_NOBODY, 1L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 15L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_CAINFO, CA_BUNDLE_PATH);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "GenshinImpact/6.7.0 Android");
    result = curl_easy_perform(curl);
    if (result == CURLE_OK)
      (void)curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_status);
    curl_easy_cleanup(curl);
  }
  g_curl_code = (int)result;
  g_http_status = http_status;
  if (result == CURLE_OK) {
    g_tls_verified = 1;
    set_connection_ui_message("Secure connection ready.");
    __atomic_store_n(&g_preflight_state, COMBO_AUTH_PREFLIGHT_PASSED,
                     __ATOMIC_RELEASE);
    __atomic_store_n(&g_phase, COMBO_AUTH_PHASE_INPUT_QUEUED,
                     __ATOMIC_RELEASE);
  } else {
    __atomic_store_n(&g_preflight_state, COMBO_AUTH_PREFLIGHT_FAILED,
                     __ATOMIC_RELEASE);
    finish_error("Secure connection failed", COMBO_AUTH_PHASE_FAILED);
  }
}

static void run_passport(void) {
  __atomic_store_n(&g_phase, COMBO_AUTH_PHASE_PASSPORT_RUNNING,
                   __ATOMIC_RELEASE);
  const int verify_retry = g_passport_verify_retry;
  const int aigis_retry = g_passport_aigis_retry;
  if (verify_retry && !g_verify_header[0]) {
    finish_error("HoYoverse verification state was lost",
                 COMBO_AUTH_PHASE_FAILED);
    return;
  }
  if (aigis_retry && !g_aigis_result[0]) {
    finish_error("HoYoverse captcha state was lost",
                 COMBO_AUTH_PHASE_FAILED);
    return;
  }
  char body[1024];
  size_t body_size = 0;
  if (make_passport_body(body, sizeof body, &body_size)) {
    finish_error("Could not prepare login request", COMBO_AUTH_PHASE_FAILED);
    return;
  }
  HttpResponse response = {0};
  HttpHeaders response_headers;
  memset(&response_headers, 0, sizeof response_headers);
  if (!verify_retry && !aigis_retry) {
    g_verify_header_state = COMBO_AUTH_VERIFY_HEADER_NOT_REQUESTED;
    g_verify_header_sources = 0;
    g_verify_header_bytes = 0;
    g_verify_header_lines = 0;
    g_aigis_header_state = COMBO_AUTH_AIGIS_HEADER_NOT_REQUESTED;
    g_aigis_header_sources = 0;
    g_aigis_header_bytes = 0;
  }
  response.capacity = HTTP_RESPONSE_CAPACITY;
  response.data = malloc(response.capacity);
  struct curl_slist *headers = passport_headers(
    verify_retry ? g_verify_header : NULL,
    aigis_retry ? g_aigis_result : NULL);
  if (!response.data || !headers) {
    erase_bytes(body, sizeof body);
    free(response.data);
    secure_slist_free_all(headers);
    erase_bytes(&response_headers, sizeof response_headers);
    finish_error("Could not prepare secure connection", COMBO_AUTH_PHASE_FAILED);
    return;
  }
  response.data[0] = 0;
  long http_status = 0;
  const CURLcode result = http_post_json(PASSPORT_PASSWORD_URL, headers,
                                          body, body_size, &response,
                                          &response_headers,
                                          &http_status);
  secure_slist_free_all(headers);
  erase_bytes(body, sizeof body);
  g_curl_code = (int)result;
  g_http_status = http_status;
  g_verify_header_sources = response_headers.verify_seen_sources;
  g_verify_header_bytes = response_headers.verify_size;
  g_verify_header_lines =
    (uint64_t)response_headers.callback_lines + response_headers.debug_lines;
  if (response_headers.verify_invalid)
    g_verify_header_state = COMBO_AUTH_VERIFY_HEADER_CAPTURE_INVALID;
  else if (response_headers.verify_size)
    g_verify_header_state = COMBO_AUTH_VERIFY_HEADER_CAPTURED;
  else
    g_verify_header_state = COMBO_AUTH_VERIFY_HEADER_MISSING;
  g_aigis_header_sources = response_headers.aigis_seen_sources;
  g_aigis_header_bytes = response_headers.aigis_size;
  if (response_headers.aigis_invalid)
    g_aigis_header_state = COMBO_AUTH_AIGIS_HEADER_CAPTURE_INVALID;
  else if (response_headers.aigis_size)
    g_aigis_header_state = COMBO_AUTH_AIGIS_HEADER_CAPTURED;
  else
    g_aigis_header_state = COMBO_AUTH_AIGIS_HEADER_MISSING;
  if (result != CURLE_OK || response.overflow) {
    erase_bytes(response.data, response.capacity);
    free(response.data);
    erase_bytes(&response_headers, sizeof response_headers);
    finish_error("Passport connection failed", COMBO_AUTH_PHASE_FAILED);
    return;
  }
  char message[MESSAGE_CAPACITY];
  const int parsed = parse_passport_success(response.data, response.size,
                                             message, sizeof message);
  erase_bytes(response.data, response.capacity);
  free(response.data);
  if (parsed == 0) {
    erase_login_ciphertext();
    erase_verifier_secrets();
    g_challenge_code = 0;
    __atomic_store_n(&g_phase, COMBO_AUTH_PHASE_COMBO_QUEUED,
                     __ATOMIC_RELEASE);
  } else if (parsed == 1) {
    if (g_server_retcode == -3101) {
      /* Porte OS 2.3.0 classifies -3101 with CommonResponse.needCaptcha(),
       * reads x-rpc-aigis, launches GeeTestManager, and retries this request
       * with the solved SDK result as x-rpc-aigis. It does not use the
       * x-rpc-verify identity-ticket path for this response. */
      if (!response_headers.aigis_invalid && response_headers.aigis_size &&
          parse_aigis_header(response_headers.aigis,
                             response_headers.aigis_size) == 0) {
        set_connection_ui_message("HoYoverse requested its official captcha.");
        __atomic_store_n(&g_phase, COMBO_AUTH_PHASE_GEETEST_QUEUED,
                         __ATOMIC_RELEASE);
      } else {
        finish_error("HoYoverse requested a captcha, but its protected Aigis capability could not be decoded",
                     COMBO_AUTH_PHASE_CHALLENGE_REQUIRED);
      }
    } else if (g_challenge_code) {
      if (!response_headers.verify_invalid && response_headers.verify_size &&
          parse_verify_header(response_headers.verify,
                              response_headers.verify_size) == 0) {
        if (g_verify_type == VERIFY_TYPE_IDENTITY) {
          set_connection_ui_message("HoYoverse requested an account security check.");
          __atomic_store_n(&g_phase, COMBO_AUTH_PHASE_VERIFIER_INFO_QUEUED,
                           __ATOMIC_RELEASE);
        } else {
          /* A verify_type=1 header is a distinct legacy Geetest route and must
           * never be sent to the identity action-ticket endpoints. */
          finish_error("HoYoverse requires an interactive browser captcha that this build cannot safely complete",
                       COMBO_AUTH_PHASE_CHALLENGE_REQUIRED);
        }
      } else {
        finish_error("HoYoverse requested a security challenge, but its protected capability could not be decoded",
                     COMBO_AUTH_PHASE_CHALLENGE_REQUIRED);
      }
    } else if (g_server_retcode == -3208) {
      /* Wrong account/password is handled inside the Android login UI rather
       * than completing callback 3. Re-open secure input and let the user
       * cancel explicitly if they do not want to retry. */
      erase_attempt_secrets();
      g_retry_pending = 1;
      set_connection_ui_message("Incorrect account or password. Try again.");
      __atomic_store_n(&g_phase, COMBO_AUTH_PHASE_INPUT_QUEUED,
                       __ATOMIC_RELEASE);
    } else {
      finish_error(message, COMBO_AUTH_PHASE_PASSPORT_REJECTED);
    }
  } else {
    finish_error("Invalid Passport response", COMBO_AUTH_PHASE_FAILED);
  }
  erase_bytes(&response_headers, sizeof response_headers);
  erase_bytes(message, sizeof message);
}

static void run_combo(void) {
  __atomic_store_n(&g_phase, COMBO_AUTH_PHASE_COMBO_RUNNING,
                   __ATOMIC_RELEASE);
  char body[TOKEN_CAPACITY * 2 + 2048];
  size_t body_size = 0;
  if (make_combo_body(body, sizeof body, &body_size)) {
    finish_error("Could not sign game login", COMBO_AUTH_PHASE_FAILED);
    return;
  }
  HttpResponse response = {0};
  response.capacity = HTTP_RESPONSE_CAPACITY;
  response.data = malloc(response.capacity);
  struct curl_slist *headers = combo_headers();
  if (!response.data || !headers) {
    erase_bytes(body, sizeof body);
    free(response.data);
    secure_slist_free_all(headers);
    finish_error("Could not prepare game login", COMBO_AUTH_PHASE_FAILED);
    return;
  }
  response.data[0] = 0;
  long http_status = 0;
  const CURLcode result = http_post_json(COMBO_LOGIN_URL, headers,
                                          body, body_size, &response,
                                          NULL,
                                          &http_status);
  secure_slist_free_all(headers);
  erase_bytes(body, sizeof body);
  g_curl_code = (int)result;
  g_http_status = http_status;
  if (result != CURLE_OK || response.overflow) {
    erase_bytes(response.data, response.capacity);
    free(response.data);
    finish_error("Game login connection failed", COMBO_AUTH_PHASE_FAILED);
    return;
  }
  char message[MESSAGE_CAPACITY];
  const int parsed = parse_combo_success(response.data, response.size,
                                          message, sizeof message);
  erase_bytes(response.data, response.capacity);
  free(response.data);
  if (parsed == 1)
    finish_error(message, COMBO_AUTH_PHASE_COMBO_REJECTED);
  else if (parsed < 0)
    finish_error("Invalid game login response", COMBO_AUTH_PHASE_FAILED);
  erase_bytes(message, sizeof message);
}

int combo_auth_init(void) {
  if (g_initialized) return 0;
  const CURLcode curl_result = curl_global_init(CURL_GLOBAL_DEFAULT);
  if (curl_result != CURLE_OK) {
    g_curl_code = (int)curl_result;
    return -1;
  }
  const Result spl_result = splInitialize();
  g_service_result = (int)spl_result;
  if (R_FAILED(spl_result)) {
    curl_global_cleanup();
    return -1;
  }
  g_spl_initialized = 1;
  uint8_t lifecycle[16];
  randomGet(lifecycle, sizeof lifecycle);
  lifecycle[6] = (lifecycle[6] & 0x0f) | 0x40;
  lifecycle[8] = (lifecycle[8] & 0x3f) | 0x80;
  snprintf(g_lifecycle_id, sizeof g_lifecycle_id,
           "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
           lifecycle[0], lifecycle[1], lifecycle[2], lifecycle[3],
           lifecycle[4], lifecycle[5], lifecycle[6], lifecycle[7],
           lifecycle[8], lifecycle[9], lifecycle[10], lifecycle[11],
           lifecycle[12], lifecycle[13], lifecycle[14], lifecycle[15]);
  erase_bytes(lifecycle, sizeof lifecycle);
  g_initialized = 1;
  return 0;
}

static int terminal_phase(int phase) {
  return phase == COMBO_AUTH_PHASE_IDLE ||
         phase == COMBO_AUTH_PHASE_SUCCEEDED ||
         phase == COMBO_AUTH_PHASE_CANCELED ||
         phase == COMBO_AUTH_PHASE_FAILED ||
         phase == COMBO_AUTH_PHASE_PASSPORT_REJECTED ||
         phase == COMBO_AUTH_PHASE_COMBO_REJECTED ||
         phase == COMBO_AUTH_PHASE_CHALLENGE_REQUIRED ||
         phase == COMBO_AUTH_PHASE_VERIFIER_UNSUPPORTED;
}

ComboAuthRequestResult combo_auth_request_login(int callback_index) {
  if (!g_initialized || callback_index < 0)
    return COMBO_AUTH_REQUEST_REJECTED;
  const int current = __atomic_load_n(&g_phase, __ATOMIC_ACQUIRE);
  if (!terminal_phase(current)) return COMBO_AUTH_REQUEST_REJECTED;
  erase_attempt_secrets();
  erase_bytes(g_asterisk_name, sizeof g_asterisk_name);
  g_callback_index = callback_index;
  g_curl_code = CURLE_OK;
  g_http_status = 0;
  g_service_result = 0;
  g_server_retcode = 0;
  g_challenge_code = 0;
  g_verify_type = 0;
  g_verify_method = 0;
  g_verify_header_state = COMBO_AUTH_VERIFY_HEADER_NOT_REQUESTED;
  g_verify_header_sources = 0;
  g_verify_header_bytes = 0;
  g_verify_header_lines = 0;
  g_aigis_header_state = COMBO_AUTH_AIGIS_HEADER_NOT_REQUESTED;
  g_aigis_header_sources = 0;
  g_aigis_header_bytes = 0;
  g_geetest_pages_served = 0;
  g_geetest_results_received = 0;
  g_geetest_requests_rejected = 0;
  g_retry_pending = 0;
  /* HOS 22.5.0 rejected the WebApplet before it served a page, and taking the
   * default NWindow for a replacement native framebuffer crashed at the first
   * console handoff. Keep the hardware-proven blocking swkbd path primary. */
  g_connection_ui_mode = COMBO_AUTH_UI_MODE_KEYBOARD_FALLBACK;
  g_connection_ui_state = COMBO_AUTH_UI_STATE_FALLBACK;
  g_connection_ui_result = 0;
  g_connection_ui_messages_sent = 0;
  g_connection_ui_messages_received = 0;
  g_connection_ui_pages_served = 0;
  set_connection_ui_message("Preparing secure sign-in.");
  ++g_login_requests;

  /* The Android SDK normally restores this state before showing its account
   * panel.  Replay the exact previously authenticated success callback once per
   * process.  If managed code asks to log in again after consuming that replay,
   * treat it as rejection/expiry, delete it, and return to interactive auth. */
  if (g_session_replay_pending) {
    combo_session_invalidate();
    ++g_session_invalidations;
    g_session_state = COMBO_AUTH_SESSION_INVALIDATED;
    g_session_replay_pending = 0;
  } else if (!g_session_checked) {
    g_session_checked = 1;
    char *saved_inner = NULL;
    size_t saved_inner_size = 0;
    char saved_name[ASTERISK_NAME_CAPACITY];
    const ComboSessionLoadResult loaded =
      combo_session_load(&saved_inner, &saved_inner_size, saved_name,
                         sizeof saved_name);
    if (loaded == COMBO_SESSION_LOAD_OK) {
      snprintf(g_asterisk_name, sizeof g_asterisk_name, "%s", saved_name);
      const int queued = enqueue_inner_callback(saved_inner, saved_inner_size);
      erase_bytes(saved_name, sizeof saved_name);
      erase_bytes(saved_inner, saved_inner_size + 1);
      free(saved_inner);
      if (queued == 0) {
        g_session_state = COMBO_AUTH_SESSION_RESTORED;
        ++g_session_restores;
        g_session_replay_pending = 1;
        g_connection_ui_mode = COMBO_AUTH_UI_MODE_NONE;
        g_connection_ui_state = COMBO_AUTH_UI_STATE_IDLE;
        set_connection_ui_message("Saved login restored.");
        __atomic_store_n(&g_preflight_state, COMBO_AUTH_PREFLIGHT_IDLE,
                         __ATOMIC_RELEASE);
        __atomic_store_n(&g_phase, COMBO_AUTH_PHASE_SUCCEEDED,
                         __ATOMIC_RELEASE);
        return COMBO_AUTH_REQUEST_SESSION_RESTORED;
      }
      erase_bytes(g_asterisk_name, sizeof g_asterisk_name);
      g_session_state = COMBO_AUTH_SESSION_IO_ERROR;
    } else {
      erase_bytes(saved_name, sizeof saved_name);
      if (loaded == COMBO_SESSION_LOAD_INVALID) {
        combo_session_invalidate();
        ++g_session_invalidations;
        g_session_state = COMBO_AUTH_SESSION_INVALIDATED;
      } else if (loaded == COMBO_SESSION_LOAD_IO_ERROR) {
        g_session_state = COMBO_AUTH_SESSION_IO_ERROR;
      } else if (loaded == COMBO_SESSION_LOAD_KEY_UNAVAILABLE) {
        g_session_state = COMBO_AUTH_SESSION_KEY_UNAVAILABLE;
      } else {
        g_session_state = COMBO_AUTH_SESSION_MISSING;
      }
    }
  }
  if (g_tls_verified) {
    __atomic_store_n(&g_preflight_state, COMBO_AUTH_PREFLIGHT_PASSED,
                     __ATOMIC_RELEASE);
    __atomic_store_n(&g_phase, COMBO_AUTH_PHASE_INPUT_QUEUED,
                     __ATOMIC_RELEASE);
  } else {
    __atomic_store_n(&g_preflight_state, COMBO_AUTH_PREFLIGHT_QUEUED,
                     __ATOMIC_RELEASE);
    __atomic_store_n(&g_phase, COMBO_AUTH_PHASE_PREFLIGHT_QUEUED,
                     __ATOMIC_RELEASE);
  }
  return COMBO_AUTH_REQUEST_QUEUED;
}

void combo_auth_invalidate_session(void) {
  combo_session_invalidate();
  ++g_session_invalidations;
  g_session_state = COMBO_AUTH_SESSION_INVALIDATED;
  g_session_checked = 1;
  g_session_replay_pending = 0;
  erase_bytes(g_asterisk_name, sizeof g_asterisk_name);
  if (__atomic_load_n(&g_phase, __ATOMIC_ACQUIRE) ==
      COMBO_AUTH_PHASE_SUCCEEDED)
    __atomic_store_n(&g_phase, COMBO_AUTH_PHASE_IDLE, __ATOMIC_RELEASE);
}

ComboAuthAction combo_auth_next_action(void) {
  const int phase = __atomic_load_n(&g_phase, __ATOMIC_ACQUIRE);
  if (phase == COMBO_AUTH_PHASE_INPUT_QUEUED)
    return COMBO_AUTH_ACTION_INPUT;
  if (phase == COMBO_AUTH_PHASE_VERIFIER_INPUT_QUEUED)
    return COMBO_AUTH_ACTION_VERIFIER_INPUT;
  if (phase == COMBO_AUTH_PHASE_GEETEST_QUEUED)
    return COMBO_AUTH_ACTION_GEETEST;
  if (phase == COMBO_AUTH_PHASE_PREFLIGHT_QUEUED ||
      phase == COMBO_AUTH_PHASE_PASSPORT_QUEUED ||
      phase == COMBO_AUTH_PHASE_COMBO_QUEUED ||
      phase == COMBO_AUTH_PHASE_VERIFIER_INFO_QUEUED ||
      phase == COMBO_AUTH_PHASE_VERIFIER_METHOD_QUEUED ||
      phase == COMBO_AUTH_PHASE_VERIFIER_CAPTCHA_QUEUED ||
      phase == COMBO_AUTH_PHASE_VERIFIER_VERIFY_QUEUED)
    return COMBO_AUTH_ACTION_NETWORK;
  return COMBO_AUTH_ACTION_NONE;
}

void combo_auth_collect_credentials(void) {
  int expected = COMBO_AUTH_PHASE_INPUT_QUEUED;
  if (!__atomic_compare_exchange_n(&g_phase, &expected,
                                   COMBO_AUTH_PHASE_INPUT_RUNNING, 0,
                                   __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
    return;
  char plaintext[INPUT_CAPACITY];
  memset(plaintext, 0, sizeof plaintext);
  Result result = show_keyboard(0, plaintext, sizeof plaintext);
  g_retry_pending = 0;
  g_service_result = (int)result;
  if (R_FAILED(result)) {
    erase_bytes(plaintext, sizeof plaintext);
    if (is_keyboard_cancel(result)) finish_cancel();
    else finish_error("Account keyboard failed", COMBO_AUTH_PHASE_FAILED);
    return;
  }
  capture_asterisk_name(plaintext);
  if (rsa_encrypt_text(plaintext, g_account_cipher,
                       sizeof g_account_cipher)) {
    erase_bytes(plaintext, sizeof plaintext);
    finish_error("Account encryption failed", COMBO_AUTH_PHASE_FAILED);
    return;
  }
  erase_bytes(plaintext, sizeof plaintext);

  result = show_keyboard(1, plaintext, sizeof plaintext);
  g_service_result = (int)result;
  if (R_FAILED(result)) {
    erase_bytes(plaintext, sizeof plaintext);
    if (is_keyboard_cancel(result)) finish_cancel();
    else finish_error("Password keyboard failed", COMBO_AUTH_PHASE_FAILED);
    return;
  }
  if (rsa_encrypt_text(plaintext, g_password_cipher,
                       sizeof g_password_cipher)) {
    erase_bytes(plaintext, sizeof plaintext);
    finish_error("Password encryption failed", COMBO_AUTH_PHASE_FAILED);
    return;
  }
  erase_bytes(plaintext, sizeof plaintext);
  __atomic_store_n(&g_phase, COMBO_AUTH_PHASE_PASSPORT_QUEUED,
                   __ATOMIC_RELEASE);
}

const char *combo_auth_asterisk_name(void) {
  return __atomic_load_n(&g_phase, __ATOMIC_ACQUIRE) ==
           COMBO_AUTH_PHASE_SUCCEEDED ? g_asterisk_name : "";
}

void combo_auth_collect_verification(void) {
  int expected = COMBO_AUTH_PHASE_VERIFIER_INPUT_QUEUED;
  if (!__atomic_compare_exchange_n(&g_phase, &expected,
                                   COMBO_AUTH_PHASE_VERIFIER_INPUT_RUNNING, 0,
                                   __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
    return;
  if (!supported_verify_method(g_verify_method)) {
    finish_error("Unsupported HoYoverse verification method",
                 COMBO_AUTH_PHASE_VERIFIER_UNSUPPORTED);
    return;
  }
  char plaintext[INPUT_CAPACITY];
  memset(plaintext, 0, sizeof plaintext);
  const Result result = show_verifier_keyboard(g_verify_method, plaintext,
                                                sizeof plaintext);
  g_service_result = (int)result;
  if (R_FAILED(result)) {
    erase_bytes(plaintext, sizeof plaintext);
    if (is_keyboard_cancel(result)) finish_cancel();
    else finish_error("Verification keyboard failed",
                      COMBO_AUTH_PHASE_FAILED);
    return;
  }
  erase_bytes(g_verifier_factor, sizeof g_verifier_factor);
  if (g_verify_method == VERIFY_METHOD_PASSWORD) {
    if (rsa_encrypt_text(plaintext, g_verifier_factor,
                         sizeof g_verifier_factor)) {
      erase_bytes(plaintext, sizeof plaintext);
      finish_error("Verification password encryption failed",
                   COMBO_AUTH_PHASE_FAILED);
      return;
    }
  } else {
    const size_t length = strlen(plaintext);
    int valid = length == 6;
    for (size_t index = 0; index < length; ++index)
      if (plaintext[index] < '0' || plaintext[index] > '9') valid = 0;
    if (!valid) {
      erase_bytes(plaintext, sizeof plaintext);
      g_verifier_retry_pending = 1;
      __atomic_store_n(&g_phase, COMBO_AUTH_PHASE_VERIFIER_INPUT_QUEUED,
                       __ATOMIC_RELEASE);
      return;
    }
    memcpy(g_verifier_factor, plaintext, length + 1);
  }
  erase_bytes(plaintext, sizeof plaintext);
  g_verifier_retry_pending = 0;
  __atomic_store_n(&g_phase, COMBO_AUTH_PHASE_VERIFIER_VERIFY_QUEUED,
                   __ATOMIC_RELEASE);
}

void combo_auth_collect_geetest(void) {
  int expected = COMBO_AUTH_PHASE_GEETEST_QUEUED;
  if (!__atomic_compare_exchange_n(&g_phase, &expected,
                                   COMBO_AUTH_PHASE_GEETEST_RUNNING, 0,
                                   __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
    return;
  if (!g_aigis_gt[0] || !g_aigis_session[0] || !g_aigis_risk_type[0]) {
    finish_error("HoYoverse captcha parameters were lost",
                 COMBO_AUTH_PHASE_CHALLENGE_REQUIRED);
    return;
  }

  /* Eight unambiguous Base32 characters carry 40 random bits.  The bridge is
   * LAN-only and lives for at most three minutes; this keeps the capability
   * unguessable in its lifetime while making the displayed URL practical to
   * type on another device. */
  uint8_t nonce_bytes[5];
  char nonce[9];
  randomGet(nonce_bytes, sizeof nonce_bytes);
  geetest_short_token(nonce, nonce_bytes);
  erase_bytes(nonce_bytes, sizeof nonce_bytes);
  char path[16];
  char complete_path[16];
  snprintf(path, sizeof path, "/g/%s", nonce);
  snprintf(complete_path, sizeof complete_path, "/c/%s", nonce);

  u32 address_value = 0;
  const Result address_result = nifmGetCurrentIpAddress(&address_value);
  g_service_result = (int)address_result;
  struct in_addr address = { .s_addr = address_value };
  char ip[INET_ADDRSTRLEN];
  char url[128];
  memset(ip, 0, sizeof ip);
  memset(url, 0, sizeof url);
  if (R_FAILED(address_result) ||
      !inet_ntop(AF_INET, &address, ip, sizeof ip)) {
    erase_bytes(nonce, sizeof nonce);
    erase_bytes(path, sizeof path);
    erase_bytes(complete_path, sizeof complete_path);
    finish_error("Could not determine the Switch LAN address for HoYoverse captcha",
                 COMBO_AUTH_PHASE_CHALLENGE_REQUIRED);
    return;
  }
  char *html = malloc(GEETEST_HTML_CAPACITY);
  size_t html_size = 0;
  if (!html || build_geetest_html(html, GEETEST_HTML_CAPACITY, &html_size,
                                  complete_path)) {
    free(html);
    erase_bytes(nonce, sizeof nonce);
    erase_bytes(path, sizeof path);
    erase_bytes(complete_path, sizeof complete_path);
    erase_bytes(ip, sizeof ip);
    erase_bytes(url, sizeof url);
    finish_error("Could not prepare the HoYoverse captcha page",
                 COMBO_AUTH_PHASE_CHALLENGE_REQUIRED);
    return;
  }

  uint16_t listen_port = GEETEST_PRIMARY_PORT;
  int listen_fd = geetest_open_listener(listen_port);
  if (listen_fd < 0) {
    listen_port = GEETEST_FALLBACK_PORT;
    listen_fd = geetest_open_listener(listen_port);
  }
  if (listen_fd < 0) {
    erase_bytes(html, GEETEST_HTML_CAPACITY);
    free(html);
    erase_bytes(nonce, sizeof nonce);
    erase_bytes(path, sizeof path);
    erase_bytes(complete_path, sizeof complete_path);
    erase_bytes(ip, sizeof ip);
    erase_bytes(url, sizeof url);
    g_service_result = errno;
    finish_error("Could not start the local HoYoverse captcha bridge",
                 COMBO_AUTH_PHASE_CHALLENGE_REQUIRED);
    return;
  }
  if (listen_port == 80)
    snprintf(url, sizeof url, "http://%s%s", ip, path);
  else
    snprintf(url, sizeof url, "http://%s:%u%s", ip,
             (unsigned)listen_port, path);

  GeetestServer server = {
    .listen_fd = listen_fd,
    .path = path,
    .complete_path = complete_path,
    .html = html,
    .html_size = html_size,
  };
  Thread thread;
  memset(&thread, 0, sizeof thread);
  Result thread_result = threadCreate(&thread, geetest_server_thread, &server,
                                      NULL, GEETEST_STACK_SIZE, 0x2c, -2);
  if (R_SUCCEEDED(thread_result)) thread_result = threadStart(&thread);
  g_service_result = (int)thread_result;
  if (R_FAILED(thread_result)) {
    if (thread.handle) threadClose(&thread);
    close(listen_fd);
    erase_bytes(html, GEETEST_HTML_CAPACITY);
    free(html);
    erase_bytes(nonce, sizeof nonce);
    erase_bytes(path, sizeof path);
    erase_bytes(complete_path, sizeof complete_path);
    erase_bytes(ip, sizeof ip);
    erase_bytes(url, sizeof url);
    finish_error("Could not start the HoYoverse captcha worker",
                 COMBO_AUTH_PHASE_CHALLENGE_REQUIRED);
    return;
  }

  g_aigis_header_state = COMBO_AUTH_AIGIS_HEADER_BRIDGE_RUNNING;
  set_connection_ui_message("Complete the official captcha at the displayed LAN URL.");
  char confirmation[8];
  memset(confirmation, 0, sizeof confirmation);
  const Result show_result = show_geetest_keyboard(url, confirmation,
                                                   sizeof confirmation);
  g_service_result = (int)show_result;
  erase_bytes(confirmation, sizeof confirmation);
  if (R_SUCCEEDED(show_result)) {
    /* Usually the phone POST arrives while the keyboard is visible. Allow a
     * short final race after Continue without leaving Unity paused forever. */
    for (unsigned attempt = 0; attempt < 1800 &&
         !__atomic_load_n(&server.result_ready, __ATOMIC_ACQUIRE); ++attempt)
      svcSleepThread(UINT64_C(100000000));
  }
  __atomic_store_n(&server.stop, 1, __ATOMIC_RELEASE);
  (void)shutdown(listen_fd, SHUT_RDWR);
  (void)threadWaitForExit(&thread);
  (void)threadClose(&thread);
  close(listen_fd);
  const int result_ready =
    __atomic_load_n(&server.result_ready, __ATOMIC_ACQUIRE);

  erase_bytes(html, GEETEST_HTML_CAPACITY);
  free(html);
  erase_bytes(nonce, sizeof nonce);
  erase_bytes(path, sizeof path);
  erase_bytes(complete_path, sizeof complete_path);
  erase_bytes(ip, sizeof ip);
  erase_bytes(url, sizeof url);
  erase_bytes(&server, sizeof server);

  if (R_FAILED(show_result)) {
    if (is_keyboard_cancel(show_result)) finish_cancel();
    else finish_error("HoYoverse captcha prompt failed",
                      COMBO_AUTH_PHASE_CHALLENGE_REQUIRED);
    return;
  }
  if (!result_ready || !g_aigis_result[0]) {
    g_geetest_retry_pending = 1;
    g_aigis_header_state = COMBO_AUTH_AIGIS_HEADER_V4_READY;
    set_connection_ui_message("No completed captcha was received. Open the new URL and try again.");
    __atomic_store_n(&g_phase, COMBO_AUTH_PHASE_GEETEST_QUEUED,
                     __ATOMIC_RELEASE);
    return;
  }

  erase_bytes(g_aigis_gt, sizeof g_aigis_gt);
  erase_bytes(g_aigis_session, sizeof g_aigis_session);
  erase_bytes(g_aigis_risk_type, sizeof g_aigis_risk_type);
  g_geetest_retry_pending = 0;
  g_passport_aigis_retry = 1;
  g_aigis_header_state = COMBO_AUTH_AIGIS_HEADER_RESULT_READY;
  set_connection_ui_message("Checking the completed HoYoverse captcha.");
  __atomic_store_n(&g_phase, COMBO_AUTH_PHASE_PASSPORT_QUEUED,
                   __ATOMIC_RELEASE);
}

void combo_auth_tick(void) {
  if (!g_initialized) return;
  const int phase = __atomic_load_n(&g_phase, __ATOMIC_ACQUIRE);
  if (phase == COMBO_AUTH_PHASE_PREFLIGHT_QUEUED) run_preflight();
  else if (phase == COMBO_AUTH_PHASE_PASSPORT_QUEUED) run_passport();
  else if (phase == COMBO_AUTH_PHASE_COMBO_QUEUED) run_combo();
  else if (phase == COMBO_AUTH_PHASE_VERIFIER_INFO_QUEUED)
    run_verifier_info();
  else if (phase == COMBO_AUTH_PHASE_VERIFIER_METHOD_QUEUED)
    run_verifier_method();
  else if (phase == COMBO_AUTH_PHASE_VERIFIER_CAPTCHA_QUEUED)
    run_verifier_captcha();
  else if (phase == COMBO_AUTH_PHASE_VERIFIER_VERIFY_QUEUED)
    run_verifier_verify();
}

void combo_auth_get_diagnostics(ComboAuthDiagnostics *out) {
  if (!out) return;
  memset(out, 0, sizeof(*out));
  out->login_requests = g_login_requests;
  out->callback_index = g_callback_index;
  out->preflight_state = (ComboAuthPreflightState)
    __atomic_load_n(&g_preflight_state, __ATOMIC_ACQUIRE);
  out->phase = (ComboAuthPhase)
    __atomic_load_n(&g_phase, __ATOMIC_ACQUIRE);
  out->curl_code = g_curl_code;
  out->http_status = g_http_status;
  out->service_result = g_service_result;
  out->server_retcode = g_server_retcode;
  out->challenge_code = g_challenge_code;
  out->verify_type = g_verify_type;
  out->verify_method = g_verify_method;
  out->verify_header_state = (ComboAuthVerifyHeaderState)g_verify_header_state;
  out->verify_header_sources = g_verify_header_sources;
  out->verify_header_bytes = g_verify_header_bytes;
  out->verify_header_lines = g_verify_header_lines;
  out->aigis_header_state =
    (ComboAuthAigisHeaderState)g_aigis_header_state;
  out->aigis_header_sources = g_aigis_header_sources;
  out->aigis_header_bytes = g_aigis_header_bytes;
  out->geetest_pages_served = g_geetest_pages_served;
  out->geetest_results_received = g_geetest_results_received;
  out->geetest_requests_rejected = g_geetest_requests_rejected;
  out->connection_ui_mode = (ComboAuthUiMode)g_connection_ui_mode;
  out->connection_ui_state = (ComboAuthUiState)g_connection_ui_state;
  out->connection_ui_result = g_connection_ui_result;
  out->connection_ui_messages_sent = g_connection_ui_messages_sent;
  out->connection_ui_messages_received = g_connection_ui_messages_received;
  out->connection_ui_pages_served = g_connection_ui_pages_served;
  out->session_state = (ComboAuthSessionState)g_session_state;
  out->session_restores = g_session_restores;
  out->session_saves = g_session_saves;
  out->session_invalidations = g_session_invalidations;
}

void combo_auth_shutdown(void) {
  erase_attempt_secrets();
  erase_bytes(g_asterisk_name, sizeof g_asterisk_name);
  erase_bytes(g_connection_ui_message, sizeof g_connection_ui_message);
  erase_bytes(g_lifecycle_id, sizeof g_lifecycle_id);
  if (g_spl_initialized) splExit();
  if (g_initialized) curl_global_cleanup();
  g_spl_initialized = 0;
  g_initialized = 0;
  g_session_checked = 0;
  g_session_replay_pending = 0;
  g_session_state = COMBO_AUTH_SESSION_UNCHECKED;
}
