/* NVK Rust panic capture.
 *
 * The NVK driver is written in Rust and its panic hook formats the payload
 * through newlib (memstream) and prints with a raw write(2) before aborting.
 * Both steps run while the loader's crash dump is still reachable, so keep
 * enough state here for the abort() override to recover the panic message:
 *
 *  - pthread_mutex_lock failures are recorded (newlib only returns EINVAL,
 *    which means the pthread_mutex_t itself is NULL or corrupt) and then
 *    reported as success so the panic hook survives long enough to flush its
 *    message.  A corrupted mutex is already a fatal condition; losing the
 *    lock guarantee changes nothing for a process that is about to die.
 *  - open_memstream output locations are remembered so the finalized buffer
 *    can be dumped verbatim from the abort path.
 */

#define _GNU_SOURCE

#include <pthread.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include <switch.h>

#include "panic_capture.h"

int __real_pthread_mutex_lock(pthread_mutex_t *mutex);
FILE *__real_open_memstream(char **buffer, size_t *size);

static pthread_mutex_t *g_failed_mutex;
static int g_failed_mutex_result;
static char **g_memstream_buffer_out;
static size_t *g_memstream_size_out;

static int range_readable(uintptr_t address, size_t bytes) {
  if (!address || !bytes) return 0;
  MemoryInfo info;
  u32 page_info;
  if (R_FAILED(svcQueryMemory(&info, &page_info, address))) return 0;
  if (!(info.perm & Perm_R)) return 0;
  if (info.type == MemType_Unmapped) return 0;
  return (size_t)(info.addr + info.size - address) >= bytes;
}

int __wrap_pthread_mutex_lock(pthread_mutex_t *mutex) {
  const int result = __real_pthread_mutex_lock(mutex);
  if (result == 0) return 0;
  g_failed_mutex = mutex;
  g_failed_mutex_result = result;
  return 0;
}

FILE *__wrap_open_memstream(char **buffer, size_t *size) {
  FILE *stream = __real_open_memstream(buffer, size);
  if (stream && buffer && size) {
    g_memstream_buffer_out = buffer;
    g_memstream_size_out = size;
  }
  return stream;
}

void panic_capture_report(FILE *out) {
  if (!out) return;

  if (g_failed_mutex) {
    fprintf(out, "mutex_lock_failure mutex=0x%lx result=%d",
            (unsigned long)(uintptr_t)g_failed_mutex,
            g_failed_mutex_result);
    if (range_readable((uintptr_t)g_failed_mutex, sizeof(unsigned int) * 4)) {
      unsigned int words[4];
      memcpy(words, (const void *)g_failed_mutex, sizeof(words));
      fprintf(out, " data=%08x %08x %08x %08x",
              words[0], words[1], words[2], words[3]);
    }
    fprintf(out, "\n");
  }

  if (g_memstream_buffer_out && g_memstream_size_out &&
      range_readable((uintptr_t)g_memstream_buffer_out, sizeof(void *)) &&
      range_readable((uintptr_t)g_memstream_size_out, sizeof(size_t))) {
    char *buffer = *g_memstream_buffer_out;
    size_t size = *g_memstream_size_out;
    if (buffer && size) {
      size_t limit = size < 4096 ? size : 4096;
      if (range_readable((uintptr_t)buffer, limit)) {
        fprintf(out, "captured_memstream size=%zu:\n", size);
        for (size_t i = 0; i < limit; i++) {
          unsigned char c = (unsigned char)buffer[i];
          fputc(c >= 0x20 || c == '\n' || c == '\t' ? (int)c : '.', out);
        }
        fputc('\n', out);
      } else {
        fprintf(out, "memstream buffer unreadable ptr=0x%lx size=%zu\n",
                (unsigned long)(uintptr_t)buffer, size);
      }
    } else {
      fprintf(out, "memstream open but not finalized (buffer=%p size=%zu)\n",
              (void *)buffer, size);
    }
  }
}
