#ifndef GENSHIN_ANDROID_LOG_SINK_H
#define GENSHIN_ANDROID_LOG_SINK_H

#include <stdarg.h>

/* Android/liblog compatibility for the guest.  Records are accepted and
 * discarded; only a fatal abort reason is persisted to fatal.txt. */
int android_log_sink_write(int buffer_id, int priority,
                           const char *tag, const char *text);
int android_log_sink_write_nonblocking(int buffer_id, int priority,
                                       const char *tag, const char *text);
int android_log_sink_vprint(int buffer_id, int priority,
                            const char *tag, const char *format, va_list args);
void android_log_sink_abort_message(const char *message);

#endif
