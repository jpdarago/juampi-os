#ifndef __KSYM_H
#define __KSYM_H

#include <stdint.h>

// Kernel symbolication: parse the kernel's own ELF image (handed back by
// Limine) for its symbol table, so panics and faults print human-readable
// backtraces. If unavailable, lookups return NULL and backtraces print raw
// addresses.

// Parse the ELF image at `elf` (Limine's kernel-file); safe to call once.
void ksym_init(void* elf);

// Name of the function containing `addr`, or NULL; *offset gets addr - symbol.
const char* ksym_lookup(uint64_t addr, uint64_t* offset);

// Print a backtrace to the console, walking frame pointers from (rip, rbp).
void backtrace_from(uint64_t rip, uint64_t rbp);
// Backtrace of the current call site.
void backtrace(void);

#endif
