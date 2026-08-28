/* Main-thread transport for genuine HoYo Combo callbacks into Unity. */
#ifndef GENSHIN_COMBO_BRIDGE_H
#define GENSHIN_COMBO_BRIDGE_H

#include <stddef.h>
#include <stdint.h>

#include "so_util.h"

typedef enum {
  COMBO_ROUTE_INIT_RESPONSE,
  COMBO_ROUTE_NOTIFICATION,
  COMBO_ROUTE_INVOKE_RESPONSE,
  COMBO_ROUTE_OTHER_NOTIFICATION,
  COMBO_ROUTE_DOWNLOAD_NOTIFICATION,
  COMBO_ROUTE_COUNT,
} ComboCallbackRoute;

/* Payload-free state snapshot for hardware boot diagnostics.  The last invoke
 * name is an SDK operation identifier such as login_init; request JSON and
 * callback payloads are never retained here. */
typedef struct {
  uint64_t observed_invokes;
  uint64_t rejected_invokes;
  uint64_t callbacks_enqueued;
  uint64_t callbacks_delivered;
  uint32_t pending_count;
  uint32_t event_count;
  int accepting;
  int focused;
  int render_ready;
  char last_invoke_name[128];
} ComboBridgeDiagnostics;

/* Resolve and verify the exact Unity export while ELF metadata is live. */
int combo_bridge_init(so_module *game_module);
const char *combo_bridge_error(void);

/* Record the managed request before an official SDK implementation starts
 * asynchronous work.  JSON is deliberately not retained by this layer. */
int combo_bridge_observe_invoke(const char *invoke_name,
                                int32_t callback_index);

/* Producer API for genuine SDK/Passport results.  Payload bytes are copied;
 * embedded NUL, oversize, overflow, unknown indices, and duplicate terminal
 * events are rejected atomically. */
int combo_bridge_enqueue_callback(ComboCallbackRoute route,
                                  int32_t callback_index,
                                  const char *payload, size_t payload_size,
                                  int terminal);

/* Lifecycle/main-loop API.  A focus gain requires one new successful render
 * before callbacks may drain.  after_render drains at most eight events. */
void combo_bridge_set_focus(int focused);
void combo_bridge_after_render(void);
void combo_bridge_get_diagnostics(ComboBridgeDiagnostics *out);
void combo_bridge_shutdown(void);

#endif
