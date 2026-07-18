; Kernel entry stub. Limine jumps here in 64-bit long mode; we enable the x87
; FPU and SSE/SSE2 (so the kernel — and Lua — can use floating point) before any
; C code runs, then call kmain. Doing this first is essential: the kernel is
; compiled with SSE codegen enabled, so the compiler may emit SSE instructions
; anywhere, and executing one while CR0.EM=1 / CR4.OSFXSR=0 would #UD.

bits 64

global kentry
extern kmain

kentry:
    mov rax, cr0
    btr rax, 2          ; CR0.EM = 0  (no x87 emulation; real FPU/SSE)
    bts rax, 1          ; CR0.MP = 1  (monitor coprocessor)
    mov cr0, rax

    mov rax, cr4
    bts rax, 9          ; CR4.OSFXSR     = 1 (enable SSE + fxsave/fxrstor)
    bts rax, 10         ; CR4.OSXMMEXCPT = 1 (unmasked SSE exceptions -> #XF)
    mov cr4, rax

    fninit              ; x87 to a known state

    ; Guarantee a 16-byte-aligned stack before the first C call. The SysV ABI
    ; requires rsp % 16 == 0 immediately before a `call`; with SSE codegen on,
    ; GCC emits movaps to aligned stack slots and a misaligned stack faults.
    and rsp, -16
    call kmain
.hang:
    cli
    hlt
    jmp .hang
