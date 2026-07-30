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
#include <ext2.h> // file-backed descriptors
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
#define E_NOENT 2
#define E_IO 5
#define E_BADF 9
#define E_NOMEM 12
#define E_FAULT 14
#define E_INVAL 22
#define E_MFILE 24
#define E_NOSYS 88

// newlib <fcntl.h> open() flags (BSD-style values, not Linux's).
#define O_RDONLY 0
#define O_WRONLY 1
#define O_RDWR 2
#define O_ACCMODE 3
#define O_APPEND 0x0008
#define O_CREAT 0x0200
#define O_TRUNC 0x0400

// A hosted file descriptor: the whole file buffered in the kernel heap. Reads
// serve from the buffer; writes grow it and flush to ext2 on close (matching
// the raw-block Lua API's simple whole-file model). fds 0-2 are the console.
#define FD_BASE 3
#define MAX_FD 16
struct hfile {
    bool used;
    bool writable;
    bool dirty;
    char* buf;
    size_t size; // logical length
    size_t cap;  // allocated capacity
    size_t pos;  // read/write cursor
    char path[128];
};
static struct hfile files[MAX_FD];

static struct hfile* fd_get(long fd)
{
    if (fd < FD_BASE || fd >= MAX_FD || !files[fd].used) {
        return NULL;
    }
    return &files[fd];
}

// Grow h->buf so at least `need` bytes fit (heap has no realloc; copy over).
static void hfile_reserve(struct hfile* h, size_t need)
{
    if (need <= h->cap) {
        return;
    }
    size_t cap = h->cap ? h->cap : 64;
    while (cap < need) {
        cap *= 2;
    }
    char* nb = new (&heap_default()->base, char, (ptrdiff_t)cap);
    if (h->buf != NULL) {
        memcpy(nb, h->buf, h->size);
        heap_free(heap_default(), h->buf);
    }
    h->buf = nb;
    h->cap = cap;
}

static void hfile_close(struct hfile* h)
{
    if (h->writable && h->dirty) {
        ext2_write_file(h->path, h->buf, h->size);
    }
    if (h->buf != NULL) {
        heap_free(heap_default(), h->buf);
    }
    *h = (struct hfile){0};
}

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
    struct hfile* h = fd_get(fd);
    if (h == NULL || !h->writable) {
        return -E_BADF;
    }
    hfile_reserve(h, h->pos + (size_t)len);
    memcpy(h->buf + h->pos, buf, (size_t)len);
    h->pos += (size_t)len;
    if (h->pos > h->size) {
        h->size = h->pos;
    }
    h->dirty = true;
    return len;
}

static long sys_read(long fd, char* buf, long len)
{
    if (buf == NULL || len < 0) {
        return -E_FAULT;
    }
    if (fd == 0) { // stdin: line-oriented console read (console echoes)
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
    struct hfile* h = fd_get(fd);
    if (h == NULL) {
        return -E_BADF;
    }
    size_t avail = h->size - h->pos;
    if (avail > (size_t)len) {
        avail = (size_t)len;
    }
    memcpy(buf, h->buf + h->pos, avail);
    h->pos += avail;
    return (long)avail;
}

static long sys_open(const char* path, long flags, long mode)
{
    (void)mode;
    if (path == NULL) {
        return -E_FAULT;
    }
    int fd = -1;
    for (int i = FD_BASE; i < MAX_FD; i++) {
        if (!files[i].used) {
            fd = i;
            break;
        }
    }
    if (fd < 0) {
        return -E_MFILE;
    }
    struct hfile* h = &files[fd];
    *h = (struct hfile){0};
    size_t k = 0;
    while (path[k] != '\0' && k < sizeof(h->path) - 1) {
        h->path[k] = path[k];
        k++;
    }
    h->path[k] = '\0';

    long acc = flags & O_ACCMODE;
    h->writable = (acc == O_WRONLY || acc == O_RDWR);

    // Load existing contents unless truncating; a read-only open must exist.
    size_t sz = 0;
    void* data = (flags & O_TRUNC) ? NULL : ext2_read_path(h->path, &sz);
    if (acc == O_RDONLY && data == NULL) {
        return -E_NOENT;
    }
    if (data != NULL) {
        hfile_reserve(h, sz);
        memcpy(h->buf, data, sz);
        h->size = sz;
        ext2_free(data);
    } else {
        hfile_reserve(h, 64);
    }
    h->pos = (flags & O_APPEND) ? h->size : 0;
    if (flags & O_TRUNC) {
        h->dirty = true; // truncation must reach the disk even with no writes
    }
    h->used = true;
    return fd;
}

static long sys_close(long fd)
{
    struct hfile* h = fd_get(fd);
    if (h == NULL) {
        return -E_BADF;
    }
    bool flush = h->writable && h->dirty;
    bool ok = !flush || ext2_write_file(h->path, h->buf, h->size);
    if (h->buf != NULL) {
        heap_free(heap_default(), h->buf);
    }
    *h = (struct hfile){0};
    return ok ? 0 : -E_IO;
}

static long sys_lseek(long fd, long off, long whence)
{
    struct hfile* h = fd_get(fd);
    if (h == NULL) {
        return -E_BADF;
    }
    long base = whence == 0 ? 0 : whence == 1 ? (long)h->pos : (long)h->size;
    long np = base + off;
    if (np < 0) {
        return -E_INVAL;
    }
    h->pos = (size_t)np;
    return np;
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

static void syscall_handler(struct interrupt_frame* f)
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
        r = sys_open((const char*)a, b, c);
        break;
    case SYS_close:
        r = sys_close(a);
        break;
    case SYS_lseek:
        r = sys_lseek(a, b, c);
        break;
    case SYS_fstat: { // libgloss only asks the kernel for a file's size
        struct hfile* h = fd_get(a);
        r = h != NULL ? (long)h->size : -E_BADF;
        break;
    }
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

    // Flush + release any descriptors the program left open (newlib's exit
    // fcloses stdio streams, but a raw open() without close() would otherwise
    // lose its writes and leak the buffer).
    for (int i = FD_BASE; i < MAX_FD; i++) {
        if (files[i].used) {
            hfile_close(&files[i]);
        }
    }

    heap_free(heap_default(), brk_base);
    brk_base = brk_cur = brk_end = NULL;
    active = false;
    return exit_status;
}
