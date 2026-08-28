/* util.c -- misc utility functions
 *
 * Copyright (C) 2021 fgsfds, Andy Nguyen
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#include <stdint.h>
#include <string.h>

#include "util.h"

/* devkitA64's C TLS runtime is rooted through TPIDRRO_EL0, independently of
 * the Android-compatible TPIDR_EL0 value below.  These bookkeeping values
 * therefore remain reachable while guest code owns TPIDR_EL0. */
static _Thread_local uintptr_t saved_host_thread_pointer;
static _Thread_local uintptr_t active_bionic_thread_pointer;

static inline uintptr_t read_thread_pointer(void) {
  uintptr_t value;
  __asm__ volatile("mrs %0, s3_3_c13_c0_2" : "=r"(value));
  return value;
}

static inline void write_thread_pointer(uintptr_t value) {
  __asm__ volatile("msr s3_3_c13_c0_2, %0" : : "r"(value) : "memory");
}

void install_bionic_tls(void *buf) {
  if (!buf) return;
  const uintptr_t current = read_thread_pointer();
  const uintptr_t guest = (uintptr_t)((uint8_t *)buf + 0x200);
  /* Preserve the original host value if a caller refreshes or replaces an
   * already-installed guest block on the same host thread. */
  if (!active_bionic_thread_pointer || current != active_bionic_thread_pointer)
    saved_host_thread_pointer = current;
  memset(buf, 0, BIONIC_TLS_SIZE);
  active_bionic_thread_pointer = guest;
  write_thread_pointer(guest);
}

int restore_bionic_tls(void) {
  const uintptr_t guest = active_bionic_thread_pointer;
  const uintptr_t host = saved_host_thread_pointer;
  if (!guest || read_thread_pointer() != guest) return 0;
  active_bionic_thread_pointer = 0;
  saved_host_thread_pointer = 0;
  write_thread_pointer(host);
  return 1;
}
