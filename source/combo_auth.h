#ifndef GENSHIN_COMBO_AUTH_H
#define GENSHIN_COMBO_AUTH_H

#include <stdint.h>

typedef enum {
  COMBO_AUTH_PREFLIGHT_IDLE = 0,
  COMBO_AUTH_PREFLIGHT_QUEUED,
  COMBO_AUTH_PREFLIGHT_RUNNING,
  COMBO_AUTH_PREFLIGHT_PASSED,
  COMBO_AUTH_PREFLIGHT_FAILED,
} ComboAuthPreflightState;

typedef enum {
  COMBO_AUTH_PHASE_IDLE = 0,
  COMBO_AUTH_PHASE_PREFLIGHT_QUEUED,
  COMBO_AUTH_PHASE_PREFLIGHT_RUNNING,
  COMBO_AUTH_PHASE_INPUT_QUEUED,
  COMBO_AUTH_PHASE_INPUT_RUNNING,
  COMBO_AUTH_PHASE_PASSPORT_QUEUED,
  COMBO_AUTH_PHASE_PASSPORT_RUNNING,
  COMBO_AUTH_PHASE_COMBO_QUEUED,
  COMBO_AUTH_PHASE_COMBO_RUNNING,
  COMBO_AUTH_PHASE_SUCCEEDED,
  COMBO_AUTH_PHASE_CANCELED,
  COMBO_AUTH_PHASE_FAILED,
  COMBO_AUTH_PHASE_PASSPORT_REJECTED,
  COMBO_AUTH_PHASE_COMBO_REJECTED,
  COMBO_AUTH_PHASE_CHALLENGE_REQUIRED,
  COMBO_AUTH_PHASE_VERIFIER_INFO_QUEUED,
  COMBO_AUTH_PHASE_VERIFIER_INFO_RUNNING,
  COMBO_AUTH_PHASE_VERIFIER_METHOD_QUEUED,
  COMBO_AUTH_PHASE_VERIFIER_METHOD_RUNNING,
  COMBO_AUTH_PHASE_VERIFIER_CAPTCHA_QUEUED,
  COMBO_AUTH_PHASE_VERIFIER_CAPTCHA_RUNNING,
  COMBO_AUTH_PHASE_VERIFIER_INPUT_QUEUED,
  COMBO_AUTH_PHASE_VERIFIER_INPUT_RUNNING,
  COMBO_AUTH_PHASE_VERIFIER_VERIFY_QUEUED,
  COMBO_AUTH_PHASE_VERIFIER_VERIFY_RUNNING,
  COMBO_AUTH_PHASE_VERIFIER_UNSUPPORTED,
  COMBO_AUTH_PHASE_GEETEST_QUEUED,
  COMBO_AUTH_PHASE_GEETEST_RUNNING,
} ComboAuthPhase;

typedef enum {
  COMBO_AUTH_ACTION_NONE = 0,
  COMBO_AUTH_ACTION_NETWORK,
  COMBO_AUTH_ACTION_INPUT,
  COMBO_AUTH_ACTION_VERIFIER_INPUT,
  COMBO_AUTH_ACTION_GEETEST,
} ComboAuthAction;

typedef enum {
  COMBO_AUTH_UI_MODE_NONE = 0,
  /* Preserve the diagnostic value used by the previous fallback builds. */
  COMBO_AUTH_UI_MODE_KEYBOARD_FALLBACK = 2,
} ComboAuthUiMode;

/* Secret-free keyboard input state. */
typedef enum {
  COMBO_AUTH_UI_STATE_IDLE = 0,
  /* Preserve the diagnostic value used by the previous fallback builds. */
  COMBO_AUTH_UI_STATE_FALLBACK = 14,
} ComboAuthUiState;

typedef enum {
  COMBO_AUTH_SESSION_UNCHECKED = 0,
  COMBO_AUTH_SESSION_MISSING,
  COMBO_AUTH_SESSION_SAVED,
  COMBO_AUTH_SESSION_RESTORED,
  COMBO_AUTH_SESSION_INVALIDATED,
  COMBO_AUTH_SESSION_IO_ERROR,
  COMBO_AUTH_SESSION_KEY_UNAVAILABLE,
} ComboAuthSessionState;

typedef enum {
  COMBO_AUTH_REQUEST_REJECTED = -1,
  COMBO_AUTH_REQUEST_QUEUED = 0,
  COMBO_AUTH_REQUEST_SESSION_RESTORED = 1,
} ComboAuthRequestResult;

/* Secret-free stages for diagnosing the Passport x-rpc-verify capability.
 * The header bytes themselves are never logged or exposed through this API. */
typedef enum {
  COMBO_AUTH_VERIFY_HEADER_NOT_REQUESTED = 0,
  COMBO_AUTH_VERIFY_HEADER_MISSING,
  COMBO_AUTH_VERIFY_HEADER_CAPTURE_INVALID,
  COMBO_AUTH_VERIFY_HEADER_CAPTURED,
  COMBO_AUTH_VERIFY_HEADER_OUTER_JSON_INVALID,
  COMBO_AUTH_VERIFY_HEADER_NESTED_JSON_INVALID,
  COMBO_AUTH_VERIFY_HEADER_FIELDS_INVALID,
  COMBO_AUTH_VERIFY_HEADER_READY,
} ComboAuthVerifyHeaderState;

/* Secret-free stages for the distinct x-rpc-aigis/Geetest capability.  The
 * ticket, captcha parameters, LAN nonce, and solved response are never logged
 * or exposed to Unity. */
typedef enum {
  COMBO_AUTH_AIGIS_HEADER_NOT_REQUESTED = 0,
  COMBO_AUTH_AIGIS_HEADER_MISSING,
  COMBO_AUTH_AIGIS_HEADER_CAPTURE_INVALID,
  COMBO_AUTH_AIGIS_HEADER_CAPTURED,
  COMBO_AUTH_AIGIS_HEADER_JSON_INVALID,
  COMBO_AUTH_AIGIS_HEADER_FIELDS_INVALID,
  COMBO_AUTH_AIGIS_HEADER_V3_UNSUPPORTED,
  COMBO_AUTH_AIGIS_HEADER_V4_READY,
  COMBO_AUTH_AIGIS_HEADER_BRIDGE_RUNNING,
  COMBO_AUTH_AIGIS_HEADER_RESULT_READY,
} ComboAuthAigisHeaderState;

typedef struct {
  uint64_t login_requests;
  int callback_index;
  ComboAuthPreflightState preflight_state;
  ComboAuthPhase phase;
  int curl_code;
  long http_status;
  int service_result;
  int64_t server_retcode;
  int challenge_code;
  int verify_type;
  int verify_method;
  ComboAuthVerifyHeaderState verify_header_state;
  uint32_t verify_header_sources;
  uint64_t verify_header_bytes;
  uint64_t verify_header_lines;
  ComboAuthAigisHeaderState aigis_header_state;
  uint32_t aigis_header_sources;
  uint64_t aigis_header_bytes;
  uint64_t geetest_pages_served;
  uint64_t geetest_results_received;
  uint64_t geetest_requests_rejected;
  ComboAuthUiMode connection_ui_mode;
  ComboAuthUiState connection_ui_state;
  int connection_ui_result;
  uint64_t connection_ui_messages_sent;
  uint64_t connection_ui_messages_received;
  uint32_t connection_ui_pages_served;
  ComboAuthSessionState session_state;
  uint64_t session_restores;
  uint64_t session_saves;
  uint64_t session_invalidations;
} ComboAuthDiagnostics;

/* Initialize the host-side HTTPS transport while the libnx thread context is
 * still installed. No request is made here. */
int combo_auth_init(void);

/* Record the real login_login boundary without retaining its JSON payload.
 * The Unity callback remains pending until a genuine authenticated result is
 * available. */
ComboAuthRequestResult combo_auth_request_login(int callback_index);

/* Clear the persisted session on the Android SDK's explicit logout or role
 * switch boundary. */
void combo_auth_invalidate_session(void);

/* Return the in-memory, redacted account label used by the Android SDK's
 * login_get_asterisk_name synchronous getter.  The original account string is
 * never retained; an empty string means that no login identity is active. */
const char *combo_auth_asterisk_name(void);

/* Query which host action is ready. Keyboard and network actions must run with
 * the libnx host thread pointer, never Unity's guest Bionic TLS pointer. */
ComboAuthAction combo_auth_next_action(void);

/* Display the system keyboards and RSA-encrypt each value immediately. Unity
 * must be paused and unfocused for this blocking applet transaction. */
void combo_auth_collect_credentials(void);

/* Collect one official HoYoverse verification factor. Six-digit codes are
 * retained in memory only until the immediately following HTTPS request;
 * passwords are RSA-encrypted before this function returns. */
void combo_auth_collect_verification(void);

/* Serve the exact SDK-selected Geetest v4 challenge to a phone on the same
 * LAN. The Switch system keyboard shows the short-lived local URL while the
 * official Geetest page is active; only a structurally valid solved result is
 * accepted and retried as x-rpc-aigis. */
void combo_auth_collect_geetest(void);

/* Run one queued HTTPS operation: preflight, Passport, account verification,
 * or Combo. */
void combo_auth_tick(void);
void combo_auth_get_diagnostics(ComboAuthDiagnostics *out);
void combo_auth_shutdown(void);

#endif
