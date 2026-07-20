#include <lab.h>
#include <elf64.h>
#include <memory.h>
#include <console.h>
#include <ktime.h>
#include <smp.h>

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

static const lab_api api = {
        .alloc = lab_alloc,
        .free = lab_free,
        .print = lab_print,
        .rdtsc = lab_rdtsc,
        .ns = lab_ns,
        .ncores = lab_ncores,
        .run_on = lab_run_on,
        .join = lab_join,
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

long lab_run(const void* image, unsigned long size, long arg)
{
    (void)size;
    lab_entry bench = load(image);
    if (bench == NULL) {
        return 0;
    }
    return bench(&api, arg);
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
