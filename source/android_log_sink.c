/* Android liblog entry points for the guest.
 *
 * Records are accepted and discarded: this port keeps no runtime log.  Only a
 * fatal guest abort is persisted, next to the libnx exception report. */

#include <stdio.h>
#include <stdarg.h>
#include <switch.h>

#include "config.h"
#include "android_log_sink.h"

#define ANDROID_LOG_TEXT_BYTES 4096u

/* The fatal reporter can be entered while another thread already owns it, or
 * from a thread the crash path has suspended, so it is never waited on. */
static Mutex g_abort_lock;

static void sanitize_line(const char *input, char *output, size_t capacity) {
  size_t used = 0;
  if (!input) input = "(null)";
  while (*input && used + 1u < capacity) {
    const unsigned char byte = (unsigned char)*input++;
    /* Never let a guest forge a new record or write terminal control bytes. */
    output[used++] = (byte < 0x20u || byte == 0x7fu) ? ' ' : (char)byte;
  }
  output[used] = '\0';
}

/* Android liblog returns 1 when a loggable message is accepted. */
int android_log_sink_write(int buffer_id, int priority,
                           const char *tag, const char *text) {
  (void)buffer_id;
  (void)priority;
  (void)tag;
  (void)text;
  return 1;
}

int android_log_sink_write_nonblocking(int buffer_id, int priority,
                                       const char *tag, const char *text) {
  (void)buffer_id;
  (void)priority;
  (void)tag;
  (void)text;
  return 0;
}

int android_log_sink_vprint(int buffer_id, int priority,
                            const char *tag, const char *format, va_list args) {
  (void)buffer_id;
  (void)priority;
  (void)tag;
  (void)format;
  (void)args;
  return 1;
}

void android_log_sink_abort_message(const char *message) {
  /* libc++abi can enter the client's Android abort reporter before a fake
   * JNIEnv exists.  Persist the reason where the exception handler appends
   * its own report, without ever blocking the fatal path. */
  char safe[ANDROID_LOG_TEXT_BYTES];
  sanitize_line(message ? message : "(null abort message)",
                safe, sizeof(safe));
  if (!mutexTryLock(&g_abort_lock)) return;
  FILE *file = fopen(GAME_HOME "/fatal.txt", "ab");
  if (file) {
    fprintf(file, "guest abort: %s\n", safe);
    fflush(file);
    fclose(file);
  }
  mutexUnlock(&g_abort_lock);
}
