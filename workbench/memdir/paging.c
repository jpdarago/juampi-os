#include "paging.h"
#include "memory.h"
#include "frame_alloc.h"
#include "exception.h"

#define PAGE_SZ 0x1000
#define PAGE_BW 12

#define ALIGN(x) ((x) & ~0xFFF)
#define NEXT_ALIGN(x) ALIGN((x)+0x1000)
#define PAGE_DIR(x) ((x) >> 22)
#define PAGE_TABLE(x) (((x)&0x3FF000) >> 12)

//Heap del kernel. De aca va a obtener el resto de la memoria
//el kernel.
kmem_map_header * kernel_heap = NULL;

#define KHEAP_START 	0xC0000000
#define KHEAP_INISIZE	 0x4000000
#define KHEAP_MAXIMUM	0xCFFFF000

//Cantidad de tablas que definimos inicialmente. Es la solucion
//mas sencilla para el problema de necesitar tablas de pagina que
//mapear al directorio del kernel cuando appendeamos memoria a la
//heap del kernel: las asignamos previamente y esperemos que alcanzen.
#define TABLES_ID_MAP	8

page_directory * current_directory, * kernel_dir;

//Devuelve la direccion fisica correspondiente a una
//direccion virtual dado el directorio de paginas.
//PRE: Las tablas del directorio deben estar mapeadas
//(Considerando que esto corre en modo kernel y que la
//administracion de paginas es propiedad del mismo 
//no me parece falto de razon)
uint physical_address(page_directory * pd, uint va)
{
	uint pdi = PAGE_DIR(va), pti = PAGE_TABLE(va);
	if(~pd->tables_phys[pdi] & PAGEF_P) 
			return (uint) -1;
	page_table * p = pd->tables_virtual[pdi];
	if(~p->entries[pti] & PAGEF_P) 
			return (uint) -1;
	return ALIGN(p->entries[pti]);
}

//Habilita paginacion activando dir como el directorio actual
void switch_page_directory(page_directory * dir)
{
	current_directory = dir;
	__asm__ __volatile__ ("mov %0, %%cr3" :: "r" (dir->physical_address));
	uint cr0;
	__asm__ __volatile__ ("mov %%cr0, %0" :: "=r" (cr0) );
	cr0 |= 0x8000000;
	__asm__ __volatile__ ("mov %0, %%cr0" :: "=r" (cr0) );
}

//Inicializa el administrador de frames (internamente usa un bitmap)
static uint frames_init(uint kernel_end, uint kernel_last_addr)
{	
	uint used_upper_mem = 0, megabyte = 1024*1024;
	uint total_upper_mem = kernel_last_addr - kernel_end;
	
	if(kernel_end > megabyte) 
		used_upper_mem = kernel_end_addr - megabyte; 
	
	uint available_frames = ( total_upper_mem - used_upper_mem/1024 )/ 4;

	return frame_alloc_init((void*)kernel_end_addr,available_frames);
}

//Siguiente posicion para pedir una tabla de paginas
static uint page_tables_vs;

//Mapea table_index a un frame (que se espera sea consecutivo porque esta
//funcion se corre al principio para generar el mapa de memoria de kernel previo
//a activar paginacion, porque despues se utiliza la heap para eso).
void create_id_table_entry(page_directory * kernel_dir, uint table_index)
{
	uint table_dir = frame_alloc(); //Como esto corre al principio,
									//frame_alloc devuelve frames consecutivos.
	if(table_dir != page_tables_vs) 
			kernel_panic("Seccion de paginas de kernel no continua");
	uint table_entry = table_dir | PAGEF_P | PAGEF_RW;
	page_tables_vs += PAGE_SZ;
		
	kernel_dir->tables_physical[table_index] = table_entry; 
	kernel_dir->tables_virtual[table_index] = table_entry;	
}	

//Extiende el mapa de memoria de kernel pasado por parametro, indicando
//la cantidad de paginas que deseamos agregar al final.
void paging_append_core(kmem_map_header * mh, uint pages)
{
	if(mh->heap_end == KHEAP_MAXIMUM) 
			kernel_panic("Heap excedida de tamaño");
	for(uint i = 0; i < pages; i++){
		uint frame = frame_alloc();
		memset((void*)frame,0,sizeof(frame));

		uint pdi = PAGE_DIR(mh->heap_end), 
			 pti = PAGE_TABLE(mh->heap_end);

		if(~kernel_dir->table_phys[pdi] & PAGEF_P)
			kernel_panic("Mapeo insuficiente "
						 "para appendear memoria");	
		
		kernel_dir->tables_virtual[pdi]->entries[pti] 
				= frame | PAGEF_P | PAGEF_RW;
		mh->heap_end += PAGE_SZ;
	}
}

void paging_init(uint end_address, uint kernel_last_addr)
{
	end_address = NEXT_ALIGN(end_address);

	kernel_dir = (page_directory *) end_address;
	memset(kernel_dir,0,sizeof(kernel_dir));
	kernel_dir->physical_address = (uint) kernel_dir;
	
	end_address += sizeof(page_directory);
	end_address = NEXT_ALIGN(end_address);
	
	//Creamos el manejador para los frames.
	end_address = frames_init(end_address,kernel_last_addr);
	page_tables_vs = end_address,page;
	
	//Asignamos las tablas necesarias para lo que tenemos de 
	//kernel hasta ahora.
	for(page = 0; page < end_adress; page += PAGE_SZ){
		if(~kernel_dir->pd[PAGE_TABLE(page)] & PAGEF_P){
			create_table_entry(kernel_dir,PAGE_TABLE(page));
		}
	}

	//Asignamos las tablas necesarias para todas las paginas de la heap
	//inicial. 
	for(page = KHEAP_START;page < KHEAP_START+KHEAP_INISIZE;page += PAGE_SZ){
		if(!kernel_dir->tables_virtual[PAGE_TABLE(page)])
			create_table_entry(kernel_dir,PAGE_TABLE(page));
	}

	//Mapeo un cierto numero de entradas de manera fija asi no tenemos dramas
	//de mapear las tablas para direcciones (con mapear 4 tablas tenemos 
	//mapeados las tablas para los primeros 8*4M = 32 MB de espacio, 
	//mas que suficiente para asignar frames de tablas de paginas para heap.
	for(uint table = 0; table < TABLES_ID_MAP; table++){
		if(!kernel_dir->tables_virtual[table]){
			create_table_entry(kernel_dir,table);
		}
	}

	end_address = page_tables_vs;
	
	//Hacemos identity mapping del kernel: Todo lo necesario es ahora
	//accesible de manera transparente.
	for(uint page = 0; page < end_address; page += PAGE_SZ){
		map_page(kernel_dir,page,page,PAGEF_P | PAGEF_RW);
	}
	
	register_exception_handler(14,page_fault_handler);
	current_directory = kernel_dir;		

	switch_page_directory(kernel_dir);

	kernel_heap = kmem_init((void*)KHEAP_START,KHEAP_INISIZE);
}

void map_page(page_directory * pd, uint va,uint pa,uint flags)
{
	uint pdi = PAGE_DIR(va),
		 pti = PAGE_TABLE(va);

	if(pd->tables_virtual[pdi]){
		void * page = NULL; uint frame;
		
		if(!kernel_heap)
				kernel_panic("La heap del kernel no esta activa");

		page = kmem_alloc_aligned(0x1000);
		frame = physical_address(kernel_dir,(uint) page);
	
		memset(page,0,0x1000);
		
		pd->tables_phys[pdi] = frame | PAGEF_P | PAGEF_RW;
		if(flags & PAGEF_U) pd->tables_phys[pdi] |= PAGEF_U;
		pd->tables_virtual[pdi] = (page_table *) page;
	}

	pd->tables_virtual[pdi]->entries[pti] = pa | flags;
}
