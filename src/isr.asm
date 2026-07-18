; x86-64 interrupt/exception entry stubs. Each of the 48 vectors (exceptions
; 0-31, PIC IRQs 32-47) pushes a uniform frame and jumps to a common trampoline
; that saves the general-purpose registers, calls the C dispatcher, restores and
; returns with iretq. Some exceptions push a hardware error code; the rest push
; a dummy 0 so the frame layout is identical either way.

bits 64

extern interrupt_dispatch

; Vectors that push a CPU error code: 8, 10, 11, 12, 13, 14, 17. The rest do not.
%macro ISR_NOERR 1
global isr%1
isr%1:
    push qword 0        ; dummy error code
    push qword %1       ; vector number
    jmp isr_common
%endmacro

%macro ISR_ERR 1
global isr%1
isr%1:
    push qword %1       ; vector number (error code already on the stack)
    jmp isr_common
%endmacro

%assign v 0
%rep 48
    %if v == 8 || v == 10 || v == 11 || v == 12 || v == 13 || v == 14 || v == 17
        ISR_ERR v
    %else
        ISR_NOERR v
    %endif
    %assign v v+1
%endrep

isr_common:
    ; Save the general-purpose registers in interrupt_frame order (r15 lowest).
    push rax
    push rbx
    push rcx
    push rdx
    push rsi
    push rdi
    push rbp
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15

    mov rdi, rsp        ; arg1: pointer to the interrupt_frame
    mov rbp, rsp        ; save unaligned stack
    and rsp, -16        ; the SysV ABI wants a 16-byte-aligned stack at the call
    call interrupt_dispatch
    mov rsp, rbp        ; restore the frame pointer

    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rbp
    pop rdi
    pop rsi
    pop rdx
    pop rcx
    pop rbx
    pop rax

    add rsp, 16         ; discard vector + error code
    iretq

; Address table so idt.c can install every stub by index.
section .rodata
global isr_stub_table
isr_stub_table:
%assign v 0
%rep 48
    dq isr %+ v
    %assign v v+1
%endrep
