#ifndef __SYSCALLS_H
#define __SYSCALLS_H

#include <types.h>
#include <proc.h>

typedef void (*syscall)(gen_regs *,int_trace *);
// Registra una systemcall dado su codigo y el handler deseado
void syscall_register(uint, syscall);
// Inicializa las systemcalls
void syscalls_initialize(void);
// Punto de entrada de todas las system calls
void syscall_entry_point(gen_regs, int_trace);

#endif
