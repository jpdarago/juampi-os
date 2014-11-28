#ifndef __FRAME_ALLOC_H
#define __FRAME_ALLOC_H

#include "types.h"

void frame_alloc_init(void * mem_start,uint);
uint frame_alloc();
void frame_free(uint frame); 

#endif
