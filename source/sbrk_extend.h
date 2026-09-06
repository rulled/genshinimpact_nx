#ifndef SBRK_EXTEND_H
#define SBRK_EXTEND_H

#include <stddef.h>
#include <stdio.h>

/* Number of times _sbrk_r fell back to the sparse host pool and the total
 * bytes handed to the dlmalloc arena that way.  Dumped with crash reports. */
extern unsigned long g_sbrk_extension_count;
extern unsigned long g_sbrk_extension_bytes;
extern unsigned long g_sbrk_extension_denied;

/* Lock-free classification of a host pointer against the registered dlmalloc
 * arena extensions.  Consumers (memory_broker.c free/realloc/usable-size)
 * must route INTERIOR pointers to the native newlib allocator and treat BASE
 * as a no-op: dlmalloc never hands out the raw extension base, so freeing it
 * would be a caller bug. */
typedef enum {
  SBRK_EXTENSION_CLASS_NONE = 0,
  SBRK_EXTENSION_CLASS_INTERIOR = 1,
  SBRK_EXTENSION_CLASS_BASE = 2,
} SbrkExtensionClass;

SbrkExtensionClass sbrk_extension_class(const void *pointer);

void sbrk_extension_report(FILE *out);

#endif
