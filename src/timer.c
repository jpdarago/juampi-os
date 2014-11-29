#include <timer.h>
#include <tasks.h>
#include <proc.h>

// Modificar frequencia del reloj
void init_timer(uint freq)
{
    uint div = 1193180/freq;
    outb(0x43,0x36);
    uchar l = (uchar)(div & 0xFF);
    uchar h = (uchar)((div>>8)&0xFF);
    outb(0x40,l);
    outb(0x40,h);
}

void schedule(uint irq_code, gen_regs regs)
{
    if(!is_preemptable()) return;

    process_info* current = get_current_task();
    if(current != NULL) {
        process_info* next = next_task();
        if(next->pid != current->pid) {
            perform_task_switch(next);
        }
    }
}
