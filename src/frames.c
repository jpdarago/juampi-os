#include <frames.h>
#include <bitset.h>
#include <paging.h>
#include <panic.h>
#include <utils.h>

#define FRAME_SZ 0x1000
#define FRAME_ALIGN(x) (((x) + FRAME_SZ - 1) & ~(uintptr)0xFFF)

// Bitmap of frame usage (1 = used), stored in the HHDM view of the managed
// region itself. (The 32-bit kernel also kept per-frame refcounts for
// copy-on-write aliasing; that returns with fork/COW.)
static bitset b;
static uintptr mem_start; // physical base of the managed region
static uint total_count;  // free frames

void frames_init(uintptr phys_base, uintptr len)
{
    uintptr base = FRAME_ALIGN(phys_base);
    uint frames = (uint)(len / FRAME_SZ);
    mem_start = base;

    // The bitmap is stored at the start of the region (through the HHDM); the
    // frames it occupies are then marked used.
    void* meta = phys_to_virt(base);
    void* meta_end = bitset_init(&b, meta, frames);

    uintptr meta_bytes = (uintptr)meta_end - (uintptr)meta;
    uint needed = (uint)CEIL(meta_bytes, FRAME_SZ);
    for (uint i = 0; i < needed; i++) {
        bitset_set(&b, i);
    }
    total_count = frames - needed;
}

uintptr frame_alloc(void)
{
    uint offset = bitset_search(&b);
    if (offset == (uint)-1) {
        kernel_panic("No frames available to use");
    }
    bitset_set(&b, offset);
    total_count--;
    return mem_start + (uintptr)FRAME_SZ * offset;
}

void frame_free(uintptr frame)
{
    if (frame < mem_start) {
        kernel_panic("Attempted to free an invalid frame");
    }
    uint offset = (uint)((frame - mem_start) / FRAME_SZ);
    bitset_clear(&b, offset);
    total_count++;
}

uintptr frames_available(void)
{
    return total_count;
}
