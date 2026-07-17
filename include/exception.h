#ifndef __EXCEPTION_H
#define __EXCEPTION_H

#include <types.h>
#include <proc.h>
#include <scrn.h>
#include <irq.h>

typedef void (*exception_handler)(exception_trace);

extern exception_handler exception_handlers[];

// Registers a special handler
void register_exception_handler(exception_handler,uint);

// Initializes the exception handlers to a default
void initialize_exception_handlers(void);

// Classic blue screen
extern void blue_screen(exception_trace);

#define kernel_panic(m,...) \
    do { irq_cli(); scrn_cls(); \
         scrn_printf("KERNEL PANIC (%s:%d):\n\t " m, \
                     __FILE__,__LINE__, ## __VA_ARGS__); while(1) ; } while(0)

#endif
