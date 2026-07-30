#ifndef __DMA_H
#define __DMA_H

#include <frames.h>
#include <paging.h>
#include <utils.h>

#include <stdint.h>

// A device-visible memory buffer: the kernel pointer and the physical address
// device registers/descriptors need, kept together so the pair can't drift
// apart. Drivers allocate these one page at a time from the frame allocator.
struct dma_buf {
    void* va;     // kernel view (through the HHDM)
    uintptr_t pa; // what the device DMAs to/from
};

// Allocate one zeroed, page-aligned DMA page. Page alignment satisfies the
// descriptor/ring alignment rules of every device we drive (NVMe, xHCI).
static inline struct dma_buf dma_page_alloc(void)
{
    struct dma_buf b;
    b.pa = frame_alloc();
    b.va = phys_to_virt(b.pa);
    memset(b.va, 0, PAGE_SZ);
    return b;
}

static inline void dma_page_free(struct dma_buf b)
{
    frame_free(b.pa);
}

#endif
