// The juampiOS syscall ABI — the one file both sides of the int-0x80 boundary
// include, so the numbers can never drift apart. The kernel dispatcher
// (src/syscall.c) implements these; the hosted libgloss stubs
// (build/hosted/syscalls.c) invoke them. Pure preprocessor on purpose: no
// types, no prototypes, so it is safe to include under kernel and newlib
// regimes alike.
//
// Convention: number in rax, args in rdi/rsi/rdx, return in rax; a negative
// return is -errno (newlib values).

#ifndef JUAMPI_ABI_H
#define JUAMPI_ABI_H

// --- POSIX core (served by the kernel; see the libgloss stubs for the
// locally-resolved calls that never trap: getpid, fork, stat, times, ...) ----
#define SYS_exit 0
#define SYS_read 1
#define SYS_write 2
#define SYS_open 3
#define SYS_close 4
#define SYS_lseek 5
#define SYS_fstat 6  // -> file size as a scalar; libgloss builds struct stat
#define SYS_isatty 7 // resolved in libgloss; never reaches the kernel
#define SYS_sbrk 8
#define SYS_gettimeofday 9 // -> Unix seconds; libgloss fills struct timeval

// --- juampiOS platform extensions (graphics, input, time, audio — see
// build/hosted/juampi.h for the C API over these) ----------------------------
#define SYS_fb_info 10 // -> (height<<16)|width of the screen, or -1 headless
#define SYS_fb_present                                                         \
    11                  // blit a w*h 0x00RRGGBB buffer, centered, to the screen
#define SYS_getkey 12   // -> raw key event (make code, bit7=ext), *pressed
#define SYS_ticks_ms 13 // -> milliseconds since boot
#define SYS_audio_play                                                         \
    14 // play 8-bit unsigned mono PCM -> voice handle, or -1;
       // arg c packs (vol & 0xFF) << 16 | (rate & 0xFFFF)
#define SYS_audio_stop 15        // stop a voice handle
#define SYS_audio_active 16      // -> 1 if a voice handle is still playing
#define SYS_audio_music_start 17 // enable streaming music, clear the ring
#define SYS_audio_music_write 18 // enqueue stereo 48 kHz frames -> accepted
#define SYS_audio_music_space 19 // -> free stereo frames in the music ring
#define SYS_audio_music_stop 20  // disable streaming music
// SYS_getmouse -> 1 if a mouse is present (else 0); fills a caller int[5] with
// {x, y, dx, dy, buttons}: absolute position clamped to the screen, the delta
// since the last call, and the button bitmask (bit0 left, 1 right, 2 middle).
#define SYS_getmouse 21

#endif
