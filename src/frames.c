#include <frames.h>
#include <bitset.h>
#include <exception.h>
#include <scrn.h>
#include <utils.h>

#define FRAME_SZ 0x1000
#define FRAME_MSK 0xFFF
#define FRAME_ALIGN(x) ((x) & ~0xFFF)

// Bitmap for the frames: 1 indicates used, 0 indicates not used
static bitset b;
// (Physical) address where the memory to manage starts
static uint mem_start, total_count;
// Count of the number of holders to which we assigned a frame
static uint* count;

// Initializes the management bitmap and returns the position
// of the first frame of useful data (i.e., computes the size of the bitset)
uint frame_alloc_init(void* _mem_start, uint frames)
{
    intptr mem = FRAME_ALIGN((intptr) _mem_start + FRAME_SZ - 1);
    mem_start = mem;
    count = bitset_init(&b,(void*)mem,frames);
    memset(count,0,frames*sizeof(uint));
    uint frame_alloc_finish = (intptr)(count + frames);
    uint needed = CEIL(frame_alloc_finish - mem_start,FRAME_SZ);
    for(uint i = 0; i < needed; i++) {
        bitset_set(&b,i);
        count[i] = 1;
    }
    total_count = frames-needed;
    return mem_start+FRAME_SZ*needed;
}

uint frame_alloc()
{
    uint offset = bitset_search(&b);
    if(offset == (uint)-1) {
        kernel_panic("No frames available to use");
    }
    bitset_set(&b,offset);
    count[offset]++;
    total_count--;

    uint frame = mem_start+FRAME_SZ*offset;
    return frame;
}

void frame_free(uint frame)
{
    if(frame < mem_start) {
        kernel_panic("Attempted to free an invalid frame");
    }
    uint offset = (frame-mem_start)/FRAME_SZ;
    if(--count[offset] == 0) {
        total_count++;
        bitset_clear(&b,offset);
    }
}

uint frames_available()
{
    return total_count;
}

void frame_add_alias(uint frame)
{
    if(frame < mem_start) {
        kernel_panic("Aliasing an invalid frame\n");
    }
    uint offset = (frame-mem_start)/FRAME_SZ;
    count[offset]++;
}
