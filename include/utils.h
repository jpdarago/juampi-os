#ifndef __SYSTEM_H
#define __SYSTEM_H

#include <types.h>

void memcpy(void* dest, const void* src, uint count);
void memset(void* dest, uchar val, uint count);

#define CEIL(a, b) (((a) + (b) - 1) / (b))

#endif
