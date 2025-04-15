#ifndef __FRAME_ALLOC_H
#define __FRAME_ALLOC_H

#include <types.h>

uint frame_alloc_init(void * mem_start,uint);
uint frame_alloc(void);
void frame_free(uint frame);
uint frames_available(void);
void frame_add_alias(uint frame);

#endif
