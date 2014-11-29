#include <syscalls.h>
#include <exception.h>
#include <utils.h>
#include <scrn.h>
#include <tasks.h>
#include <fs.h>
#include <cmos.h>

#define MAX_SYSCALLS 32
syscall syscalls[MAX_SYSCALLS];

//Syscalls de monitor
void syscall_scrn_print(gen_regs* r, int_trace* it)
{
    uchar row = r->ebx, col = r->ecx;
    char* str = (char*) r->edx;
    r->eax = scrn_pos_print(row,col,str);
}

extern void syscall_fork(gen_regs* r, int_trace* it);
void syscall_exit(gen_regs* r,int_trace* it)
{
    uint eflags = irq_cli();
    do_exit();
    irq_sti(eflags);
}
void syscall_wait(gen_regs* r,int_trace* it)
{
    do_wait(r->ebx);
}
void syscall_sleep(gen_regs* r,int_trace* it)
{
    do_sleep();
}
void syscall_coma(gen_regs* r,int_trace* it)
{
    do_coma();
}
void syscall_kill(gen_regs* r,int_trace* it)
{
    r->eax = do_kill(r->ebx,r->ecx);
}
void syscall_signal(gen_regs* r,int_trace* it)
{
    r->eax = do_signal(r->ebx,(signal_handler)r->ecx);
}
void syscall_clear_signal(gen_regs* r,int_trace* it)
{
    r->eax = do_clear_signal(r->ebx);
}

void syscall_open(gen_regs *r, int_trace *it)
{
    char * pathname = (char *)r->ebx;
    uint flags = r->ecx;

    r->eax = do_open(pathname,flags);
}

void syscall_read(gen_regs *r, int_trace *it)
{
    int fd = r->ebx;
    uint bytes = r->ecx;
    void * buffer = (void *) r->edx;

    r->eax = do_read(fd,bytes,buffer);
}

void syscall_close(gen_regs *r, int_trace *it)
{
    int fd = r->ebx;
    r->eax = do_close(fd);
}

void syscall_write(gen_regs *r, int_trace *it)
{
    int fd = r->ebx;
    uint bytes = r->ecx;
    void * buffer = (void *) r->edx;

    r->eax = do_write(fd,bytes,buffer);
}

void syscall_exec(gen_regs *r, int_trace *it)
{
    //TODO: el cli aca implica que la llamada a disco
    //que hace exec es super bloqueante. Considerar cambiar eso.
    uint eflags = irq_cli();
    char * filename = (char *) r->ebx;
    char ** args = (char **) r->ecx;
    int res = do_exec(filename,args,it,&r->ebp);
    irq_sti(eflags);
    r->eax = res;
}

void syscall_get_pid(gen_regs *r, int_trace *it)
{
    r->eax = do_get_pid();
}

void syscall_get_cwd(gen_regs *r, int_trace *it)
{
    char * buf = (char *) r->ebx;
    do_get_cwd(buf);
}

void syscall_set_cwd(gen_regs *r, int_trace *it)
{
    const char * new_dir = (const char *) r->ebx;
    r->eax = do_set_cwd(new_dir);
}

void syscall_readdir(gen_regs *r, int_trace *it)
{
    int fd = r->ebx;
    dirent * d = (dirent *) r->ecx;
    r->eax = do_readdir(fd,d);
}

void syscall_gettime(gen_regs *r, int_trace *it)
{
    date * d = (date *) r->ebx;
    get_current_date(d);
    r->eax = 0;
}

//Stubs generales
void syscall_register(uint code, syscall s)
{
    if(syscalls[code] || code >= MAX_SYSCALLS) {
        kernel_panic("Se intento cargar una syscall "
                     "ya existente o invalida");
    }
    syscalls[code] = s;
}

void syscalls_initialize()
{
    memset(syscalls,0,sizeof(syscalls));
    syscall_register(0,syscall_scrn_print);
    syscall_register(1,syscall_fork);
    syscall_register(2,syscall_exit);
    syscall_register(3,syscall_wait);
    syscall_register(4,syscall_sleep);
    syscall_register(5,syscall_kill);
    syscall_register(6,syscall_signal);
    syscall_register(7,syscall_clear_signal);
    syscall_register(8,syscall_open);
    syscall_register(9,syscall_read);
    syscall_register(10,syscall_close);
    syscall_register(11,syscall_write);
    syscall_register(12,syscall_exec);
    syscall_register(13,syscall_get_pid);
    syscall_register(14,syscall_get_cwd);
    syscall_register(15,syscall_set_cwd);
    syscall_register(16,syscall_readdir);
    syscall_register(17,syscall_gettime);
    syscall_register(MAX_SYSCALLS-1,syscall_coma);
}

void syscalls_entry_point(gen_regs g,int_trace t)
{
    uint code = g.eax;
    if(code < MAX_SYSCALLS && syscalls[code]) {
        syscall handler = syscalls[code];
        if(syscalls[code] == NULL)
            kernel_panic("Syscall invalida");
        handler(&g,&t);
    }
}
