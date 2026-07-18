// A minimal 64-bit user program for the x86-64 port. Built as a freestanding,
// statically-linked ELF64 and loaded by the kernel as a Limine module (see
// docs/x86-64-port.md, milestone 5). It talks to the kernel over the int 0x80
// syscall ABI: number in rax, args in rdi/rsi, return in rax.

static long sys_write(const char* buf, long len)
{
    long ret;
    __asm__ __volatile__("int $0x80"
                         : "=a"(ret)
                         : "a"(1L), "D"(buf), "S"(len)
                         : "memory");
    return ret;
}

static void sys_exit(long code)
{
    __asm__ __volatile__("int $0x80" : : "a"(2L), "D"(code) : "memory");
    for (;;) {
    }
}

void _start(void)
{
    const char msg[] = "hello from ELF64 userland\n";
    sys_write(msg, sizeof(msg) - 1);
    sys_exit(0);
}
