#include <sched.h>
#include <panic.h>
#include <utils.h>

#include <stdint.h>
#include <stdbool.h>

#define MAX_THREADS 8
#define STACK_SZ 0x4000 // 16 KiB kernel stack per thread
#define FXSAVE_SZ 512   // fxsave/fxrstor area, must be 16-byte aligned

typedef struct {
    uint64_t rsp; // saved stack pointer when the thread is not running
    void* fparea; // 512-byte fxsave area for this thread's FPU/SSE state
    bool used;
} thread_t;

static thread_t threads[MAX_THREADS];
static int nthreads;
static int current;

// A clean FPU/SSE state image, copied into each new thread so its first
// fxrstor loads a valid state (a zeroed area would give an invalid MXCSR).
static uint8_t fp_template[FXSAVE_SZ] __attribute__((aligned(16)));

// Defined in context.asm.
extern void context_switch(uint64_t* old_rsp, uint64_t new_rsp, void* old_fp,
                           void* new_fp);

void sched_init(allocator* mem)
{
    // Capture a clean FPU/SSE state (x87 reset, default MXCSR) as the seed for
    // new threads.
    uint32_t mxcsr = 0x1F80;
    __asm__ __volatile__("fninit");
    __asm__ __volatile__("ldmxcsr %0" ::"m"(mxcsr));
    __asm__ __volatile__("fxsave (%0)" ::"r"(fp_template) : "memory");

    // Thread 0 is the boot context we are already running on; its rsp and FPU
    // state are captured the first time it yields (the fparea contents here are
    // overwritten by that first fxsave).
    threads[0].used = true;
    threads[0].rsp = 0;
    threads[0].fparea = new (mem, uint8_t, FXSAVE_SZ);
    nthreads = 1;
    current = 0;
}

int thread_create(allocator* mem, void (*entry)(void))
{
    if (nthreads >= MAX_THREADS) {
        kernel_panic("Too many threads");
    }
    int id = nthreads++;

    // Build an initial stack whose top the context switch will "return" into:
    // six zeroed callee-saved slots then the entry address, so the switch's pop
    // sequence + ret lands at `entry`. The entry-address slot sits on a 16-byte
    // boundary so that after that `ret` the thread starts with rsp % 16 == 8,
    // exactly as a normal SysV call leaves it (required now that SSE is on).
    uint64_t top =
            (((uint64_t)new (mem, char, STACK_SZ) + STACK_SZ) & ~0xFull) - 8;
    uint64_t* sp = (uint64_t*)top;
    *--sp = (uint64_t)entry; // ret target
    *--sp = 0;               // rbx
    *--sp = 0;               // rbp
    *--sp = 0;               // r12
    *--sp = 0;               // r13
    *--sp = 0;               // r14
    *--sp = 0;               // r15

    threads[id].rsp = (uint64_t)sp;
    threads[id].fparea = new (mem, uint8_t, FXSAVE_SZ);
    memcpy(threads[id].fparea, fp_template, FXSAVE_SZ);
    threads[id].used = true;
    return id;
}

void yield(void)
{
    if (nthreads < 2) {
        return;
    }
    int prev = current;
    current = (current + 1) % nthreads;
    context_switch(&threads[prev].rsp, threads[current].rsp,
                   threads[prev].fparea, threads[current].fparea);
}
