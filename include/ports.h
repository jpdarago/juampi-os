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
// Reads a dword from a port
extern uint32_t inl(uint16_t);
// Writes a dword value to a port
extern void outl(uint16_t, uint32_t);

#endif
