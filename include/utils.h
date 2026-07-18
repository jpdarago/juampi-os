#ifndef __SYSTEM_H
#define __SYSTEM_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

// Standard C signatures: GCC may emit implicit calls to these for aggregate
// copies/zeroing even under -fno-builtin, and it assumes the standard ABI.
void* memset(void* dest, int val, size_t count);
void* memcpy(void* dest, const void* src, size_t count);

#define CEIL(a, b) (((a) + (b) - 1) / (b))

#endif
