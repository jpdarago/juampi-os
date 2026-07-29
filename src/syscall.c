// The int-0x80 syscall dispatcher and the runner for hosted (newlib-linked)
// programs (see syscall.h). A hosted program is a normal ANSI-C ELF: crt0 calls
// main() and, on return, exit(), which flushes stdio and traps out with the
// exit syscall. The program runs in ring 0 (like the lab binaries) but talks to
// the kernel only through this trap, so its libc doesn't link against kernel
// symbols. One program runs at a time, synchronously, on the caller's stack.
//
// Syscall ABI: number in rax, args in rdi/rsi/rdx, return in rax. A negative
// return is -errno (newlib values); the libgloss stubs unpack it. The numbers
// here must match build/hosted/syscalls.c.

#include <syscall.h>
#include <idt.h>
#include <fault.h> // fault_jmp_buf, setjmp, longjmp (klibc_setjmp.S)
#include <console.h>
#include <memory.h>
#include <alloc.h>
#include <rtc.h>
#include <elf64.h>
#include <utils.h>

#define SYS_exit 0
#define SYS_read 1
#define SYS_write 2
#define SYS_open 3
#define SYS_close 4
#define SYS_lseek 5
#define SYS_fstat 6
#define SYS_isatty 7 // resolved in libgloss; never reaches the kernel
#define SYS_sbrk 8
#define SYS_gettimeofday 9

// errno values returned as negatives (must match newlib's <errno.h>).
#define E_BADF 9
#define E_NOMEM 12
#define E_FAULT 14
#define E_NOSYS 88

#define HOSTED_HEAP_SZ (8u * 1024 * 1024) // program heap for sbrk-based malloc

// One hosted program at a time.
static fault_jmp_buf exit_env; // hosted_run's return point (set before jumping)
static bool active;
static int exit_status;

// The running program's break (heap) region.
static uint8_t* brk_base;
static uint8_t* brk_cur;
static uint8_t* brk_end;

static long sys_write(long fd, const char* buf, long len)
{
    if (buf == NULL || len < 0) {
        return -E_FAULT;
    }
    if (fd == 1 || fd == 2) { // stdout / stderr -> console
        console_write(buf, (size_t)len);
        return len;
    }
    return -E_BADF; // ext2-backed file descriptors land in the next iteration
}

static long sys_read(long fd, char* buf, long len)
{
    if (buf == NULL || len < 0) {
        return -E_FAULT;
    }
    if (fd != 0 || len == 0) {
        return fd == 0 ? 0 : -E_BADF;
    }
    // Line-oriented read from the console (echoing is the console's job).
    long i = 0;
    while (i < len) {
        int c = console_getch();
        if (c < 0) {
            break;
        }
        buf[i++] = (char)c;
        if (c == '\n' || c == '\r') {
            break;
        }
    }
    return i;
}

// Move the program break. Returns the previous break, or -1 (newlib -> ENOMEM)
// if the request would leave the program's heap region.
static long sys_sbrk(long incr)
{
    if (brk_cur == NULL) {
        return -1;
    }
    uint8_t* prev = brk_cur;
    uint8_t* next = brk_cur + incr;
    if (next < brk_base || next > brk_end) {
        return -1;
    }
    brk_cur = next;
    return (long)(uintptr_t)prev;
}

static void syscall_handler(interrupt_frame* f)
{
    long n = (long)f->rax;
    long a = (long)f->rdi;
    long b = (long)f->rsi;
    long c = (long)f->rdx;
    long r;
    switch (n) {
    case SYS_exit:
        exit_status = (int)a;
        longjmp(exit_env, 1); // unwind back to hosted_run; never returns
        return;               // (unreachable)
    case SYS_write:
        r = sys_write(a, (const char*)b, c);
        break;
    case SYS_read:
        r = sys_read(a, (char*)b, c);
        break;
    case SYS_sbrk:
        r = sys_sbrk(a);
        break;
    case SYS_gettimeofday:
        r = (long)rtc_epoch();
        break;
    case SYS_open:
    case SYS_close:
    case SYS_lseek:
    case SYS_fstat:
        r = -E_NOSYS; // ext2-backed files: next iteration
        break;
    default:
        r = -E_NOSYS;
        break;
    }
    f->rax = (uint64_t)r;
}

void syscall_init(void)
{
    register_interrupt_handler(0x80, syscall_handler);
}

int hosted_run(const void* image, size_t size, int argc, char** argv)
{
    (void)size;
    if (active) {
        return -1; // no nested hosted programs
    }
    // Must be a hosted ELF (defines _start via our crt0); lab ELFs don't.
    if (elf64_symbol(image, "_start") == 0) {
        return -1;
    }
    uint64_t entry = elf64_load_exec((void*)(uintptr_t)image);
    if (entry == 0) {
        return -1;
    }

    // Give the program a heap for sbrk-based malloc.
    brk_base = new (&heap_default()->base, uint8_t, HOSTED_HEAP_SZ);
    brk_cur = brk_base;
    brk_end = brk_base + HOSTED_HEAP_SZ;

    active = true;
    exit_status = -1;
    if (setjmp(exit_env) == 0) {
        // Call the crt0 entry as _start(argc, argv): argc in rdi, argv in rsi.
        void (*start)(long, char**) = (void (*)(long, char**))(uintptr_t)entry;
        start((long)argc, argv);
        // _start never returns (it calls exit); if it does, treat as exit(0).
        exit_status = 0;
    } else {
        // Reached via the exit syscall's longjmp. The int-0x80 gate masked
        // interrupts and we bypassed iret, so re-enable them (as fault recovery
        // does) before returning to the shell.
        __asm__ __volatile__("sti");
    }

    heap_free(heap_default(), brk_base);
    brk_base = brk_cur = brk_end = NULL;
    active = false;
    return exit_status;
}
