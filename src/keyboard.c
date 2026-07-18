#include <keyboard.h>
#include <idt.h>
#include <ports.h>

#include <stdint.h>
#include <stdbool.h>

#define PS2_DATA 0x60
#define KBD_IRQ 1
#define KBD_VECTOR 33 // PIC remap base 0x20 + IRQ 1

// Scancode set 1 (the i8042 translates set 2 to set 1 by default), US layout.
// Index = make code; 0 = no printable mapping. Break codes are make | 0x80.
static const char keymap[128] = {
        0,   27,   '1',  '2', '3',  '4', '5', '6', '7', '8', '9', '0', '-',
        '=', '\b', '\t', 'q', 'w',  'e', 'r', 't', 'y', 'u', 'i', 'o', 'p',
        '[', ']',  '\n', 0,   'a',  's', 'd', 'f', 'g', 'h', 'j', 'k', 'l',
        ';', '\'', '`',  0,   '\\', 'z', 'x', 'c', 'v', 'b', 'n', 'm', ',',
        '.', '/',  0,    '*', 0,    ' ', 0,   0,   0,   0,   0,   0,   0,
        0,   0,    0,    0,   0,    0,   0,   0,   0,   0,   0,   0,   0,
};
static const char keymap_shift[128] = {
        0,   27,   '!',  '@', '#', '$', '%', '^', '&', '*', '(', ')', '_',
        '+', '\b', '\t', 'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P',
        '{', '}',  '\n', 0,   'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L',
        ':', '"',  '~',  0,   '|', 'Z', 'X', 'C', 'V', 'B', 'N', 'M', '<',
        '>', '?',  0,    '*', 0,   ' ', 0,   0,   0,   0,   0,   0,   0,
        0,   0,    0,    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
};

#define SC_LSHIFT 0x2A
#define SC_RSHIFT 0x36
#define SC_CTRL 0x1D
#define SC_CAPS 0x3A
#define SC_EXTENDED 0xE0

// Ring buffer filled by the IRQ handler, drained by keyboard_poll. Single
// producer (the interrupt) and single consumer; on this single CPU the consumer
// masks nothing because index updates are single writes.
#define KBUF_SZ 128
static volatile char kbuf[KBUF_SZ];
static volatile uint32_t khead, ktail;

static bool shift, ctrl, caps, extended;

static void kbd_irq(interrupt_frame* f)
{
    (void)f;
    uint8_t sc = inb(PS2_DATA);

    if (sc == SC_EXTENDED) {
        // Extended prefix (arrows, right ctrl/alt, ...): consume the prefix and
        // ignore the extended key itself for now.
        extended = true;
        return;
    }
    if (extended) {
        extended = false;
        if ((sc & 0x7F) == SC_CTRL) {
            ctrl = !(sc & 0x80);
        }
        return;
    }

    bool release = sc & 0x80;
    uint8_t code = sc & 0x7F;

    if (code == SC_LSHIFT || code == SC_RSHIFT) {
        shift = !release;
        return;
    }
    if (code == SC_CTRL) {
        ctrl = !release;
        return;
    }
    if (code == SC_CAPS) {
        if (!release) {
            caps = !caps;
        }
        return;
    }
    if (release) {
        return;
    }

    char c = shift ? keymap_shift[code] : keymap[code];
    if (c == 0) {
        return;
    }
    if (caps && !shift && c >= 'a' && c <= 'z') {
        c -= 'a' - 'A';
    } else if (caps && shift && c >= 'A' && c <= 'Z') {
        c += 'a' - 'A';
    }
    if (ctrl && (c >= 'a' && c <= 'z')) {
        c &= 0x1F; // Ctrl-A .. Ctrl-Z
    }

    uint32_t next = (khead + 1) % KBUF_SZ;
    if (next != ktail) { // drop the key if the buffer is full
        kbuf[khead] = c;
        khead = next;
    }
}

int keyboard_poll(void)
{
    if (khead == ktail) {
        return -1;
    }
    char c = kbuf[ktail];
    ktail = (ktail + 1) % KBUF_SZ;
    return (unsigned char)c;
}

void keyboard_init(void)
{
    // Drain any pending byte so a stale scancode does not wedge the FIFO.
    (void)inb(PS2_DATA);
    register_interrupt_handler(KBD_VECTOR, kbd_irq);
    irq_unmask(KBD_IRQ);
}
