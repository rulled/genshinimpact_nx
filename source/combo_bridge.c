/* Safe native-to-Unity queue for the exact Genshin 7.0.1 Combo callback ABI. */

#include <switch.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "combo_bridge.h"

#define UNITY_SEND_MESSAGE_RVA 0x04DF1FF4u
#define COMBO_EVENT_CAPACITY 64u
#define COMBO_PENDING_CAPACITY 64u
#define COMBO_PAYLOAD_CAPACITY (256u * 1024u)
#define COMBO_AGGREGATE_CAPACITY (2u * 1024u * 1024u)
#define COMBO_DRAIN_PER_FRAME 8u
#define COMBO_INVOKE_NAME_CAPACITY 128u

typedef void (*UnitySendMessageFn)(const char *object_name,
                                   const char *method_name,
                                   const char *payload);

typedef struct {
  ComboCallbackRoute route;
  int32_t callback_index;
  uint64_t arrival_sequence;
  char *payload;
  size_t payload_size;
  uint8_t terminal;
} ComboEvent;

typedef struct {
  int32_t callback_index;
  char invoke_name[COMBO_INVOKE_NAME_CAPACITY];
  uint8_t active;
  uint8_t terminal_queued;
} PendingInvoke;

static const char *const g_route_methods[COMBO_ROUTE_COUNT] = {
  "OnInitResponse",
  "OnNotificationCallback",
  "OnGetInvokeResponse",
  "OnOtherNotifyCallback",
  "OnDownloadNotificationCallback",
};

static Mutex g_lock;
static UnitySendMessageFn g_unity_send_message;
static ComboEvent g_events[COMBO_EVENT_CAPACITY];
static PendingInvoke g_pending[COMBO_PENDING_CAPACITY];
static size_t g_event_head;
static size_t g_event_count;
static size_t g_payload_bytes;
static uint64_t g_arrival_sequence;
static uint64_t g_observed_invokes;
static uint64_t g_rejected_invokes;
static uint64_t g_callbacks_enqueued;
static uint64_t g_callbacks_delivered;
static char g_last_invoke_name[COMBO_INVOKE_NAME_CAPACITY];
static int g_accepting;
static int g_focused;
static int g_render_ready;
static char g_error[192];

static void erase_bytes(void *memory, size_t size) {
  volatile unsigned char *p = memory;
  while (p && size--) *p++ = 0;
}

static void set_error_locked(const char *message) {
  snprintf(g_error, sizeof(g_error), "%s", message ? message : "unknown error");
}

const char *combo_bridge_error(void) { return g_error[0] ? g_error : NULL; }

static PendingInvoke *find_pending_locked(int32_t callback_index) {
  for (size_t i = 0; i < COMBO_PENDING_CAPACITY; ++i)
    if (g_pending[i].active &&
        g_pending[i].callback_index == callback_index)
      return &g_pending[i];
  return NULL;
}

static PendingInvoke *alloc_pending_locked(void) {
  for (size_t i = 0; i < COMBO_PENDING_CAPACITY; ++i)
    if (!g_pending[i].active) return &g_pending[i];
  return NULL;
}

static int route_requires_pending(ComboCallbackRoute route) {
  return route == COMBO_ROUTE_INIT_RESPONSE ||
         route == COMBO_ROUTE_INVOKE_RESPONSE;
}

int combo_bridge_init(so_module *game_module) {
  memset(g_events, 0, sizeof(g_events));
  memset(g_pending, 0, sizeof(g_pending));
  g_event_head = g_event_count = g_payload_bytes = 0;
  g_arrival_sequence = 0;
  g_observed_invokes = g_rejected_invokes = 0;
  g_callbacks_enqueued = g_callbacks_delivered = 0;
  g_last_invoke_name[0] = 0;
  g_focused = g_render_ready = 0;
  g_accepting = 0;
  g_error[0] = 0;
  mutexInit(&g_lock);

  if (!game_module || !game_module->load_virtbase ||
      game_module->load_size <= UNITY_SEND_MESSAGE_RVA) {
    set_error_locked("Unity module bounds are unavailable");
    return -1;
  }
  const uintptr_t base = (uintptr_t)game_module->load_virtbase;
  const uintptr_t expected = base + UNITY_SEND_MESSAGE_RVA;
  const uintptr_t found = so_try_find_addr_rx(game_module, "UnitySendMessage");
  if (!found || found != expected || (found & 3u) != 0 ||
      found < base || found + 4u < found ||
      found + 4u > base + game_module->load_size) {
    set_error_locked("UnitySendMessage export does not match the exact supported image");
    return -1;
  }

  g_unity_send_message = (UnitySendMessageFn)found;
  g_accepting = 1;
  return 0;
}

int combo_bridge_observe_invoke(const char *invoke_name,
                                int32_t callback_index) {
  if (!invoke_name) {
    mutexLock(&g_lock);
    ++g_rejected_invokes;
    mutexUnlock(&g_lock);
    return -1;
  }
  const size_t name_size = strnlen(invoke_name, COMBO_INVOKE_NAME_CAPACITY);
  if (!name_size || name_size == COMBO_INVOKE_NAME_CAPACITY) {
    mutexLock(&g_lock);
    ++g_rejected_invokes;
    mutexUnlock(&g_lock);
    return -1;
  }

  mutexLock(&g_lock);
  if (!g_accepting) {
    ++g_rejected_invokes;
    set_error_locked("Combo callback bridge is not accepting requests");
    mutexUnlock(&g_lock);
    return -1;
  }
  /* The shipped LaunchModule uses a negative callback index for one-way
   * operations such as launch_del_notification.  DEX dispatch performs the
   * action and returns without constructing DefaultInvokeCallback.  Record
   * these calls, but never consume a pending slot or demand a response. */
  if (callback_index < 0) {
    ++g_observed_invokes;
    memcpy(g_last_invoke_name, invoke_name, name_size + 1);
    mutexUnlock(&g_lock);
    return 0;
  }
  if (find_pending_locked(callback_index)) {
    ++g_rejected_invokes;
    set_error_locked("duplicate active Combo callback index");
    mutexUnlock(&g_lock);
    return -1;
  }
  PendingInvoke *pending = alloc_pending_locked();
  if (!pending) {
    ++g_rejected_invokes;
    set_error_locked("Combo pending-invocation table is full");
    mutexUnlock(&g_lock);
    return -1;
  }
  pending->callback_index = callback_index;
  memcpy(pending->invoke_name, invoke_name, name_size + 1);
  pending->terminal_queued = 0;
  pending->active = 1;
  ++g_observed_invokes;
  memcpy(g_last_invoke_name, invoke_name, name_size + 1);
  mutexUnlock(&g_lock);
  return 0;
}

int combo_bridge_enqueue_callback(ComboCallbackRoute route,
                                  int32_t callback_index,
                                  const char *payload, size_t payload_size,
                                  int terminal) {
  if (route < 0 || route >= COMBO_ROUTE_COUNT || !payload ||
      payload_size > COMBO_PAYLOAD_CAPACITY ||
      memchr(payload, '\0', payload_size) != NULL)
    return -1;

  char *owned = malloc(payload_size + 1);
  if (!owned) return -1;
  memcpy(owned, payload, payload_size);
  owned[payload_size] = 0;

  mutexLock(&g_lock);
  PendingInvoke *pending = NULL;
  if (!g_accepting) {
    set_error_locked("Combo callback bridge is shutting down");
    goto reject;
  }
  if (route_requires_pending(route)) {
    pending = find_pending_locked(callback_index);
    if (!pending) {
      set_error_locked("Combo callback index is not pending");
      goto reject;
    }
    if (route == COMBO_ROUTE_INIT_RESPONSE &&
        strcmp(pending->invoke_name, "login_init")) {
      set_error_locked("Combo initialization response does not match login_init");
      goto reject;
    }
    if (terminal && pending->terminal_queued) {
      set_error_locked("duplicate terminal Combo callback");
      goto reject;
    }
  }
  if (g_event_count == COMBO_EVENT_CAPACITY ||
      payload_size > COMBO_AGGREGATE_CAPACITY - g_payload_bytes) {
    set_error_locked("Combo callback queue capacity exceeded");
    goto reject;
  }

  const size_t tail = (g_event_head + g_event_count) % COMBO_EVENT_CAPACITY;
  ComboEvent *event = &g_events[tail];
  event->route = route;
  event->callback_index = callback_index;
  event->arrival_sequence = ++g_arrival_sequence;
  event->payload = owned;
  event->payload_size = payload_size;
  event->terminal = terminal ? 1 : 0;
  ++g_event_count;
  ++g_callbacks_enqueued;
  g_payload_bytes += payload_size;
  if (pending && terminal) pending->terminal_queued = 1;
  mutexUnlock(&g_lock);
  return 0;

reject:
  mutexUnlock(&g_lock);
  erase_bytes(owned, payload_size + 1);
  free(owned);
  return -1;
}

void combo_bridge_set_focus(int focused) {
  mutexLock(&g_lock);
  g_focused = focused ? 1 : 0;
  g_render_ready = 0;
  mutexUnlock(&g_lock);
}

static int pop_event_after_render(ComboEvent *out) {
  mutexLock(&g_lock);
  if (!g_accepting || !g_focused || !g_render_ready || !g_event_count) {
    mutexUnlock(&g_lock);
    return 0;
  }
  *out = g_events[g_event_head];
  memset(&g_events[g_event_head], 0, sizeof(g_events[g_event_head]));
  g_event_head = (g_event_head + 1) % COMBO_EVENT_CAPACITY;
  --g_event_count;
  g_payload_bytes -= out->payload_size;
  mutexUnlock(&g_lock);
  return 1;
}

static void complete_terminal_event(const ComboEvent *event) {
  if (!route_requires_pending(event->route) || !event->terminal) return;
  mutexLock(&g_lock);
  PendingInvoke *pending = find_pending_locked(event->callback_index);
  if (pending) {
    erase_bytes(pending, sizeof(*pending));
  }
  mutexUnlock(&g_lock);
}

void combo_bridge_after_render(void) {
  mutexLock(&g_lock);
  if (g_accepting && g_focused) g_render_ready = 1;
  const int can_drain = g_accepting && g_focused && g_render_ready;
  mutexUnlock(&g_lock);
  if (!can_drain || !g_unity_send_message) return;

  for (size_t drained = 0; drained < COMBO_DRAIN_PER_FRAME; ++drained) {
    ComboEvent event;
    if (!pop_event_after_render(&event)) break;
    const char *method = g_route_methods[event.route];
    g_unity_send_message("MiHoYoSDK", method, event.payload);
    mutexLock(&g_lock);
    ++g_callbacks_delivered;
    mutexUnlock(&g_lock);
    complete_terminal_event(&event);
    erase_bytes(event.payload, event.payload_size + 1);
    free(event.payload);
  }
}

void combo_bridge_get_diagnostics(ComboBridgeDiagnostics *out) {
  if (!out) return;
  memset(out, 0, sizeof(*out));
  mutexLock(&g_lock);
  out->observed_invokes = g_observed_invokes;
  out->rejected_invokes = g_rejected_invokes;
  out->callbacks_enqueued = g_callbacks_enqueued;
  out->callbacks_delivered = g_callbacks_delivered;
  for (size_t i = 0; i < COMBO_PENDING_CAPACITY; ++i)
    if (g_pending[i].active) ++out->pending_count;
  out->event_count = (uint32_t)g_event_count;
  out->accepting = g_accepting;
  out->focused = g_focused;
  out->render_ready = g_render_ready;
  memcpy(out->last_invoke_name, g_last_invoke_name,
         sizeof(out->last_invoke_name));
  out->last_invoke_name[sizeof(out->last_invoke_name) - 1] = 0;
  mutexUnlock(&g_lock);
}

void combo_bridge_shutdown(void) {
  mutexLock(&g_lock);
  g_accepting = g_focused = g_render_ready = 0;
  for (size_t i = 0; i < COMBO_EVENT_CAPACITY; ++i) {
    ComboEvent *event = &g_events[i];
    if (event->payload) {
      erase_bytes(event->payload, event->payload_size + 1);
      free(event->payload);
    }
    erase_bytes(event, sizeof(*event));
  }
  erase_bytes(g_pending, sizeof(g_pending));
  erase_bytes(g_last_invoke_name, sizeof(g_last_invoke_name));
  g_event_head = g_event_count = g_payload_bytes = 0;
  g_unity_send_message = NULL;
  mutexUnlock(&g_lock);
}
