// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <stddef.h>
#include <stdint.h>

// Fixed ELF64 file layouts, independent of host operating-system headers.
typedef struct { uint8_t ident[16]; uint16_t type, machine; uint32_t version;
  uint64_t entry, phoff, shoff; uint32_t flags; uint16_t ehsize, phentsize, phnum, shentsize, shnum, shstrndx; } PmElfHeader;
typedef struct { uint32_t type, flags; uint64_t offset, vaddr, paddr, filesz, memsz, align; } PmElfProgram;
typedef struct { uint32_t name, type; uint64_t flags, addr, offset, size; uint32_t link, info; uint64_t align, entsize; } PmElfSection;
typedef struct { uint64_t offset, info; int64_t addend; } PmElfRela;
typedef struct { int64_t tag; uint64_t value; } PmElfDynamic;
typedef struct { uint64_t offset, count; } PmRelocationTable;
typedef struct {
  PmElfHeader header;
  uint64_t lowest, highest;
  PmRelocationTable relocations[128];
  unsigned relocation_tables;
} PmElfImage;

// No pointers to unvalidated input escape this function. Zero means accepted.
int pm_elf_inspect(const void *data, size_t size, PmElfImage *image, char *error, size_t error_size);
// Materialize the validated image into a bounded local mirror before any remote write.
int pm_elf_materialize(const void *data, size_t size, const PmElfImage *image,
                        uint64_t mapped_base, void *mirror, size_t mirror_size);
