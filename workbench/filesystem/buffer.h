#ifndef __BUFFER_H
#define __BUFFER_H

#include "hdd.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

typedef struct {
	//Cantidad de bytes en buffer
	uint top;
	//Tamaño del cluster en sectores
	uint cluster_size;
	//Cluster actual
	uint cluster;
	//Bytes consumidos del buffer
	uint consumed;
	//Buffer de datos
	void * buffer;
	//Funcion para determinar proximo cluster
	uint (*next_cluster)(uint);
	//Dice si el buffer fue creado dinamicamente o pasado por
	//parametro
	uint buffer_alloc;
} fs_hdd_buffer;

typedef struct {
	void * buffer;
	uint (*next_cluster)(uint);
	uint offset;
} _fs_buffer_params;

uint consecutive_cluster(uint cluster);

fs_hdd_buffer * _buffer_create(uint cluster_start, uint cluster_size,
	_fs_buffer_params params);

#define buffer_create(cluster_start,cluster_size, ...) \
	_buffer_create(cluster_start,cluster_size, \
		(_fs_buffer_params) { \
					.buffer = NULL,\
					.next_cluster = consecutive_cluster,\
					.offset = 0, __VA_ARGS__ })

void buffer_set_cluster(fs_hdd_buffer *, uint);
void buffer_set_offset(fs_hdd_buffer *, uint);
void buffer_set_ncfp(fs_hdd_buffer *, uint (*callback)(uint));

void buffer_destroy(fs_hdd_buffer * bf);
int buffer_full(fs_hdd_buffer * bf);
int buffer_read(fs_hdd_buffer *, uint, void *);
int buffer_read_reverse(fs_hdd_buffer *, uint, void *);
int buffer_write(fs_hdd_buffer * bf,uint bytes, void * source);
void buffer_reset(fs_hdd_buffer * bf, uint new_lba, uint offset);
void buffer_debug(fs_hdd_buffer * bf, FILE * f);
void buffer_refresh(fs_hdd_buffer * bf);

#define buffer_readobj(bf,l) buffer_read(bf,sizeof(l),&(l))
#define buffer_writeobj(bf,l) buffer_write(bf,sizeof(l),&(l))

#endif
