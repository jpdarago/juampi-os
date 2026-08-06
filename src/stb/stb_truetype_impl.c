// The stb_truetype implementation, isolated in its own translation unit so the
// warning-noisy vendored code can be built with -w while src/ttf.c (which uses
// its API) keeps the kernel's -Werror. Built with the klibc shim on the include
// path (like the other vendored libraries), so stb's own defaults satisfy its
// <math.h> and <string.h> needs (floor/sqrt/…, strlen/memcpy/memset) from the
// kernel's existing implementations — no hand-rolled copies. Only the allocator
// is overridden, to route through the kernel heap.

#include <stddef.h>
#include <stdint.h>

#include <memory.h> // heap_default, heap_free
#include <alloc.h>  // new()

// stb threads an allocation context (stbtt_fontinfo.userdata) through every
// STBTT_malloc/free call. src/ttf.c sets that to the font's injected heap, so
// all rasterisation scratch is drawn from the caller's allocator rather than a
// global — matching the kernel's allocator-injection convention. heap_default()
// is only a fallback for any stb call that somehow arrives without a context.
static void* stb_malloc(size_t n, void* u)
{
    struct heap_allocator* h =
            u != NULL ? (struct heap_allocator*)u : heap_default();
    return new (&h->base, uint8_t, (ptrdiff_t)n);
}
static void stb_free(void* p, void* u)
{
    if (p != NULL) {
        heap_free(u != NULL ? (struct heap_allocator*)u : heap_default(), p);
    }
}

#define STBTT_malloc(x, u) stb_malloc((x), (u))
#define STBTT_free(x, u) stb_free((x), (u))
#define STBTT_assert(x) ((void)0)

#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"
