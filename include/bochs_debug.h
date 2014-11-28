#ifndef __BOCHS_DEBUG
#define __BOCHS_DEBUG

#include <types.h>
#include <vargs.h>

void dbg_putc(char);
void dbg_print(char *);
void dbg_vprintf(char * fmt, varg_list v);
void dbg_printf(char * fmt, ...);
#endif
