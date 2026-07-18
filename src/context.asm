; Software context switch for x86-64. Long mode has no hardware task switching
; (the 32-bit kernel jumped through a TSS), so threads are switched in software:
; save the callee-saved registers of the outgoing thread, stash its stack
; pointer, load the incoming thread's stack pointer, restore its callee-saved
; registers and return into it. Caller-saved registers are preserved by the C
; calling convention around the call, so they need not be saved here.

bits 64

global context_switch
; void context_switch(uint64* old_rsp /rdi/, uint64 new_rsp /rsi/)
context_switch:
    push rbx
    push rbp
    push r12
    push r13
    push r14
    push r15

    mov [rdi], rsp          ; save outgoing stack pointer
    mov rsp, rsi            ; load incoming stack pointer

    pop r15
    pop r14
    pop r13
    pop r12
    pop rbp
    pop rbx
    ret                     ; return into the incoming thread
