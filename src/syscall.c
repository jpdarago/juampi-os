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
#include <ktime.h>
#include <elf64.h>
#include <ext2.h>  // file-backed descriptors
#include <audio.h> // PCM playback for games (Doom SFX)
#include <gfx.h>   // framebuffer for graphical hosted programs
#include <ui.h> // suspend the desktop compositor while a program owns the screen
#include <keyboard.h> // raw key events for games
#include <utils.h>

// The syscall numbers live in the shared ABI header, included verbatim by both
// this dispatcher and the hosted-side libgloss stubs so they cannot drift.
#include "../build/hosted/juampi_abi.h"

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

#define HOSTED_HEAP_SZ (16u * 1024 * 1024) // program heap for sbrk-based malloc

// One hosted program at a time.
static fault_jmp_buf exit_env; // hosted_run's return point (set before jumping)
static bool active;
static bool fb_active; // program took over the screen (compositor suspended)
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

// Blit a w*h buffer of 0x00RRGGBB pixels, centered, straight to the screen,
// packing to the framebuffer's channel layout. Lets a graphical hosted program
// present a frame without knowing the pitch or pixel format (SYS_fb_present).
static void fb_blit_centered(const uint32_t* px, int w, int h)
{
    uint64_t pitch = 0;
    uint8_t* base = (uint8_t*)gfx_framebuffer(NULL, &pitch);
    if (base == NULL || px == NULL || w <= 0 || h <= 0) {
        return;
    }
    int sw = (int)gfx_width();
    int sh = (int)gfx_height();
    uint8_t rs, gs, bs;
    gfx_shifts(&rs, &gs, &bs);
    int ox = (sw - w) / 2;
    int oy = (sh - h) / 2;
    for (int y = 0; y < h; y++) {
        int dy = oy + y;
        if (dy < 0 || dy >= sh) {
            continue;
        }
        uint32_t* dst = (uint32_t*)(base + (uint64_t)dy * pitch);
        const uint32_t* src = px + (size_t)y * (size_t)w;
        for (int x = 0; x < w; x++) {
            int dx = ox + x;
            if (dx < 0 || dx >= sw) {
                continue;
            }
            uint32_t p = src[x];
            dst[dx] = (uint32_t)(((p >> 16) & 0xFF) << rs) |
                      (uint32_t)(((p >> 8) & 0xFF) << gs) |
                      (uint32_t)((p & 0xFF) << bs);
        }
    }
}

// Play 8-bit unsigned mono PCM (Doom's sound format) through the mixer, which
// resamples to the mixer rate and mixes it with other voices. `ratevol` packs
// the sample rate (low 16 bits) and volume 0-127 (bits 16-23). Returns the
// mixer voice handle, or -1.
static long sys_audio_play(const uint8_t* u8, long count, long ratevol)
{
    if (u8 == NULL || count <= 0 || !audio_present()) {
        return -1;
    }
    uint32_t rate = (uint32_t)(ratevol & 0xFFFF);
    uint32_t vol = (uint32_t)((ratevol >> 16) & 0xFF);
    if (rate == 0) {
        rate = 11025; // Doom's usual DMX rate
    }
    int16_t* s16 = new (&heap_default()->base, int16_t, (ptrdiff_t)count);
    for (long i = 0; i < count; i++) {
        s16[i] = (int16_t)(((int)u8[i] - 128) << 8); // u8 [0,255] -> s16
    }
    // Declick: DMX samples rarely start/end exactly at the DC midpoint, so a
    // one-shot that stops on a nonzero sample snaps to silence with an audible
    // click. Ramp a short window (~2 ms) in from and out to zero so both edges
    // are continuous. These are one-shots (never looped), so this is safe.
    long fade = (long)rate / 500;
    if (fade > count / 2) {
        fade = count / 2;
    }
    for (long i = 0; i < fade; i++) {
        s16[i] = (int16_t)((int32_t)s16[i] * i / fade);
        s16[count - 1 - i] = (int16_t)((int32_t)s16[count - 1 - i] * i / fade);
    }
    float gain = (float)vol / 127.0f;
    int h = audio_play_pcm(s16, (uint32_t)count, rate, 1, false, gain);
    heap_free(heap_default(), s16); // audio_play_pcm copies into a voice buffer
    return h;
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
    case SYS_fb_info:
        r = gfx_framebuffer(NULL, NULL) == NULL
                    ? -1
                    : (long)(((uint32_t)gfx_height() << 16) |
                             (uint32_t)gfx_width());
        break;
    case SYS_fb_present:
        if (gfx_framebuffer(NULL, NULL) == NULL) {
            r = -1;
            break;
        }
        if (!fb_active && ui_available()) {
            ui_fullscreen_begin(); // take over the screen from the desktop
        }
        fb_active = true;
        fb_blit_centered((const uint32_t*)a, (int)b, (int)c);
        r = 0;
        break;
    case SYS_getkey: {
        int pressed = 0;
        int k = keyboard_poll_raw(&pressed);
        if (k >= 0 && a != 0) {
            *(int*)a = pressed;
        }
        r = k;
        break;
    }
    case SYS_ticks_ms:
        r = (long)ktime_ms();
        break;
    case SYS_audio_play:
        r = sys_audio_play((const uint8_t*)a, b, c);
        break;
    case SYS_audio_stop:
        audio_stop((int)a);
        r = 0;
        break;
    case SYS_audio_active:
        r = audio_voice_active((int)a) ? 1 : 0;
        break;
    case SYS_audio_music_start:
        audio_music_start();
        r = 0;
        break;
    case SYS_audio_music_write:
        r = (b > 0 && a != 0)
                    ? (long)audio_music_write((const int16_t*)a, (uint32_t)b)
                    : 0;
        break;
    case SYS_audio_music_space:
        r = (long)audio_music_space();
        break;
    case SYS_audio_music_stop:
        audio_music_stop();
        r = 0;
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

    // Flush + release any descriptors the program left open (newlib's exit
    // fcloses stdio streams, but a raw open() without close() would otherwise
    // lose its writes and leak the buffer).
    for (int i = FD_BASE; i < MAX_FD; i++) {
        if (files[i].used) {
            hfile_close(&files[i]);
        }
    }

    // Hand the screen back to the desktop if the program took it over, and drop
    // any keystrokes the program left in the input rings (e.g. the 'y' that
    // confirmed a game's quit) so they don't bleed into the shell prompt.
    if (fb_active) {
        ui_fullscreen_end();
        fb_active = false;
        while (keyboard_poll() >= 0) {
        }
        while (keyboard_poll_raw(NULL) >= 0) {
        }
    }

    heap_free(heap_default(), brk_base);
    brk_base = brk_cur = brk_end = NULL;
    active = false;
    return exit_status;
}
