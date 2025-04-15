#ifndef __SYSTEM_H
#define __SYSTEM_H

extern void memcpy(unsigned char *dest, const unsigned char *src, int count);
extern void memsetb(unsigned char *dest, unsigned char val, int count);
extern void memsetw(unsigned short *dest, unsigned short val, int count);
extern void strlen(const char *str);
extern unsigned char inportb (unsigned short _port);
extern void outportb (unsigned short _port, unsigned char _data);
extern void breakpoint();
#endif
