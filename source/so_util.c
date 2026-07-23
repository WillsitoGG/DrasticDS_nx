/* so_util.c -- utils to load and hook .so modules
 *
 * Copyright (C) 2021 Andy Nguyen, fgsfds
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 *
 * Supports loading multiple modules (libopenal.so as C++ runtime donor +
 * libGame.so), an arbitrary number of PT_LOAD segments, RELR/ANDROID_RELR
 * packed relocations and cross-module symbol resolution.
 */

#include <switch.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <malloc.h>
#include <elf.h>

#include "config.h"
#include "drastic_jit.h"
#include "libc_shim.h"
#include "so_util.h"
#include "util.h"
#include "error.h"

// not in devkitA64's elf.h
#ifndef DT_RELR
#define DT_RELR    36
#define DT_RELRSZ  35
#define DT_RELRENT 37
#endif
#define DT_ANDROID_RELR    0x6fffe000
#define DT_ANDROID_RELRSZ  0x6fffe001
#define DT_ANDROID_RELRENT 0x6fffe003

static so_module *so_list = NULL;

#define SO_PAGE_SIZE 0x1000

_Static_assert((DRASTIC_JIT_ALIAS_CONGRUENCE &
                (DRASTIC_JIT_ALIAS_CONGRUENCE - 1)) == 0,
               "JIT alias congruence must be a power of two");

int so_add_jit_range(so_module *mod, size_t offset, size_t size) {
  if (!mod || !size || (offset & (SO_PAGE_SIZE - 1)) ||
      (size & (SO_PAGE_SIZE - 1)) || offset > mod->load_size ||
      size > mod->load_size - offset ||
      mod->jit_range_count >= SO_MAX_JIT_RANGES)
    return 0;

  int insert = mod->jit_range_count;
  for (int i = 0; i < mod->jit_range_count; i++) {
    SoJitRange *range = &mod->jit_ranges[i];
    if (offset < range->offset)
      insert = i;
    const size_t end = offset + size;
    const size_t range_end = range->offset + range->size;
    if (offset < range_end && range->offset < end)
      return 0;
    if (insert != mod->jit_range_count)
      break;
  }

  if (insert < mod->jit_range_count)
    memmove(&mod->jit_ranges[insert + 1], &mod->jit_ranges[insert],
            (mod->jit_range_count - insert) * sizeof(mod->jit_ranges[0]));
  mod->jit_ranges[insert] = (SoJitRange){
      .offset = offset,
      .size = size,
      .handle = INVALID_HANDLE,
  };
  mod->jit_range_count++;
  return 1;
}

static int so_page_is_jit(const so_module *mod, size_t page) {
  const size_t offset = page * SO_PAGE_SIZE;
  for (int i = 0; i < mod->jit_range_count; i++) {
    const SoJitRange *range = &mod->jit_ranges[i];
    if (offset >= range->offset && offset - range->offset < range->size)
      return 1;
  }
  return 0;
}

static Result so_map_plain_runs(so_module *mod) {
  const Handle self = envGetOwnProcessHandle();
  size_t cursor = 0;
  for (int i = 0; i <= mod->jit_range_count; i++) {
    const size_t end = i < mod->jit_range_count
                           ? mod->jit_ranges[i].offset : mod->load_size;
    if (end > cursor) {
      const Result rc = svcMapProcessCodeMemory(
          self, (u64)mod->load_virtbase + cursor,
          (u64)mod->load_base + cursor, end - cursor);
      if (R_FAILED(rc))
        return rc;
    }
    if (i < mod->jit_range_count)
      cursor = mod->jit_ranges[i].offset + mod->jit_ranges[i].size;
  }
  return 0;
}

static int address_ranges_overlap(uintptr_t first, size_t first_size,
                                  uintptr_t second, size_t second_size) {
  if (!first_size || !second_size)
    return 0;
  if (first <= second)
    return second - first < first_size;
  return first - second < second_size;
}

static int so_address_is_reserved(uintptr_t address, size_t size) {
  for (const so_module *loaded = so_list; loaded; loaded = loaded->next) {
    if (loaded->load_virtbase && loaded->load_size &&
        address_ranges_overlap(address, size,
                               (uintptr_t)loaded->load_virtbase,
                               loaded->load_size))
      return 1;
  }
  return 0;
}

static void *so_find_congruent_code_memory(uintptr_t rx, size_t size,
                                           size_t guard_size) {
  u64 aslr_address, aslr_size, alias_address, alias_size;
  u64 heap_address, heap_size;
  if (R_FAILED(svcGetInfo(&aslr_address, InfoType_AslrRegionAddress,
                          CUR_PROCESS_HANDLE, 0)) ||
      R_FAILED(svcGetInfo(&aslr_size, InfoType_AslrRegionSize,
                          CUR_PROCESS_HANDLE, 0)) ||
      R_FAILED(svcGetInfo(&alias_address, InfoType_AliasRegionAddress,
                          CUR_PROCESS_HANDLE, 0)) ||
      R_FAILED(svcGetInfo(&alias_size, InfoType_AliasRegionSize,
                          CUR_PROCESS_HANDLE, 0)) ||
      R_FAILED(svcGetInfo(&heap_address, InfoType_HeapRegionAddress,
                          CUR_PROCESS_HANDLE, 0)) ||
      R_FAILED(svcGetInfo(&heap_size, InfoType_HeapRegionSize,
                          CUR_PROCESS_HANDLE, 0)))
    return NULL;

  const uintptr_t granularity = DRASTIC_JIT_ALIAS_CONGRUENCE;
  const uintptr_t mask = granularity - 1;
  const uintptr_t region_first = (uintptr_t)aslr_address + guard_size;
  const uintptr_t region_end = (uintptr_t)(aslr_address + aslr_size);
  if (region_first < (uintptr_t)aslr_address || region_end < region_first ||
      size > region_end - region_first ||
      guard_size > region_end - region_first - size)
    return NULL;

  const uintptr_t residue = rx & mask;
  uintptr_t candidate = (region_first & ~mask) | residue;
  if (candidate < region_first) {
    if (candidate > UINTPTR_MAX - granularity)
      return NULL;
    candidate += granularity;
  }

  for (; candidate <= region_end - size - guard_size;) {
    const uintptr_t guarded = candidate - guard_size;
    const size_t guarded_size = size + guard_size * 2;
    if (candidate != rx &&
        !address_ranges_overlap(guarded, guarded_size,
                                (uintptr_t)alias_address,
                                (size_t)alias_size) &&
        !address_ranges_overlap(guarded, guarded_size,
                                (uintptr_t)heap_address,
                                (size_t)heap_size) &&
        !so_address_is_reserved(guarded, guarded_size)) {
      MemoryInfo info;
      u32 page_info;
      if (R_SUCCEEDED(svcQueryMemory(&info, &page_info, guarded)) &&
          info.type == MemType_Unmapped && info.addr <= guarded &&
          info.size >= guarded - info.addr &&
          guarded_size <= info.size - (guarded - info.addr))
        return (void *)candidate;
    }

    if (candidate > UINTPTR_MAX - granularity)
      break;
    candidate += granularity;
  }
  return NULL;
}

static Result so_map_jit_range(so_module *mod, SoJitRange *range) {
  const uintptr_t rx = (uintptr_t)mod->load_virtbase + range->offset;
  const uintptr_t source = (uintptr_t)mod->load_base + range->offset;

  virtmemLock();
  void *rw = so_find_congruent_code_memory(rx, range->size, SO_PAGE_SIZE);
  VirtmemReservation *reservation = rw
      ? virtmemAddReservation(rw, range->size) : NULL;
  virtmemUnlock();
  if (!rw || !reservation) {
    return MAKERESULT(Module_Libnx, LibnxError_OutOfMemory);
  }

  Handle handle = INVALID_HANDLE;
  Result rc = svcCreateCodeMemory(&handle, (void *)source, range->size);
  if (R_SUCCEEDED(rc)) {
    rc = svcControlCodeMemory(handle, CodeMapOperation_MapOwner, rw,
                              range->size, Perm_Rw);
  }
  if (R_SUCCEEDED(rc)) {
    rc = svcControlCodeMemory(handle, CodeMapOperation_MapSlave, (void *)rx,
                              range->size, Perm_Rx);
  }
  if (R_SUCCEEDED(rc) &&
      ((((uintptr_t)rw ^ rx) &
        (DRASTIC_JIT_ALIAS_CONGRUENCE - 1)) != 0 ||
       (uintptr_t)rw == rx)) {
    rc = MAKERESULT(Module_Libnx, LibnxError_BadInput);
  }
  if (R_SUCCEEDED(rc) &&
      !jit_redirect_register(rx, (uintptr_t)rw, range->size)) {
    rc = MAKERESULT(Module_Libnx, LibnxError_OutOfMemory);
  }

  if (R_FAILED(rc)) {
    if (handle != INVALID_HANDLE) {
      svcControlCodeMemory(handle, CodeMapOperation_UnmapSlave, (void *)rx,
                           range->size, 0);
      svcControlCodeMemory(handle, CodeMapOperation_UnmapOwner, rw,
                           range->size, 0);
      svcCloseHandle(handle);
    }
    virtmemLock();
    virtmemRemoveReservation(reservation);
    virtmemUnlock();
    return rc;
  }

  range->rw_alias = rw;
  range->rw_memrv = reservation;
  range->handle = handle;
  range->mapped = 1;
  return 0;
}

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

  /* Map ordinary module pages with process code memory. JIT subranges use a
   * congruent CodeMemory RW alias consumed directly by the patched emitter. */
  rc = so_map_plain_runs(mod);
  if (R_FAILED(rc)) fatal_error("Error: svcMapProcessCodeMemory failed:\n%08x", rc);

  for (int i = 0; i < mod->jit_range_count; i++) {
    rc = so_map_jit_range(mod, &mod->jit_ranges[i]);
    if (R_FAILED(rc))
      fatal_error("Error: could not create Drastic JIT mapping:\n%08x", rc);
  }

  // the kernel never allows W -> X transitions on code memory, so the RX
  // segments must be set first, straight from the freshly-mapped state;
  // everything else (data segments AND the alignment gaps lld leaves
  // between segments) becomes RW afterwards. build a page map to emit
  // each permission exactly once per page.
  const size_t num_pages = mod->load_size / SO_PAGE_SIZE;
  uint8_t *is_x_page = calloc(num_pages, 1);
  if (!is_x_page) fatal_error("Error: out of memory in so_finalize");

  for (int i = 0; i < mod->phnum; i++) {
    const Elf64_Phdr *p = &mod->phdr[i];
    if (p->p_type != PT_LOAD || (p->p_flags & PF_X) != PF_X)
      continue;
    const size_t first = p->p_vaddr / SO_PAGE_SIZE;
    const size_t last = (ALIGN_MEM(p->p_vaddr + p->p_memsz,
                                   SO_PAGE_SIZE) / SO_PAGE_SIZE) - 1;
    for (size_t pg = first; pg <= last && pg < num_pages; pg++)
      is_x_page[pg] = 1;
  }

  // pass 0 sets the executable runs to RX, pass 1 sets the rest to RW
  for (int want_x = 1; want_x >= 0; want_x--) {
    size_t pg = 0;
    while (pg < num_pages) {
      if (so_page_is_jit(mod, pg) || is_x_page[pg] != want_x) {
        pg++;
        continue;
      }
      size_t run_end = pg;
      while (run_end < num_pages && !so_page_is_jit(mod, run_end) &&
             is_x_page[run_end] == want_x)
        run_end++;
      const u64 addr = (u64)mod->load_virtbase + pg * SO_PAGE_SIZE;
      const u64 size = (run_end - pg) * SO_PAGE_SIZE;
      rc = svcSetProcessMemoryPermission(envGetOwnProcessHandle(), addr, size, want_x ? Perm_Rx : Perm_Rw);
      if (R_FAILED(rc)) fatal_error("Error: could not map %u bytes of %s memory at %p:\n%08x", (u32)size, want_x ? "RX" : "RW", (void *)addr, rc);
      pg = run_end;
    }
  }

  free(is_x_page);
}

uint32_t so_unload(so_module *mod) {
  if (!mod || !mod->load_virtbase || !mod->load_base || !mod->load_size)
    return 0;

  const Handle self = envGetOwnProcessHandle();
  Result rc = 0;
  size_t cursor = 0;
  for (int i = 0; i <= mod->jit_range_count; i++) {
    const size_t end = i < mod->jit_range_count
                           ? mod->jit_ranges[i].offset : mod->load_size;
    if (end > cursor) {
      rc = svcUnmapProcessCodeMemory(self,
                                     (u64)mod->load_virtbase + cursor,
                                     (u64)mod->load_base + cursor,
                                     end - cursor);
      if (R_FAILED(rc))
        return rc;
    }
    if (i < mod->jit_range_count)
      cursor = mod->jit_ranges[i].offset + mod->jit_ranges[i].size;
  }

  for (int i = 0; i < mod->jit_range_count; i++) {
    SoJitRange *range = &mod->jit_ranges[i];
    if (!range->mapped)
      continue;
    const void *rx = (char *)mod->load_virtbase + range->offset;
    jit_redirect_unregister((uintptr_t)rx);
    rc = svcControlCodeMemory(range->handle, CodeMapOperation_UnmapOwner,
                              range->rw_alias, range->size, 0);
    if (R_FAILED(rc)) return rc;
    rc = svcControlCodeMemory(range->handle, CodeMapOperation_UnmapSlave,
                              (void *)rx, range->size, 0);
    if (R_FAILED(rc)) return rc;
    rc = svcCloseHandle(range->handle);
    if (R_FAILED(rc)) return rc;
    if (range->rw_memrv) {
      virtmemLock();
      virtmemRemoveReservation((VirtmemReservation *)range->rw_memrv);
      virtmemUnlock();
    }
    memset(range, 0, sizeof(*range));
  }

  if (mod->load_memrv) {
    virtmemLock();
    virtmemRemoveReservation((VirtmemReservation *)mod->load_memrv);
    virtmemUnlock();
  }

  so_module **link = &so_list;
  while (*link && *link != mod)
    link = &(*link)->next;
  if (*link == mod)
    *link = mod->next;

  mod->next = NULL;
  mod->load_base = NULL;
  mod->load_virtbase = NULL;
  mod->load_size = 0;
  mod->load_memrv = NULL;
  mod->jit_range_count = 0;
  mod->elf_hdr = NULL;
  mod->prog_hdr = NULL;
  mod->sec_hdr = NULL;
  mod->syms = NULL;
  mod->dynstrtab = NULL;
  return 0;
}

int so_load(so_module *mod, const char *filename, void *base, size_t max_size) {
  int res = 0;

  memset(mod, 0, sizeof(*mod));
  strncpy(mod->name, filename, sizeof(mod->name) - 1);

  FILE *fd = fopen(filename, "rb");
  if (fd == NULL)
    return -1;

  fseek(fd, 0, SEEK_END);
  mod->so_size = ftell(fd);
  fseek(fd, 0, SEEK_SET);

  mod->so_base = malloc(mod->so_size);
  if (!mod->so_base) {
    fclose(fd);
    return -2;
  }

  fread(mod->so_base, mod->so_size, 1, fd);
  fclose(fd);

  if (memcmp(mod->so_base, ELFMAG, SELFMAG) != 0) {
    res = -1;
    goto err_free_so;
  }

  mod->elf_hdr = (Elf64_Ehdr *)mod->so_base;
  mod->prog_hdr = (Elf64_Phdr *)((uintptr_t)mod->so_base + mod->elf_hdr->e_phoff);
  mod->sec_hdr = (Elf64_Shdr *)((uintptr_t)mod->so_base + mod->elf_hdr->e_shoff);
  mod->shstrtab = (char *)((uintptr_t)mod->so_base + mod->sec_hdr[mod->elf_hdr->e_shstrndx].sh_offset);

  if (mod->elf_hdr->e_phnum > SO_MAX_SEGMENTS * 2) {

    res = -4;
    goto err_free_so;
  }

  // save a pristine copy of the phdrs for dl_iterate_phdr
  mod->phnum = mod->elf_hdr->e_phnum;
  memcpy(mod->phdr, mod->prog_hdr, mod->phnum * sizeof(Elf64_Phdr));

  // total size of the LOAD zone = highest vaddr+memsz of all LOAD segments
  mod->load_size = 0;
  for (int i = 0; i < mod->elf_hdr->e_phnum; i++) {
    if (mod->prog_hdr[i].p_type == PT_LOAD) {
      const size_t seg_end = mod->prog_hdr[i].p_vaddr + mod->prog_hdr[i].p_memsz;
      if (seg_end > mod->load_size)
        mod->load_size = seg_end;
    }
  }

  // align total size to page size
  mod->load_size = ALIGN_MEM(mod->load_size, 0x1000);
  if (mod->load_size > max_size) {

    res = -3;
    goto err_free_so;
  }

  mod->load_base = base;
  if (!mod->load_base) goto err_free_so;
  memset(mod->load_base, 0, mod->load_size);

  // reserve virtual memory space for the entire LOAD zone
  virtmemLock();
  mod->load_virtbase = virtmemFindCodeMemory(mod->load_size, 0x1000);
  mod->load_memrv = virtmemAddReservation(mod->load_virtbase, mod->load_size);
  virtmemUnlock();



  // copy segments to where they belong, then fix up the runtime phdrs to
  // point into the virtual address space
  for (int i = 0; i < mod->elf_hdr->e_phnum; i++) {
    Elf64_Phdr *p = &mod->prog_hdr[i];
    if (p->p_type == PT_LOAD) {
      memcpy((void *)((uintptr_t)mod->load_base + p->p_vaddr),
             (void *)((uintptr_t)mod->so_base + p->p_offset),
             p->p_filesz);
    }
    p->p_vaddr += (Elf64_Addr)mod->load_virtbase;
  }

  mod->syms = NULL;
  mod->dynstrtab = NULL;

  for (int i = 0; i < mod->elf_hdr->e_shnum; i++) {
    char *sh_name = mod->shstrtab + mod->sec_hdr[i].sh_name;
    if (strcmp(sh_name, ".dynsym") == 0) {
      mod->syms = (Elf64_Sym *)((uintptr_t)mod->load_base + mod->sec_hdr[i].sh_addr);
      mod->num_syms = mod->sec_hdr[i].sh_size / sizeof(Elf64_Sym);
    } else if (strcmp(sh_name, ".dynstr") == 0) {
      mod->dynstrtab = (char *)((uintptr_t)mod->load_base + mod->sec_hdr[i].sh_addr);
    }
  }

  if (mod->syms == NULL || mod->dynstrtab == NULL) {
    res = -2;
    goto err_free_load;
  }

  // register in the global module list (append, load order matters for resolution)
  mod->next = NULL;
  if (!so_list) {
    so_list = mod;
  } else {
    so_module *m = so_list;
    while (m->next) m = m->next;
    m->next = mod;
  }

  return 0;

err_free_load:
  virtmemLock();
  virtmemRemoveReservation(mod->load_memrv);
  virtmemUnlock();
err_free_so:
  free(mod->so_base);
  mod->so_base = NULL;

  return res;
}

// find a dynamic tag in the file image; returns 0 if missing
static Elf64_Xword so_dynamic_tag(so_module *mod, Elf64_Sxword tag) {
  for (int i = 0; i < mod->phnum; i++) {
    if (mod->phdr[i].p_type == PT_DYNAMIC) {
      const Elf64_Dyn *dyn = (const Elf64_Dyn *)((uintptr_t)mod->so_base + mod->phdr[i].p_offset);
      for (; dyn->d_tag != DT_NULL; dyn++)
        if (dyn->d_tag == tag)
          return dyn->d_un.d_val;
    }
  }
  return 0;
}

// RELR/ANDROID_RELR packed relative relocations (used by libGame.so);
// each location gets the load base added to the value stored in the file
static void so_process_relr(so_module *mod, const Elf64_Xword *relr, size_t relrsz) {
  uintptr_t where = 0;
  const size_t count = relrsz / sizeof(Elf64_Xword);
  for (size_t i = 0; i < count; i++) {
    const Elf64_Xword entry = relr[i];
    if ((entry & 1) == 0) {
      // even entry: address of the next relocation
      where = (uintptr_t)entry;
      *(uint64_t *)((uintptr_t)mod->load_base + where) += (uint64_t)mod->load_virtbase;
      where += 8;
    } else {
      // odd entry: bitmap for the next 63 words
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
    if (strcmp(sh_name, ".rela.dyn") == 0 || strcmp(sh_name, ".rela.plt") == 0) {
      Elf64_Rela *rels = (Elf64_Rela *)((uintptr_t)mod->load_base + mod->sec_hdr[i].sh_addr);
      for (size_t j = 0; j < mod->sec_hdr[i].sh_size / sizeof(Elf64_Rela); j++) {
        uintptr_t *ptr = (uintptr_t *)((uintptr_t)mod->load_base + rels[j].r_offset);
        Elf64_Sym *sym = &mod->syms[ELF64_R_SYM(rels[j].r_info)];

        int type = ELF64_R_TYPE(rels[j].r_info);
        switch (type) {
          case R_AARCH64_ABS64:
            if (sym->st_shndx == SHN_UNDEF) {
              // Imported absolute relocations (notably RTTI/type_info vtable
              // references) must be resolved after all modules are loaded.
              *ptr = rels[j].r_addend;
            } else {
              *ptr = (uintptr_t)mod->load_virtbase + sym->st_value + rels[j].r_addend;
            }
            break;

          case R_AARCH64_RELATIVE:
            // sometimes the value of r_addend is also at *ptr
            *ptr = (uintptr_t)mod->load_virtbase + rels[j].r_addend;
            break;

          case R_AARCH64_GLOB_DAT:
          case R_AARCH64_JUMP_SLOT:
          {
            if (sym->st_shndx != SHN_UNDEF)
              *ptr = (uintptr_t)mod->load_virtbase + sym->st_value + rels[j].r_addend;
            break;
          }

          default:
            fatal_error("Error: unknown relocation type:\n%x\n", type);
            break;
        }
      }
    }
  }

  // packed relative relocations, if present (lld emits these for newer libs)
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

// look up an exported (defined) symbol in a single module;
// returns its virtual (runtime) address or 0
static uintptr_t so_lookup_export(so_module *mod, const char *name) {
  for (int i = 0; i < mod->num_syms; i++) {
    if (mod->syms[i].st_shndx == SHN_UNDEF)
      continue;
    if (ELF64_ST_BIND(mod->syms[i].st_info) == STB_LOCAL)
      continue;
    const char *sname = mod->dynstrtab + mod->syms[i].st_name;
    if (sname[0] == name[0] && strcmp(sname, name) == 0)
      return (uintptr_t)mod->load_virtbase + mod->syms[i].st_value;
  }
  return 0;
}

static uintptr_t so_resolve_symbol(so_module *mod, DynLibFunction *funcs, int num_funcs, const char *name) {
  // The import table takes priority over module exports: the C++ runtime
  // donor (the APK's libopenal.so) exports the full OpenAL API alongside
  // its statically-linked libc++, and the AL calls must go to the native
  // openal-soft from the table. Everything not in the table (std::, __cxa_,
  // RTTI data) falls through to the donor's exports.
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

int so_resolve(so_module *mod, DynLibFunction *funcs, int num_funcs, int taint_missing_imports) {
  for (int i = 0; i < mod->elf_hdr->e_shnum; i++) {
    char *sh_name = mod->shstrtab + mod->sec_hdr[i].sh_name;
    if (strcmp(sh_name, ".rela.dyn") == 0 || strcmp(sh_name, ".rela.plt") == 0) {
      Elf64_Rela *rels = (Elf64_Rela *)((uintptr_t)mod->load_base + mod->sec_hdr[i].sh_addr);
      for (size_t j = 0; j < mod->sec_hdr[i].sh_size / sizeof(Elf64_Rela); j++) {
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
              } else {
                if (taint_missing_imports)
                  *ptr = rels[j].r_offset;
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

  return 0;
}

void so_execute_init_array(so_module *mod) {
  for (int i = 0; i < mod->elf_hdr->e_shnum; i++) {
    char *sh_name = mod->shstrtab + mod->sec_hdr[i].sh_name;
    if (strcmp(sh_name, ".init_array") == 0) {
      int (** init_array)() = (void *)((uintptr_t)mod->load_virtbase + mod->sec_hdr[i].sh_addr);
      for (size_t j = 0; j < mod->sec_hdr[i].sh_size / 8; j++) {
        if (init_array[j] != 0)
          init_array[j]();
      }
    }
  }
}

uintptr_t so_find_addr_rx(so_module *mod, const char *symbol) {
  const uintptr_t addr = so_try_find_addr_rx(mod, symbol);
  if (!addr)
    fatal_error("Error: could not find symbol:\n%s\n", symbol);
  return addr;
}

uintptr_t so_try_find_addr_rx(so_module *mod, const char *symbol) {
  for (int i = 0; i < mod->num_syms; i++) {
    char *name = mod->dynstrtab + mod->syms[i].st_name;
    if (strcmp(name, symbol) == 0)
      return (uintptr_t)mod->load_virtbase + mod->syms[i].st_value;
  }
  return 0;
}

DynLibFunction *so_find_import(DynLibFunction *funcs, int num_funcs, const char *name) {
  for (int i = 0; i < num_funcs; ++i)
    if (!strcmp(funcs[i].symbol, name))
      return &funcs[i];
  return NULL;
}

// matches the layout bionic/libunwind expect
struct so_dl_phdr_info {
  Elf64_Addr dlpi_addr;
  const char *dlpi_name;
  const Elf64_Phdr *dlpi_phdr;
  Elf64_Half dlpi_phnum;
};

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
