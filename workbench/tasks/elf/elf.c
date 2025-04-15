#include "elf.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char elf_magic[] = { 0x7F, 'E', 'L', 'F' };

//Arma la estructura de un ELF a partir de un
//buffer de memoria
elf_file * elf_exec_create(void * image)
{
	char * imagep = image;

	elf_file * elf = malloc(sizeof(elf_file));
	elf->header = image;
	elf_header * header = elf->header;
	
	if(memcmp(header->magic,elf_magic,sizeof(elf_magic)) 
		|| header->magic[EI_CLASS] != ELFCLASS32
		|| header->magic[EI_DATAENC] != ELFDATA2LSB)
	{
		return NULL;
	}

	elf->program_header = NULL; 

	if(header->ph_offset){
		elf->program_header = (elf_pheader *) 
			(imagep+header->ph_offset); 
	}else{ 
		printf("Error: No encontre program headers validos\n");
		return NULL;
	}
	return elf;
}

void elf_destroy(elf_file * elf)
{
	free(elf->header);
	free(elf);
}

unsigned int elf_entry_point(elf_file * elf)
{
	return elf->header->entry_point;
}

elf_segment * elf_get_segment(elf_file * elf, uint index)
{
	if(index > elf->header->ph_entry_count) return NULL;
	
	elf_pheader * ph = &elf->program_header[index];	
	if(ph->memory_size == 0) return NULL;

	elf_segment * e = malloc(sizeof(elf_segment));
	memset(e,0,sizeof(elf_segment));
	
	e->data = (char *) elf->header + ph->offset;
	e->virtual_address = ph->virtual_address;
	e->file_size = ph->file_size;
	e->mem_size = ph->memory_size;
	e->alignment = ph->align;
	e->flags = ph->flags;
	e->type = ph->type;	

	return e;
}

void elf_free_segment(elf_segment * e){
	free(e);
}
