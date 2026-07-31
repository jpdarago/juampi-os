#include <elf64.h>
#include <paging.h>
#include <frames.h>
#include <utils.h>

// e_ident magic: the four bytes "\x7FELF" then a class byte (index EI_CLASS).
#define ELF_MAG0 0x7F
#define EI_CLASS 4
#define ELFCLASS64 2

// Map every page spanned by [start, start+len) into the current address space
// with the given PAGEF_* flags, reusing any page that is already present.
static void map_range(uint64_t start, uint64_t len, uint64_t flags)
{
    for (uint64_t p = start & ~0xFFFull; p < start + len; p += PAGE_SZ) {
        if (physical_address(current_directory, p) == (uintptr_t)-1) {
            map_page(current_directory, p, frame_alloc(), flags);
        }
    }
}

// Validate an ELF64 header, map each PT_LOAD segment with `seg_flags`, copy its
// file contents in, and zero the .bss remainder. Returns the entry point, or 0
// if the image is not a valid ELF64.
static uint64_t load_segments(void* image, uint64_t seg_flags)
{
    struct Elf64_Ehdr* eh = image;
    const uint8_t* id = eh->e_ident;
    if (id[0] != ELF_MAG0 || id[1] != 'E' || id[2] != 'L' || id[3] != 'F' ||
        id[EI_CLASS] != ELFCLASS64) {
        return 0;
    }

    struct Elf64_Phdr* ph = (struct Elf64_Phdr*)((uint8_t*)image + eh->e_phoff);
    for (uint32_t i = 0; i < eh->e_phnum; i++) {
        if (ph[i].p_type != PT_LOAD) {
            continue;
        }
        // The segment is written through the kernel's view of the
        // freshly-mapped pages (same, active address space).
        map_range(ph[i].p_vaddr, ph[i].p_memsz, seg_flags);
        memcpy((void*)ph[i].p_vaddr, (uint8_t*)image + ph[i].p_offset,
               (uint32_t)ph[i].p_filesz);
        for (uint64_t z = ph[i].p_filesz; z < ph[i].p_memsz; z++) {
            ((uint8_t*)ph[i].p_vaddr)[z] = 0;
        }
    }
    return eh->e_entry;
}

// Load a binary to be called directly in ring 0 (the "sterile lab"): segments
// mapped kernel-only. They are executable because the kernel does not enforce
// NX (see src/paging.c).
uint64_t elf64_load_exec(void* image)
{
    return load_segments(image, PAGEF_P | PAGEF_RW);
}

static bool name_eq(const char* a, const char* b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

uint64_t elf64_symbol(const void* image, const char* name)
{
    const struct Elf64_Ehdr* eh = image;
    if (eh->e_shoff == 0 || eh->e_shentsize != sizeof(struct Elf64_Shdr)) {
        return 0;
    }
    const uint8_t* base = image;
    const struct Elf64_Shdr* sh =
            (const struct Elf64_Shdr*)(base + eh->e_shoff);
    for (uint32_t i = 0; i < eh->e_shnum; i++) {
        if (sh[i].sh_type != SHT_SYMTAB || sh[i].sh_entsize == 0) {
            continue;
        }
        const struct Elf64_Sym* sym =
                (const struct Elf64_Sym*)(base + sh[i].sh_offset);
        uint32_t n = (uint32_t)(sh[i].sh_size / sh[i].sh_entsize);
        const char* str = (const char*)(base + sh[sh[i].sh_link].sh_offset);
        for (uint32_t j = 0; j < n; j++) {
            if (sym[j].st_name != 0 && name_eq(str + sym[j].st_name, name)) {
                return sym[j].st_value;
            }
        }
    }
    return 0;
}
