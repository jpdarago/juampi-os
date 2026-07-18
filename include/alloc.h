#ifndef __ALLOC_H
#define __ALLOC_H

#include <stddef.h>
#include <stdalign.h>

// Generic allocation interface, after "Arena allocator tips and tricks"
// (https://nullprogram.com/blog/2023/12/17/): an allocation is (size, align,
// count), the returned memory is zeroed, and out-of-memory is a kernel panic —
// call sites never check for NULL. Modules that need memory take an allocator*
// parameter rather than calling a global allocator; concrete implementations
// (the kernel heap in memory.h, the arena in arena.h) embed this struct first
// and fill in the callback.
typedef struct allocator allocator;
struct allocator {
    void* (*alloc)(allocator* a, ptrdiff_t size, ptrdiff_t align,
                   ptrdiff_t count);
};

static inline void* alloc(allocator* a, ptrdiff_t size, ptrdiff_t align,
                          ptrdiff_t count)
{
    return a->alloc(a, size, align, count);
}

// Typed allocation: new(a, int, 16) -> int* to 16 zeroed ints.
#define new(a, t, n) ((t*)alloc((a), sizeof(t), alignof(t), (n)))

#endif
