#ifndef __TIMER_H
#define __TIMER_H

#include <types.h>
#include <proc.h>
#include <scrn.h>
#include <utils.h>
#include <ports.h>
#include <irq.h>

extern void init_timer(uint);
extern void schedule(uint,gen_regs);

#endif
