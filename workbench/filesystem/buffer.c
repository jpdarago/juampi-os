/* Buffer struct */
#include "buffer.h"
#include "memory.h"

//Default: Siguiente cluster en el disco
uint consecutive_cluster(uint cluster){
	return cluster+1;
}

//Utilidades de acceso de parametros del buffer
void buffer_set_cluster(fs_hdd_buffer * bf, uint cluster)
{	
	bf->cluster = cluster;
}
void buffer_set_offset(fs_hdd_buffer * bf, uint offset)
{
	bf->consumed = offset;
}
void buffer_set_ncfp(fs_hdd_buffer * bf, uint (*callback)(uint))
{
	bf->next_cluster = callback;
}

//Crea un nuevo buffer indicando el cluster de inicio y tamaño de cluster.
fs_hdd_buffer *
_buffer_create( uint cluster_start, 
				uint cluster_size,
				_fs_buffer_params params)
{
	fs_hdd_buffer * bf = kmem_alloc(sizeof(fs_hdd_buffer));
	
	uint lba_start = cluster_start*cluster_size;
		
	bf->cluster_size = cluster_size;
	bf->top = cluster_size*ATA_SECTSIZE;
	bf->consumed = params.offset; 
	bf->buffer = params.buffer;
	bf->buffer_alloc = false;
	if(!bf->buffer){ 
		bf->buffer = kmem_alloc(bf->top);
		bf->buffer_alloc = true;		
	}
	bf->cluster = cluster_start; //Lba es en sectores
	bf->next_cluster = params.next_cluster;

	hdd_read(lba_start,cluster_size,bf->buffer);
	return bf;
}

//Destruye el buffer
void buffer_destroy(fs_hdd_buffer * bf){
	if(!bf) return;
	if(bf->buffer_alloc)
		kmem_free(bf->buffer);
	kmem_free(bf);
}

//Devuelve si el buffer esta lleno o no.
int buffer_full(fs_hdd_buffer * bf){
	return bf->consumed >= bf->top;
}

//Lee una cantidad de bytes, manteniendo el sector coherente buffereado
//Devuelve la cantidad de bytes leidos o -1 en caso de error
//
//TODO: En vez de requerir un buffer se podria devolver un puntero al mismo
//buffer para mejorar la performance. Asi tambien se podria usar solamente
//un solo tipo de buffer backupeado por HDD.
int buffer_read(fs_hdd_buffer * bf, uint bytes, void * _dump){
	int total_read = 0;
	char * bf_buffer = bf->buffer, * dump = _dump;
	for(;bytes;){
		if(buffer_full(bf)){
			uint next = bf->next_cluster(bf->cluster);
			if(next == 0) return total_read; 
		
			bf->cluster = next;
			uint lba = bf->cluster*bf->cluster_size;
			
			hdd_read(lba,bf->cluster_size,bf->buffer);
			bf->consumed = 0;	
		}
	
		uint read = bytes > bf->top ? bf->top : bytes;
		memcpy(dump+total_read,bf_buffer+bf->consumed,read);
		
		bf->consumed += read; bytes -= read; total_read += read;
	}
	return total_read;
}

int buffer_read_reverse(fs_hdd_buffer *bf, uint bytes, void * _dump){
	int total_read = 0;
	char * bf_buffer = bf->buffer, * dump = _dump;
	for(;bytes;){
		if(bf->consumed == 0){
			uint next = bf->next_cluster(bf->cluster);
			if(next == 0) return total_read;

			bf->cluster = next;
			uint lba = bf->cluster*bf->cluster_size;
			
			hdd_read(lba,bf->cluster_size,bf->buffer);
			bf->consumed = bf->top;
		}

		uint read = bytes > bf->consumed ? bf->consumed : bytes;
		memcpy( dump+total_read, bf_buffer, read + bytes - read);
		
		bf->consumed -= read; bytes -= read; total_read += read;
	}
	return total_read;
}

//Escribe una cantidad de bytes, manteniendo el sector coherente buffereado
//Devuelve la cantidad de bytes leidos o -1 en caso de error
int buffer_write(fs_hdd_buffer * bf, uint bytes, void * _source){
	char * bf_buffer = bf->buffer, * source = _source;
	int total_written = 0;

	for(;bytes;){
		uint to_copy = (bytes > bf->top) ? bf->top : bytes;
		memcpy(bf_buffer+bf->consumed,source+total_written,to_copy);

		//Tenemos que flushear el sector actual
		uint lba = bf->cluster*bf->cluster_size;
		hdd_write(lba,bf->cluster_size,bf_buffer);
	
		bytes -= to_copy;
		total_written += to_copy;	
		bf->consumed += to_copy;

		if(bf->consumed >= bf->top){
			//Bufferear el proximo sector
			uint next = bf->next_cluster(bf->cluster);
			if(next == 0) return total_written;
			
			bf->cluster = next;
			lba = next*bf->cluster_size;
			hdd_read(lba,bf->cluster_size,bf->buffer);
			
			bf->consumed = 0;
		}
	}
	
	return total_written;	
}

//Mueve el buffer a otra posicion
void buffer_reset(fs_hdd_buffer * bf, uint new_cluster, uint offset){
	bf->consumed = offset;
	if(bf->cluster != new_cluster){
		hdd_read(new_cluster,bf->cluster_size,bf->buffer);
	}
	bf->cluster = new_cluster;
}

//Recarga el sector actual
void buffer_refresh(fs_hdd_buffer * bf){
	hdd_read(bf->cluster,bf->cluster_size,bf->buffer);
}

//Print de debuggeo
#define RAYA "================================================================================"
void buffer_debug(fs_hdd_buffer * bf, FILE * out){
	fprintf(out,RAYA "\n");
	fprintf(out,
		"\tbf->cluster      = %d\n"
		"\tbf->consumed     = %d\n"
		"\tbf->cluster_size = %d\n"
		"\tbf->buffer	    = %.20s...\n",
		bf->cluster,bf->consumed,bf->cluster_size,(char*)bf->buffer);
	fprintf(out,RAYA "\n");
}

/* Fin buffer struct */
