#ifndef __PAGING_H
#define __PAGING_H

#include <types.h>
#include <exception.h>

#define PAGE_SZ 0x1000

typedef struct {
	uint present:1;
	uint read_write:1;
	uint user:1;
	uint write_through:1;
	uint cache_disabled:1;
	uint accessed:1;
	uint dirty:1;
	uint __zero_1;
	uint global:1;
	uint avail:3;
	uint frame:20;
} page_entry;

#define PAGEF_P 	1
#define PAGEF_RW 	2
#define PAGEF_U 	4
#define PAGEF_A		8

typedef struct {
	uint present:1;
	uint read_write:1;
	uint user:1;
	uint write_through:1;
	uint cache_disabled:1;
	uint accessed:1;
	uint __zero:1;
	uint size:1;
	uint global:1;
	uint avail:3;
	uint frame:20;
} page_table_entry;

typedef struct {
	page_entry entries[1024];
} page_table;

typedef struct {
	//Las direcciones (en espacio de memorias virtual)
	//correspondientes a los contenidos de las tablas
	//de directorio		
	page_table * tables_virtual[1024];
	//Las entradas (que contienen direcciones fisicas) 
	//de las tablas de este directorio. Esto es para cuando
	//tenemos que clonar las entradas (al forkear el proceso).
	page_table_entry tables_phys[1024];
	//Direccion fisica donde comienzan las entradas 
	//de directorio (las tablas). Esto va en el cr3 del kernel.
	uint physical_address;
} page_directory;

void paging_init(page_directory * kernel_dir);
void map_page(page_directory * pd, uint va, uint pa, uint flags);
uint paging_assign(page_directory * pd, uint va);
void switch_paging(page_directory * pd);
void page_fault_handler(exception_trace);

#endif
