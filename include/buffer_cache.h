#ifndef __BUFFER_CACHE_H
#define __BUFFER_CACHE_H

#include <types.h>
#include <sem.h>
#include <rw_sem.h>
#include <klist.h>

typedef struct dirty_ln dirty_ln;

typedef struct buffer {
    void * data;

    uint block;
    uint block_size;

    bool dirty,free;
    uint readers;
    uint writers;

    sem * sem;
    rw_sem * rw_lock;

    struct list_head buffers;
    dirty_ln * dlist_ptr;
} buffer;


struct dirty_ln {
    uint inonum;
    buffer * buffer;
    struct list_head dlist;
};

void minix_hdd_read(uint start_block,uint blocks, void * data);
void minix_hdd_write(uint start_block,uint blocks, void * data);

void buffer_cache_init();
int buffered_write(uint, uint block, uint offset, uint, void * data);
int buffered_read(uint block, uint offset, uint, void * data);
void buffers_flush_all();
void buffers_flush(uint inode_number);

int buffered_read_several(uint, uint, void *);
int buffered_write_several(uint, uint, uint, void *);

#endif
