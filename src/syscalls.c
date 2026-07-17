#include <syscalls.h>
#include <exception.h>
#include <utils.h>
#include <scrn.h>
#include <tasks.h>
#include <fs.h>
#include <cmos.h>
#include <paging.h>

#define MAX_SYSCALLS 32
syscall syscalls[MAX_SYSCALLS];

// Guards for pointers crossing the syscall boundary: reject any that does not
// lie in the calling process's own user-accessible memory, failing the syscall
// with -EFAULT rather than letting the kernel dereference an arbitrary address.
#define REQUIRE_USER_STR_MAX(p, max)                                           \
    do {                                                                       \
        if (!user_string_ok((const char*)(p), (max))) {                        \
            r->eax = -EFAULT;                                                  \
            return;                                                            \
        }                                                                      \
    } while (0)
#define REQUIRE_USER_STR(p) REQUIRE_USER_STR_MAX((p), FS_MAXLEN + 1)
#define REQUIRE_USER_BUF(p, n, write)                                          \
    do {                                                                       \
        if (!user_access_ok((uint)(intptr)(p), (n), (write))) {                \
            r->eax = -EFAULT;                                                  \
            return;                                                            \
        }                                                                      \
    } while (0)

// Monitor syscalls
void syscall_scrn_print(gen_regs* r, int_trace* it)
{
    uchar row = r->ebx, col = r->ecx;
    char* str = (char*)((intptr)r->edx);
    REQUIRE_USER_STR_MAX(str, 4096);
    r->eax = scrn_pos_print(row, col, str);
}

extern void syscall_fork(gen_regs* r, int_trace* it);
void syscall_exit(gen_regs* r, int_trace* it)
{
    uint eflags = irq_cli();
    do_exit();
    irq_sti(eflags);
}
void syscall_wait(gen_regs* r, int_trace* it)
{
    do_wait(r->ebx);
}
void syscall_sleep(gen_regs* r, int_trace* it)
{
    do_sleep();
}
void syscall_coma(gen_regs* r, int_trace* it)
{
    do_coma();
}
void syscall_kill(gen_regs* r, int_trace* it)
{
    r->eax = do_kill(r->ebx, r->ecx);
}
void syscall_signal(gen_regs* r, int_trace* it)
{
    // The handler runs in user mode, so it must be a user-space address.
    if (r->ecx != 0 && !user_access_ok(r->ecx, 1, false)) {
        r->eax = -EFAULT;
        return;
    }
    r->eax = do_signal(r->ebx, (signal_handler)((intptr)r->ecx));
}
void syscall_clear_signal(gen_regs* r, int_trace* it)
{
    r->eax = do_clear_signal(r->ebx);
}

void syscall_open(gen_regs* r, int_trace* it)
{
    char* pathname = (char*)((intptr)r->ebx);
    uint flags = r->ecx;

    REQUIRE_USER_STR(pathname);
    r->eax = do_open(pathname, flags);
}

void syscall_read(gen_regs* r, int_trace* it)
{
    int fd = r->ebx;
    uint bytes = r->ecx;
    void* buffer = (void*)((intptr)r->edx);

    REQUIRE_USER_BUF(buffer, bytes, true);
    r->eax = do_read(fd, bytes, buffer);
}

void syscall_close(gen_regs* r, int_trace* it)
{
    int fd = r->ebx;
    r->eax = do_close(fd);
}

void syscall_write(gen_regs* r, int_trace* it)
{
    int fd = r->ebx;
    uint bytes = r->ecx;
    void* buffer = (void*)((intptr)r->edx);

    REQUIRE_USER_BUF(buffer, bytes, false);
    r->eax = do_write(fd, bytes, buffer);
}

void syscall_exec(gen_regs* r, int_trace* it)
{
    char* filename = (char*)((intptr)r->ebx);
    char** args = (char**)((intptr)r->ecx);

    // Validate everything before disabling interrupts, so the -EFAULT early
    // returns can't leave the CPU with interrupts off. The argv array must be a
    // NULL-terminated list of readable pointers to bounded, readable strings.
    REQUIRE_USER_STR(filename);
    for (uint i = 0;; i++) {
        if (i >= EXEC_MAX_ARGC) {
            r->eax = -EFAULT;
            return;
        }
        if (!user_access_ok((uint)(intptr)&args[i], sizeof(char*), false)) {
            r->eax = -EFAULT;
            return;
        }
        if (args[i] == NULL)
            break;
        if (!user_string_ok(args[i], EXEC_MAX_ARGSZ)) {
            r->eax = -EFAULT;
            return;
        }
    }

    // TODO: the cli here implies that the disk call
    // that exec makes is super blocking. Consider changing that.
    uint eflags = irq_cli();
    int res = do_exec(filename, args, it, &r->ebp);
    irq_sti(eflags);
    r->eax = res;
}

void syscall_get_pid(gen_regs* r, int_trace* it)
{
    r->eax = do_get_pid();
}

void syscall_get_cwd(gen_regs* r, int_trace* it)
{
    char* buf = (char*)((intptr)r->ebx);
    REQUIRE_USER_BUF(buf, FS_MAXLEN, true);
    do_get_cwd(buf);
}

void syscall_set_cwd(gen_regs* r, int_trace* it)
{
    const char* new_dir = (const char*)((intptr)r->ebx);
    REQUIRE_USER_STR(new_dir);
    r->eax = do_set_cwd(new_dir);
}

void syscall_readdir(gen_regs* r, int_trace* it)
{
    int fd = r->ebx;
    dirent* d = (dirent*)((intptr)r->ecx);
    REQUIRE_USER_BUF(d, sizeof(dirent), true);
    r->eax = do_readdir(fd, d);
}

void syscall_gettime(gen_regs* r, int_trace* it)
{
    date* d = (date*)((intptr)r->ebx);
    REQUIRE_USER_BUF(d, sizeof(date), true);
    get_current_date(d);
    r->eax = 0;
}

void syscall_mkdir(gen_regs* r, int_trace* it)
{
    char* pathname = (char*)((intptr)r->ebx);
    REQUIRE_USER_STR(pathname);
    r->eax = do_mkdir(pathname);
}

void syscall_unlink(gen_regs* r, int_trace* it)
{
    char* pathname = (char*)((intptr)r->ebx);
    REQUIRE_USER_STR(pathname);
    r->eax = do_unlink(pathname);
}

void syscall_rmdir(gen_regs* r, int_trace* it)
{
    char* pathname = (char*)((intptr)r->ebx);
    REQUIRE_USER_STR(pathname);
    r->eax = do_rmdir(pathname);
}

// General stubs
void syscall_register(uint code, syscall s)
{
    if (syscalls[code] || code >= MAX_SYSCALLS) {
        kernel_panic("Attempted to register a syscall "
                     "that already exists or is invalid");
    }
    syscalls[code] = s;
}

void syscalls_initialize()
{
    memset(syscalls, 0, sizeof(syscalls));
    syscall_register(0, syscall_scrn_print);
    syscall_register(1, syscall_fork);
    syscall_register(2, syscall_exit);
    syscall_register(3, syscall_wait);
    syscall_register(4, syscall_sleep);
    syscall_register(5, syscall_kill);
    syscall_register(6, syscall_signal);
    syscall_register(7, syscall_clear_signal);
    syscall_register(8, syscall_open);
    syscall_register(9, syscall_read);
    syscall_register(10, syscall_close);
    syscall_register(11, syscall_write);
    syscall_register(12, syscall_exec);
    syscall_register(13, syscall_get_pid);
    syscall_register(14, syscall_get_cwd);
    syscall_register(15, syscall_set_cwd);
    syscall_register(16, syscall_readdir);
    syscall_register(17, syscall_gettime);
    syscall_register(18, syscall_mkdir);
    syscall_register(19, syscall_unlink);
    syscall_register(20, syscall_rmdir);
    syscall_register(MAX_SYSCALLS - 1, syscall_coma);
}

void syscalls_entry_point(gen_regs g, int_trace t)
{
    uint code = g.eax;
    if (code < MAX_SYSCALLS && syscalls[code]) {
        syscall handler = syscalls[code];
        handler(&g, &t);
    }
}
