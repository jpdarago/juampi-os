#ifndef __ELF64_H
#define __ELF64_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

// Minimal ELF64 definitions — just enough to load a static executable's
// loadable segments.
typedef struct {
    uint8_t e_ident[16];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint64_t e_entry;
    uint64_t e_phoff;
    uint64_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
} __attribute__((__packed__)) Elf64_Ehdr;

typedef struct {
    uint32_t p_type;
    uint32_t p_flags;
    uint64_t p_offset;
    uint64_t p_vaddr;
    uint64_t p_paddr;
    uint64_t p_filesz;
    uint64_t p_memsz;
    uint64_t p_align;
} __attribute__((__packed__)) Elf64_Phdr;

#define PT_LOAD 1

// Load an ELF64 image (already in memory at `image`) into the current address
// space, mapping each loadable segment as user pages. Returns the entry point,
// or 0 if the image is not a valid ELF64 executable.
uint64_t elf64_load(void* image);

// Like elf64_load but maps segments as kernel-only pages, for a binary that is
// called directly in ring 0 (the "sterile lab" — see lab.h). Returns the entry.
uint64_t elf64_load_exec(void* image);

#endif
