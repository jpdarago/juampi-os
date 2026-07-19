// x86-64 interrupt/exception entry stubs. Each of the 256 vectors pushes a
// uniform frame and jumps to a common trampoline that saves the general-purpose
// registers, calls the C dispatcher, restores and returns with iretq. Some
// exceptions (8, 10-14, 17) push a hardware error code; the rest push a dummy 0
// so the frame layout is identical either way. An isr_stub_table exports the
// stub addresses for the IDT.

.intel_syntax noprefix
.altmacro

.text

// One entry stub for vector `num`.
.macro ISR num
    .global isr\num
isr\num:
    .if (\num == 8) || (\num == 10) || (\num == 11) || (\num == 12) || (\num == 13) || (\num == 14) || (\num == 17)
        push \num           // error code already pushed by the CPU
    .else
        push 0              // dummy error code
        push \num
    .endif
    jmp isr_common
.endm

// Emit stubs for vectors 0..255.
.set vec, 0
.rept 256
    ISR %vec
    .set vec, vec + 1
.endr

isr_common:
    // Save the general-purpose registers in interrupt_frame order (r15 lowest).
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

    mov rdi, rsp            // arg1: pointer to the interrupt_frame
    mov rbp, rsp            // save the unaligned stack
    and rsp, -16            // the SysV ABI wants a 16-byte-aligned stack at call
    call interrupt_dispatch
    mov rsp, rbp            // restore

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

    add rsp, 16            // discard vector + error code
    iretq

// Address table so idt.c can install every stub by index.
.macro TBLENT num
    .quad isr\num
.endm

.section .rodata
.global isr_stub_table
isr_stub_table:
.set vec, 0
.rept 256
    TBLENT %vec
    .set vec, vec + 1
.endr

.section .note.GNU-stack, "", @progbits
