#ifndef KLIBC_SETJMP_H
#define KLIBC_SETJMP_H
// x86-64 setjmp/longjmp (klibc_setjmp.asm): saves rbx, rbp, r12-r15, rsp, rip.
typedef unsigned long jmp_buf[8];
int setjmp(jmp_buf env);
void longjmp(jmp_buf env, int val);
#endif
