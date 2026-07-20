#ifndef __PARALLEL_H
#define __PARALLEL_H

#include <alloc.h>
#include <stdbool.h>

// Parallel Lua (M9): one lua_State per core, driven from Lua via the `thread`
// and `mem` libraries (src/lua/lua_thread.c). Each worker state has its own
// heap, so cores allocate without a lock. Work is dispatched onto the SMP
// mailbox (smp_run_on/smp_join).

// Create the per-core worker states. Call once at boot, after smp_init (cores
// up) and with the kernel heap ready; `global` backs the per-core heaps.
void parallel_init(allocator* global);

// Boot self-test: run a trivial Lua chunk on every application processor and
// check each returns its own core id. True if all cores ran correctly.
bool parallel_selftest(void);

#endif
