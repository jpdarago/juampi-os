#include <memory.h>
#include <frames.h>
#include <paging.h>
#include <scrn.h>
#include <exception.h>

// The memory manager is a minimal adaptation of the one proposed in the K & R.
// Initializes the Kernel memory manager to work over
// the given space and size.
kmem_map_header* kmem_init(void* memory_start, int memory_units)
{
    kmem_map_header* mem = memory_start;
    mem->freep = (kmem_header*)((char*)memory_start + sizeof(kmem_map_header));
    mem->freep->next = mem->freep;
    mem->freep->size =
            memory_units - sizeof(kmem_map_header) - sizeof(kmem_header);
    mem->heap_end = (intptr)memory_start + memory_units;
    return mem;
}

static kmem_header* best_fit_prev(kmem_map_header* mh, int units)
{
    kmem_header *ptr = mh->freep->next, *best = NULL;
    kmem_header *ptr_prev = mh->freep, *best_prev = NULL;
    kmem_header* stop = mh->freep;
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

// Requests more memory
static void* kmem_append_core(kmem_map_header* mh, uint units)
{
    uint pages = (units + 0xFFF) / 0x1000;
    if (!pages) {
        pages = 1;
    }
    kmem_header* mem = paging_append_core(mh, pages);
    mem->size = pages * 0x1000;
    mem->next = NULL;
    kmem_free(mh, (void*)mem);
    return mh->freep;
}

// Allocates free Kernel RAM memory. Returns NULL if there is no memory
void* kmem_alloc(kmem_map_header* mh, int size)
{
    if (size <= 0) {
        return NULL;
    }
    // I also need to request the space for the header.
    int units = size + sizeof(kmem_header);
    // Find a free block of the necessary size.
    // Implementation: Best Fit
    kmem_header *best_prev = best_fit_prev(mh, units), *best;
    if (best_prev == NULL) {
        kmem_append_core(mh, units);
        best_prev = best_fit_prev(mh, units);
        if (best_prev == NULL) {
            kernel_panic("No more kernel heap");
            return NULL;
        }
    }
    best = best_prev->next;
    // Update the circular list of free blocks
    if (best->size == units) {
        if (best == best->next) {
            // The block is the only one in the free list.
            mh->freep = NULL;
        } else {
            mh->freep = best_prev;
            best_prev->next = best->next;
        }
    } else {
        best->size -= units;
        best = (kmem_header*)((intptr)best + best->size);
        best->size = units;
        mh->freep = best_prev;
    }
    return (void*)(best + 1);
}

// Frees Kernel RAM memory. It is invalid to try to return memory
// not allocated with malloc.
void kmem_free(kmem_map_header* mh, void* p)
{
    if (p == NULL) {
        return;
    }
    kmem_header* bptr = (kmem_header*)p - 1;
    // Search for which block it should be linked to.
    if (!mh->freep) {
        // No one is free. The only block is p's. Free it
        mh->freep = bptr;
        bptr->next = bptr;
        return;
    }
    kmem_header* ptr = mh->freep;
    for (; bptr <= ptr || bptr >= ptr->next; ptr = ptr->next)
        if (ptr >= ptr->next && (bptr > ptr || bptr < ptr->next)) {
            break;
        }
    // Clean up the end blocks that are free
    intptr bptr_addr = (intptr)bptr, ptr_addr = (intptr)ptr;
    if (bptr_addr + bptr->size == (intptr)ptr->next) {
        bptr->size += ptr->next->size;
        bptr->next = ptr->next->next;
    } else {
        bptr->next = ptr->next;
    }
    if (ptr_addr + ptr->size == (intptr)bptr) {
        ptr->size += bptr->size;
        ptr->next = bptr->next;
    } else {
        ptr->next = bptr;
    }
    // Mark as the new free entry, to speed things up.
    mh->freep = ptr;
}

// Allocates free Kernel RAM memory, page-aligned.
void* kmem_alloc_aligned(kmem_map_header* mh, int size)
{
    if (size == 0) {
        return NULL;
    }
    // I also need to request the space for the header.
    // I request an additional PAGE_SIZE - 1 so that I ensure that in the
    // obtained memory space there will be an aligned chunk.
    uint units = size + PAGE_SZ - 1 + sizeof(kmem_header);
    // Find a free block of the necessary size.
    // Implementation: Best Fit
    kmem_header *best_prev = best_fit_prev(mh, units), *best;
    if (best_prev == NULL) {
        kmem_append_core(mh, units);
        best_prev = best_fit_prev(mh, units);
        if (best_prev == NULL) {
            kernel_panic("No more memory to allocate aligned");
            return NULL;
        }
    }
    best = best_prev->next;
    // Get the address of the chunk of memory
    intptr best_addr = (intptr)best, header_sz = sizeof(kmem_header);
    intptr block_addr = ALIGN(PAGE_SZ - 1 + header_sz + best_addr);
    intptr header_addr = block_addr - header_sz;
    intptr prev_size = best->size;
    kmem_header* header = (kmem_header*)header_addr;
    header->size = size + header_sz;
    if (best_addr != header_addr) {
        // Adjust the original chunk so that it holds the memory between
        // the original block and the aligned block.
        best->size = header_addr - best_addr - header_sz;
    } else {
        // Since the original is aligned, we only need to adjust for
        // the space we want and move best_prev (since we requested more space
        // than necessary there is surely leftover).
        best_prev->next = (kmem_header*)(block_addr + size);
    }
    if (block_addr + size < best_addr + header_sz + prev_size) {
        // There is leftover space following the block. We need to create a new
        // block that contains this space, and add it to the list of free
        // blocks.
        kmem_header* new_header = (kmem_header*)(block_addr + size);
        new_header->size =
                best_addr + prev_size - (block_addr + size + header_sz);
        new_header->next = best->next;
        best->next = new_header;
    }
    mh->freep = best_prev;
    return (void*)block_addr;
}

uint kmem_available(kmem_map_header* mh)
{
    kmem_header* ptr = mh->freep;
    if (!ptr) {
        return 0;
    }
    uint total = 0;
    do {
        total += ptr->size;
        ptr = ptr->next;
    } while (ptr != mh->freep);
    return total;
}

void* kmalloc(uint size)
{
    kmem_map_header* kernel_heap = get_kernel_heap();
    void* res = kmem_alloc(kernel_heap, size);
    if (res == NULL)
        kernel_panic("No more memory to "
                     "alloc %d bytes\n",
                     size);
    return res;
}

void kfree(void* mem)
{
    if (mem == NULL)
        return;
    kmem_map_header* kernel_heap = get_kernel_heap();
    kmem_free(kernel_heap, mem);
}
