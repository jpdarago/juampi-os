#ifndef __SYSTEM_H
#define __SYSTEM_H

#include "types.h"

extern void memcpy(void *dest, void *src, uint count);
extern void memsetb(void *dest, uchar val, uint count);
extern void memsetw(void *dest, ushort val, uint count);

#define memset(dst,val,count) memsetb(dst,val,count)

extern uint strlen(const char *str);

extern void strcpy(char * dst, const char * src);
extern void strcat(char * dst, const char * src);

extern int memcmp(void * , void * , uint );
extern int strcmp(char *, char *);

extern uint umax(uint, uint);

#define BOCHSBREAK __asm__ __volatile__("xchg %bx,%bx");
#define CODE_SEGMENT 0x08
#define DATA_SEGMENT 0x10

#endif
