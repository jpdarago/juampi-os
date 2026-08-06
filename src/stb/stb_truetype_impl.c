// The stb_truetype implementation, isolated in its own translation unit so the
// warning-noisy vendored code can be built with -w while src/font.c (which uses
// its API) keeps the kernel's -Werror. Every libc dependency stb_truetype has
// is routed through an STBTT_* macro to a kernel facility, so no libc header is
// pulled in: the transcendental math comes from the Lua klibc math (linked into
// the kernel), memory from the kernel heap, and mem/str ops from utils.

#include <stddef.h>
#include <stdint.h>

#include <memory.h> // heap_default, heap_free
#include <alloc.h>  // new()
#include <utils.h>  // memcpy, memset

// Math implemented in src/lua/klibc_math.c and linked into the kernel; declare
// the handful stb_truetype's glyph rasteriser uses (pow/cos/acos only appear in
// the signed-distance-field path we don't call).
extern double floor(double);
extern double ceil(double);
extern double sqrt(double);
extern double fabs(double);
extern double fmod(double, double);
extern double cos(double);
extern double acos(double);
extern double pow(double, double);

static void* stb_malloc(size_t n)
{
    return new (&heap_default()->base, uint8_t, (ptrdiff_t)n);
}
static void stb_free(void* p)
{
    if (p != NULL) {
        heap_free(heap_default(), p);
    }
}
static size_t stb_strlen(const char* s)
{
    size_t n = 0;
    while (s[n] != '\0') {
        n++;
    }
    return n;
}

#define STBTT_ifloor(x) ((int)floor(x))
#define STBTT_iceil(x) ((int)ceil(x))
#define STBTT_sqrt(x) sqrt(x)
#define STBTT_pow(x, y) pow(x, y)
#define STBTT_fmod(x, y) fmod(x, y)
#define STBTT_cos(x) cos(x)
#define STBTT_acos(x) acos(x)
#define STBTT_fabs(x) fabs(x)
#define STBTT_malloc(x, u) ((void)(u), stb_malloc(x))
#define STBTT_free(x, u) ((void)(u), stb_free(x))
#define STBTT_assert(x) ((void)0)
#define STBTT_strlen(x) stb_strlen(x)
#define STBTT_memcpy memcpy
#define STBTT_memset memset

#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"
