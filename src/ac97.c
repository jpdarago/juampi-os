// AC'97 audio output backend (see audio.h). AC'97 is an I/O-port-mapped PCI
// device: BAR0 is the mixer (NAM — volumes, reset), BAR1 is the bus master
// (NABM — the DMA engine). Playback is driven by a Buffer Descriptor List: a
// 32-entry ring of {buffer address, length} the controller plays in a loop,
// with CIV (current index) as the play cursor and LVI (last valid index) as the
// producer cursor. We keep a small number of periods queued ahead of CIV for
// low latency, refilled by the mixer via next_period()/commit_period().
//
// Interrupts (IOC per buffer) come later; for now the mixer refills from the
// idle loops (audio_pump), which is ample given the ~64 ms queued ahead.

#include <audio.h>
#include <pci.h>
#include <ports.h>
#include <dma.h>
#include <idt.h>     // register_interrupt_handler, irq_unmask, interrupt_frame
#include <acpi.h>    // acpi_pci_route (_PRT)
#include <barrier.h> // dma_wmb before the LVI doorbell; cpu_relax in spins
#include <console.h>
#include <utils.h>

// PCI: multimedia / audio device. QEMU's AC97 is 8086:2415.
#define PCI_CLASS_MULTIMEDIA 0x04
#define PCI_SUBCLASS_AUDIO 0x01

// NAM (mixer) register offsets, from BAR0, accessed 16-bit.
#define NAM_RESET 0x00
#define NAM_MASTER_VOL 0x02
#define NAM_PCM_VOL 0x18

// NABM (bus master) register offsets, from BAR1. The PCM-Out box is at 0x10.
#define NABM_PO_BDBAR 0x10 // 32-bit: BDL physical base
#define NABM_PO_CIV 0x14   // 8-bit RO: current index (play cursor)
#define NABM_PO_LVI 0x15   // 8-bit: last valid index (producer cursor)
#define NABM_PO_SR 0x16    // 16-bit: status (W1C)
#define NABM_PO_CR 0x1B    // 8-bit: control
#define NABM_GLOB_CNT 0x2C // 32-bit: global control
#define NABM_GLOB_STA 0x30 // 32-bit: global status

#define CR_RPBM 0x01       // run/pause bus master (1 = run)
#define CR_RR 0x02         // reset registers (self-clears)
#define CR_IOCE 0x10       // interrupt on completion enable
#define SR_LVBCI 0x04      // last-valid-buffer completion interrupt (W1C)
#define SR_BCIS 0x08       // buffer completion interrupt status (W1C)
#define GLOB_CNT_COLD 0x02 // AC'97 cold reset (deasserted = 1)
#define GLOB_STA_PCR 0x100 // primary codec ready

// BDL: 32 entries of {addr, ctl}; ctl = samples(15:0) | BUP(30) | IOC(31).
#define BDL_ENTRIES 32
#define BD_IOC (1u << 31) // interrupt when this buffer completes
#define BD_BUP (1u << 30) // on underrun, repeat the last sample (no noise)
#define AC97_PERIOD 512   // stereo frames per period (~10.7 ms at 48 kHz)
#define AC97_QUEUE 6 // periods kept queued ahead of the play cursor (~64 ms)
#define PERIOD_SAMPLES (AC97_PERIOD * AUDIO_CH) // 16-bit samples per period

struct bd {
    uint32_t addr;
    uint32_t ctl;
} __attribute__((packed));

static struct {
    bool present;
    uint16_t nam, nabm; // I/O bases
    dma_buf bdl;        // 32 buffer descriptors
    dma_buf buf[BDL_ENTRIES];
    uint8_t lvi; // mirror of PO_LVI (last committed period)
    bool running;
    const char* fail;
    // Completion-interrupt (legacy PCI INTx) state.
    uint8_t pci_bus, pci_dev, pci_func; // for _PRT routing
    uint8_t irq_line;                   // PCI Interrupt Line (config 0x3C)
    uint8_t irq_pin;  // PCI Interrupt Pin (config 0x3D, 1=INTA)
    bool irq_on;      // INTx routed + IOC enabled
    bool irq_via_prt; // routed through the ACPI _PRT (else Interrupt Line)
    volatile uint64_t irqs; // completion interrupts taken
    void (*refill)(void);   // mixer pump, called from the ISR
} ac;

static uint8_t r8(uint16_t off)
{
    return inb((uint16_t)(ac.nabm + off));
}
static void w8(uint16_t off, uint8_t v)
{
    outb((uint16_t)(ac.nabm + off), v);
}

// Periods currently owned by the hardware (from the play cursor to LVI).
static int queued(void)
{
    uint8_t civ = r8(NABM_PO_CIV);
    return (ac.lvi - civ + BDL_ENTRIES) % BDL_ENTRIES;
}

static uint32_t ac97_period_frames(void)
{
    return AC97_PERIOD;
}

static int16_t* ac97_next_period(void)
{
    if (!ac.present || queued() >= AC97_QUEUE) {
        return NULL;
    }
    uint8_t nxt = (uint8_t)((ac.lvi + 1) % BDL_ENTRIES);
    if (nxt == r8(NABM_PO_CIV)) {
        return NULL; // never overwrite the buffer being played
    }
    return (int16_t*)ac.buf[nxt].va;
}

static void ac97_commit_period(void)
{
    // The mixer's writes into this period buffer must be visible before we
    // advance LVI to hand it to the DMA engine.
    dma_wmb();
    ac.lvi = (uint8_t)((ac.lvi + 1) % BDL_ENTRIES);
    w8(NABM_PO_LVI, ac.lvi);
}

static void ac97_start(void)
{
    if (!ac.present || ac.running) {
        return;
    }
    w8(NABM_PO_CR, (uint8_t)(CR_RPBM | (ac.irq_on ? CR_IOCE : 0)));
    ac.running = true;
}

// Completion ISR: clear the status bits (INTx is level-triggered, so it stays
// asserted until we do), then refill the ring. lapic_eoi is done by
// interrupt_dispatch after this returns.
static void ac97_irq(interrupt_frame* f)
{
    (void)f;
    uint16_t sr = inw((uint16_t)(ac.nabm + NABM_PO_SR));
    if (sr & (SR_BCIS | SR_LVBCI)) {
        outw((uint16_t)(ac.nabm + NABM_PO_SR),
             (uint16_t)(sr & (SR_BCIS | SR_LVBCI))); // W1C
        ac.irqs++;
        if (ac.refill != NULL) {
            ac.refill();
        }
    }
}

// Route the AC'97 completion interrupt (legacy PCI INTx) to ac97_irq and enable
// interrupt-on-completion per buffer. The mixer's pump (`refill`) then runs
// from the ISR; the idle-loop pump stays as a backstop.
static void ac97_enable_irq(void (*refill)(void))
{
    if (!ac.present) {
        return;
    }
    ac.refill = refill;
    struct bd* bd = ac.bdl.va;
    for (int i = 0; i < BDL_ENTRIES; i++) {
        bd[i].ctl |= BD_IOC;
    }

    // Prefer the ACPI _PRT (correct GSI + level/active-low polarity for PCI
    // INTx); fall back to the firmware-programmed PCI Interrupt Line register.
    uint32_t gsi;
    uint16_t flags;
    if (ac.irq_pin >= 1 && ac.irq_pin <= 4 &&
        acpi_pci_route(ac.pci_dev, (uint8_t)(ac.irq_pin - 1), &gsi, &flags)) {
        ac.irq_line = (uint8_t)gsi;
        ac.irq_via_prt = true;
        register_interrupt_handler(32 + gsi, ac97_irq);
        irq_route_gsi(gsi, flags);
        ac.irq_on = true;
    } else if (ac.irq_line != 0 && ac.irq_line != 0xFF) {
        register_interrupt_handler((uint32_t)(32 + ac.irq_line), ac97_irq);
        irq_unmask(ac.irq_line);
        ac.irq_on = true;
    }
    if (ac.irq_on) {
        console_printf("juampiOS: ac97 intx gsi=%u (%s)\n",
                       (unsigned)ac.irq_line,
                       ac.irq_via_prt ? "_PRT" : "Interrupt Line");
    }
}

static bool ac97_irq_driven(void)
{
    return ac.irq_on;
}
static uint64_t ac97_irq_count(void)
{
    return ac.irqs;
}

static void ac97_stop(void)
{
    if (!ac.present) {
        return;
    }
    w8(NABM_PO_CR, 0);
    ac.running = false;
}

static const char* ac97_fail_reason(void)
{
    return ac.fail;
}

static bool ac97_init(void)
{
    pci_addr a = pci_find_class(PCI_CLASS_MULTIMEDIA, PCI_SUBCLASS_AUDIO, 0x00);
    if (!a.found) {
        a = pci_find(0x8086, 0x2415); // QEMU AC97
    }
    if (!a.found) {
        return false; // no device — silent (audio is optional)
    }
    pci_enable_bus_master(a);
    ac.nam = (uint16_t)pci_bar(a, 0);
    ac.nabm = (uint16_t)pci_bar(a, 1);
    ac.pci_bus = a.bus;
    ac.pci_dev = a.dev;
    ac.pci_func = a.func;
    uint32_t intr = pci_read32(a.bus, a.dev, a.func, 0x3C);
    ac.irq_line = (uint8_t)(intr & 0xFF);       // Interrupt Line
    ac.irq_pin = (uint8_t)((intr >> 8) & 0xFF); // Interrupt Pin (1=INTA)

    // Bring the codec out of cold reset and wait for it to report ready.
    outl((uint16_t)(ac.nabm + NABM_GLOB_CNT), GLOB_CNT_COLD);
    bool ready = false;
    for (int i = 0; i < 1000; i++) {
        if (inl((uint16_t)(ac.nabm + NABM_GLOB_STA)) & GLOB_STA_PCR) {
            ready = true;
            break;
        }
        cpu_relax();
    }
    if (!ready) {
        ac.fail = "codec not ready";
        return false;
    }

    // Reset the mixer, then unmute + set master and PCM to full (0 dB =
    // 0x0000).
    outw((uint16_t)(ac.nam + NAM_RESET), 1);
    outw((uint16_t)(ac.nam + NAM_MASTER_VOL), 0x0000);
    outw((uint16_t)(ac.nam + NAM_PCM_VOL), 0x0000);

    // Reset the PCM-Out DMA engine and wait for the reset bit to self-clear.
    w8(NABM_PO_CR, CR_RR);
    for (int i = 0; i < 1000 && (r8(NABM_PO_CR) & CR_RR); i++) {
        cpu_relax();
    }

    // Allocate the BDL + all 32 period buffers (zeroed = silence). Every BDL
    // entry is valid and fixed; playback just moves LVI. Buffers are refilled
    // in place, so the descriptors never change after setup.
    ac.bdl = dma_page_alloc();
    struct bd* bd = ac.bdl.va;
    for (int i = 0; i < BDL_ENTRIES; i++) {
        ac.buf[i] = dma_page_alloc();
        bd[i].addr = (uint32_t)ac.buf[i].pa;
        bd[i].ctl = PERIOD_SAMPLES | BD_BUP;
    }
    outl((uint16_t)(ac.nabm + NABM_PO_BDBAR), (uint32_t)ac.bdl.pa);

    // Queue an initial cushion of (silent) periods so DMA has somewhere to go
    // the moment it starts; the mixer keeps refilling ahead of the play cursor.
    ac.lvi = AC97_QUEUE;
    w8(NABM_PO_LVI, ac.lvi);

    ac.present = true;
    return true;
}

const audio_output ac97_backend = {
        .name = "ac97",
        .init = ac97_init,
        .period_frames = ac97_period_frames,
        .next_period = ac97_next_period,
        .commit_period = ac97_commit_period,
        .start = ac97_start,
        .stop = ac97_stop,
        .fail_reason = ac97_fail_reason,
        .enable_irq = ac97_enable_irq,
        .irq_driven = ac97_irq_driven,
        .irq_count = ac97_irq_count,
};
