// juampiOS platform frontend for doomgeneric: wire Doom's 6 hooks to the hosted
// platform layer (juampi.h) — DG_DrawFrame -> fb_present, DG_GetKey -> raw key
// events, DG_GetTicksMs/SleepMs -> the ms clock. main() points Doom at the WAD
// on the ext2 disk and runs the frame loop. Also provides a few harmless POSIX
// stubs Doom references but doesn't need here.

#include "doomgeneric.h"
#include "doomkeys.h"

#include <juampi.h>
#include <errno.h>
#include <stddef.h>

void DG_Init(void)
{
}

void DG_DrawFrame(void)
{
    juampi_fb_present(DG_ScreenBuffer, DOOMGENERIC_RESX, DOOMGENERIC_RESY);
}

uint32_t DG_GetTicksMs(void)
{
    return (uint32_t)juampi_ticks_ms();
}

void DG_SleepMs(uint32_t ms)
{
    unsigned long t = juampi_ticks_ms();
    while (juampi_ticks_ms() - t < ms) {
    }
}

void DG_SetWindowTitle(const char* title)
{
    (void)title;
}

// PS/2 set-1 make code (non-extended) -> ASCII, for menu / weapon / y-n keys.
static const unsigned char sc_ascii[128] = {
        0,   27,  '1', '2',  '3', '4', '5', '6', '7', '8', '9', '0', '-',
        '=', 8,   9,   'q',  'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p',
        '[', ']', 13,  0,    'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l',
        ';', '\'', '`', 0,   '\\', 'z', 'x', 'c', 'v', 'b', 'n', 'm', ',',
        '.', '/', 0,   '*',  0,   ' ',
};

// Translate a raw key (PS/2 make code, bit 0x80 = E0-extended) to a Doom key.
static unsigned char to_doomkey(int code)
{
    switch (code) {
    case JK_RIGHT:
        return KEY_RIGHTARROW;
    case JK_LEFT:
        return KEY_LEFTARROW;
    case JK_UP:
        return KEY_UPARROW;
    case JK_DOWN:
        return KEY_DOWNARROW;
    case JK_ESC:
        return KEY_ESCAPE;
    case JK_ENTER:
        return KEY_ENTER;
    case JK_TAB:
        return KEY_TAB;
    case JK_CTRL:
        return KEY_FIRE; // left ctrl
    case JK_SPACE:
        return KEY_USE;
    case JK_SHIFT:
    case (0x36): // right shift
        return KEY_RSHIFT;
    case JK_ALT:
        return KEY_RALT;
    case 0x0E: // backspace
        return KEY_BACKSPACE;
    default:
        break;
    }
    if (code >= 0 && code < 128) {
        return sc_ascii[code]; // letters/digits for menus & weapon select
    }
    return 0;
}

int DG_GetKey(int* pressed, unsigned char* key)
{
    for (;;) {
        int p = 0;
        int code = juampi_getkey(&p);
        if (code < 0) {
            return 0; // no more events
        }
        unsigned char dk = to_doomkey(code);
        if (dk == 0) {
            continue; // a key Doom doesn't use — skip and keep draining
        }
        *pressed = p;
        *key = dk;
        return 1;
    }
}

int main(int argc, char** argv)
{
    (void)argc;
    (void)argv;
    // Point Doom straight at the WAD on the ext2 disk (skips the IWAD search).
    char* av[] = {"doom", "-iwad", "/doom1.wad", NULL};
    doomgeneric_Create(3, av);
    for (;;) {
        doomgeneric_Tick();
    }
    return 0;
}

// --- POSIX bits Doom references but doesn't need in this environment ---------
char* getenv(const char* name)
{
    (void)name;
    return NULL;
}
int access(const char* path, int mode)
{
    (void)path;
    (void)mode;
    return -1; // "no such file" — Doom falls back to the -iwad path
}
int mkdir(const char* path, unsigned mode)
{
    (void)path;
    (void)mode;
    return 0;
}
int system(const char* cmd)
{
    (void)cmd;
    errno = ENOSYS;
    return -1;
}
int usleep(unsigned us)
{
    unsigned long t = juampi_ticks_ms();
    unsigned long ms = us / 1000u;
    while (juampi_ticks_ms() - t < ms) {
    }
    return 0;
}
int remove(const char* path)
{
    (void)path; // savegame cleanup — harmless to no-op here
    return 0;
}
// Minimal sscanf: Doom only ever parses a single integer ("%d"/"%i"/"%x"/"%o",
// optionally after a leading space and a 0x/0 prefix). Avoids pulling in
// newlib's full vfscanf (wide-char + float + refill machinery).
int sscanf(const char* str, const char* fmt, ...)
{
    (void)fmt;
    __builtin_va_list ap;
    __builtin_va_start(ap, fmt);
    int* out = __builtin_va_arg(ap, int*);
    __builtin_va_end(ap);
    while (*str == ' ' || *str == '\t') {
        str++;
    }
    int neg = 0;
    if (*str == '-') {
        neg = 1;
        str++;
    } else if (*str == '+') {
        str++;
    }
    int base = 10;
    if (str[0] == '0' && (str[1] == 'x' || str[1] == 'X')) {
        base = 16;
        str += 2;
    } else if (str[0] == '0' && str[1] >= '0' && str[1] <= '7') {
        base = 8;
        str += 1;
    }
    long v = 0;
    int any = 0;
    for (;;) {
        int c = *str, d;
        if (c >= '0' && c <= '9') {
            d = c - '0';
        } else if (c >= 'a' && c <= 'f') {
            d = c - 'a' + 10;
        } else if (c >= 'A' && c <= 'F') {
            d = c - 'A' + 10;
        } else {
            break;
        }
        if (d >= base) {
            break;
        }
        v = v * base + d;
        any = 1;
        str++;
    }
    if (!any) {
        return 0;
    }
    if (out != NULL) {
        *out = (int)(neg ? -v : v);
    }
    return 1;
}
int rename(const char* a, const char* b)
{
    (void)a;
    (void)b;
    return -1;
}
// Minimal atof (config parsing) — avoids pulling in newlib's strtod/gdtoa.
double atof(const char* s)
{
    while (*s == ' ' || *s == '\t') {
        s++;
    }
    int neg = 0;
    if (*s == '-') {
        neg = 1;
        s++;
    } else if (*s == '+') {
        s++;
    }
    double v = 0.0;
    while (*s >= '0' && *s <= '9') {
        v = v * 10.0 + (*s++ - '0');
    }
    if (*s == '.') {
        s++;
        double f = 0.1;
        while (*s >= '0' && *s <= '9') {
            v += (*s++ - '0') * f;
            f *= 0.1;
        }
    }
    return neg ? -v : v;
}
