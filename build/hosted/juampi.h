// juampiOS platform extensions for hosted programs — the bits beyond POSIX that
// a graphical/interactive program (a game) needs: the framebuffer, raw key
// events, and a millisecond clock. Implemented over `int 0x80` in syscalls.c
// and serviced by the kernel in src/syscall.c.

#ifndef JUAMPI_H
#define JUAMPI_H

// Screen size in pixels. Returns 0 and fills *w/*h, or -1 if headless (no
// framebuffer). Optional — programs may just render at a fixed size and present.
int juampi_fb_info(int* w, int* h);

// Present one frame: blit a w*h buffer of 0x00RRGGBB pixels, centered, to the
// screen. The first call takes the screen over from the desktop; it is handed
// back when the program exits. Returns 0, or -1 if headless.
int juampi_fb_present(const void* pixels, int w, int h);

// Non-blocking raw key event, or -1 if none pending. The return value is the
// PS/2 set-1 make code (bit 0x80 set for E0-extended keys: arrows, right-ctrl);
// *pressed is 1 for key-down, 0 for key-up. Reports both press and release.
// A few useful make codes:
#define JK_ESC 0x01
#define JK_CTRL 0x1D
#define JK_ENTER 0x1C
#define JK_SPACE 0x39
#define JK_SHIFT 0x2A
#define JK_ALT 0x38
#define JK_TAB 0x0F
#define JK_UP (0x48 | 0x80)    // extended
#define JK_DOWN (0x50 | 0x80)  // extended
#define JK_LEFT (0x4B | 0x80)  // extended
#define JK_RIGHT (0x4D | 0x80) // extended
int juampi_getkey(int* pressed);

// Milliseconds since boot (monotonic).
unsigned long juampi_ticks_ms(void);

// Play 8-bit unsigned mono PCM (`nsamples` at `rate` Hz, volume 0-127) through
// the kernel mixer, which resamples and mixes it with other sounds. Returns a
// voice handle (>=0) or -1. juampi_audio_stop() halts it; juampi_audio_playing()
// reports whether it's still going. Doom's sound effects use this.
int juampi_audio_play(const void* pcm_u8, int nsamples, int rate, int vol);
void juampi_audio_stop(int voice);
int juampi_audio_playing(int voice);

#endif
