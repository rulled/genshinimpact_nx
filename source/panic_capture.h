#ifndef PANIC_CAPTURE_H
#define PANIC_CAPTURE_H

#include <stdio.h>

/* Appends any captured Rust panic diagnostics (failed mutex internals and the
 * most recent open_memstream payload) to the crash dump file.  Safe to call
 * from the abort()/exit() overrides. */
void panic_capture_report(FILE *out);

#endif
