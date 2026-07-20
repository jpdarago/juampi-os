#ifndef __SMP_H
#define __SMP_H

#include <alloc.h>
#include <gdt64.h>

#include <stdint.h>

// Symmetric multiprocessing: bring up the application processors (APs) that
// Limine started for us and park them on a work mailbox. This is the M8
// foundation for the parallel-Lua goal (docs/lua-shell.md): each core gets its
// own GDT/TSS and per-CPU data, and work is handed to a core by posting a
// function to its mailbox (the SPMD dispatch M9 will build thread.spawn on).
//
// M8 scope: APs run with interrupts disabled, never allocate, and never touch
// Lua — so the single-threaded kernel heap stays safe. No LAPIC/IPI: cores are
// driven by spin-polled mailboxes, not interrupts.

// One work item handed to a core: run fn(arg) and report completion.
enum { CPU_MBOX_IDLE = 0, CPU_MBOX_PENDING, CPU_MBOX_DONE };

typedef struct cpu {
    struct cpu* self; // first field: read back via %gs:0 (smp_this_cpu)
    uint32_t index;   // 0..count-1 (0 is the BSP)
    uint32_t lapic_id;
    uint64_t gdt[7];     // this core's GDT (the BSP keeps its boot GDT instead)
    tss64 tss;           // this core's TSS
    uint64_t kstack_top; // interrupt stack (tss.rsp0) for this core
    // Work mailbox, polled by the AP's dispatch loop.
    void (*fn)(void* arg);
    void* arg;
    volatile int mbox;  // CPU_MBOX_*
    volatile int ready; // set by an AP once it has finished coming up
} cpu;

// Start the APs and wait for them to come online. `mem` backs the per-CPU
// array (allocated while the heap is still single-threaded). Safe on a
// uniprocessor / when Limine reports no MP response (records one core).
void smp_init(allocator* mem);

uint64_t smp_cpu_count(void);   // number of cores online (>= 1)
uint32_t smp_bsp_index(void);   // index of the bootstrap processor
struct cpu* smp_this_cpu(void); // the core executing this call

// Post fn(arg) to core `index` (must not be the caller's own core) and, later,
// block until it has finished. One outstanding job per core.
void smp_run_on(uint32_t index, void (*fn)(void* arg), void* arg);
void smp_join(uint32_t index);

#endif
