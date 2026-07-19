#include <fault.h>

fault_jmp_buf fault_env;
volatile bool fault_armed;
volatile uint64_t fault_vector;
volatile uint64_t fault_addr;
volatile uint64_t fault_rip;

bool fault_recover(interrupt_frame* f)
{
    if (!fault_armed) {
        return false;
    }
    fault_armed = false; // disarm before unwinding (avoid recursive recovery)
    uint64_t cr2;
    __asm__ __volatile__("mov %%cr2, %0" : "=r"(cr2));
    fault_vector = f->vector;
    fault_addr = cr2;
    fault_rip = f->rip;
    longjmp(fault_env, 1); // back to the shell's recovery point; never returns
    return true;           // unreachable
}
