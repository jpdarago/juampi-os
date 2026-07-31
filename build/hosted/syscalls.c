// libgloss syscall stubs for hosted programs on juampiOS. newlib is built with
// --disable-newlib-supplied-syscalls, so its reentrant layer (_write_r, etc.)
// calls these bare POSIX names; each traps into the kernel with `int $0x80`
// (number in rax, args in rdi/rsi/rdx, return in rax). `int 0x80` works from
// ring 0 too — the program runs in ring 0, so the trap just enters the kernel's
// syscall dispatcher (src/syscall.c). Keep the numbers in sync with that file.

#include <sys/stat.h>
#include <sys/types.h>
#include <sys/times.h>
#include <sys/time.h>
#include <errno.h>
#include <stddef.h>

#define SYS_exit 0
#define SYS_read 1
#define SYS_write 2
#define SYS_open 3
#define SYS_close 4
#define SYS_lseek 5
#define SYS_fstat 6
#define SYS_isatty 7
#define SYS_sbrk 8
#define SYS_gettimeofday 9
#define SYS_fb_info 10
#define SYS_fb_present 11
#define SYS_getkey 12
#define SYS_ticks_ms 13
#define SYS_audio_play 14
#define SYS_audio_stop 15
#define SYS_audio_active 16

static long trap(long n, long a, long b, long c)
{
    long ret;
    __asm__ __volatile__("int $0x80"
                         : "=a"(ret)
                         : "a"(n), "D"(a), "S"(b), "d"(c)
                         : "memory");
    return ret;
}

// A negative return is -errno; unpack it into errno and -1 like Linux libcs.
static long ret_errno(long r)
{
    if (r < 0) {
        errno = (int)-r;
        return -1;
    }
    return r;
}

void _exit(int code)
{
    trap(SYS_exit, code, 0, 0);
    for (;;) {
    }
}

// --- juampiOS platform extensions (see juampi.h) ----------------------------
int juampi_fb_info(int* w, int* h)
{
    long r = trap(SYS_fb_info, 0, 0, 0);
    if (r < 0) {
        return -1;
    }
    if (w != NULL) {
        *w = (int)(r & 0xFFFF);
    }
    if (h != NULL) {
        *h = (int)((r >> 16) & 0xFFFF);
    }
    return 0;
}
int juampi_fb_present(const void* pixels, int w, int h)
{
    return (int)trap(SYS_fb_present, (long)pixels, w, h);
}
int juampi_getkey(int* pressed)
{
    return (int)trap(SYS_getkey, (long)pressed, 0, 0);
}
unsigned long juampi_ticks_ms(void)
{
    return (unsigned long)trap(SYS_ticks_ms, 0, 0, 0);
}
int juampi_audio_play(const void* pcm, int nsamples, int rate, int vol)
{
    long ratevol = (rate & 0xFFFF) | ((long)(vol & 0xFF) << 16);
    return (int)trap(SYS_audio_play, (long)pcm, nsamples, ratevol);
}
void juampi_audio_stop(int voice)
{
    trap(SYS_audio_stop, voice, 0, 0);
}
int juampi_audio_playing(int voice)
{
    return (int)trap(SYS_audio_active, voice, 0, 0);
}

int write(int fd, const char* buf, int len)
{
    return (int)ret_errno(trap(SYS_write, fd, (long)buf, len));
}

int read(int fd, char* buf, int len)
{
    return (int)ret_errno(trap(SYS_read, fd, (long)buf, len));
}

int open(const char* path, int flags, int mode)
{
    return (int)ret_errno(trap(SYS_open, (long)path, flags, mode));
}

int close(int fd)
{
    return (int)ret_errno(trap(SYS_close, fd, 0, 0));
}

off_t lseek(int fd, off_t off, int whence)
{
    return (off_t)ret_errno(trap(SYS_lseek, fd, (long)off, whence));
}

// Filled locally so the kernel need not know newlib's struct stat layout. The
// three standard streams are the console (a char device); everything else is a
// regular file whose size we fetch with one scalar syscall.
int fstat(int fd, struct stat* st)
{
    if (fd >= 0 && fd < 3) {
        st->st_mode = S_IFCHR;
        return 0;
    }
    long sz = trap(SYS_fstat, fd, 0, 0);
    if (sz < 0) {
        errno = (int)-sz;
        return -1;
    }
    st->st_mode = S_IFREG;
    st->st_size = sz;
    return 0;
}

int isatty(int fd)
{
    return (fd >= 0 && fd < 3) ? 1 : 0;
}

void* sbrk(ptrdiff_t incr)
{
    long r = trap(SYS_sbrk, (long)incr, 0, 0);
    if (r == -1) {
        errno = ENOMEM;
        return (void*)-1;
    }
    return (void*)r;
}

int gettimeofday(struct timeval* tv, void* tz)
{
    (void)tz;
    long secs = trap(SYS_gettimeofday, 0, 0, 0); // Unix seconds from the RTC
    if (tv != NULL) {
        tv->tv_sec = (time_t)secs;
        tv->tv_usec = 0;
    }
    return 0;
}

// Process-model stubs: juampiOS runs one hosted program at a time, in ring 0.
int getpid(void)
{
    return 1;
}
int kill(int pid, int sig)
{
    (void)pid;
    (void)sig;
    errno = EINVAL;
    return -1;
}
int fork(void)
{
    errno = ENOSYS;
    return -1;
}
int wait(int* status)
{
    (void)status;
    errno = ECHILD;
    return -1;
}
int execve(const char* n, char* const* a, char* const* e)
{
    (void)n;
    (void)a;
    (void)e;
    errno = ENOSYS;
    return -1;
}
int link(const char* a, const char* b)
{
    (void)a;
    (void)b;
    errno = EMLINK;
    return -1;
}
int unlink(const char* a)
{
    (void)a;
    errno = ENOENT;
    return -1;
}
int stat(const char* a, struct stat* st)
{
    (void)a;
    (void)st;
    errno = ENOSYS;
    return -1;
}
clock_t times(struct tms* buf)
{
    (void)buf;
    errno = ENOSYS;
    return (clock_t)-1;
}
