#include <frames.h>
#include <bitset.h>
#include <paging.h>
#include <panic.h>
#include <utils.h>

#define FRAME_SZ 0x1000
#define FRAME_ALIGN(x) (((x) + FRAME_SZ - 1) & ~(uintptr_t)0xFFF)

// Bitmap of frame usage (1 = used), stored in the HHDM view of the managed
// region itself. (The 32-bit kernel also kept per-frame refcounts for
// copy-on-write aliasing; that returns with fork/COW.)
static bitset b;
static uintptr_t mem_start;  // physical base of the managed region
static uint32_t total_count; // free frames

void frames_init(uintptr_t phys_base, uintptr_t len)
{
    uintptr_t base = FRAME_ALIGN(phys_base);
    uint32_t frames = (uint32_t)(len / FRAME_SZ);
    mem_start = base;

    // The bitmap is stored at the start of the region (through the HHDM); the
    // frames it occupies are then marked used.
    void* meta = phys_to_virt(base);
    void* meta_end = bitset_init(&b, meta, frames);

    uintptr_t meta_bytes = (uintptr_t)meta_end - (uintptr_t)meta;
    uint32_t needed = (uint32_t)CEIL(meta_bytes, FRAME_SZ);
    for (uint32_t i = 0; i < needed; i++) {
        bitset_set(&b, i);
    }
    total_count = frames - needed;
}

uintptr_t frame_alloc(void)
{
    uint32_t offset = bitset_search(&b);
    if (offset == (uint32_t)-1) {
        kernel_panic("No frames available to use");
    }
    bitset_set(&b, offset);
    total_count--;
    return mem_start + (uintptr_t)FRAME_SZ * offset;
}

void frame_free(uintptr_t frame)
{
    if (frame < mem_start) {
        kernel_panic("Attempted to free an invalid frame");
    }
    uint32_t offset = (uint32_t)((frame - mem_start) / FRAME_SZ);
    bitset_clear(&b, offset);
    total_count++;
}

uintptr_t frames_available(void)
{
    return total_count;
}
