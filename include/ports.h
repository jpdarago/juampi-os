#ifndef __PORTS_H
#define __PORTS_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

// Reads a byte from a port
extern uint8_t inb(uint16_t);
// Reads a short from a port
extern uint16_t inw(uint16_t);
// Writes a byte value to a port
extern void outb(uint16_t, uint8_t);
// Writes a short value to a port
extern void outw(uint16_t, uint16_t);

#endif
