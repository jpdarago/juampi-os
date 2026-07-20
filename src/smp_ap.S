// Application-processor entry trampoline. Limine starts each AP in 64-bit long
// mode (on the kernel's page tables) at the address we write to its goto_address
// field, passing a `struct limine_mp_info*` in RDI. As in kentry (entry.S), we
// must enable the FPU and SSE before any C runs — the kernel is built with SSE
// codegen, so a stray SSE instruction with CR0.EM=1 / CR4.OSFXSR=0 would #UD.
// RDI is preserved through the CR setup and handed to ap_main.

.intel_syntax noprefix
.text
.global ap_entry

ap_entry:
    mov rax, cr0
    btr rax, 2          // CR0.EM = 0  (no x87 emulation; real FPU/SSE)
    bts rax, 1          // CR0.MP = 1  (monitor coprocessor)
    mov cr0, rax

    mov rax, cr4
    bts rax, 9          // CR4.OSFXSR     = 1 (enable SSE + fxsave/fxrstor)
    bts rax, 10         // CR4.OSXMMEXCPT = 1 (unmasked SSE exceptions -> #XF)
    mov cr4, rax

    fninit              // x87 to a known state

    and rsp, -16        // 16-byte-align the stack before the first C call
    call ap_main        // ap_main(struct limine_mp_info*) — RDI still valid
1:  cli
    hlt
    jmp 1b

.section .note.GNU-stack, "", @progbits
