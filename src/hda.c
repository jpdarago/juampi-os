// Intel High Definition Audio output backend (see audio.h). HDA is the audio
// controller on essentially every modern x86 machine (the target Dell XPS
// included), and QEMU models it faithfully (`-device ich9-intel-hda` +
// `-device hda-output`), so this backend is developed and verified there.
//
// Unlike AC'97 (fixed-function, I/O-port mapped), HDA is MMIO and codec-driven:
//   - You talk to codecs by sending 32-bit "verbs" through the CORB command
//     ring and reading responses from the RIRB response ring.
//   - Playback is a DMA "stream": a Buffer Descriptor List (a cyclic ring of
//     {addr,len} period buffers) played continuously, with the position tracked
//     by the stream's Link Position In Buffer register (SDLPIB).
// The controller reset, ring setup, codec walk (find a DAC + an output pin,
// unmute both), and stream format all have to be programmed by hand. Once
// running it exposes the same period-ring vtable as AC'97, so the mixer, Lua
// bindings and IRQ pump are untouched.

#include <audio.h>
#include <pci.h>
#include <paging.h> // iomap + PAGEF_* for the MMIO BAR
#include <mmio.h>   // typed register access
#include <dma.h>
#include <idt.h>  // register_interrupt_handler, irq_unmask, interrupt_frame
#include <acpi.h> // acpi_pci_route (_PRT)
#include <barrier.h>
#include <console.h>
#include <ktime.h> // ktime_ns for bounded reset/response waits
#include <utils.h>

// PCI: multimedia / HD Audio device (class 0x04, subclass 0x03).
#define PCI_CLASS_MULTIMEDIA 0x04
#define PCI_SUBCLASS_HDA 0x03

// --- Controller global registers (MMIO BAR0) --------------------------------
#define REG_GCAP 0x00     // 16: stream counts (OSS/ISS/BSS)
#define REG_GCTL 0x08     // 32: bit0 CRST (0=reset)
#define REG_STATESTS 0x0E // 16: per-SDIN codec-present latch
#define REG_INTCTL 0x20   // 32: bit31 GIE, bit30 CIE, bits[n] per-stream SIE
#define REG_INTSTS 0x24   // 32: bit31 GIS, bits[n] per-stream status
#define REG_CORBLBASE 0x40
#define REG_CORBUBASE 0x44
#define REG_CORBWP 0x48   // 16: write pointer (low 8 bits)
#define REG_CORBRP 0x4A   // 16: read pointer; bit15 = reset
#define REG_CORBCTL 0x4C  // 8: bit1 CORBRUN, bit0 CMEIE
#define REG_CORBSIZE 0x4E // 8: bits[1:0] size select
#define REG_RIRBLBASE 0x50
#define REG_RIRBUBASE 0x54
#define REG_RIRBWP 0x58   // 16: write pointer; bit15 = reset
#define REG_RINTCNT 0x5A  // 16: response interrupt count
#define REG_RIRBCTL 0x5C  // 8: bit1 RIRBDMAEN, bit0 RINTCTL
#define REG_RIRBSTS 0x5D  // 8: bit0 RINTFL (W1C)
#define REG_RIRBSIZE 0x5E // 8: bits[1:0] size select

#define GCTL_CRST 0x01     // controller out of reset when set
#define CORBRP_RST 0x8000  // CORBRP reset bit
#define CORBCTL_RUN 0x02   // CORB DMA run
#define RIRBWP_RST 0x8000  // RIRBWP reset bit
#define RIRBCTL_DMAEN 0x02 // RIRB DMA run
#define INTCTL_GIE 0x80000000u
#define INTCTL_CIE 0x40000000u
#define INTSTS_GIS 0x80000000u

// --- Stream descriptor registers (relative to the stream's 0x20-byte block) --
#define SD_CTL 0x00  // 24-bit control; byte0 has SRST/RUN/IOCE, byte2 has STRM
#define SD_STS 0x03  // 8: bit2 BCIS (W1C), bit4 FIFORDY
#define SD_LPIB 0x04 // 32: link position in buffer (play cursor, bytes)
#define SD_CBL 0x08  // 32: cyclic buffer length (total bytes)
#define SD_LVI 0x0C  // 16: last valid BDL index
#define SD_FMT 0x12  // 16: stream format
#define SD_BDPL 0x18 // 32: BDL base low
#define SD_BDPU 0x1C // 32: BDL base high
#define SDCTL_SRST 0x01 // stream reset
#define SDCTL_RUN 0x02  // stream DMA run
#define SDCTL_IOCE 0x04 // interrupt-on-completion enable
#define SDSTS_BCIS 0x04 // buffer-completion interrupt status (W1C)

// Stream format for 48 kHz / 16-bit / stereo: base48 | 16bit(0b001<<4) | 2ch.
#define SDFMT_48K_S16_STEREO 0x0011

// --- Codec verbs -------------------------------------------------------------
// A verb: [31:28] codec addr | [27:20] node id | [19:0] command+payload.
#define VERB(cad, nid, cmd)                                                    \
    (((uint32_t)(cad) << 28) | ((uint32_t)(nid) << 20) | ((cmd) & 0xFFFFF))
#define V_GET_PARAM 0xF0000  // 12-bit verb, payload = parameter id
#define V_SET_FORMAT 0x20000 // 4-bit verb 0x2, payload = format
#define V_SET_STREAM 0x70600 // set converter stream/channel
#define V_SET_AMP 0x30000    // 4-bit verb 0x3, 16-bit payload
#define V_SET_PINCTL 0x70700 // set pin widget control
#define V_SET_POWER 0x70500  // set power state
#define V_SET_EAPD 0x70C00   // set EAPD/BTL enable
// Parameters (payload of V_GET_PARAM).
#define P_SUBNODE_COUNT 0x04 // -> [start<<16 | count]
#define P_FN_GROUP_TYPE 0x05 // -> low byte: 0x01 = audio function group
#define P_WIDGET_CAP 0x09    // -> bits[23:20] widget type
#define P_PIN_CAP 0x0C       // -> bit4 output-capable
#define WIDGET_TYPE(cap) (((cap) >> 20) & 0xF)
#define WIDGET_DAC 0x0  // audio output converter
#define WIDGET_PIN 0x4  // pin complex
#define PINCAP_OUT 0x10 // pin is output-capable

// Amp payload: output amp | left | right | unmute | gain in bits[6:0].
#define AMP_OUT_UNMUTE(gain) (0x8000 | 0x2000 | 0x1000 | ((gain) & 0x7F))
// Pin control: bit6 output-enable, bit7 headphone-drive-enable.
#define PINCTL_OUT 0x40
#define PINCTL_HP 0x80

// Ring geometry. CORB/RIRB use 256 entries (the max) — one page each fits.
#define CORB_ENTRIES 256
#define RIRB_ENTRIES 256

// Playback ring: fixed period buffers played cyclically (like the AC'97 BDL).
#define BDL_ENTRIES 32
#define HDA_PERIOD 512 // stereo frames per period (~10.7 ms at 48 kHz)
#define HDA_QUEUE 6    // periods kept filled ahead of the play cursor (~64 ms)
#define PERIOD_SAMPLES (HDA_PERIOD * AUDIO_CH) // s16 samples per period
#define PERIOD_BYTES (PERIOD_SAMPLES * (int)sizeof(int16_t))

// A BDL entry: 64-bit buffer address, byte length, flags (bit0 = IOC).
struct bdl_entry {
    uint64_t addr;
    uint32_t len;
    uint32_t flags;
} __attribute__((packed));
#define BDL_IOC 0x01

static struct {
    bool present;
    volatile uint8_t* mmio; // BAR0 register window
    uint32_t sd;            // our output stream descriptor's byte offset
    uint8_t stream_tag;     // stream number programmed into SDCTL + the codec
    uint32_t sd_int_bit;    // INTCTL/INTSTS bit for this stream

    struct dma_buf corb, rirb;       // command / response rings
    uint16_t rirb_rp;                // our RIRB read pointer (entries consumed)
    struct dma_buf bdl;              // buffer descriptor list
    struct dma_buf buf[BDL_ENTRIES]; // period buffers
    uint32_t wr; // next period to hand the mixer (producer cursor)

    uint8_t codec; // codec address on the link
    uint16_t dac;  // audio-output converter node
    uint16_t pin;  // output pin-complex node

    bool running;
    const char* fail;

    // Completion-interrupt (legacy PCI INTx) state.
    uint8_t pci_dev;
    uint8_t irq_line, irq_pin;
    bool irq_on, irq_via_prt;
    volatile uint64_t irqs;
    void (*refill)(void);
} hda;

static uint32_t r32(uint32_t off)
{
    return mmio_r32(hda.mmio, off);
}
static void w32(uint32_t off, uint32_t v)
{
    mmio_w32(hda.mmio, off, v);
}
static uint16_t r16(uint32_t off)
{
    return mmio_r16(hda.mmio, off);
}
static void w16(uint32_t off, uint16_t v)
{
    mmio_w16(hda.mmio, off, v);
}
static uint8_t r8(uint32_t off)
{
    return mmio_r8(hda.mmio, off);
}
static void w8(uint32_t off, uint8_t v)
{
    mmio_w8(hda.mmio, off, v);
}

// Spin on a register bit reaching `want` (masked), up to `ms` milliseconds.
static bool wait_bits(uint32_t off, uint32_t mask, uint32_t want, uint32_t ms)
{
    uint64_t deadline = ktime_ns() + (uint64_t)ms * 1000000ull;
    while (ktime_ns() < deadline) {
        if ((r32(off) & mask) == want) {
            return true;
        }
        cpu_relax();
    }
    return (r32(off) & mask) == want;
}

// --- CORB/RIRB verb transport -----------------------------------------------
// Push one verb onto the CORB, ring the write pointer, and wait for its
// response to appear in the RIRB. Single outstanding command at a time — more
// than enough for the one-shot enumeration/config we do. Returns the 32-bit
// codec response (0 on timeout).
static uint32_t codec_cmd(uint8_t cad, uint16_t nid, uint32_t verb)
{
    uint32_t* corb = hda.corb.va;
    uint16_t wp = (uint16_t)((r16(REG_CORBWP) + 1) % CORB_ENTRIES);
    corb[wp] = VERB(cad, nid, verb);
    dma_wmb();
    w16(REG_CORBWP, wp);

    // The response lands at RIRB[rp] as a pair {response, resp_extended}.
    uint64_t deadline = ktime_ns() + 20000000ull; // 20 ms
    while (ktime_ns() < deadline) {
        if (((r16(REG_RIRBWP) & 0xFF)) != (hda.rirb_rp & 0xFF)) {
            hda.rirb_rp = (uint16_t)((hda.rirb_rp + 1) % RIRB_ENTRIES);
            const volatile uint32_t* rirb = hda.rirb.va;
            uint32_t resp = rirb[hda.rirb_rp * 2];
            // Acknowledge the response: clearing RINTFL resets the controller's
            // response counter, which otherwise stalls CORB processing once it
            // reaches RINTCNT (we drive the rings by polling, not interrupts).
            w8(REG_RIRBSTS, 0x05); // RINTFL (bit0) + overrun (bit2), W1C
            return resp;
        }
        cpu_relax();
    }
    return 0;
}

static uint32_t get_param(uint16_t nid, uint32_t param)
{
    return codec_cmd(hda.codec, nid, V_GET_PARAM | param);
}

// --- Codec enumeration -------------------------------------------------------
// Walk the codec's node tree to find an audio-output converter (DAC) and an
// output-capable pin complex. Simple topology only (the first of each), which
// covers QEMU's codec and the common laptop line-out/headphone path; complex
// codecs with routing selectors would need connection-list following.
static bool enumerate_codec(void)
{
    uint32_t sub = get_param(0, P_SUBNODE_COUNT); // root -> function groups
    uint16_t fg_start = (uint16_t)(sub >> 16);
    uint16_t fg_count = (uint16_t)(sub & 0xFF);

    for (uint16_t fg = fg_start; fg < fg_start + fg_count; fg++) {
        if ((get_param(fg, P_FN_GROUP_TYPE) & 0xFF) != 0x01) {
            continue; // not an audio function group
        }
        uint32_t w = get_param(fg, P_SUBNODE_COUNT);
        uint16_t w_start = (uint16_t)(w >> 16);
        uint16_t w_count = (uint16_t)(w & 0xFF);
        for (uint16_t nid = w_start; nid < w_start + w_count; nid++) {
            uint32_t cap = get_param(nid, P_WIDGET_CAP);
            uint32_t type = WIDGET_TYPE(cap);
            if (type == WIDGET_DAC && hda.dac == 0) {
                hda.dac = nid;
            } else if (type == WIDGET_PIN && hda.pin == 0 &&
                       (get_param(nid, P_PIN_CAP) & PINCAP_OUT)) {
                hda.pin = nid;
            }
        }
        if (hda.dac != 0 && hda.pin != 0) {
            return true;
        }
    }
    return false;
}

// Configure the found DAC + pin for playback: power both up, route the stream
// to the DAC, unmute both amps, and enable the pin's output driver.
static void configure_path(void)
{
    codec_cmd(hda.codec, hda.dac, V_SET_POWER | 0x00); // D0
    codec_cmd(hda.codec, hda.pin, V_SET_POWER | 0x00);
    codec_cmd(hda.codec, hda.dac, V_SET_FORMAT | SDFMT_48K_S16_STEREO);
    // Stream tag in bits[7:4], channel 0 in bits[3:0].
    codec_cmd(hda.codec, hda.dac, V_SET_STREAM | (hda.stream_tag << 4));
    codec_cmd(hda.codec, hda.dac, V_SET_AMP | AMP_OUT_UNMUTE(0x7F));
    codec_cmd(hda.codec, hda.pin, V_SET_AMP | AMP_OUT_UNMUTE(0x7F));
    codec_cmd(hda.codec, hda.pin, V_SET_PINCTL | PINCTL_OUT | PINCTL_HP);
    codec_cmd(hda.codec, hda.pin, V_SET_EAPD | 0x02); // EAPD on (amp enable)
}

// --- Playback ring (the audio_output vtable) --------------------------------
static uint32_t hda_period_frames(void)
{
    return HDA_PERIOD;
}

// The period the DMA engine is currently playing, from the link position.
static uint32_t play_index(void)
{
    uint32_t pos = r32(hda.sd + SD_LPIB);
    return (pos / (uint32_t)PERIOD_BYTES) % BDL_ENTRIES;
}

// Periods filled ahead of the play cursor.
static int queued(void)
{
    return (int)((hda.wr - play_index() + BDL_ENTRIES) % BDL_ENTRIES);
}

static int16_t* hda_next_period(void)
{
    if (!hda.present || queued() >= HDA_QUEUE) {
        return NULL;
    }
    if (hda.wr == play_index()) {
        return NULL; // never overwrite the buffer being played
    }
    return (int16_t*)hda.buf[hda.wr].va;
}

static void hda_commit_period(void)
{
    // The BDL is free-running and fixed, so there is no producer doorbell — the
    // mixer's writes just need to be visible before the DMA cursor reaches this
    // period. Advance our producer index.
    dma_wmb();
    hda.wr = (hda.wr + 1) % BDL_ENTRIES;
}

static void hda_start(void)
{
    if (!hda.present || hda.running) {
        return;
    }
    uint8_t ctl = (uint8_t)(SDCTL_RUN | (hda.irq_on ? SDCTL_IOCE : 0));
    w8(hda.sd + SD_CTL, (uint8_t)(r8(hda.sd + SD_CTL) | ctl));
    hda.running = true;
}

static void hda_stop(void)
{
    if (!hda.present) {
        return;
    }
    w8(hda.sd + SD_CTL, (uint8_t)(r8(hda.sd + SD_CTL) & ~(SDCTL_RUN)));
    hda.running = false;
}

// Completion ISR: acknowledge the stream's buffer-completion status (level-
// triggered, so it stays asserted until cleared) and refill the ring.
static void hda_irq(struct interrupt_frame* f)
{
    (void)f;
    uint32_t is = r32(REG_INTSTS);
    if (!(is & hda.sd_int_bit)) {
        return;
    }
    uint8_t sts = r8(hda.sd + SD_STS);
    if (sts & SDSTS_BCIS) {
        w8(hda.sd + SD_STS, SDSTS_BCIS); // W1C
        hda.irqs++;
        if (hda.refill != NULL) {
            hda.refill();
        }
    }
}

static void hda_enable_irq(void (*refill)(void))
{
    if (!hda.present) {
        return;
    }
    hda.refill = refill;

    // Prefer the ACPI _PRT (correct GSI + level/active-low polarity); fall back
    // to the firmware-programmed PCI Interrupt Line register.
    uint32_t gsi;
    uint16_t flags;
    if (hda.irq_pin >= 1 && hda.irq_pin <= 4 &&
        acpi_pci_route(hda.pci_dev, (uint8_t)(hda.irq_pin - 1), &gsi, &flags)) {
        hda.irq_line = (uint8_t)gsi;
        hda.irq_via_prt = true;
        register_interrupt_handler(32 + gsi, hda_irq);
        irq_route_gsi(gsi, flags);
        hda.irq_on = true;
    } else if (hda.irq_line != 0 && hda.irq_line != 0xFF) {
        register_interrupt_handler((uint32_t)(32 + hda.irq_line), hda_irq);
        irq_unmask(hda.irq_line);
        hda.irq_on = true;
    }
    if (!hda.irq_on) {
        return;
    }

    // Enable per-buffer IOC in the BDL, the stream's interrupt-on-completion,
    // and the controller's global + per-stream interrupt delivery.
    struct bdl_entry* bd = hda.bdl.va;
    for (int i = 0; i < BDL_ENTRIES; i++) {
        bd[i].flags |= BDL_IOC;
    }
    w8(hda.sd + SD_CTL, (uint8_t)(r8(hda.sd + SD_CTL) | SDCTL_IOCE));
    w32(REG_INTCTL, INTCTL_GIE | INTCTL_CIE | hda.sd_int_bit);
    console_printf("juampiOS: hda intx gsi=%u (%s)\n", (unsigned)hda.irq_line,
                   hda.irq_via_prt ? "_PRT" : "Interrupt Line");
}

static bool hda_irq_driven(void)
{
    return hda.irq_on;
}
static uint64_t hda_irq_count(void)
{
    return hda.irqs;
}
static const char* hda_fail_reason(void)
{
    return hda.fail;
}

// --- Bring-up ----------------------------------------------------------------
static void ring_setup(void)
{
    // CORB: stop DMA, point at our page, reset the read pointer, 256 entries.
    w8(REG_CORBCTL, 0);
    hda.corb = dma_page_alloc();
    w32(REG_CORBLBASE, (uint32_t)hda.corb.pa);
    w32(REG_CORBUBASE, (uint32_t)(hda.corb.pa >> 32));
    w8(REG_CORBSIZE, 0x02); // 256 entries
    w16(REG_CORBWP, 0);
    w16(REG_CORBRP, CORBRP_RST); // assert reset
    wait_bits(REG_CORBRP, CORBRP_RST, CORBRP_RST, 10);
    w16(REG_CORBRP, 0); // deassert
    wait_bits(REG_CORBRP, CORBRP_RST, 0, 10);
    w8(REG_CORBCTL, CORBCTL_RUN);

    // RIRB: point at our page, reset the write pointer, 256 entries, DMA on.
    w8(REG_RIRBCTL, 0);
    hda.rirb = dma_page_alloc();
    w32(REG_RIRBLBASE, (uint32_t)hda.rirb.pa);
    w32(REG_RIRBUBASE, (uint32_t)(hda.rirb.pa >> 32));
    w8(REG_RIRBSIZE, 0x02); // 256 entries
    w16(REG_RIRBWP, RIRBWP_RST);
    // Large response-interrupt count: we poll the RIRB, so we don't want the
    // controller to stall CORB processing on a low threshold (it halts once the
    // unacknowledged response count reaches RINTCNT). We ack in codec_cmd too.
    w16(REG_RINTCNT, 0xFF);
    hda.rirb_rp = 0;
    w8(REG_RIRBCTL, RIRBCTL_DMAEN);
}

static void stream_setup(void)
{
    // Allocate the BDL + fixed period buffers (zeroed = silence), like AC'97.
    hda.bdl = dma_page_alloc();
    struct bdl_entry* bd = hda.bdl.va;
    for (int i = 0; i < BDL_ENTRIES; i++) {
        hda.buf[i] = dma_page_alloc();
        bd[i].addr = hda.buf[i].pa;
        bd[i].len = (uint32_t)PERIOD_BYTES;
        bd[i].flags = 0; // IOC set later, only if interrupts come up
    }

    // Reset the stream and wait for the reset to take and release.
    w8(hda.sd + SD_CTL, SDCTL_SRST);
    wait_bits(hda.sd + SD_CTL, SDCTL_SRST, SDCTL_SRST, 10);
    w8(hda.sd + SD_CTL, 0);
    wait_bits(hda.sd + SD_CTL, SDCTL_SRST, 0, 10);

    // Program the stream tag (bits[23:20] of SDCTL, i.e. byte 2 high nibble),
    // format, cyclic buffer length, last valid index and the BDL pointer.
    w8(hda.sd + SD_CTL + 2, (uint8_t)(hda.stream_tag << 4));
    w16(hda.sd + SD_FMT, SDFMT_48K_S16_STEREO);
    w32(hda.sd + SD_CBL, (uint32_t)(BDL_ENTRIES * PERIOD_BYTES));
    w16(hda.sd + SD_LVI, BDL_ENTRIES - 1);
    w32(hda.sd + SD_BDPL, (uint32_t)hda.bdl.pa);
    w32(hda.sd + SD_BDPU, (uint32_t)(hda.bdl.pa >> 32));
}

static bool hda_init(void)
{
    struct pci_addr a =
            pci_find_class(PCI_CLASS_MULTIMEDIA, PCI_SUBCLASS_HDA, 0x00);
    if (!a.found) {
        return false; // no HDA controller — silent (audio is optional)
    }
    pci_enable_bus_master(a);
    hda.pci_dev = a.dev;
    uint32_t intr = pci_read32(a.bus, a.dev, a.func, 0x3C);
    hda.irq_line = (uint8_t)(intr & 0xFF);
    hda.irq_pin = (uint8_t)((intr >> 8) & 0xFF);

    hda.mmio = iomap(pci_bar64(a, 0), 0x2000, PAGEF_P | PAGEF_RW | PAGEF_UC);

    // Bring the controller out of reset (CRST 0 -> 1) and let it settle.
    w32(REG_GCTL, 0);
    wait_bits(REG_GCTL, GCTL_CRST, 0, 10);
    w32(REG_GCTL, GCTL_CRST);
    if (!wait_bits(REG_GCTL, GCTL_CRST, GCTL_CRST, 100)) {
        hda.fail = "controller reset timeout";
        return false;
    }
    // Codecs need ~521 us after reset to report presence in STATESTS.
    uint64_t deadline = ktime_ns() + 2000000ull;
    while (ktime_ns() < deadline && r16(REG_STATESTS) == 0) {
        cpu_relax();
    }
    uint16_t sts = r16(REG_STATESTS);
    if (sts == 0) {
        hda.fail = "no codec detected";
        return false;
    }
    for (int i = 0; i < 15; i++) {
        if (sts & (1u << i)) {
            hda.codec = (uint8_t)i;
            break;
        }
    }

    // Our output stream: descriptors are [inputs][outputs][bidir]; the first
    // output starts after the input-stream descriptors (ISS of them).
    uint16_t gcap = r16(REG_GCAP);
    uint32_t iss = (gcap >> 8) & 0x0F;
    uint32_t out0 = iss; // global stream index of the first output stream
    hda.sd = 0x80 + out0 * 0x20;
    hda.stream_tag = 1;
    hda.sd_int_bit = 1u << out0;

    ring_setup();
    if (!enumerate_codec()) {
        hda.fail = "no output DAC/pin on codec";
        return false;
    }
    stream_setup();
    configure_path();

    hda.wr = HDA_QUEUE; // initial silent cushion, like AC'97
    hda.present = true;
    console_printf("juampiOS: hda codec %u dac 0x%x pin 0x%x stream %u\n",
                   (unsigned)hda.codec, hda.dac, hda.pin,
                   (unsigned)hda.stream_tag);
    return true;
}

const struct audio_output hda_backend = {
        .name = "hda",
        .init = hda_init,
        .period_frames = hda_period_frames,
        .next_period = hda_next_period,
        .commit_period = hda_commit_period,
        .start = hda_start,
        .stop = hda_stop,
        .fail_reason = hda_fail_reason,
        .enable_irq = hda_enable_irq,
        .irq_driven = hda_irq_driven,
        .irq_count = hda_irq_count,
};
