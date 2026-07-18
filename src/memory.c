#include <memory.h>
#include <panic.h>
#include <utils.h>

// The kernel heap is a minimal adaptation of the K&R free-list allocator,
// exposed through the alloc.h interface (size/align/count, zeroed memory,
// panic on OOM). Every block — header included — is a multiple of 16 bytes and
// starts 16-aligned, so payloads satisfy any fundamental alignment; requests
// with a larger alignment are a bug (page-aligned memory comes from the frame
// allocator, not the heap).

#define HEAP_ALIGN 16
#define ROUND_UP(x) (((x) + HEAP_ALIGN - 1) & ~(ptrdiff_t)(HEAP_ALIGN - 1))

// Best-fit search: returns the node whose *next* block is the smallest free
// block that fits `units` bytes, or NULL if none fits.
static kmem_header* best_fit_prev(heap_allocator* h, ptrdiff_t units)
{
    if (h->freep == NULL) {
        return NULL;
    }
    kmem_header *ptr = h->freep->next, *best = NULL;
    kmem_header *ptr_prev = h->freep, *best_prev = NULL;
    kmem_header* stop = h->freep;
    do {
        if (ptr->size >= units && (!best || ptr->size < best->size)) {
            best = ptr;
            best_prev = ptr_prev;
        }
        ptr = ptr->next;
        ptr_prev = ptr_prev->next;
    } while (ptr_prev != stop);
    return best_prev;
}

static void* heap_alloc(allocator* a, ptrdiff_t size, ptrdiff_t align,
                        ptrdiff_t count)
{
    heap_allocator* h = (heap_allocator*)a;
    if (align > HEAP_ALIGN) {
        kernel_panic("Heap allocation with alignment larger than 16");
    }
    if (size <= 0 || count < 0 ||
        (size > 0 && count > (PTRDIFF_MAX - HEAP_ALIGN) / size)) {
        kernel_panic("Invalid heap allocation size");
    }
    ptrdiff_t units = ROUND_UP(size * count + (ptrdiff_t)sizeof(kmem_header));

    kmem_header *best_prev = best_fit_prev(h, units), *best;
    if (best_prev == NULL) {
        kernel_panic("Kernel heap out of memory");
    }
    best = best_prev->next;
    if (best->size - units < (ptrdiff_t)sizeof(kmem_header) + HEAP_ALIGN) {
        // Too small to split: hand out the whole block.
        if (best == best->next) {
            h->freep = NULL; // it was the only free block
        } else {
            h->freep = best_prev;
            best_prev->next = best->next;
        }
    } else {
        // Split: shrink the free block and carve the tail off for the caller.
        best->size -= units;
        best = (kmem_header*)((char*)best + best->size);
        best->size = units;
        h->freep = best_prev;
    }
    return memset(best + 1, 0, units - sizeof(kmem_header));
}

heap_allocator heap_init(void* beg, ptrdiff_t size)
{
    heap_allocator h;
    h.base.alloc = heap_alloc;
    h.heap_end = (char*)beg + size;
    h.freep = beg;
    h.freep->next = h.freep;
    h.freep->size = size;
    return h;
}

void heap_free(heap_allocator* h, void* p)
{
    if (p == NULL) {
        return;
    }
    kmem_header* bptr = (kmem_header*)p - 1;
    if (!h->freep) {
        // Nothing else is free; p's block becomes the whole free list.
        h->freep = bptr;
        bptr->next = bptr;
        return;
    }
    // Find the free block after which this one belongs (list is address-
    // ordered and circular).
    kmem_header* ptr = h->freep;
    for (; bptr <= ptr || bptr >= ptr->next; ptr = ptr->next)
        if (ptr >= ptr->next && (bptr > ptr || bptr < ptr->next)) {
            break;
        }
    // Coalesce with the following and preceding blocks when adjacent.
    if ((char*)bptr + bptr->size == (char*)ptr->next) {
        bptr->size += ptr->next->size;
        bptr->next = ptr->next->next;
    } else {
        bptr->next = ptr->next;
    }
    if ((char*)ptr + ptr->size == (char*)bptr) {
        ptr->size += bptr->size;
        ptr->next = bptr->next;
    } else {
        ptr->next = bptr;
    }
    h->freep = ptr;
}
