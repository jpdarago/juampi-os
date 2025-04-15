#ifndef __KEYBOARD_H
#define __KEYBOARD_H

#include <types.h>
#include <proc.h>

void keybuffer_produce(uint key);
int  keybuffer_consume(void);
void keybuffer_init(int buffer_size);

void keyboard_irq_handler(uint, gen_regs);

#endif
