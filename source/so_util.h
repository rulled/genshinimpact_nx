/* so_util.h -- utils to load and hook .so modules
 *
 * Copyright (C) 2021 Andy Nguyen, fgsfds
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#ifndef __SO_UTIL_H__
#define __SO_UTIL_H__

#include <stdint.h>
#include <stddef.h>
#include <elf.h>

/* Cast the mask to the width of x.  A 32-bit unsigned alignment otherwise
 * clears the upper half of high Switch virtual addresses before subtraction. */
#define ALIGN_MEM(x, align) \
  (((x) + (__typeof__(x))(align) - (__typeof__(x))1) & \
   ~((__typeof__(x))(align) - (__typeof__(x))1))

#define SO_MAX_SEGMENTS 8

typedef struct {
  char *symbol;
  uintptr_t func;
} DynLibFunction;

typedef struct so_module {
  struct so_module *next;
  char name[256];

  // entire LOAD zone
  void *load_base, *load_virtbase;
  size_t load_size;
  void *load_memrv; // VirtmemReservation *
  int finalized;

  // copy of the unmodified program headers (link-time vaddrs),
  // needed for dl_iterate_phdr / exception unwinding
  Elf64_Phdr phdr[SO_MAX_SEGMENTS * 2];
  int phnum;

  // temporary file image
  void *so_base;
  size_t so_size;

  Elf64_Ehdr *elf_hdr;
  Elf64_Phdr *prog_hdr;
  Elf64_Shdr *sec_hdr;
  Elf64_Sym *syms;
  int num_syms;
  char *shstrtab;
  char *dynstrtab;
  size_t dynstr_size;
} so_module;

typedef uintptr_t (*so_symbol_resolver)(const char *name, void *opaque);

void so_flush_caches(so_module *mod);
void so_free_temp(so_module *mod);
int so_load(so_module *mod, const char *filename, void *base, size_t max_size);
int so_relocate(so_module *mod);
void so_resolve(so_module *mod, DynLibFunction *funcs, int num_funcs);
void so_execute_init_array(so_module *mod);
/* Return zero when an executable symbol is absent. */
uintptr_t so_try_find_addr_rx(so_module *mod, const char *symbol);
// dlsym() backing: search every loaded module's exports for `name`.
void *so_resolve_external(const char *name);
void *so_resolve_external_except(const char *name, const so_module *skip);
/* Resolve one export in exactly one module. */
void *so_resolve_in_module(so_module *mod, const char *name);
/* Locate/validate stable so_module handles already registered with the loader. */
so_module *so_find_module(const char *basename);
int so_is_loaded_module(const so_module *mod);
/* Inspect DT_NEEDED entries while the module's temporary metadata is alive. */
int so_get_needed(so_module *mod, size_t index, const char **name_out);
/* Validate every imported relocation without mutating it or aborting. */
int so_preflight_imports(so_module *mod, so_symbol_resolver resolver,
                         void *opaque, char *error, size_t error_cap);
/* Apply undefined-symbol relocations through the same resolver contract used
 * by so_preflight_imports().  This is the scoped alternative to so_resolve();
 * callers control exactly which guest modules are visible. */
int so_resolve_imports(so_module *mod, so_symbol_resolver resolver,
                       void *opaque, char *error, size_t error_cap);
/* Remove a successfully loaded module which has not been finalized. */
void so_discard_unfinalized(so_module *mod);
int so_dump_maps(char *buf, size_t cap);
void so_finalize(so_module *mod);

// dl_iterate_phdr() replacement operating on all loaded modules;
// required by the libunwind embedded in libc++_shared.so
int so_dl_iterate_phdr(int (*callback)(void *info, size_t size, void *data), void *data);

/* Bionic dladdr() replacement operating on the guest module registry.  The
 * output pointer is opaque so the host libc cannot change the Android ABI. */
int so_dladdr(const void *address, void *info);

int so_patch_code(void *dst, const void *src, size_t len);

#endif
