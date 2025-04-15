#ifndef __ELF_H
#define __ELF_H

#include "types.h"

typedef struct {
    char magic[16];
    uint16 type;
    uint16 machine;
    uint32 version;
    uint32 entry_point;
    uint32 ph_offset;
    uint32 sh_offset;
    uint32 flags;
    uint16 header_size;
    uint16 ph_entry_size;
    uint16 ph_entry_count;
    uint16 sh_entry_size;
    uint16 sh_entry_count;
    uint16 sh_string_table_index;
} __attribute__((__packed__)) elf_header;

#define EI_CLASS 4
#define EI_DATAENC 5

#define EM_386 3

#define ELFCLASSNONE 0
#define ELFCLASS32 1
#define ELFCLASS64 2

#define ELFDATANONE 0
#define ELFDATA2LSB 1
#define ELFDATA2MSB 2

typedef struct {
    uint32 name;
    uint32 type;
    uint32 flags;
    uint32 address;
    uint32 offset;
    uint32 size;
    uint32 link;
    uint32 info;
    uint32 address_align;
    uint32 entry_size;
} __attribute__((__packed__)) elf_sheader;

#define SHN_UNDEF 0

#define ELF_NONE 0
#define ELF_LOAD 1

#define ELF_ATTR_XB 1
#define ELF_ATTR_WB 2
#define ELF_ATTR_RB 4

typedef struct {
    uint32 type;
    uint32 offset;
    uint32 virtual_address;
    uint32 physical_address;
    uint32 file_size;
    uint32 memory_size;
    uint32 flags;
    uint32 align;
} __attribute__((__packed__)) elf_pheader;

typedef struct {
    void * data;
    intptr virtual_address;
    uint type;
    uint file_size,mem_size;
    uint attributes;
    uint flags;
    uint alignment;
} __attribute__((__packed__)) elf_segment;

typedef struct {
    elf_header  * header;
    elf_pheader * program_header;
} __attribute__((__packed__)) elf_file;

elf_file * elf_read_exec(void * image);
void elf_destroy(elf_file * elf);

unsigned int elf_entry_point(elf_file * elf);
elf_segment * elf_get_segment(elf_file * elf, uint index);
void elf_free_segment(elf_segment * e);

#endif
