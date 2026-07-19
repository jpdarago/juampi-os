#ifndef __FRAME_ALLOC_H
#define __FRAME_ALLOC_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

// Physical page-frame allocator. Manages one contiguous physical region (the
// largest usable range Limine reports); addresses are physical, reached by the
// kernel through the HHDM.
void frames_init(uintptr_t phys_base, uintptr_t len);
uintptr_t frame_alloc(void);
void frame_free(uintptr_t frame);
uintptr_t frames_available(void);
uintptr_t frames_total(void);

#endif
