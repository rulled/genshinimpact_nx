/* Android liblog entry points for the guest.
 *
 * Records are captured to run_log.txt for diagnostics.  Only a fatal guest
 * abort is also persisted to fatal.txt. */

#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <switch.h>

#include "config.h"
#include "android_log_sink.h"

#define ANDROID_LOG_TEXT_BYTES 4096u

static Mutex g_abort_lock;
static Mutex g_log_lock;

static const char *priority_tag(int priority) {
  switch (priority) {
    case 2: return "V";
    case 3: return "D";
    case 4: return "I";
    case 5: return "W";
    case 6: return "E";
    case 7: return "F";
    default: return "?";
  }
}

static void sanitize_line(const char *input, char *output, size_t capacity) {
  size_t used = 0;
  if (!input) input = "(null)";
  while (*input && used + 1u < capacity) {
    const unsigned char byte = (unsigned char)*input++;
    output[used++] = (byte < 0x20u || byte == 0x7fu) ? ' ' : (char)byte;
  }
  output[used] = '\0';
}

static void write_log_line(int priority, const char *tag, const char *text) {
  char safe_text[ANDROID_LOG_TEXT_BYTES];
  char safe_tag[128];
  sanitize_line(text, safe_text, sizeof(safe_text));
  sanitize_line(tag ? tag : "(null)", safe_tag, sizeof(safe_tag));
  if (!mutexTryLock(&g_log_lock)) return;
  FILE *file = fopen(GAME_HOME "/run_log.txt", "ab");
  if (file) {
    fprintf(file, "[%s] %s: %s\n", priority_tag(priority), safe_tag, safe_text);
    fflush(file);
    fclose(file);
  }
  mutexUnlock(&g_log_lock);
}

int android_log_sink_write(int buffer_id, int priority,
                           const char *tag, const char *text) {
  (void)buffer_id;
  if (priority >= 4 && text)
    write_log_line(priority, tag, text);
  return 1;
}

int android_log_sink_write_nonblocking(int buffer_id, int priority,
                                       const char *tag, const char *text) {
  (void)buffer_id;
  if (priority >= 4 && text)
    write_log_line(priority, tag, text);
  return 1;
}

int android_log_sink_vprint(int buffer_id, int priority,
                            const char *tag, const char *format, va_list args) {
  (void)buffer_id;
  if (priority < 4 || !format) return 1;
  char text[ANDROID_LOG_TEXT_BYTES];
  vsnprintf(text, sizeof(text), format, args);
  write_log_line(priority, tag, text);
  return 1;
}

void android_log_sink_abort_message(const char *message) {
  char safe[ANDROID_LOG_TEXT_BYTES];
  sanitize_line(message ? message : "(null abort message)",
                safe, sizeof(safe));
  write_log_line(7, "libc++abi", safe);
  if (!mutexTryLock(&g_abort_lock)) return;
  FILE *file = fopen(GAME_HOME "/fatal.txt", "ab");
  if (file) {
    fprintf(file, "guest abort: %s\n", safe);
    fflush(file);
    fclose(file);
  }
  mutexUnlock(&g_abort_lock);
}
