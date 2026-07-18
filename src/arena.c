#include <arena.h>
#include <panic.h>
#include <utils.h>

#include <stdint.h>

static void* arena_alloc(allocator* a, ptrdiff_t size, ptrdiff_t align,
                         ptrdiff_t count)
{
    arena* ar = (arena*)a;
    ptrdiff_t padding = -(uintptr_t)ar->beg & (align - 1);
    ptrdiff_t available = ar->end - ar->beg - padding;
    if (available < 0 || count > available / size) {
        kernel_panic("Arena out of memory");
    }
    void* p = ar->beg + padding;
    ar->beg += padding + count * size;
    return memset(p, 0, count * size);
}

arena arena_init(void* beg, ptrdiff_t size)
{
    arena a;
    a.base.alloc = arena_alloc;
    a.beg = beg;
    a.end = (char*)beg + size;
    return a;
}
