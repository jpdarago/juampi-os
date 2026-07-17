#ifndef __SYSCALLS_H
#define __SYSCALLS_H

#include <types.h>
#include <proc.h>

typedef void (*syscall)(gen_regs *,int_trace *);
// Registers a syscall given its code and the desired handler
void syscall_register(uint, syscall);
// Initializes the syscalls
void syscalls_initialize(void);
// Entry point of all system calls
void syscall_entry_point(gen_regs, int_trace);

#endif
