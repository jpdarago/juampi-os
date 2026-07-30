#ifndef __LAB_H
#define __LAB_H

#include <stdint.h>
#include <stddef.h>

// The "sterile lab": load a freestanding native binary into the kernel and call
// it directly (ring 0) to benchmark algorithm implementations with clean, low-
// noise measurement. A binary is a static ELF64 whose entry symbol is:
//
//     long bench(const lab_api* api, long arg);
//
// The kernel calls it with a table of helper callbacks and the caller's `arg`
// (typically a problem size); the return value is a result/checksum. Because it
// runs in ring 0 with no syscall boundary, timing is measured exactly like
// k.bench times Lua. A fault inside a benchmark unwinds to the shell prompt via
// the shell's fault recovery, so a buggy binary can't halt the machine.
//
// This header is shared verbatim by the kernel and by the benchmark sources in
// build/lab/ (compiled freestanding), so it must stay self-contained.

struct lab_api {
    void* (*alloc)(unsigned long size); // zeroed kernel-heap allocation
    void (*free)(void* p);
    void (*print)(const char* s);  // write a string to the console
    unsigned long (*rdtsc)(void);  // raw cycle counter
    unsigned long (*ns)(void);     // monotonic nanoseconds
    unsigned long (*ncores)(void); // number of CPU cores online
    void (*run_on)(unsigned index, // dispatch fn(arg) to core `index`...
                   void (*fn)(void*), void* arg);
    void (*join)(unsigned index); // ...then wait for it to finish
    // Framebuffer, for native programs that draw (a raytracer, say). All zero
    // when headless. Pixels are 32bpp; pack them with the fb_shifts() layout
    // and store at fb() + y*fb_pitch() + x*4. fb_pitch() is in bytes and may
    // exceed width*4. Broadens the "sterile lab" beyond pure compute — the
    // framebuffer is the one piece of hardware a native binary can touch.
    void* (*fb)(void);               // live framebuffer base, NULL if headless
    unsigned long (*fb_width)(void); // in pixels
    unsigned long (*fb_height)(void);
    unsigned long (*fb_pitch)(void); // bytes per scanline
    void (*fb_shifts)(unsigned char* r, unsigned char* g, unsigned char* b);
};

typedef long (*lab_entry)(const struct lab_api* api, long arg);

// --- Kernel side (ignored by benchmark builds) -----------------------------

// Load the ELF64 `image` and call its entry once, returning its result. If
// `target` is non-NULL, the binary's api->fb() draws into that w*h buffer (a
// canvas) instead of the live screen — so a graphical program renders into a
// window. lab_drew() then reports whether it actually fetched the framebuffer.
long lab_run(const void* image, unsigned long size, long arg, uint32_t* target,
             unsigned long w, unsigned long h);
// True if the most recent lab_run's binary fetched api->fb() (drew graphics).
int lab_drew(void);
// Load once, then call the entry `iters` times inside a TSC fence; return the
// total elapsed cycles.
unsigned long lab_bench(const void* image, unsigned long size, long arg,
                        unsigned long iters);

#endif
