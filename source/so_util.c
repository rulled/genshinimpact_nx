/* Android arm64 ELF loader and patcher.
 *
 * Copyright (C) 2021 Andy Nguyen, fgsfds
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#include <switch.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <elf.h>

#include "so_util.h"
#include "error.h"

/* Missing from devkitA64's elf.h. */
#ifndef DT_RELR
#define DT_RELR    36
#define DT_RELRSZ  35
#define DT_RELRENT 37
#endif
#define DT_ANDROID_RELR    0x6fffe000
#define DT_ANDROID_RELRSZ  0x6fffe001
#define DT_ANDROID_RELRENT 0x6fffe003
#define DT_ANDROID_RELA    0x60000011
#define DT_ANDROID_RELASZ  0x60000012
#define DT_ANDROID_RELAENT 0x60000013

/* Android's APS2 packed relocation stream uses groups of delta-encoded
 * SLEB128 values.  Genshin's monolithic libyuanshen.so stores its 1.6M
 * relative relocations this way instead of as an Elf64_Rela array. */
#define RELOCATION_GROUPED_BY_INFO_FLAG         1
#define RELOCATION_GROUPED_BY_OFFSET_DELTA_FLAG 2
#define RELOCATION_GROUPED_BY_ADDEND_FLAG       4
#define RELOCATION_GROUP_HAS_ADDEND_FLAG        8

static so_module *so_list = NULL;

void so_flush_caches(so_module *mod) {
  armDCacheFlush(mod->load_virtbase, mod->load_size);
  armICacheInvalidate(mod->load_virtbase, mod->load_size);
}

void so_free_temp(so_module *mod) {
  free(mod->so_base);
  mod->so_base = NULL;
}

void so_finalize(so_module *mod) {
  Result rc = 0;

  rc = svcMapProcessCodeMemory(envGetOwnProcessHandle(), (u64)mod->load_virtbase, (u64)mod->load_base, mod->load_size);
  if (R_FAILED(rc)) fatal_error("Error: svcMapProcessCodeMemory failed:\n%08x", rc);

  /* Set executable pages before writable pages. */
  const size_t num_pages = mod->load_size / 0x1000;
  uint8_t *is_x_page = calloc(num_pages, 1);
  if (!is_x_page) fatal_error("Error: out of memory in so_finalize");

  for (int i = 0; i < mod->phnum; i++) {
    const Elf64_Phdr *p = &mod->phdr[i];
    if (p->p_type != PT_LOAD || (p->p_flags & PF_X) != PF_X)
      continue;
    const size_t first = p->p_vaddr / 0x1000;
    const size_t last = (ALIGN_MEM(p->p_vaddr + p->p_memsz, 0x1000) / 0x1000) - 1;
    for (size_t pg = first; pg <= last && pg < num_pages; pg++)
      is_x_page[pg] = 1;
  }

  for (int want_x = 1; want_x >= 0; want_x--) {
    size_t pg = 0;
    while (pg < num_pages) {
      if (is_x_page[pg] != want_x) {
        pg++;
        continue;
      }
      size_t run_end = pg;
      while (run_end < num_pages && is_x_page[run_end] == want_x)
        run_end++;
      const u64 addr = (u64)mod->load_virtbase + pg * 0x1000;
      const u64 size = (run_end - pg) * 0x1000;
      rc = svcSetProcessMemoryPermission(envGetOwnProcessHandle(), addr, size, want_x ? Perm_Rx : Perm_Rw);
      if (R_FAILED(rc)) fatal_error("Error: could not map %u bytes of %s memory at %p:\n%08x", (u32)size, want_x ? "RX" : "RW", (void *)addr, rc);
      pg = run_end;
    }
  }

  free(is_x_page);

  /* Rebase symbol tables after donating the source code pages. */
  const uintptr_t delta = (uintptr_t)mod->load_virtbase - (uintptr_t)mod->load_base;
  if (mod->syms)      mod->syms      = (Elf64_Sym *)((uintptr_t)mod->syms + delta);
  if (mod->dynstrtab) mod->dynstrtab = (char *)((uintptr_t)mod->dynstrtab + delta);
  mod->finalized = 1;
}

int so_load(so_module *mod, const char *filename, void *base, size_t max_size) {
  int res = -1;
  FILE *fd = NULL;
  Elf64_Shdr *section_copy = NULL;
  uint8_t *io_buffer = NULL;

  memset(mod, 0, sizeof(*mod));
  strncpy(mod->name, filename, sizeof(mod->name) - 1);
  fd = fopen(filename, "rb");
  if (!fd) return -1;
  if (fseek(fd, 0, SEEK_END) != 0) goto out;
  long file_size = ftell(fd);
  if (file_size < (long)sizeof(Elf64_Ehdr) || fseek(fd, 0, SEEK_SET) != 0) goto out;
  mod->so_size = (size_t)file_size;

  Elf64_Ehdr header;
  if (fread(&header, 1, sizeof header, fd) != sizeof header ||
      memcmp(header.e_ident, ELFMAG, SELFMAG) != 0 ||
      header.e_ident[EI_CLASS] != ELFCLASS64 || header.e_machine != EM_AARCH64 ||
      header.e_phentsize != sizeof(Elf64_Phdr) || header.e_shentsize != sizeof(Elf64_Shdr) ||
      !header.e_phnum || header.e_phnum > SO_MAX_SEGMENTS * 2 || !header.e_shnum ||
      header.e_shstrndx >= header.e_shnum) {
    res = -4;
    goto out;
  }
  const size_t ph_size = (size_t)header.e_phnum * sizeof(Elf64_Phdr);
  const size_t sh_size = (size_t)header.e_shnum * sizeof(Elf64_Shdr);
  if (header.e_phoff > mod->so_size || ph_size > mod->so_size - header.e_phoff ||
      header.e_shoff > mod->so_size || sh_size > mod->so_size - header.e_shoff)
    goto out;

  section_copy = malloc(sh_size);
  if (!section_copy) { res = -2; goto out; }
  if (fseek(fd, (long)header.e_shoff, SEEK_SET) != 0 ||
      fread(section_copy, 1, sh_size, fd) != sh_size)
    goto out;
  const Elf64_Shdr *shstr = &section_copy[header.e_shstrndx];
  if (shstr->sh_offset > mod->so_size || shstr->sh_size > mod->so_size - shstr->sh_offset)
    goto out;

  /* Keep only ELF metadata.  Retaining the whole 346 MiB file alongside its
   * load image would double the boot-time peak, so the image is streamed. */
  const size_t meta_size = sizeof header + ph_size + sh_size + (size_t)shstr->sh_size + 64;
  uint8_t *meta = calloc(1, meta_size);
  if (!meta) { res = -2; goto out; }
  mod->so_base = meta;
  uintptr_t cursor = (uintptr_t)meta;
  mod->elf_hdr = (Elf64_Ehdr *)cursor; cursor += sizeof header;
  cursor = ALIGN_MEM(cursor, 8);
  mod->prog_hdr = (Elf64_Phdr *)cursor; cursor += ph_size;
  cursor = ALIGN_MEM(cursor, 8);
  mod->sec_hdr = (Elf64_Shdr *)cursor; cursor += sh_size;
  mod->shstrtab = (char *)cursor;
  memcpy(mod->elf_hdr, &header, sizeof header);
  memcpy(mod->sec_hdr, section_copy, sh_size);
  if (fseek(fd, (long)header.e_phoff, SEEK_SET) != 0 ||
      fread(mod->prog_hdr, 1, ph_size, fd) != ph_size ||
      fseek(fd, (long)shstr->sh_offset, SEEK_SET) != 0 ||
      fread(mod->shstrtab, 1, (size_t)shstr->sh_size, fd) != (size_t)shstr->sh_size)
    goto out;

  mod->phnum = header.e_phnum;
  memcpy(mod->phdr, mod->prog_hdr, ph_size);
  mod->load_size = 0;
  size_t load_align = 0x1000;
  for (int i = 0; i < header.e_phnum; i++) {
    const Elf64_Phdr *p = &mod->prog_hdr[i];
    if (p->p_type != PT_LOAD) continue;
    if (p->p_filesz > p->p_memsz || p->p_offset > mod->so_size ||
        p->p_filesz > mod->so_size - p->p_offset ||
        p->p_vaddr > SIZE_MAX - p->p_memsz)
      goto out;
    const size_t seg_end = (size_t)(p->p_vaddr + p->p_memsz);
    if (seg_end > mod->load_size) mod->load_size = seg_end;
    if (p->p_align > load_align) load_align = (size_t)p->p_align;
  }
  if ((load_align & (load_align - 1)) != 0) load_align = 0x1000;
  mod->load_size = ALIGN_MEM(mod->load_size, load_align);
  if (mod->load_size > max_size) { res = -3; goto out; }
  mod->load_base = base;
  if (!base || ((uintptr_t)base & (load_align - 1))) { res = -3; goto out; }
  memset(base, 0, mod->load_size);

  virtmemLock();
  mod->load_virtbase = virtmemFindCodeMemory(mod->load_size, load_align);
  mod->load_memrv = mod->load_virtbase
    ? virtmemAddReservation(mod->load_virtbase, mod->load_size) : NULL;
  virtmemUnlock();
  if (!mod->load_virtbase || !mod->load_memrv) { res = -2; goto out; }

  const size_t io_buffer_size = 1024 * 1024;
  io_buffer = malloc(io_buffer_size);
  if (!io_buffer) { res = -2; goto out; }
  for (int i = 0; i < header.e_phnum; i++) {
    Elf64_Phdr *p = &mod->prog_hdr[i];
    if (p->p_type == PT_LOAD && p->p_filesz) {
      if (fseek(fd, (long)p->p_offset, SEEK_SET) != 0) goto out;
      size_t left = (size_t)p->p_filesz;
      uint8_t *dst = (uint8_t *)base + p->p_vaddr;
      while (left) {
        const size_t chunk = left < io_buffer_size ? left : io_buffer_size;
        if (fread(io_buffer, 1, chunk, fd) != chunk) goto out;
        memcpy(dst, io_buffer, chunk); dst += chunk; left -= chunk;
      }
    }
    p->p_vaddr += (Elf64_Addr)mod->load_virtbase;
  }

  for (int i = 0; i < header.e_shnum; i++) {
    if ((size_t)mod->sec_hdr[i].sh_name >= (size_t)shstr->sh_size ||
        !memchr(mod->shstrtab + mod->sec_hdr[i].sh_name, 0,
                (size_t)shstr->sh_size - (size_t)mod->sec_hdr[i].sh_name))
      goto out;
    const char *name = mod->shstrtab + mod->sec_hdr[i].sh_name;
    if (strcmp(name, ".dynsym") == 0) {
      if (mod->sec_hdr[i].sh_addr > mod->load_size ||
          mod->sec_hdr[i].sh_size > mod->load_size - mod->sec_hdr[i].sh_addr ||
          mod->sec_hdr[i].sh_size % sizeof(Elf64_Sym))
        goto out;
      mod->syms = (Elf64_Sym *)((uintptr_t)base + mod->sec_hdr[i].sh_addr);
      mod->num_syms = (int)(mod->sec_hdr[i].sh_size / sizeof(Elf64_Sym));
    } else if (strcmp(name, ".dynstr") == 0) {
      if (mod->sec_hdr[i].sh_addr > mod->load_size ||
          mod->sec_hdr[i].sh_size > mod->load_size - mod->sec_hdr[i].sh_addr)
        goto out;
      mod->dynstrtab = (char *)((uintptr_t)base + mod->sec_hdr[i].sh_addr);
      mod->dynstr_size = (size_t)mod->sec_hdr[i].sh_size;
    }
  }
  if (!mod->syms || !mod->dynstrtab) { res = -2; goto out; }

  mod->next = NULL;
  if (!so_list) so_list = mod;
  else { so_module *tail = so_list; while (tail->next) tail = tail->next; tail->next = mod; }
  res = 0;

out:
  if (fd) fclose(fd);
  free(io_buffer);
  free(section_copy);
  if (res != 0) {
    if (mod->load_memrv) {
      virtmemLock(); virtmemRemoveReservation(mod->load_memrv); virtmemUnlock();
      mod->load_memrv = NULL;
    }
    free(mod->so_base); mod->so_base = NULL;
  }
  return res;
}

static Elf64_Xword so_dynamic_tag(so_module *mod, Elf64_Sxword tag) {
  for (int i = 0; i < mod->phnum; i++) {
    if (mod->phdr[i].p_type == PT_DYNAMIC) {
      if (mod->phdr[i].p_vaddr > mod->load_size ||
          mod->phdr[i].p_memsz > mod->load_size - mod->phdr[i].p_vaddr)
        return 0;
      const Elf64_Dyn *dyn = (const Elf64_Dyn *)((uintptr_t)mod->load_base + mod->phdr[i].p_vaddr);
      const size_t count = (size_t)mod->phdr[i].p_memsz / sizeof(*dyn);
      for (size_t j = 0; j < count && dyn[j].d_tag != DT_NULL; ++j)
        if (dyn[j].d_tag == tag)
          return dyn[j].d_un.d_val;
    }
  }
  return 0;
}

typedef struct {
  const uint8_t *cur;
  const uint8_t *end;
  int failed;
} SlebReader;

static int64_t sleb128_read(SlebReader *r) {
  uint64_t value = 0;
  unsigned shift = 0;
  uint8_t byte = 0;
  do {
    if (r->cur >= r->end || shift >= 64) {
      r->failed = 1;
      return 0;
    }
    byte = *r->cur++;
    value |= (uint64_t)(byte & 0x7f) << shift;
    shift += 7;
  } while (byte & 0x80);
  if (shift < 64 && (byte & 0x40))
    value |= (~0ULL) << shift;
  return (int64_t)value;
}

typedef int (*packed_rela_callback)(so_module *, const Elf64_Rela *, void *);

static int so_iterate_android_rela(so_module *mod, packed_rela_callback cb, void *opaque) {
  const Elf64_Xword packed_off = so_dynamic_tag(mod, DT_ANDROID_RELA);
  const Elf64_Xword packed_size = so_dynamic_tag(mod, DT_ANDROID_RELASZ);
  if (!packed_off || !packed_size)
    return 0;
  if (packed_off > mod->load_size || packed_size > mod->load_size - packed_off)
    return -2;

  SlebReader rd = {
    .cur = (const uint8_t *)mod->load_base + packed_off,
    .end = (const uint8_t *)mod->load_base + packed_off + packed_size,
    .failed = 0,
  };
  if ((size_t)(rd.end - rd.cur) < 4 || memcmp(rd.cur, "APS2", 4) != 0)
    return -2;
  rd.cur += 4;

  const int64_t relocation_count_signed = sleb128_read(&rd);
  if (rd.failed || relocation_count_signed < 0)
    return -2;
  const uint64_t relocation_count = (uint64_t)relocation_count_signed;
  uint64_t emitted = 0;
  Elf64_Rela rela = {0};
  rela.r_offset = (Elf64_Addr)sleb128_read(&rd);

  while (!rd.failed && emitted < relocation_count) {
    const int64_t group_size_signed = sleb128_read(&rd);
    const int64_t group_flags_signed = sleb128_read(&rd);
    if (rd.failed || group_size_signed <= 0 || group_flags_signed < 0 ||
        (uint64_t)group_size_signed > relocation_count - emitted) {
      rd.failed = 1;
      break;
    }
    const uint64_t group_size = (uint64_t)group_size_signed;
    const uint64_t flags = (uint64_t)group_flags_signed;
    const int64_t group_offset_delta =
      (flags & RELOCATION_GROUPED_BY_OFFSET_DELTA_FLAG) ? sleb128_read(&rd) : 0;
    const int64_t group_info =
      (flags & RELOCATION_GROUPED_BY_INFO_FLAG) ? sleb128_read(&rd) : 0;
    const int64_t group_addend_delta =
      ((flags & RELOCATION_GROUP_HAS_ADDEND_FLAG) &&
       (flags & RELOCATION_GROUPED_BY_ADDEND_FLAG)) ? sleb128_read(&rd) : 0;

    for (uint64_t i = 0; !rd.failed && i < group_size; i++, emitted++) {
      const int64_t off_delta = (flags & RELOCATION_GROUPED_BY_OFFSET_DELTA_FLAG)
        ? group_offset_delta : sleb128_read(&rd);
      rela.r_offset = (Elf64_Addr)((int64_t)rela.r_offset + off_delta);
      rela.r_info = (Elf64_Xword)((flags & RELOCATION_GROUPED_BY_INFO_FLAG)
        ? group_info : sleb128_read(&rd));
      if (flags & RELOCATION_GROUP_HAS_ADDEND_FLAG) {
        const int64_t addend_delta = (flags & RELOCATION_GROUPED_BY_ADDEND_FLAG)
          ? group_addend_delta : sleb128_read(&rd);
        rela.r_addend += addend_delta;
      } else {
        rela.r_addend = 0;
      }
      if (rd.failed || rela.r_offset > mod->load_size - sizeof(uintptr_t)) {
        rd.failed = 1;
        break;
      }
      if (cb(mod, &rela, opaque) != 0)
        return -1;
    }
  }

  if (rd.failed || emitted != relocation_count || rd.cur != rd.end)
    return -2;
  return 1;
}

static int relocate_one(so_module *mod, const Elf64_Rela *rel, void *opaque) {
  (void)opaque;
  uintptr_t *ptr = (uintptr_t *)((uintptr_t)mod->load_base + rel->r_offset);
  Elf64_Sym *sym = &mod->syms[ELF64_R_SYM(rel->r_info)];
  switch (ELF64_R_TYPE(rel->r_info)) {
    case R_AARCH64_NONE:
      break;
    case R_AARCH64_ABS64:
      *ptr = (sym->st_shndx == SHN_UNDEF)
        ? (uintptr_t)rel->r_addend
        : (uintptr_t)mod->load_virtbase + sym->st_value + rel->r_addend;
      break;
    case R_AARCH64_RELATIVE:
      *ptr = (uintptr_t)mod->load_virtbase + rel->r_addend;
      break;
    case R_AARCH64_GLOB_DAT:
    case R_AARCH64_JUMP_SLOT:
      if (sym->st_shndx != SHN_UNDEF)
        *ptr = (uintptr_t)mod->load_virtbase + sym->st_value + rel->r_addend;
      break;
    default:
      fatal_error("Error: unknown relocation type in %s:\n%x", mod->name,
                  ELF64_R_TYPE(rel->r_info));
  }
  return 0;
}

/* Apply packed relative relocations. */
static void so_process_relr(so_module *mod, const Elf64_Xword *relr, size_t relrsz) {
  uintptr_t where = 0;
  const size_t count = relrsz / sizeof(Elf64_Xword);
  for (size_t i = 0; i < count; i++) {
    const Elf64_Xword entry = relr[i];
    if ((entry & 1) == 0) {
      where = (uintptr_t)entry;
      *(uint64_t *)((uintptr_t)mod->load_base + where) += (uint64_t)mod->load_virtbase;
      where += 8;
    } else {
      for (int bit = 1; bit < 64; bit++) {
        if (entry & (1ull << bit))
          *(uint64_t *)((uintptr_t)mod->load_base + where + (bit - 1) * 8) += (uint64_t)mod->load_virtbase;
      }
      where += 63 * 8;
    }
  }
}

int so_relocate(so_module *mod) {
  for (int i = 0; i < mod->elf_hdr->e_shnum; i++) {
    char *sh_name = mod->shstrtab + mod->sec_hdr[i].sh_name;
    /* SHT_ANDROID_RELA also calls itself .rela.dyn but contains APS2 bytes. */
    if (mod->sec_hdr[i].sh_type == SHT_RELA &&
        (strcmp(sh_name, ".rela.dyn") == 0 || strcmp(sh_name, ".rela.plt") == 0)) {
      Elf64_Rela *rels = (Elf64_Rela *)((uintptr_t)mod->load_base + mod->sec_hdr[i].sh_addr);
      for (int j = 0; j < mod->sec_hdr[i].sh_size / sizeof(Elf64_Rela); j++) {
        relocate_one(mod, &rels[j], NULL);
      }
    }
  }

  if (so_iterate_android_rela(mod, relocate_one, NULL) < 0)
    fatal_error("Error: corrupt Android relocation stream in %s.", mod->name);

  Elf64_Xword relr_off = so_dynamic_tag(mod, DT_RELR);
  Elf64_Xword relr_size = so_dynamic_tag(mod, DT_RELRSZ);
  if (!relr_off) {
    relr_off = so_dynamic_tag(mod, DT_ANDROID_RELR);
    relr_size = so_dynamic_tag(mod, DT_ANDROID_RELRSZ);
  }
  if (relr_off && relr_size) {
    so_process_relr(mod, (const Elf64_Xword *)((uintptr_t)mod->load_base + relr_off), relr_size);
  }

  return 0;
}

static uintptr_t so_lookup_export(so_module *mod, const char *name) {
  for (int i = 0; i < mod->num_syms; i++) {
    if (mod->syms[i].st_shndx == SHN_UNDEF)
      continue;
    if (ELF64_ST_BIND(mod->syms[i].st_info) == STB_LOCAL)
      continue;
    if ((size_t)mod->syms[i].st_name >= mod->dynstr_size)
      continue;
    const char *sname = mod->dynstrtab + mod->syms[i].st_name;
    if (!memchr(sname, 0, mod->dynstr_size - (size_t)mod->syms[i].st_name))
      continue;
    if (sname[0] == name[0] && strcmp(sname, name) == 0)
      return (uintptr_t)mod->load_virtbase + mod->syms[i].st_value;
  }
  return 0;
}

void *so_resolve_in_module(so_module *mod, const char *name) {
  if (!mod || !name || !so_is_loaded_module(mod))
    return NULL;
  const uintptr_t addr = so_lookup_export(mod, name);
  return addr ? (void *)addr : NULL;
}

static const char *so_basename(const char *path) {
  if (!path) return "";
  const char *slash = strrchr(path, '/');
  return slash ? slash + 1 : path;
}

so_module *so_find_module(const char *basename) {
  if (!basename || !*basename) return NULL;
  basename = so_basename(basename);
  for (so_module *mod = so_list; mod; mod = mod->next)
    if (strcmp(so_basename(mod->name), basename) == 0)
      return mod;
  return NULL;
}

int so_is_loaded_module(const so_module *wanted) {
  if (!wanted) return 0;
  for (const so_module *mod = so_list; mod; mod = mod->next)
    if (mod == wanted) return 1;
  return 0;
}

void so_discard_unfinalized(so_module *mod) {
  if (!mod || mod->finalized) return;
  so_module **link = &so_list;
  while (*link && *link != mod) link = &(*link)->next;
  if (*link == mod) *link = mod->next;
  if (mod->load_memrv) {
    virtmemLock();
    virtmemRemoveReservation((VirtmemReservation *)mod->load_memrv);
    virtmemUnlock();
  }
  free(mod->so_base);
  memset(mod, 0, sizeof(*mod));
}

int so_get_needed(so_module *mod, size_t index, const char **name_out) {
  if (name_out) *name_out = NULL;
  if (!mod || !mod->dynstrtab || !mod->dynstr_size) return -1;
  size_t found = 0;
  for (int i = 0; i < mod->phnum; ++i) {
    const Elf64_Phdr *ph = &mod->phdr[i];
    if (ph->p_type != PT_DYNAMIC || ph->p_vaddr > mod->load_size ||
        ph->p_memsz > mod->load_size - ph->p_vaddr)
      continue;
    const Elf64_Dyn *dyn = (const Elf64_Dyn *)
      ((uintptr_t)mod->load_base + ph->p_vaddr);
    const size_t count = (size_t)ph->p_memsz / sizeof(*dyn);
    for (size_t j = 0; j < count && dyn[j].d_tag != DT_NULL; ++j) {
      if (dyn[j].d_tag != DT_NEEDED) continue;
      if (found++ != index) continue;
      const size_t off = (size_t)dyn[j].d_un.d_val;
      if (off >= mod->dynstr_size ||
          !memchr(mod->dynstrtab + off, 0, mod->dynstr_size - off))
        return -1;
      if (name_out) *name_out = mod->dynstrtab + off;
      return 1;
    }
  }
  return 0;
}

void *so_resolve_external_except(const char *name, const so_module *skip) {
  if (!name)
    return NULL;
  for (so_module *m = so_list; m; m = m->next) {
    if (m == skip) continue;
    const uintptr_t addr = so_lookup_export(m, name);
    if (addr)
      return (void *)addr;
  }
  return NULL;
}

void *so_resolve_external(const char *name) {
  return so_resolve_external_except(name, NULL);
}

/* Emit loaded modules in /proc/self/maps format. */
int so_dump_maps(char *buf, size_t cap) {
  if (!buf || cap == 0) return 0;
  size_t off = 0;
  for (so_module *m = so_list; m; m = m->next) {
    uintptr_t s = (uintptr_t)m->load_virtbase;
    uintptr_t e = s + m->load_size;
    int n = snprintf(buf + off, cap - off,
                     "%012lx-%012lx r-xp 00000000 00:00 0          %s\n",
                     (unsigned long)s, (unsigned long)e, m->name[0] ? m->name : "module");
    if (n < 0 || (size_t)n >= cap - off) break;
    off += (size_t)n;
  }
  buf[off < cap ? off : cap - 1] = '\0';
  return (int)off;
}

static uintptr_t so_resolve_symbol(so_module *mod, DynLibFunction *funcs, int num_funcs, const char *name) {
  /* Prefer host shims over guest exports. */
  for (int k = 0; k < num_funcs; k++) {
    if (strcmp(name, funcs[k].symbol) == 0)
      return funcs[k].func;
  }

  for (so_module *m = so_list; m; m = m->next) {
    if (m == mod)
      continue;
    const uintptr_t addr = so_lookup_export(m, name);
    if (addr)
      return addr;
  }

  return 0;
}

typedef struct {
  DynLibFunction *funcs;
  int num_funcs;
} ResolveContext;

typedef struct {
  so_symbol_resolver resolver;
  void *opaque;
  char *error;
  size_t error_cap;
} ImportResolveContext;

typedef struct {
  so_symbol_resolver resolver;
  void *opaque;
  char *error;
  size_t error_cap;
} PreflightContext;

static int preflight_fail(PreflightContext *ctx, const char *fmt,
                          const char *text, unsigned value) {
  if (ctx->error && ctx->error_cap) {
    if (text) snprintf(ctx->error, ctx->error_cap, fmt, text);
    else snprintf(ctx->error, ctx->error_cap, fmt, value);
  }
  return -1;
}

static int preflight_one(so_module *mod, const Elf64_Rela *rel, void *opaque) {
  PreflightContext *ctx = opaque;
  const unsigned type = ELF64_R_TYPE(rel->r_info);
  if (type != R_AARCH64_NONE &&
      (rel->r_offset > mod->load_size ||
       sizeof(uintptr_t) > mod->load_size - rel->r_offset))
    return preflight_fail(ctx, "relocation target is outside image (%u)", NULL,
                          (unsigned)rel->r_offset);
  switch (type) {
    case R_AARCH64_NONE:
    case R_AARCH64_RELATIVE:
      return 0;
    case R_AARCH64_ABS64:
    case R_AARCH64_GLOB_DAT:
    case R_AARCH64_JUMP_SLOT:
      break;
    default:
      return preflight_fail(ctx, "unsupported AArch64 relocation %u", NULL, type);
  }

  const size_t sym_index = (size_t)ELF64_R_SYM(rel->r_info);
  if (sym_index >= (size_t)mod->num_syms)
    return preflight_fail(ctx, "invalid dynamic symbol index %u", NULL,
                          (unsigned)sym_index);
  const Elf64_Sym *sym = &mod->syms[sym_index];
  if (sym->st_shndx != SHN_UNDEF)
    return 0;
  if ((size_t)sym->st_name >= mod->dynstr_size)
    return preflight_fail(ctx, "invalid dynamic string offset %u", NULL,
                          (unsigned)sym->st_name);
  const char *name = mod->dynstrtab + sym->st_name;
  if (!memchr(name, 0, mod->dynstr_size - (size_t)sym->st_name))
    return preflight_fail(ctx, "unterminated import name at offset %u", NULL,
                          (unsigned)sym->st_name);
  if (ctx->resolver && ctx->resolver(name, ctx->opaque))
    return 0;
  if (ELF64_ST_BIND(sym->st_info) == STB_WEAK)
    return 0;
  if (ctx->error && ctx->error_cap)
    snprintf(ctx->error, ctx->error_cap, "unresolved import '%s'", name);
  return -1;
}

int so_preflight_imports(so_module *mod, so_symbol_resolver resolver,
                         void *opaque, char *error, size_t error_cap) {
  if (error && error_cap) error[0] = 0;
  if (!mod || !mod->elf_hdr || !mod->sec_hdr || !mod->shstrtab ||
      !mod->syms || !mod->dynstrtab) {
    if (error && error_cap) snprintf(error, error_cap, "incomplete ELF metadata");
    return -1;
  }
  PreflightContext ctx = { resolver, opaque, error, error_cap };
  for (int i = 0; i < mod->elf_hdr->e_shnum; ++i) {
    const Elf64_Shdr *section = &mod->sec_hdr[i];
    const char *sh_name = mod->shstrtab + section->sh_name;
    if (section->sh_type != SHT_RELA ||
        (strcmp(sh_name, ".rela.dyn") != 0 && strcmp(sh_name, ".rela.plt") != 0))
      continue;
    if (section->sh_addr > mod->load_size ||
        section->sh_size > mod->load_size - section->sh_addr ||
        section->sh_size % sizeof(Elf64_Rela)) {
      if (error && error_cap) snprintf(error, error_cap, "invalid %s bounds", sh_name);
      return -1;
    }
    const Elf64_Rela *rels = (const Elf64_Rela *)
      ((uintptr_t)mod->load_base + section->sh_addr);
    const size_t count = (size_t)section->sh_size / sizeof(*rels);
    for (size_t j = 0; j < count; ++j)
      if (preflight_one(mod, &rels[j], &ctx) != 0)
        return -1;
  }

  const int packed = so_iterate_android_rela(mod, preflight_one, &ctx);
  if (packed < 0) {
    if (packed == -2 && error && error_cap)
      snprintf(error, error_cap, "invalid APS2 relocation stream");
    return -1;
  }

  Elf64_Xword relr_off = so_dynamic_tag(mod, DT_RELR);
  Elf64_Xword relr_size = so_dynamic_tag(mod, DT_RELRSZ);
  if (!relr_off) {
    relr_off = so_dynamic_tag(mod, DT_ANDROID_RELR);
    relr_size = so_dynamic_tag(mod, DT_ANDROID_RELRSZ);
  }
  if (relr_off && (relr_off > mod->load_size ||
                   relr_size > mod->load_size - relr_off ||
                   relr_size % sizeof(Elf64_Xword))) {
    if (error && error_cap) snprintf(error, error_cap, "invalid RELR bounds");
    return -1;
  }
  if (relr_off) {
    const Elf64_Xword *relr = (const Elf64_Xword *)
      ((uintptr_t)mod->load_base + relr_off);
    const size_t count = (size_t)relr_size / sizeof(*relr);
    uintptr_t where = 0;
    for (size_t i = 0; i < count; ++i) {
      const Elf64_Xword entry = relr[i];
      if ((entry & 1) == 0) {
        where = (uintptr_t)entry;
        if (where > mod->load_size || sizeof(uint64_t) > mod->load_size - where) {
          if (error && error_cap) snprintf(error, error_cap, "RELR target is outside image");
          return -1;
        }
        where += sizeof(uint64_t);
      } else {
        for (unsigned bit = 1; bit < 64; ++bit) {
          if (!(entry & (1ull << bit))) continue;
          const uintptr_t target = where + (uintptr_t)(bit - 1) * sizeof(uint64_t);
          if (target < where || target > mod->load_size ||
              sizeof(uint64_t) > mod->load_size - target) {
            if (error && error_cap) snprintf(error, error_cap, "RELR bitmap target is outside image");
            return -1;
          }
        }
        if (where > UINTPTR_MAX - 63u * sizeof(uint64_t)) {
          if (error && error_cap) snprintf(error, error_cap, "RELR cursor overflow");
          return -1;
        }
        where += 63u * sizeof(uint64_t);
      }
    }
  }
  return 0;
}

static int resolve_one(so_module *mod, const Elf64_Rela *rel, void *opaque) {
  const int type = ELF64_R_TYPE(rel->r_info);
  if (type != R_AARCH64_ABS64 && type != R_AARCH64_GLOB_DAT &&
      type != R_AARCH64_JUMP_SLOT)
    return 0;
  Elf64_Sym *sym = &mod->syms[ELF64_R_SYM(rel->r_info)];
  if (sym->st_shndx != SHN_UNDEF)
    return 0;
  ResolveContext *ctx = opaque;
  const char *name = mod->dynstrtab + sym->st_name;
  const uintptr_t addr = so_resolve_symbol(mod, ctx->funcs, ctx->num_funcs, name);
  if (!addr && ELF64_ST_BIND(sym->st_info) == STB_WEAK) {
    uintptr_t *ptr = (uintptr_t *)((uintptr_t)mod->load_base + rel->r_offset);
    *ptr = 0;
    return 0;
  }
  if (!addr)
    fatal_error("Unsupported import in %s:\n%s", mod->name, name);
  uintptr_t *ptr = (uintptr_t *)((uintptr_t)mod->load_base + rel->r_offset);
  *ptr = addr + rel->r_addend;
  return 0;
}

static int resolve_import_one(so_module *mod, const Elf64_Rela *rel,
                              void *opaque) {
  ImportResolveContext *ctx = opaque;
  const unsigned type = ELF64_R_TYPE(rel->r_info);
  if (type != R_AARCH64_ABS64 && type != R_AARCH64_GLOB_DAT &&
      type != R_AARCH64_JUMP_SLOT)
    return 0;

  const size_t sym_index = (size_t)ELF64_R_SYM(rel->r_info);
  if (sym_index >= (size_t)mod->num_syms) {
    if (ctx->error && ctx->error_cap)
      snprintf(ctx->error, ctx->error_cap,
               "invalid dynamic symbol index %u", (unsigned)sym_index);
    return -1;
  }
  const Elf64_Sym *sym = &mod->syms[sym_index];
  if (sym->st_shndx != SHN_UNDEF)
    return 0;
  if ((size_t)sym->st_name >= mod->dynstr_size) {
    if (ctx->error && ctx->error_cap)
      snprintf(ctx->error, ctx->error_cap,
               "invalid dynamic string offset %u", (unsigned)sym->st_name);
    return -1;
  }
  const char *name = mod->dynstrtab + sym->st_name;
  if (!memchr(name, 0, mod->dynstr_size - (size_t)sym->st_name)) {
    if (ctx->error && ctx->error_cap)
      snprintf(ctx->error, ctx->error_cap,
               "unterminated import name at offset %u", (unsigned)sym->st_name);
    return -1;
  }

  const uintptr_t addr = ctx->resolver
    ? ctx->resolver(name, ctx->opaque) : 0;
  uintptr_t *ptr = (uintptr_t *)((uintptr_t)mod->load_base + rel->r_offset);
  if (addr) {
    *ptr = addr + rel->r_addend;
    return 0;
  }
  if (ELF64_ST_BIND(sym->st_info) == STB_WEAK) {
    *ptr = 0;
    return 0;
  }
  if (ctx->error && ctx->error_cap)
    snprintf(ctx->error, ctx->error_cap, "unresolved import '%s'", name);
  return -1;
}

int so_resolve_imports(so_module *mod, so_symbol_resolver resolver,
                       void *opaque, char *error, size_t error_cap) {
  if (error && error_cap) error[0] = 0;
  if (!mod || !mod->elf_hdr || !mod->sec_hdr || !mod->shstrtab ||
      !mod->syms || !mod->dynstrtab) {
    if (error && error_cap) snprintf(error, error_cap, "incomplete ELF metadata");
    return -1;
  }

  ImportResolveContext ctx = { resolver, opaque, error, error_cap };
  for (int i = 0; i < mod->elf_hdr->e_shnum; ++i) {
    const Elf64_Shdr *section = &mod->sec_hdr[i];
    const char *sh_name = mod->shstrtab + section->sh_name;
    if (section->sh_type != SHT_RELA ||
        (strcmp(sh_name, ".rela.dyn") != 0 && strcmp(sh_name, ".rela.plt") != 0))
      continue;
    if (section->sh_addr > mod->load_size ||
        section->sh_size > mod->load_size - section->sh_addr ||
        section->sh_size % sizeof(Elf64_Rela)) {
      if (error && error_cap) snprintf(error, error_cap, "invalid %s bounds", sh_name);
      return -1;
    }
    const Elf64_Rela *rels = (const Elf64_Rela *)
      ((uintptr_t)mod->load_base + section->sh_addr);
    const size_t count = (size_t)section->sh_size / sizeof(*rels);
    for (size_t j = 0; j < count; ++j) {
      const unsigned type = ELF64_R_TYPE(rels[j].r_info);
      if (type != R_AARCH64_NONE &&
          (rels[j].r_offset > mod->load_size ||
           sizeof(uintptr_t) > mod->load_size - rels[j].r_offset)) {
        if (error && error_cap)
          snprintf(error, error_cap, "relocation target is outside image (%u)",
                   (unsigned)rels[j].r_offset);
        return -1;
      }
      if (resolve_import_one(mod, &rels[j], &ctx) != 0)
        return -1;
    }
  }

  const int packed = so_iterate_android_rela(mod, resolve_import_one, &ctx);
  if (packed < 0) {
    if (packed == -2 && error && error_cap)
      snprintf(error, error_cap, "invalid APS2 relocation stream");
    return -1;
  }
  return 0;
}

void so_resolve(so_module *mod, DynLibFunction *funcs, int num_funcs) {
  for (int i = 0; i < mod->elf_hdr->e_shnum; i++) {
    char *sh_name = mod->shstrtab + mod->sec_hdr[i].sh_name;
    if (mod->sec_hdr[i].sh_type == SHT_RELA &&
        (strcmp(sh_name, ".rela.dyn") == 0 || strcmp(sh_name, ".rela.plt") == 0)) {
      Elf64_Rela *rels = (Elf64_Rela *)((uintptr_t)mod->load_base + mod->sec_hdr[i].sh_addr);
      for (int j = 0; j < mod->sec_hdr[i].sh_size / sizeof(Elf64_Rela); j++) {
        uintptr_t *ptr = (uintptr_t *)((uintptr_t)mod->load_base + rels[j].r_offset);
        Elf64_Sym *sym = &mod->syms[ELF64_R_SYM(rels[j].r_info)];

        int type = ELF64_R_TYPE(rels[j].r_info);
        switch (type) {
          case R_AARCH64_ABS64:
          case R_AARCH64_GLOB_DAT:
          case R_AARCH64_JUMP_SLOT:
          {
            if (sym->st_shndx == SHN_UNDEF) {
              char *name = mod->dynstrtab + sym->st_name;
              uintptr_t addr = so_resolve_symbol(mod, funcs, num_funcs, name);
              if (addr) {
                *ptr = addr + rels[j].r_addend;
              } else if (ELF64_ST_BIND(sym->st_info) == STB_WEAK) {
                *ptr = 0;
              } else {
                fatal_error("Unsupported import in %s:\n%s", mod->name, name);
              }
            }

            break;
          }

          default:
            break;
        }
      }
    }
  }

  ResolveContext ctx = { funcs, num_funcs };
  if (so_iterate_android_rela(mod, resolve_one, &ctx) < 0)
    fatal_error("Error: corrupt Android relocation stream in %s.", mod->name);
}

void so_execute_init_array(so_module *mod) {
  for (int i = 0; i < mod->elf_hdr->e_shnum; i++) {
    char *sh_name = mod->shstrtab + mod->sec_hdr[i].sh_name;
    if (strcmp(sh_name, ".init_array") == 0) {
      int (** init_array)() = (void *)((uintptr_t)mod->load_virtbase + mod->sec_hdr[i].sh_addr);
      int n = (int)(mod->sec_hdr[i].sh_size / 8);
      for (int j = 0; j < n; j++) {
        uintptr_t function = (uintptr_t)init_array[j];
        if (function != 0) ((int (*)())function)();
      }
    }
  }
}

uintptr_t so_try_find_addr_rx(so_module *mod, const char *symbol) {
  for (int i = 0; i < mod->num_syms; i++) {
    char *name = mod->dynstrtab + mod->syms[i].st_name;
    if (strcmp(name, symbol) == 0)
      return (uintptr_t)mod->load_virtbase + mod->syms[i].st_value;
  }
  return 0;
}

/* Bionic dl_phdr_info layout. */
struct so_dl_phdr_info {
  Elf64_Addr dlpi_addr;
  const char *dlpi_name;
  const Elf64_Phdr *dlpi_phdr;
  Elf64_Half dlpi_phnum;
};

/* Android arm64's Dl_info is four consecutive pointers.  Keep a private ABI
 * definition instead of including the host's dlfcn.h: Unity consumes the
 * second field directly as an ELF image base during initJni. */
typedef struct {
  const char *dli_fname;
  void *dli_fbase;
  const char *dli_sname;
  void *dli_saddr;
} SoDlInfo;

_Static_assert(sizeof(SoDlInfo) == 32, "Android arm64 Dl_info size");
_Static_assert(offsetof(SoDlInfo, dli_fbase) == 8,
               "Android arm64 Dl_info dli_fbase offset");

int so_dladdr(const void *address, void *info_) {
  SoDlInfo *info = (SoDlInfo *)info_;
  if (!info) return 0;
  memset(info, 0, sizeof(*info));
  if (!address) return 0;

  const uintptr_t at = (uintptr_t)address;
  for (so_module *mod = so_list; mod; mod = mod->next) {
    const uintptr_t base = (uintptr_t)mod->load_virtbase;
    if (!base || !mod->load_size || at < base || at - base >= mod->load_size)
      continue;

    info->dli_fname = mod->name;
    info->dli_fbase = mod->load_virtbase;

    /* dladdr succeeds for an image even when no public symbol covers the
     * address.  Supplying the nearest defined symbol also helps libunwind. */
    const uintptr_t offset = at - base;
    uintptr_t nearest = 0;
    int have_nearest = 0;
    if (mod->syms && mod->dynstrtab && mod->dynstr_size) {
      for (int i = 0; i < mod->num_syms; ++i) {
        const Elf64_Sym *sym = &mod->syms[i];
        const unsigned type = ELF64_ST_TYPE(sym->st_info);
        if (sym->st_shndx == SHN_UNDEF ||
            (type != STT_NOTYPE && type != STT_OBJECT && type != STT_FUNC) ||
            sym->st_value >= mod->load_size || sym->st_value > offset ||
            (size_t)sym->st_name >= mod->dynstr_size)
          continue;
        const char *name = mod->dynstrtab + sym->st_name;
        if (!name[0] ||
            !memchr(name, 0, mod->dynstr_size - (size_t)sym->st_name) ||
            (have_nearest && sym->st_value < nearest))
          continue;
        nearest = (uintptr_t)sym->st_value;
        have_nearest = 1;
        info->dli_sname = name;
        info->dli_saddr = (void *)(base + nearest);
      }
    }
    return 1;
  }
  return 0;
}

int so_dl_iterate_phdr(int (*callback)(void *info, size_t size, void *data), void *data) {
  int ret = 0;
  for (so_module *mod = so_list; mod; mod = mod->next) {
    struct so_dl_phdr_info info;
    info.dlpi_addr = (Elf64_Addr)mod->load_virtbase;
    info.dlpi_name = mod->name;
    info.dlpi_phdr = mod->phdr; // link-time vaddrs + dlpi_addr = runtime
    info.dlpi_phnum = mod->phnum;
    ret = callback(&info, sizeof(info), data);
    if (ret)
      break;
  }
  return ret;
}

/* Patch RX code through a temporary writable alias. */
int so_patch_code(void *dst, const void *src, size_t len) {
  uintptr_t start = (uintptr_t)dst & ~0xFFFull;
  size_t maplen = ((((uintptr_t)dst + len) - start) + 0xFFF) & ~0xFFFull;
  size_t off = (uintptr_t)dst - start;
  virtmemLock();
  void *alias = virtmemFindAslr(maplen, 0);
  VirtmemReservation *rv = alias ? virtmemAddReservation(alias, maplen) : NULL;
  virtmemUnlock();
  if (!alias) return -1;
  Result rc = svcMapProcessMemory(alias, envGetOwnProcessHandle(), (u64)start, maplen);
  if (R_FAILED(rc)) {
    virtmemLock(); if (rv) virtmemRemoveReservation(rv); virtmemUnlock();
    return -2;
  }
  memcpy((uint8_t *)alias + off, src, len);
  svcUnmapProcessMemory(alias, envGetOwnProcessHandle(), (u64)start, maplen);
  virtmemLock(); if (rv) virtmemRemoveReservation(rv); virtmemUnlock();
  armDCacheFlush(dst, len);
  armICacheInvalidate(dst, len);
  return 0;
}
