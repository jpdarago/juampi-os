#include <buffer_cache.h>
#include <hdd.h>
#include <fs_minix.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define kmalloc malloc
#define kfree free

static struct list_head buffer_list_head;
static sem * buffer_global_mutex;
dirty_ln * dirty_list;

void minix_hdd_read(uint start_block,
	uint blocks, void * data)
{
	hdd_read(start_block*MINIX_BLOCK_BSIZE,
	         blocks*MINIX_BLOCK_BSIZE,
	         data);
}

void minix_hdd_write(uint start_block,
	uint blocks, void * data)
{
	hdd_write(start_block*MINIX_BLOCK_BSIZE,
		blocks*MINIX_BLOCK_BSIZE,data);
}

void buffer_cache_init()
{
	dirty_list = NULL;	
	INIT_LIST_HEAD(&buffer_list_head);
	buffer_global_mutex = NULL;
}

static void add_buffer_to_list(buffer * b)
{
	do_atomic(buffer_global_mutex,
		list_add(&b->buffers,&buffer_list_head);
	);
}

static buffer * buffer_create(uint block_number,uint block_size)
{
	buffer * b = kmalloc(sizeof(buffer));

	b->block  		= block_number;
	b->dirty 		= false;
	b->free			= true;
	b->readers 		= b->writers	= 0;
	b->rw_lock	 	= rw_sem_create();
	b->sem			= sem_create(1);
	b->dlist_ptr	= NULL;

	b->data = kmalloc(block_size);
	INIT_LIST_HEAD(&b->buffers);
	add_buffer_to_list(b);

	return b;
}

void buffer_flush(buffer * b)
{
	do_atomic(b->sem,
		if(b->dirty){
			minix_hdd_write(b->block,1,b->data);
			dirty_ln * dln = b->dlist_ptr; 
			if(dln != NULL){
				if(list_empty(&dirty_list->dlist)){
					dirty_list = NULL;
				}else{
					list_del(&dln->dlist);
				}
				kfree(dln);
			}
			b->dlist_ptr = NULL;
		}
		b->dirty = false;
	);
}

buffer * get_buffer(uint block) 
{
	buffer * res = NULL;
	//sem_wait(buffer_global_mutex);
	if(list_empty(&buffer_list_head))
		goto end;

	buffer * ptr; list_head * l;
	list_head * blh = &buffer_list_head;
	list_for_each(l,blh) {
		ptr = list_entry(l,buffer,buffers);
		//sem_wait(ptr->sem);
		if(ptr->block == block) {
			res = ptr;
			list_move(l,blh);
			buffer_flush(res);
		}
		//sem_signal(ptr->sem);
		if(res) break;
	}
end:
	//sem_signal(buffer_global_mutex);
	return res;
}

buffer * get_free_buffer()
{
	buffer * res = NULL;
	//sem_wait(buffer_global_mutex);
	if(list_empty(&buffer_list_head))
		goto end;

	list_head *l, * blh = &buffer_list_head;
	
	list_for_each_prev(l,blh) {
		buffer * ptr = list_entry(l,buffer,buffers);
		//sem_wait(ptr->sem);
		if(ptr->free) {
			res = ptr;
			res->free = false;
			list_move(l,blh);
			buffer_flush(res);
		}
		//sem_signal(ptr->sem);
		if(res) break;
	}
end:
	//sem_signal(buffer_global_mutex);
	return res;
}

uchar is_free(buffer * ptr, uint block)
{
	return ptr->free;
}
uchar is_block(buffer * ptr,uint block)
{
	return ptr->block == block;
}

void buffer_load(buffer * b, uint block)
{
	do_atomic(b->sem,
		b->block = block;
		minix_hdd_read(block,1,b->data);	
		b->dirty = false;
	);
}

buffer * obtain_buffer(uint block)
{
	buffer * b = get_buffer(block);
	if(b == NULL) {
		b = get_free_buffer(block);
		if(b == NULL) {
			b = buffer_create(block,MINIX_BLOCK_SIZE);
		}
		if(b != NULL) buffer_load(b,block);	
	}
	return b;
}

void mark_buffer(buffer * b)
{		
	if(b->readers + b->writers == 0) 
		b->free = true;
}

void mark_dirty(uint inonum, buffer * b){
	b->dirty = true;
	dirty_ln * d = kmalloc(sizeof(dirty_ln));
	d->inonum = inonum;	
	d->buffer = b;
	b->dlist_ptr = d;
	INIT_LIST_HEAD(&d->dlist);
	if(dirty_list == NULL){ 
		dirty_list = d;
	}else{
		list_add(&d->dlist,&dirty_list->dlist);
	}
}

int buffered_write(uint block, uint offset,
                   uint bytes, uint inonum,
				   void * data)
{
	if(offset >= MINIX_BLOCK_SIZE)
		return -1;

	buffer * b = obtain_buffer(block);	
	do_atomic(b->sem,b->writers++);	
	
	do_atomic_reader(b->rw_lock,
		char * dump = b->data;
		memcpy(dump + offset,data,bytes);
		mark_dirty(inonum,b);
	);

	do_atomic(b->sem,
		b->writers--;
		mark_buffer(b);
	);
	
	return 0;
}

int buffered_read(uint block, uint offset,
	uint bytes, void * data)
{
	if(offset >= MINIX_BLOCK_SIZE)
		return -1;

	buffer * b = obtain_buffer(block);

	do_atomic(b->sem,b->readers++);
	do_atomic_reader(b->rw_lock,
		char * dump = b->data;
		memcpy(data,dump+offset,bytes);
	);
	do_atomic(b->sem,
		b->readers--;
		mark_buffer(b);
	);

	return bytes;
}

void buffers_flush(uint inode_number)
{
	if(dirty_list == NULL) return;
	dirty_ln * dln, * dlntemp;
	list_for_each_entry_safe(dln,dlntemp,
		&dirty_list->dlist,dlist){
		
		buffer_flush(dln->buffer);
	}
}

void buffers_flush_all(uint inode_number)
{
	if(dirty_list == NULL) return;
	dirty_ln * dln, * dlntemp;
	list_for_each_entry_safe(dln,dlntemp,
		&dirty_list->dlist,dlist){
		
		buffer_flush(dln->buffer);
	}
}

void buffer_free(uint block)
{
	buffer * b = get_buffer(block);
	if(b == NULL) return;
	do_atomic(b->sem,	
		if(b->readers == 0 && b->writers == 0)
			b->free = true;	
	);
}

int buffered_write_several(uint start_block,
	uint blocks, uint inonum, void * _data)
{
	char * data = _data;
	int total = 0, last_block = start_block + blocks;
	for(uint block = start_block; block < last_block; ++block) {
		int read = buffered_write(block,0,MINIX_BLOCK_SIZE,
			inonum, data);
		data += read; total += read;
		buffer_free(block);
	}
	return total;
}

int buffered_read_several(uint start_block,
	uint blocks, void * _data)
{
	char * data = _data;
	int total = 0, last_block = start_block + blocks;
	for(uint block = start_block; block < last_block; ++block) {
		int read = buffered_read(block,0,MINIX_BLOCK_SIZE,data);
		data += read; total += read;
		buffer_free(block);
	}
	return total;
}
