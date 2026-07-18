#include <memory.h>
#include <panic.h>
#include <utils.h>

#include <stdint.h>

#define SLAB_SIZE 0x10000u // 64 KiB
#define SLAB_SHIFT 16
#define MAX_SMALL 8192 // largest size class; above this -> large (multi-slab)
#define LARGE_CLASS 0xFFFFFFFFu

// One descriptor per slab. For a small slab it names the size class and threads
// that slab's free blocks; for the first slab of a large run it records the run
// length. `next` links a slab into either its class partial list or the
// free-run list.
struct slab {
    slab* next;
    void* free_list;     // intra-slab free blocks (small slabs)
    uint32_t class_idx;  // size-class index, or LARGE_CLASS
    uint32_t free_count; // free blocks in this slab (small)
    uint32_t nslabs;     // contiguous slabs in this run (>=1)
};

// Size classes, all multiples of 16 so every block is 16-byte aligned (block
// address = 64 KiB-aligned slab base + i * class_size).
static const uint32_t class_size[HEAP_NCLASSES] = {
        16,  32,  48,   64,   96,   128,  192,  256,  384,
        512, 768, 1024, 1536, 2048, 3072, 4096, 6144, 8192,
};

static uint32_t size_to_class(ptrdiff_t n)
{
    for (uint32_t i = 0; i < HEAP_NCLASSES; i++) {
        if ((ptrdiff_t)class_size[i] >= n) {
            return i;
        }
    }
    return LARGE_CLASS;
}

static slab* slab_of(heap_allocator* h, void* p)
{
    uintptr_t off = (uintptr_t)p - (uintptr_t)h->base_addr;
    return &h->descs[off >> SLAB_SHIFT];
}

static void* slab_base(heap_allocator* h, slab* s)
{
    return h->base_addr + (size_t)(s - h->descs) * SLAB_SIZE;
}

// Reserve `n` contiguous fresh slabs from the bump region.
static slab* carve(heap_allocator* h, uint32_t n)
{
    if (h->bump + (size_t)n * SLAB_SIZE > h->end) {
        kernel_panic("Kernel heap out of memory");
    }
    slab* s = slab_of(h, h->bump);
    h->bump += (size_t)n * SLAB_SIZE;
    return s;
}

static void* alloc_small(heap_allocator* h, ptrdiff_t n)
{
    uint32_t c = size_to_class(n);
    slab* s = h->partial[c];
    if (s == NULL) {
        // No slab of this class has a free block: carve a fresh one and thread
        // all its blocks onto its free list.
        s = carve(h, 1);
        uint32_t bs = class_size[c];
        uint32_t count = SLAB_SIZE / bs;
        char* base = slab_base(h, s);
        void* fl = NULL;
        for (uint32_t i = count; i-- > 0;) {
            void* blk = base + (size_t)i * bs;
            *(void**)blk = fl;
            fl = blk;
        }
        s->free_list = fl;
        s->free_count = count;
        s->class_idx = c;
        s->nslabs = 1;
        s->next = NULL;
        h->partial[c] = s;
    }

    void* blk = s->free_list;
    s->free_list = *(void**)blk;
    s->free_count--;
    if (s->free_count == 0) { // slab now full: drop it from the partial list
        h->partial[c] = s->next;
        s->next = NULL;
    }
    return memset(blk, 0, n);
}

static void free_small(heap_allocator* h, slab* s, void* p)
{
    bool was_full = (s->free_count == 0);
    *(void**)p = s->free_list;
    s->free_list = p;
    s->free_count++;
    if (was_full) { // slab re-enters its class's partial list
        s->next = h->partial[s->class_idx];
        h->partial[s->class_idx] = s;
    }
}

static void* alloc_large(heap_allocator* h, ptrdiff_t total)
{
    uint32_t n = (uint32_t)(((size_t)total + SLAB_SIZE - 1) / SLAB_SIZE);

    // First fit over the free-run list, splitting a larger run if found.
    for (slab** pp = &h->free_runs; *pp != NULL; pp = &(*pp)->next) {
        slab* r = *pp;
        if (r->nslabs < n) {
            continue;
        }
        *pp = r->next;
        if (r->nslabs > n) {
            slab* rem = r + n;
            rem->nslabs = r->nslabs - n;
            rem->class_idx = LARGE_CLASS;
            rem->next = h->free_runs;
            h->free_runs = rem;
        }
        r->nslabs = n;
        r->class_idx = LARGE_CLASS;
        return memset(slab_base(h, r), 0, total);
    }

    slab* s = carve(h, n);
    s->class_idx = LARGE_CLASS;
    s->nslabs = n;
    return memset(slab_base(h, s), 0, total);
}

static void* heap_alloc(allocator* a, ptrdiff_t size, ptrdiff_t align,
                        ptrdiff_t count)
{
    heap_allocator* h = (heap_allocator*)a;
    if (align > 16) {
        // Everything the heap returns is 16-aligned; larger alignment (whole
        // pages) belongs to the frame allocator.
        kernel_panic("Heap alignment larger than 16");
    }
    if (size <= 0 || count < 0 || (count > 0 && size > PTRDIFF_MAX / count)) {
        kernel_panic("Invalid heap allocation size");
    }
    ptrdiff_t total = size * count;
    if (total == 0) {
        total = 1;
    }
    return total <= MAX_SMALL ? alloc_small(h, total) : alloc_large(h, total);
}

heap_allocator heap_init(void* beg, ptrdiff_t size)
{
    heap_allocator h;
    h.base.alloc = heap_alloc;

    // Lay the descriptor array (one entry per potential slab) at the front of
    // the region, then start the slabs at the next 64 KiB boundary.
    uintptr_t start = (uintptr_t)beg;
    uint32_t max_slabs = (uint32_t)((size_t)size / SLAB_SIZE);
    h.descs = (slab*)beg;
    uintptr_t after = start + (uintptr_t)max_slabs * sizeof(slab);
    uintptr_t base = (after + SLAB_SIZE - 1) & ~(uintptr_t)(SLAB_SIZE - 1);

    h.base_addr = (char*)base;
    h.bump = (char*)base;
    h.end = (char*)((start + (uintptr_t)size) & ~(uintptr_t)(SLAB_SIZE - 1));
    for (uint32_t i = 0; i < HEAP_NCLASSES; i++) {
        h.partial[i] = NULL;
    }
    h.free_runs = NULL;
    return h;
}

void heap_free(heap_allocator* h, void* p)
{
    if (p == NULL) {
        return;
    }
    slab* s = slab_of(h, p);
    if (s->class_idx == LARGE_CLASS) {
        s->next = h->free_runs; // add the run back to the free-run list
        h->free_runs = s;
    } else {
        free_small(h, s, p);
    }
}

size_t heap_usable_size(heap_allocator* h, void* p)
{
    slab* s = slab_of(h, p);
    if (s->class_idx == LARGE_CLASS) {
        return (size_t)s->nslabs * SLAB_SIZE;
    }
    return class_size[s->class_idx];
}

static heap_allocator* g_default_heap;
void heap_set_default(heap_allocator* h)
{
    g_default_heap = h;
}
heap_allocator* heap_default(void)
{
    return g_default_heap;
}
