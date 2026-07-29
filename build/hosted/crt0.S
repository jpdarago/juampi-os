// crt0 for hosted (newlib-linked) programs run in ring 0 by juampiOS.
// The kernel loads the ELF, then calls the entry (_start) like a C function
// with argc in %rdi and argv in %rsi (SysV first two args). We run C++/ctor
// init arrays, call main(argc, argv), then hand the result to exit() — newlib's
// exit flushes stdio and traps out via the _exit syscall, so _start never
// returns (the kernel's runner regains control through that syscall).
    .section .text
    .global _start
_start:
    xorq %rbp, %rbp        // mark the outermost frame
    movq %rdi, %r12        // save argc across the init-array call
    movq %rsi, %r13        // save argv
    andq $-16, %rsp        // 16-byte-align the stack per the SysV ABI

    call __libc_init_array // run .init_array constructors (safe if empty)

    movq %r12, %rdi        // argc
    movq %r13, %rsi        // argv
    call main

    movl %eax, %edi        // exit status = main's return
    call exit              // never returns (flushes stdio, traps via _exit)
1:  hlt
    jmp 1b

// __libc_init_array (constructors) calls _init; __libc_fini_array calls _fini.
// We have no .init/.fini sections (no crti.o/crtn.o), so provide empty ones.
    .global _init
_init:
    ret
    .global _fini
_fini:
    ret

    // No executable stack.
    .section .note.GNU-stack,"",@progbits
