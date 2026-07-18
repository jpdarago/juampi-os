; Software context switch for x86-64. Long mode has no hardware task switching
; (the 32-bit kernel jumped through a TSS), so threads are switched in software:
; save the callee-saved registers of the outgoing thread, stash its stack
; pointer, load the incoming thread's stack pointer, restore its callee-saved
; registers and return into it. Caller-saved registers are preserved by the C
; calling convention around the call, so they need not be saved here.
;
; The full FPU/SSE state is saved and restored via fxsave/fxrstor into each
; thread's 512-byte, 16-byte-aligned save area, so a thread's floating-point and
; XMM state survives being switched away and is isolated from other threads.

bits 64

global context_switch
; void context_switch(uint64* old_rsp /rdi/, uint64 new_rsp /rsi/,
;                     void* old_fp /rdx/, void* new_fp /rcx/)
context_switch:
    push rbx
    push rbp
    push r12
    push r13
    push r14
    push r15

    fxsave [rdx]            ; save outgoing FPU/SSE state
    mov [rdi], rsp          ; save outgoing stack pointer
    mov rsp, rsi            ; load incoming stack pointer
    fxrstor [rcx]           ; restore incoming FPU/SSE state

    pop r15
    pop r14
    pop r13
    pop r12
    pop rbp
    pop rbx
    ret                     ; return into the incoming thread
