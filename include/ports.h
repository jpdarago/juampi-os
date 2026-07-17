#ifndef __PORTS_H
#define __PORTS_H

#include <types.h>

// Reads a byte from a port
extern uchar inb(ushort);
// Reads a short from a port
extern ushort inw(ushort);
// Writes a byte value to a port
extern void outb(ushort, uchar);
// Writes a short value to a port
extern void outw(ushort, ushort);

#endif
