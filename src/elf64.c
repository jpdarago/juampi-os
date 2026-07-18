#include <elf64.h>
#include <paging.h>
#include <frames.h>
#include <utils.h>

// Map every page spanned by [start, start+len) into the current address space
// as user-accessible, reusing any page that is already present.
static void map_user_range(uint64 start, uint64 len)
{
    for (uint64 p = start & ~0xFFFull; p < start + len; p += PAGE_SZ) {
        if (physical_address(current_directory, p) == (uintptr)-1) {
            map_page(current_directory, p, frame_alloc(),
                     PAGEF_P | PAGEF_RW | PAGEF_U);
        }
    }
}

uint64 elf64_load(void* image)
{
    Elf64_Ehdr* eh = image;
    const uint8* id = eh->e_ident;
    if (id[0] != 0x7F || id[1] != 'E' || id[2] != 'L' || id[3] != 'F' ||
        id[4] != 2 /* ELFCLASS64 */) {
        return 0;
    }

    Elf64_Phdr* ph = (Elf64_Phdr*)((uint8*)image + eh->e_phoff);
    for (uint i = 0; i < eh->e_phnum; i++) {
        if (ph[i].p_type != PT_LOAD) {
            continue;
        }
        // Map the segment's pages, copy its file contents in, and zero the
        // remainder (.bss). The segment is written through the kernel's view of
        // the freshly-mapped user pages (same, active address space).
        map_user_range(ph[i].p_vaddr, ph[i].p_memsz);
        memcpy((void*)ph[i].p_vaddr, (uint8*)image + ph[i].p_offset,
               (uint)ph[i].p_filesz);
        for (uint64 z = ph[i].p_filesz; z < ph[i].p_memsz; z++) {
            ((uint8*)ph[i].p_vaddr)[z] = 0;
        }
    }
    return eh->e_entry;
}
