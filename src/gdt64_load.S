// GDT / TSS loading and the ring-0 -> ring-3 transition for the x86-64 port.

.intel_syntax noprefix
.text
.global gdt_flush
.global tss_flush
.global enter_user_mode

// void gdt_flush(void* gdtr /rdi/)
// Load the GDT, reload the data segment registers to kernel data, and reload CS
// to the kernel code selector via a far return.
gdt_flush:
    lgdt [rdi]
    mov ax, 0x10           // kernel data selector
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov fs, ax
    mov gs, ax
    // Far-return to reload CS = 0x08 (kernel code).
    pop rax                // near return address
    mov rcx, 0x08
    push rcx               // CS
    push rax               // RIP
    retfq

// void tss_flush(uint16 selector /di/)
tss_flush:
    mov ax, di
    ltr ax
    ret

// void enter_user_mode(uint64 rip /rdi/, uint64 rsp /rsi/)
// Build an iretq frame for ring 3 and drop into it. Does not return.
enter_user_mode:
    mov ax, 0x23           // user data selector (0x20 | 3)
    mov ds, ax
    mov es, ax

    push 0x23              // SS  = user data | 3
    push rsi               // RSP = user stack
    pushfq
    pop rax
    or rax, 0x200          // set IF so interrupts stay enabled in ring 3
    push rax               // RFLAGS
    push 0x1B              // CS  = user code (0x18 | 3)
    push rdi               // RIP = user entry
    iretq

.section .note.GNU-stack, "", @progbits
