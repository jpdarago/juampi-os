; A tiny, position-independent ring-3 program used to exercise the user-mode +
; syscall path. It is copied byte-for-byte into a user-accessible page and
; entered via iretq. Syscall ABI (port): number in rax, first argument in rdi;
; the kernel returns a value in rax. It makes a "write" syscall (1) and then an
; "exit" syscall (2); if the kernel ever returns from exit it just spins.

bits 64

global user_stub
global user_stub_end

user_stub:
    mov rax, 1             ; syscall: write
    mov rdi, 42            ; argument the kernel will echo
    int 0x80

    mov rax, 2             ; syscall: exit
    mov rdi, 0
    int 0x80
.hang:
    jmp .hang
user_stub_end:
