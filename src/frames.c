#include <frames.h>
#include <bitset.h>
#include <paging.h>
#include <panic.h>
#include <utils.h>

#define FRAME_SZ 0x1000
#define FRAME_ALIGN(x) (((x) + FRAME_SZ - 1) & ~(uintptr)0xFFF)

// Bitmap of frame usage (1 = used) plus a per-frame reference count (so a frame
// aliased by copy-on-write is only freed once all holders release it). Both
// live in the HHDM view of the managed region itself.
static bitset b;
static uintptr mem_start; // physical base of the managed region
static uint total_count;  // free frames
static uint* count;       // refcount per frame (virtual, in the HHDM)

void frames_init(uintptr phys_base, uintptr len)
{
    uintptr base = FRAME_ALIGN(phys_base);
    uint frames = (uint)(len / FRAME_SZ);
    mem_start = base;

    // The bitmap and count array are stored at the start of the region (through
    // the HHDM); the frames they occupy are then marked used.
    void* meta = phys_to_virt(base);
    count = bitset_init(&b, meta, frames);
    memset(count, 0, frames * sizeof(uint));

    uintptr meta_bytes = (uintptr)(count + frames) - (uintptr)meta;
    uint needed = (uint)CEIL(meta_bytes, FRAME_SZ);
    for (uint i = 0; i < needed; i++) {
        bitset_set(&b, i);
        count[i] = 1;
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
    count[offset]++;
    total_count--;
    return mem_start + (uintptr)FRAME_SZ * offset;
}

void frame_free(uintptr frame)
{
    if (frame < mem_start) {
        kernel_panic("Attempted to free an invalid frame");
    }
    uint offset = (uint)((frame - mem_start) / FRAME_SZ);
    if (--count[offset] == 0) {
        total_count++;
        bitset_clear(&b, offset);
    }
}

uintptr frames_available(void)
{
    return total_count;
}

void frame_add_alias(uintptr frame)
{
    if (frame < mem_start) {
        kernel_panic("Aliasing an invalid frame");
    }
    uint offset = (uint)((frame - mem_start) / FRAME_SZ);
    count[offset]++;
}
