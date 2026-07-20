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

typedef struct lab_api {
    void* (*alloc)(unsigned long size); // zeroed kernel-heap allocation
    void (*free)(void* p);
    void (*print)(const char* s);  // write a string to the console
    unsigned long (*rdtsc)(void);  // raw cycle counter
    unsigned long (*ns)(void);     // monotonic nanoseconds
    unsigned long (*ncores)(void); // number of CPU cores online
    void (*run_on)(unsigned index, // dispatch fn(arg) to core `index`...
                   void (*fn)(void*), void* arg);
    void (*join)(unsigned index); // ...then wait for it to finish
} lab_api;

typedef long (*lab_entry)(const lab_api* api, long arg);

// --- Kernel side (ignored by benchmark builds) -----------------------------

// Load the ELF64 `image` and call its entry once, returning its result.
long lab_run(const void* image, unsigned long size, long arg);
// Load once, then call the entry `iters` times inside a TSC fence; return the
// total elapsed cycles.
unsigned long lab_bench(const void* image, unsigned long size, long arg,
                        unsigned long iters);

#endif
