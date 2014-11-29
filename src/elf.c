#include <elf.h>
#include <memory.h>
#include <paging.h>
#include <utils.h>
#include <scrn.h>

static char elf_magic[] = { 0x7F, 'E', 'L', 'F' };

// Arma la estructura de un ELF a partir de un
// buffer de memoria
elf_file* elf_read_exec(void* image)
{
    char* imagep = image;
    elf_file* elf = kmalloc(sizeof(elf_file));
    elf->header = image;
    elf_header* header = elf->header;
    if(memcmp(header->magic,elf_magic,sizeof(elf_magic))
       || header->magic[EI_CLASS] != ELFCLASS32
       || header->magic[EI_DATAENC] != ELFDATA2LSB) {

        return NULL;
    }
    elf->program_header = NULL;
    if(header->ph_offset) {
        elf->program_header = (elf_pheader*)
                              (imagep+header->ph_offset);
    } else return NULL;

    return elf;
}

void elf_destroy(elf_file* elf)
{
    if(elf == NULL) return;
    kfree(elf);
}

uint elf_entry_point(elf_file* elf)
{
    return elf->header->entry_point;
}

elf_segment* elf_get_segment(elf_file* elf, uint index)
{
    if(index > elf->header->ph_entry_count) {
        return NULL;
    }
    elf_pheader* ph = &elf->program_header[index];
    elf_segment* e = kmalloc(sizeof(elf_segment));
    memset(e,0,sizeof(elf_segment));
    e->data = (char*) elf->header + ph->offset;
    e->virtual_address = ph->virtual_address;
    e->file_size = ph->file_size;
    e->mem_size = ph->memory_size;
    e->alignment = ph->align;
    e->flags = ph->flags;
    e->type = ph->type;
    return e;
}

void elf_free_segment(elf_segment* e)
{
    kfree(e);
}
