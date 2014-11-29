#ifndef __PROC_H
#define __PROC_H

#include <types.h>

//Estructura con los registros de proposito general
typedef struct {
    uint32 edi,esi,ebp,esp,ebx,edx,ecx,eax;
} gen_regs;

//Traza de interrupccion
typedef struct {
    intptr eip;
    uint32 cs;
    uint32 eflags;
    intptr useresp;
    uint32 ss;
} __attribute__((__packed__)) int_trace;

//Registros de control
typedef struct {
    uint32 cr0,cr2,cr3,cr4;
} ctrl_regs;

//Registros de selectores de segmento
typedef struct {
    uint32 cs,ds,es,fs,gs,ss;
} sel_regs;

typedef struct {
    uint excp_index;
    ctrl_regs ctrace;
    sel_regs strace;
    gen_regs rtrace;
    uint error_code;
    int_trace itrace;
} exception_trace;

//Setter y getter para la task register (tr)
short get_tr(void);
void set_tr(short);

short get_cs(void);
uint32 get_eflags(void);

#endif
