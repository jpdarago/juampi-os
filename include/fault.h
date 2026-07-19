#ifndef __FAULT_H
#define __FAULT_H

#include <idt.h>

#include <stdint.h>
#include <stdbool.h>

// Recoverable faults: while `fault_armed` is set (the shell has armed it around
// evaluating a line of Lua), a CPU exception with no handler — e.g. a bad
// k.poke/k.rdmsr from a script — unwinds back to the shell's recovery point
// instead of halting the machine. Outside that window, faults are kernel bugs
// and still panic with a backtrace.

typedef uint64_t fault_jmp_buf[8];
int setjmp(fault_jmp_buf env);          // src/lua/klibc_setjmp.S
void longjmp(fault_jmp_buf env, int v); // src/lua/klibc_setjmp.S

extern fault_jmp_buf fault_env;
extern volatile bool fault_armed;
extern volatile uint64_t fault_vector; // vector of the last recovered fault
extern volatile uint64_t fault_addr;   // cr2 at the fault (for page faults)
extern volatile uint64_t fault_rip;    // instruction pointer at the fault

// If recovery is armed, record the fault and longjmp to fault_env (never
// returns); otherwise return false so the caller proceeds to panic.
bool fault_recover(interrupt_frame* f);

#endif
