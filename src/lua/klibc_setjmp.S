// x86-64 setjmp/longjmp for the vendored Lua (its pcall/error mechanism). The
// jmp_buf holds the callee-saved registers, the stack pointer and the return
// address: rbx, rbp, r12, r13, r14, r15, rsp, rip.

.intel_syntax noprefix
.text
.global setjmp
.global longjmp

// int setjmp(jmp_buf env /rdi/)
setjmp:
    mov [rdi + 0x00], rbx
    mov [rdi + 0x08], rbp
    mov [rdi + 0x10], r12
    mov [rdi + 0x18], r13
    mov [rdi + 0x20], r14
    mov [rdi + 0x28], r15
    lea rax, [rsp + 8]          // caller's rsp (above our return address)
    mov [rdi + 0x30], rax
    mov rax, [rsp]              // return address
    mov [rdi + 0x38], rax
    xor eax, eax               // setjmp returns 0
    ret

// void longjmp(jmp_buf env /rdi/, int val /esi/)
longjmp:
    mov rbx, [rdi + 0x00]
    mov rbp, [rdi + 0x08]
    mov r12, [rdi + 0x10]
    mov r13, [rdi + 0x18]
    mov r14, [rdi + 0x20]
    mov r15, [rdi + 0x28]
    mov rsp, [rdi + 0x30]
    mov eax, esi               // return `val` from setjmp...
    test eax, eax
    jnz 1f
    inc eax                    // ...but never 0 (longjmp(env,0) yields 1)
1:  jmp qword ptr [rdi + 0x38] // resume at the saved return address

.section .note.GNU-stack, "", @progbits
