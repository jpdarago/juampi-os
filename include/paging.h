#ifndef __PAGING_H
#define __PAGING_H

#include <types.h>
#include <exception.h>
#include <memory.h>

#define PAGE_SZ 0x1000
#define PAGE_TABLES 1024
#define PAGE_FRAMES	1024

#define PAGING_ENABLED_MASK (1 << 31)
#define WP_MASK (1 << 16)

typedef struct {
	uint present:1;
	uint read_write:1;
	uint user:1;
	uint write_through:1;
	uint cache_disabled:1;
	uint accessed:1;
	uint dirty:1;
	uint __zero_1:1;
	uint global:1;
	//Copy on write: Si una pagina esta con este bit
	//y alguien la trata de escribir cuando tiene modo solo 
	//lectura, hay que hacer una copia y reasignarla. Es parte
	//de los bits disponibles de intel. 	
	uint copy_on_write:1;
	uint avail:2;
	uint frame:20;
} page_entry;

#define PAGEF_P 	1
#define PAGEF_RW 	2
#define PAGEF_U 	4
#define PAGEF_A		8
#define PAGEF_FULL (PAGEF_P | PAGEF_RW | PAGEF_U)

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
	//Las entradas (que contienen direcciones fisicas) 
	//de las tablas de este directorio. Esto es para cuando
	//tenemos que clonar las entradas (al forkear el proceso).
	page_table_entry tables_phys[1024];
	//Las direcciones (en espacio de memorias virtual)
	//correspondientes a los contenidos de las tablas
	//de directorio		
	page_table * tables_virtual[1024];
	//Direccion fisica donde comienzan las entradas 
	//de directorio (las tablas). Esto va en el cr3 del kernel.
	uint physical_address;
} page_directory;

#define PAGE_DIR(x) ((x) >> 22)
#define PAGE_TABLE(x) (((x)&0x3FF000) >> 12)
#define PAGE_OFFSET(x) ((x) & 0xFFF)

#define ALIGN(x) ((x) & ~0xFFF)
#define NEXT_ALIGN(x) ALIGN((x)+0x1000)

//Devuelve la direccion fisica de la direccion virtual en
//el directorio, ambos pasados por parametro
uint physical_address(page_directory *, uint);
//Inicializa paginacion haciendo identity mapping y creando
//los handlers de excepcion y estructuras necesarias
void paging_init(uint end_address, uint kernel_last_addr);
//Mapea una direccion virtual a una fisica en un directorio dados
//los flags que se desean
void map_page(page_directory * pd, uint va, uint pa, uint flags);
//Consigue mas memoria para el mapa de memoria indicado
void * paging_append_core(kmem_map_header *,uint);
//Cambia el directorio de paginas al pasado por parametro
void switch_page_directory(page_directory * pd);
void page_fault_handler(exception_trace);
//Clona un directorio haciendo copy on write
page_directory * clone_directory(page_directory *);
//Directorio de paginas actual y directorio de paginas de
//kernel (el segundo es para saber que cosas son de kernel)
extern page_directory * current_directory, * kernel_dir;
//Copia dos frames: Deshabilita paginacion para ello y por
//eso esta escrito en assembler en copy_frame.asm
extern void copy_frame(uint dst, uint src);
//Cambia el directorio actual por otro
void set_current_directory(page_directory *);
//Devuelve la direccion de la heap de kernel
kmem_map_header * get_kernel_heap();
//Limpia una entrada de pagina o tabla del directorio actual
void clear_page_entry(page_directory * pe, uint, uint);
void clear_table_entry(page_directory * pe, uint);
//Eliminar un directorio de paginas completo
void page_directory_destroy(page_directory *);
#endif
