#ifndef __BOCHS_DEBUG
#define __BOCHS_DEBUG

#include <types.h>
#include <vargs.h>

void dbg_putc(char);
void dbg_print(const char *);
void dbg_printf(const char * fmt, ...);

#endif
