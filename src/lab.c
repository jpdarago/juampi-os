#include <lab.h>
#include <elf64.h>
#include <memory.h>
#include <console.h>
#include <ktime.h>
#include <smp.h>
#include <gfx.h>

// The kernel side of the "sterile lab": load a native ELF64 benchmark and call
// it directly in ring 0, handing it a table of helper callbacks (lab_api).

static void* lab_alloc(unsigned long size)
{
    return alloc(&heap_default()->base, (ptrdiff_t)size, 16, 1);
}
static void lab_free(void* p)
{
    heap_free(heap_default(), p);
}
static void lab_print(const char* s)
{
    console_print(s);
}
static unsigned long lab_rdtsc(void)
{
    return rdtsc();
}
static unsigned long lab_ns(void)
{
    return ktime_ns();
}
static unsigned long lab_ncores(void)
{
    return smp_cpu_count();
}
static void lab_run_on(unsigned index, void (*fn)(void*), void* arg)
{
    smp_run_on(index, fn, arg);
}
static void lab_join(unsigned index)
{
    smp_join(index);
}
// The current run's render target: a canvas buffer (drawn into a window) when
// lab_run was given one, else NULL to draw straight to the live screen. Native
// binaries are sequential (BSP, one at a time), so a single current-target is
// enough — and fetching it via lab_fb() flags that the program drew.
static uint32_t* lab_fbuf;
static unsigned long lab_fbw, lab_fbh;
static int lab_fb_used;

static void* lab_fb(void)
{
    if (lab_fbuf != NULL) {
        lab_fb_used = 1;
        return lab_fbuf;
    }
    return gfx_framebuffer(NULL, NULL);
}
static unsigned long lab_fb_width(void)
{
    return lab_fbuf != NULL ? lab_fbw : gfx_width();
}
static unsigned long lab_fb_height(void)
{
    return lab_fbuf != NULL ? lab_fbh : gfx_height();
}
static unsigned long lab_fb_pitch(void)
{
    return lab_fbuf != NULL ? lab_fbw * 4 : gfx_pitch();
}
static void lab_fb_shifts(unsigned char* r, unsigned char* g, unsigned char* b)
{
    gfx_shifts(r, g, b);
}

static const struct lab_api api = {
        .alloc = lab_alloc,
        .free = lab_free,
        .print = lab_print,
        .rdtsc = lab_rdtsc,
        .ns = lab_ns,
        .ncores = lab_ncores,
        .run_on = lab_run_on,
        .join = lab_join,
        .fb = lab_fb,
        .fb_width = lab_fb_width,
        .fb_height = lab_fb_height,
        .fb_pitch = lab_fb_pitch,
        .fb_shifts = lab_fb_shifts,
};

// Load `image` for ring-0 execution and return its entry, or NULL if it is not
// a valid ELF64.
static lab_entry load(const void* image)
{
    // elf64_load_exec maps into the active address space and copies the image
    // to its link VA; the returned entry is a directly-callable function. The
    // loader only reads `image`, so laundering away const is safe.
    uint64_t entry = elf64_load_exec((void*)(uintptr_t)image);
    return (lab_entry)entry;
}

long lab_run(const void* image, unsigned long size, long arg, uint32_t* target,
             unsigned long w, unsigned long h)
{
    (void)size;
    lab_entry bench = load(image);
    if (bench == NULL) {
        return 0;
    }
    lab_fbuf = target;
    lab_fbw = w;
    lab_fbh = h;
    lab_fb_used = 0;
    long r = bench(&api, arg);
    lab_fbuf = NULL; // stop aliasing the (soon-freed) canvas buffer
    return r;
}

int lab_drew(void)
{
    return lab_fb_used;
}

unsigned long lab_bench(const void* image, unsigned long size, long arg,
                        unsigned long iters)
{
    (void)size;
    lab_entry bench = load(image);
    if (bench == NULL || iters == 0) {
        return 0;
    }
    uint64_t start = rdtsc();
    for (unsigned long i = 0; i < iters; i++) {
        bench(&api, arg);
    }
    return rdtsc() - start;
}
