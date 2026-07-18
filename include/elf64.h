#ifndef __ELF64_H
#define __ELF64_H

#include <types.h>

// Minimal ELF64 definitions — just enough to load a static executable's
// loadable segments.
typedef struct {
    uint8 e_ident[16];
    uint16 e_type;
    uint16 e_machine;
    uint32 e_version;
    uint64 e_entry;
    uint64 e_phoff;
    uint64 e_shoff;
    uint32 e_flags;
    uint16 e_ehsize;
    uint16 e_phentsize;
    uint16 e_phnum;
    uint16 e_shentsize;
    uint16 e_shnum;
    uint16 e_shstrndx;
} __attribute__((__packed__)) Elf64_Ehdr;

typedef struct {
    uint32 p_type;
    uint32 p_flags;
    uint64 p_offset;
    uint64 p_vaddr;
    uint64 p_paddr;
    uint64 p_filesz;
    uint64 p_memsz;
    uint64 p_align;
} __attribute__((__packed__)) Elf64_Phdr;

#define PT_LOAD 1

// Load an ELF64 image (already in memory at `image`) into the current address
// space, mapping each loadable segment as user pages. Returns the entry point,
// or 0 if the image is not a valid ELF64 executable.
uint64 elf64_load(void* image);

#endif
