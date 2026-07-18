#include <sched.h>
#include <memory.h>
#include <panic.h>

#define MAX_THREADS 8
#define STACK_SZ 0x4000 // 16 KiB kernel stack per thread

typedef struct {
    uint64 rsp; // saved stack pointer when the thread is not running
    bool used;
} thread_t;

static thread_t threads[MAX_THREADS];
static int nthreads;
static int current;

// Defined in context.asm.
extern void context_switch(uint64* old_rsp, uint64 new_rsp);

void sched_init(void)
{
    // Thread 0 is the boot context we are already running on; its rsp is filled
    // in the first time it yields.
    threads[0].used = true;
    threads[0].rsp = 0;
    nthreads = 1;
    current = 0;
}

int thread_create(void (*entry)(void))
{
    if (nthreads >= MAX_THREADS) {
        kernel_panic("Too many threads");
    }
    int id = nthreads++;

    // Build an initial stack whose top the context switch will "return" into:
    // six zeroed callee-saved slots then the entry address, so the switch's pop
    // sequence + ret lands at `entry`.
    uint64 top = ((uint64)kmalloc(STACK_SZ) + STACK_SZ) & ~0xFull;
    uint64* sp = (uint64*)top;
    *--sp = (uint64)entry; // ret target
    *--sp = 0;             // rbx
    *--sp = 0;             // rbp
    *--sp = 0;             // r12
    *--sp = 0;             // r13
    *--sp = 0;             // r14
    *--sp = 0;             // r15

    threads[id].rsp = (uint64)sp;
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
    context_switch(&threads[prev].rsp, threads[current].rsp);
}
