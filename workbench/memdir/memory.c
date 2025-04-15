#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <assert.h>

#ifdef DEBUG_FLAG
	#define DEBUG(m,...) fprintf(stderr,m,##__VA_ARGS__)
#else
	#define DEBUG(m,...)
#endif

//El manejador de memoria es una adaptacion minima del propuesto en el K & R.
typedef unsigned int uint;
typedef struct kmem_header{
	struct kmem_header * next;
	uint size;
} kmem_header;

typedef struct {
	kmem_header * freep;
	uint heap_end;
} kmem_map_header;

//Inicializa el manejador de memoria del Kernel para funcionar sobre
//el espacio y tamanio dados.
kmem_map_header * kmem_init(void * memory_start, uint memory_units){
	kmem_map_header * mem = memory_start;
	
	mem->freep = (kmem_header *) ((char*)memory_start+sizeof(kmem_map_header));
	mem->freep->next = mem->freep;
	mem->freep->size = memory_units-sizeof(kmem_map_header)-sizeof(kmem_header);
	mem->heap_end = (uint) memory_start + memory_units;
	
	return mem;
}

static kmem_header * best_fit_prev(kmem_map_header * mh,uint units){
	kmem_header * ptr = mh->freep->next, * best = NULL;
	kmem_header * ptr_prev = mh->freep, * best_prev = NULL;
	kmem_header * stop = mh->freep;
	do{
		if(ptr->size >= units && (!best || ptr->size < best->size)){	
			best = ptr;
			best_prev = ptr_prev;
		}
		ptr = ptr->next;
		ptr_prev = ptr_prev->next;
	}while(ptr_prev != stop);
	return best_prev;
}

//Pide mas memoria
void kmem_free(kmem_map_header*,void*);
static void * kmem_append_core(kmem_map_header * mh, uint units){
	if(units < 1024) units = 1024;
	kmem_header * mem = sbrk(units);
	if(mem == (kmem_header*)-1) 
		return NULL;
	mem->size = units;
	kmem_free(mh,(void*)(mem+1));
	DEBUG("\nNueva memoria %p %x para %x unidades\n",mh->freep,mh->freep->size,units);
	return mh->freep;
}

//Asigna memoria RAM libre de Kernel. Devuelve NULL si no hay memoria
void * kmem_alloc(kmem_map_header * mh, uint size){
	if(size == 0) return NULL;
	//Necesito pedir el espacio para el encabezado tambien.
	uint units = size + sizeof(kmem_header);

	//Encontrar bloque libre del tamanio necesario.
	//Implementacion: Best Fit
	kmem_header * best_prev = best_fit_prev(mh,units), * best;
	if(best_prev == NULL){ 
		kmem_append_core(mh,units);	
		best_prev = best_fit_prev(mh,units);
		if(best_prev == NULL) return NULL;
	}
	best = best_prev->next;
	//Actualizar lista circular de bloques libres
	if(best->size == units){
		if(best == best->next){
			//El bloque es el unico de la lista libre.
			mh->freep = NULL;
		}else{
			mh->freep = best_prev;
			best_prev->next = best->next;
		}
	}else{
		best->size -= units;
		best = (kmem_header *) ((uint) best + best->size);
		best->size = units;
		mh->freep = best_prev;
	}
	return (void*) (best+1);
}

//Libera memoria RAM del Kernel. Es invalido tratar de devolver memoria
//no asignada con malloc.
void kmem_free(kmem_map_header * mh, void * p){
	DEBUG("Liberando a %p\n",p);
	if(p == NULL) return;
	kmem_header * bptr = (kmem_header*)p - 1;
	DEBUG("TAMAÑO %x\n",bptr->size);
	
	//DEBUG("LIBERANDO BLOQUE QUE EMPIEZA EN LA DIRECCION %p DE TAMANIO %d\n",bptr+1,bptr->size);
	//Buscar con que bloque hay que enlazarlo.
	if(!mh->freep){
		//No hay nadie libre. El unico bloque es el de p. A liberarlo
		mh->freep = bptr;
		bptr->next = bptr;
		return;
	}
	
	kmem_header * ptr = mh->freep;
	for(;bptr <= ptr || bptr >= ptr->next; ptr = ptr->next)
		if(ptr >= ptr->next && (bptr > ptr || bptr < ptr->next))
			break;

	//Limpiar los bloques de los extremos que estan libres
	uint bptr_addr = (uint)bptr,ptr_addr = (uint)ptr;
	if(bptr_addr + bptr->size == (uint) ptr->next){
		bptr->size += ptr->next->size;
		bptr->next = ptr->next->next;
	}else{
		bptr->next = ptr->next;
	}
	if(ptr_addr + ptr->size == (uint) bptr){
		ptr->size += bptr->size;
		ptr->next = bptr->next;
	}else{
		ptr->next = bptr;
	}
	//Marcar como nuevo tipo libre, para acelerar.
	mh->freep = ptr;
}

#define MASK_PALIGN ~0xFFF
//Tamanio en bytes de una pagina.
#define PAGE_SIZE 0x1000
//Asigna memoria RAM libre de Kernel, alineada a pagina.
void * kmem_alloc_aligned(kmem_map_header * mh, uint size){
	if(size == 0) return NULL;
	//Necesito pedir el espacio para el encabezado tambien.
	//Pido PAGE_SIZE - 1 adicionales asi me aseguro que en el
	//espacio de memoria obtenido va a haber un trozo alineado.
	uint units = size + PAGE_SIZE - 1 + sizeof(kmem_header);
	//Encontrar bloque libre del tamanio necesario.
	//Implementacion: Best Fit
	kmem_header * best_prev = best_fit_prev(mh,units), * best;
	if(best_prev == NULL){ 
		kmem_append_core(mh,units);
		best_prev = best_fit_prev(mh,units);
		if(best_prev == NULL) return NULL;	
	}
	best = best_prev->next;
	//Obtengo la direccion del pedazo de memoria
	uint best_addr = (uint) best, header_sz = sizeof(kmem_header);
	uint block_addr= (PAGE_SIZE - 1 + header_sz + best_addr) & MASK_PALIGN;
	uint header_addr = block_addr - header_sz;

	uint prev_size = best->size;
	
	kmem_header * header = (kmem_header * ) header_addr;
	header->size = size+header_sz;

	if(best_addr != header_addr){
		//Acomodar el trozo original para que tenga la memoria entre
		//el bloque original y el bloque alineado.
		best->size = header_addr - best_addr - header_sz;
	}else{
		//Como el original esta alineado, solo hay que acomodar para
		//el espacio que queremos y mover best_prev (como pedimos mas espacio
		//que el necesario seguro sobra).
		best_prev->next = (kmem_header *) (block_addr+size);
	}
	if(block_addr + size < best_addr + header_sz + prev_size){
		//Sobra espacio seguido del bloque. Hay que crear un bloque nuevo que
		//contenga este espacio, y agregarlo a la lista de bloques libres.
		kmem_header * new_header = (kmem_header *) (block_addr + size);
		new_header->size = best_addr + prev_size -
					(block_addr + size + header_sz);
		new_header->next = best->next;
		best->next = new_header;
	}
	mh->freep = best_prev;
	return (void *) block_addr;
}

uint kmem_available(kmem_map_header * mh){
	kmem_header * ptr = mh->freep;
	uint total = 0;
	do{
		total += ptr->size;
		ptr = ptr->next;
	}while(ptr != mh->freep);
	return total;
}

//TESTS
#ifdef TEST_FLAG
const char * raya = "================================================================================";
//Revisa que la lista este bien armada

#define RUN_TEST(x,y) { fprintf(stderr,"Corriendo test %s...",(y)); x(); fprintf(stderr,"OK\n"); }
typedef void (*test_t)(void);
#define ASSERT(x,M,...) if(!(x)){ DEBUG("ERROR: %s:%d: " M "\n",__FILE__,__LINE__, ## __VA_ARGS__); assert(0); }

int debug_integrity(kmem_map_header * mh){
	DEBUG("%s\n",raya);
	if(!mh->freep){ 
		DEBUG("%s\n","NO HAY MAS MEMORIA");
		return 1;
	}
	kmem_header * ptr = mh->freep;
	do{
		DEBUG("%p => [%p %x]\n",ptr,ptr->next,ptr->size);
		ptr = ptr->next;
	}while(ptr != mh->freep);
	DEBUG("%s\n",raya);
	return 1;
}

//Memoria a manejar.
#define MEM_SIZE 201 * 0x1000
void * memory;

void test_one_at_a_time(){
	memory = calloc(MEM_SIZE,sizeof(char));
	kmem_map_header * mh = kmem_init(memory,MEM_SIZE);
	
	int overhead = 2*sizeof(kmem_header)+sizeof(kmem_map_header);
	int i,j,sizes[] = {	1000,1,2,1023,123,121,23,512,1024,
				99,139,2000,1999,MEM_SIZE-overhead, 
				1000,60,12,MEM_SIZE-overhead,3,4,5,124,51,500+PAGE_SIZE-1,-1 };
	
	for(i = 0; sizes[i] > 0; i++){
		DEBUG("Pidiendo tamaño %x %x\n",i,sizes[i]);
		char * ptr = (char *) kmem_alloc(mh,sizes[i]);
		debug_integrity(mh);
		ASSERT(sizes[i] < MEM_SIZE && ptr != NULL,"returned pointer %p is invalid.",ptr);
		for(j = 0; j < sizes[i]; j++)
			ptr[j] = (char)i;

		for(j = 0; j < sizes[i]; j++)
			ASSERT(ptr[j] == i,"Expected %d found %d",i,ptr[j]);
		kmem_free(mh,ptr);	
		debug_integrity(mh);
	}
	free(memory);
}

void test_concurrent_asignations(){
	memory = calloc(MEM_SIZE,sizeof(char));
	kmem_map_header * mh = kmem_init(memory,MEM_SIZE);
	int i,j,sizes[] = {1,2,42,4,12,8,300,16,124,512,9,1,1024,PAGE_SIZE-1,500+PAGE_SIZE-1,-1};
	char * ptrs[20];
	for(i = 0; sizes[i] > 0; i++){
		ptrs[i] = (char *) kmem_alloc(mh,sizes[i]);
		debug_integrity(mh);
		for(j = 0; j < sizes[i]; j++){
			ptrs[i][j] = 1+(char)i;
		}
	}
	for(i = 0; sizes[i] > 0; i++)
		for(j = 0; j < sizes[i]; j++)
			ASSERT(ptrs[i][j] == 1+(char)i,"Expected %d found %d\n",1+(char)i,ptrs[i][j]);
	for(i = 0; sizes[i] > 0; i++){
		kmem_free(mh,ptrs[i]);
		debug_integrity(mh);
	}
	free(memory);
}

void test_alternate_frees(){
	memory = calloc(MEM_SIZE,sizeof(char));
	kmem_map_header * mh = kmem_init(memory,MEM_SIZE);
	int i,j,sizes[] = {1,2,4,8,9,16,21,150,123,512,1024,
				MEM_SIZE/2,123,9989,434,12343,1233,998,5564,-1};
	char * ptrs[20];
	for(i = 0; sizes[i] > 0; i++){
		ptrs[i] = (char *) kmem_alloc(mh,sizes[i]);
		debug_integrity(mh);
		for(j = 0; j < sizes[i]; j++){
			ptrs[i][j] = '0';
		}
		if(i % 2 == 0){ 
			kmem_free(mh,ptrs[i]);
			debug_integrity(mh);
			ptrs[i] = NULL;
		}
	}
	for(i = 0; sizes[i] > 0; i++){
		kmem_free(mh,ptrs[i]);
		debug_integrity(mh);
	}
	free(memory);
}

void test_get_aligned_memory(){
	memory = calloc(MEM_SIZE,sizeof(char));
	kmem_map_header * mh = kmem_init(memory,MEM_SIZE);
	int i,j,sizes[] = {1,2,3,4,5,20,3,50,30,-1};
	DEBUG("%s\n","ANTES DE EMPEZAR");
	debug_integrity(mh);
	for(i = 0; sizes[i] > 0; i++){
		DEBUG("OBTENIENDO %x BYTES\n",sizes[i]*PAGE_SIZE);
		char * ptr = (char *) kmem_alloc_aligned(mh,sizes[i]*PAGE_SIZE);
		debug_integrity(mh);
		DEBUG("DIRECCION DEVUELTA: %p\n",ptr);
		uint address = (uint) ptr;
		assert(ptr != NULL && (address & MASK_PALIGN) == address);
		for(j = 0; j < sizes[i]; j++)
			ptr[j] = 1+(char)i;

		DEBUG("LIBERANDO LOS %x BYTES\n",sizes[i]*PAGE_SIZE);
		kmem_free(mh,ptr);
		debug_integrity(mh);
	}
	free(memory);
}

void test_get_aligned_several(){
	memory = calloc(MEM_SIZE,sizeof(char));
	kmem_map_header * mh = kmem_init(memory,MEM_SIZE);
	int i,j,sizes[] = {1,2,3,4,5,10,20,30,-1};
	char * ptrs[8];
	DEBUG("%s\n","ANTES DE EMPEZAR");
	debug_integrity(mh);

	for(i = 0; sizes[i] > 0; i++){
		DEBUG("OBTENIENDO %x BYTES\n",sizes[i]*PAGE_SIZE);

		char * ptr = (char *) kmem_alloc_aligned(mh,sizes[i]*PAGE_SIZE);
		ptrs[i] = ptr;

		debug_integrity(mh);
		DEBUG("DIRECCION DEVUELTA: %p\n",ptr);

		uint address = (uint) ptr;
		ASSERT(ptr != NULL && (address & MASK_PALIGN) == address, "Address %p is NULL or not aligned",ptr);
		for(j = 0; j < sizes[i]*PAGE_SIZE; j++)
			ptr[j] = 1+(char)i;
	}
	for(i = 0; sizes[i] > 0; ++i)
		for(j = 0; j < sizes[i]*PAGE_SIZE; j++)
			ASSERT(ptrs[i][j] == 1+(char)i,"Expected %d found %d",1+(char)i,ptrs[i][j]);

	free(memory);
}

void test_get_aligned_and_not(){
	memory = calloc(MEM_SIZE,sizeof(char));
	kmem_map_header * mh = kmem_init(memory,MEM_SIZE);
	int i, j,sizes[] = {1,2,3,4,PAGE_SIZE,500,241,878,2*PAGE_SIZE,6*PAGE_SIZE,60,123,1023,6*PAGE_SIZE,-1};
	DEBUG("%s\n","ANTES DE EMPEZAR");
	debug_integrity(mh);
	for(i = 0; sizes[i] > 0; i++){
		char * ptr;
		DEBUG("OBTENIENDO %x BYTES",sizes[i]);
		if(sizes[i] & 0xFFF){
			DEBUG("%s\n","SIN ALINEAR\n");
			ptr = (char *) kmem_alloc(mh,sizes[i]);
			DEBUG("DIRECCION DEVUELTA: %p\n",ptr);

			debug_integrity(mh);
			ASSERT(ptr != NULL,"Pointer ptr is NULL");
		}else{
			DEBUG("%s\n","ALINEADOS\n");
			ptr = (char *) kmem_alloc_aligned(mh,sizes[i]);
			DEBUG("DIRECCION DEVUELTA: %p\n",ptr);

			uint address = (uint) ptr;
			debug_integrity(mh);
			ASSERT(ptr != NULL && (address & MASK_PALIGN) == address,"Address %p is NULL or not aligned",ptr);
		}
		for(j = 0; j < sizes[i]; j++)
			ptr[j] = 1+(char)i;
	}
	free(memory);
}

void test_array_of_arrays(){
	memory = calloc(MEM_SIZE, sizeof(char));
	kmem_map_header * mh = kmem_init(memory,MEM_SIZE);

	unsigned int i,j, av = kmem_available(mh);
	unsigned int size = 100;
	unsigned int** ptrs = (unsigned int **) kmem_alloc(mh,size*sizeof(uint*));
	for(i = 0; i < size; i++){
		ptrs[i] = (unsigned int*) kmem_alloc(mh,size*sizeof(uint));
		for(j = 0; j < size; j++)
			ptrs[i][j] = size*i+j;
	}
	for(i = 0; i < size; i++)
		for(j = 0; j < size; j++)
			ASSERT(ptrs[i][j] == size*i+j, "Expected %d found %d\n",size*i+j,ptrs[i][j]);

	for(i = 0; i < size; i++)
		kmem_free(mh,ptrs[i]);
	kmem_free(mh,ptrs);
	ASSERT(kmem_available(mh) == av, "Memory was not completely freed.\n");
}

void test_available_memory_invariant(){
	memory = calloc(MEM_SIZE,sizeof(char));
	kmem_map_header * mh = kmem_init(memory,MEM_SIZE);
	uint available_start = kmem_available(mh);

	int i,sizes[] = {1,2,4,8,16,23,4312,121,444,32,765,11,-1};
	for(i = 0; sizes[i] > 0; i++){
		char * ptr = (char *) kmem_alloc(mh,sizes[i]);
		kmem_free(mh,ptr);
	}
	ASSERT(kmem_available(mh) == available_start, "Expected %d found %d",available_start,kmem_available(mh));
	free(memory);
}

void test_request_memory(){
	memory = malloc(MEM_SIZE);
	kmem_map_header * mh = kmem_init(memory,MEM_SIZE);
	
	unsigned char * ptr = kmem_alloc(mh,2*MEM_SIZE);
	ASSERT(ptr != NULL,"the pointer is NULL");
	for(int i = 0; i < 2*MEM_SIZE; i++) ptr[i] = 0xEF;	
	for(int i = 0; i < 2*MEM_SIZE; i++)
		ASSERT(ptr[i] == 0xEF,"invalid memory at position %d has value %d",i,ptr[i]);
	kmem_free(mh,ptr);
}

static test_t tests[] = {
	test_one_at_a_time,
	test_concurrent_asignations,
	test_alternate_frees,
	test_get_aligned_memory,
	test_get_aligned_several,
	test_get_aligned_and_not,
	test_available_memory_invariant,
	test_array_of_arrays,
	test_request_memory,
	NULL
};
static char * messages[] = {
	"asignacion de a una",
	"asignaciones todas al mismo tiempo",
	"asignaciones alternadas con liberaciones",
	"asignaciones de memoria alineada a pagina",
	"asignaciones de memoria alineada a pagina todas seguidas",
	"asignaciones alineadas y no alineadas, al mismo tiempo",
	"memoria es invariante si se libera todo lo pedido",
	"arreglo de arreglos",
	"tiene que pedir memoria"
};
int main(int argc, char * argv[]){
	int i;
	if(argc == 1){
		for(i = 0; tests[i]; i++){
			RUN_TEST(tests[i],messages[i]);
		}
	}else{
		i = atoi(argv[1]);
		RUN_TEST(tests[i],messages[i]);
	}
	return 0;
}
#endif
