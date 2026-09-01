#ifndef SBRK_EXTEND_H
#define SBRK_EXTEND_H

#include <stdio.h>

/* Number of times _sbrk_r fell back to the sparse host pool and the total
 * bytes handed to the dlmalloc arena that way.  Dumped with crash reports. */
extern unsigned long g_sbrk_extension_count;
extern unsigned long g_sbrk_extension_bytes;
extern unsigned long g_sbrk_extension_denied;

void sbrk_extension_report(FILE *out);

#endif
