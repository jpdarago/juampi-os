#ifndef __FRAME_ALLOC_H
#define __FRAME_ALLOC_H

#include <types.h>

// Physical page-frame allocator. Manages one contiguous physical region (the
// largest usable range Limine reports); addresses are physical, reached by the
// kernel through the HHDM.
void frames_init(uintptr phys_base, uintptr len);
uintptr frame_alloc(void);
void frame_free(uintptr frame);
uintptr frames_available(void);

#endif
