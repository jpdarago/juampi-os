#ifndef __TSS_H
#define __TSS_H

#include "types.h"

typedef struct {
    uint16 prev_task;
    uint16 __reserved1;
    intptr esp0;
    uint16 ss0;
    uint16 __reserved2;
    intptr esp1;
    uint16 ss1;
    uint16 __reserved3;
    intptr esp2;
    uint16 ss2;
    uint16 __reserved4;
    uint32 cr3;
    intptr eip;
    uint32 eflags;
    uint32 eax,ecx,edx,ebx;
    intptr esp;
    uint32 ebp,esi,edi;
    uint16 es;
    uint16 __reserved6;
    uint16 cs;
    uint16 __reserved7;
    uint16 ss;
    uint16 __reserved8;
    uint16 ds;
    uint16 __reserved9;
    uint16 fs;
    uint16 __reserved10;
    uint16 gs;
    uint16 __reserved11;
    uint16 ldt;
    uint16 __reserved12;
    uint16 T : 1;
    uint16 __reserved13 : 15;
    uint16 iomap_addr;
} __attribute__((__packed__)) tss;

#endif
