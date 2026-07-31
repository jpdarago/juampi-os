// PS/2 mouse driver over the i8042 aux port (see mouse.h). The controller
// multiplexes the keyboard (IRQ 1) and the mouse (IRQ 12) through the same data
// port 0x60; a status-port bit distinguishes aux bytes. We enable the aux port,
// turn on IRQ 12 in the controller config, tell the mouse to stream, and
// assemble its 3-byte packets in the IRQ handler. Mirrors keyboard.c.

#include <mouse.h>
#include <idt.h>
#include <ports.h>

#include <stdint.h>
#include <stdbool.h>

#define PS2_DATA 0x60
#define PS2_STATUS 0x64 // read: status; write: command
#define MOUSE_IRQ 12
#define MOUSE_VECTOR 44 // vector 32 + IRQ 12 (routed via the IOAPIC)

// i8042 status-register bits.
#define ST_OUTPUT_FULL                                                         \
    0x01                   // a byte is waiting in the output buffer (port 0x60)
#define ST_INPUT_FULL 0x02 // the input buffer is full; wait before writing
#define ST_AUX_DATA 0x20   // the pending output byte came from the aux (mouse)

// i8042 controller commands (written to PS2_STATUS).
#define CMD_ENABLE_AUX 0xA8   // enable the aux (mouse) port
#define CMD_READ_CONFIG 0x20  // next data byte read <- the config byte
#define CMD_WRITE_CONFIG 0x60 // next data byte -> the config byte
#define CMD_WRITE_AUX 0xD4    // next data byte -> the aux device

// Controller config-byte bits.
#define CFG_AUX_IRQ 0x02           // bit 1: raise IRQ12 on aux data
#define CFG_AUX_CLOCK_DISABLE 0x20 // bit 5: gate the aux clock (keep clear)

// PS/2 mouse device commands and its ACK reply.
#define MOUSE_RESET 0xFF
#define MOUSE_SET_DEFAULTS 0xF6
#define MOUSE_ENABLE_REPORTING 0xF4
#define MOUSE_ACK 0xFA

static bool present;

// Movement/buttons accumulated by the IRQ, drained by mouse_poll.
static volatile int acc_dx, acc_dy;
static volatile uint8_t buttons;

// 3-byte packet assembly state.
static volatile uint8_t pkt[3];
static volatile int pkt_idx;

// Spin (bounded) until the controller can accept a write / has a byte to read,
// so a missing device can't wedge boot.
static void wait_write(void)
{
    for (int i = 0; i < 100000; i++) {
        if (!(inb(PS2_STATUS) & ST_INPUT_FULL)) {
            return;
        }
    }
}
static void wait_read(void)
{
    for (int i = 0; i < 100000; i++) {
        if (inb(PS2_STATUS) & ST_OUTPUT_FULL) {
            return;
        }
    }
}

static void ctrl_cmd(uint8_t cmd)
{
    wait_write();
    outb(PS2_STATUS, cmd);
}
static void ctrl_write(uint8_t v)
{
    wait_write();
    outb(PS2_DATA, v);
}
static uint8_t ctrl_read(void)
{
    wait_read();
    return inb(PS2_DATA);
}

// Send a byte to the mouse (aux) and return its acknowledgement (0xFA on ok).
static uint8_t mouse_cmd(uint8_t v)
{
    ctrl_cmd(CMD_WRITE_AUX); // the next data byte is for the aux device
    ctrl_write(v);
    return ctrl_read();
}

// Assemble one aux byte into the 3-byte packet stream. Called from the IRQ 12
// handler below, and from the keyboard IRQ when it finds an aux byte pending in
// the shared output buffer (see kbd_irq): whichever handler drains port 0x60
// must route the byte by its status bit, or the mouse stream desyncs and the
// stray byte becomes a phantom scancode.
void mouse_handle_byte(uint8_t b)
{
    switch (pkt_idx) {
    case 0:
        if (!(b & 0x08)) {
            return; // bit 3 is always set on byte 0 — resync if it isn't
        }
        pkt[0] = b;
        pkt_idx = 1;
        break;
    case 1:
        pkt[1] = b;
        pkt_idx = 2;
        break;
    default:
        pkt[2] = b;
        pkt_idx = 0;
        uint8_t flags = pkt[0];
        // 9-bit signed deltas: the sign bit lives in the flags byte.
        int dx = (int)pkt[1] - ((flags & 0x10) ? 256 : 0);
        int dy = (int)pkt[2] - ((flags & 0x20) ? 256 : 0);
        acc_dx += dx;
        acc_dy -= dy; // PS/2 Y grows upward; screen Y grows downward
        buttons = flags & 0x07;
        break;
    }
}

static void mouse_irq(struct interrupt_frame* f)
{
    (void)f;
    uint8_t st = inb(PS2_STATUS);
    if (!(st & ST_OUTPUT_FULL) || !(st & ST_AUX_DATA)) {
        return; // not an aux byte (a keyboard byte pends: leave it for IRQ 1)
    }
    mouse_handle_byte(inb(PS2_DATA));
}

void mouse_flush(void)
{
    pkt_idx = 0;
    acc_dx = acc_dy = 0;
    buttons = 0;
}

void mouse_init(void)
{
    ctrl_cmd(CMD_ENABLE_AUX);

    // Enable IRQ 12 in the controller config byte, keeping the aux clock
    // running.
    ctrl_cmd(CMD_READ_CONFIG);
    uint8_t cfg = ctrl_read();
    cfg |= CFG_AUX_IRQ;
    cfg = (uint8_t)(cfg & ~CFG_AUX_CLOCK_DISABLE);
    ctrl_cmd(CMD_WRITE_CONFIG);
    ctrl_write(cfg);

    // Reset, then set defaults and enable streaming. Require the ACKs so we
    // bail cleanly on a machine with no mouse.
    if (mouse_cmd(MOUSE_RESET) != MOUSE_ACK) {
        return;
    }
    ctrl_read(); // 0xAA self-test result
    ctrl_read(); // 0x00 device id
    if (mouse_cmd(MOUSE_SET_DEFAULTS) != MOUSE_ACK) {
        return;
    }
    if (mouse_cmd(MOUSE_ENABLE_REPORTING) != MOUSE_ACK) {
        return;
    }

    register_interrupt_handler(MOUSE_VECTOR, mouse_irq);
    irq_unmask(MOUSE_IRQ); // route IRQ 12 via the IOAPIC (no PIC cascade now)
    present = true;
}

// Feed movement/buttons from another input source (the USB HID mouse) into the
// same accumulators the PS/2 IRQ fills. HID Y is already screen-downward, so
// no inversion. Marks the mouse present so mouse_poll starts reporting.
void mouse_inject(int dx, int dy, uint8_t btns)
{
    acc_dx += dx;
    acc_dy += dy;
    buttons = btns;
    present = true;
}

bool mouse_poll(int* dx, int* dy, uint8_t* btns)
{
    // Snapshot and clear the accumulators atomically w.r.t. the IRQ.
    __asm__ __volatile__("cli");
    int x = acc_dx, y = acc_dy;
    acc_dx = 0;
    acc_dy = 0;
    uint8_t b = buttons;
    __asm__ __volatile__("sti");
    if (dx != NULL) {
        *dx = x;
    }
    if (dy != NULL) {
        *dy = y;
    }
    if (btns != NULL) {
        *btns = b;
    }
    return present;
}
