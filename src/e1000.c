// Intel 82540EM ("e1000") NIC driver — poll-driven, no interrupts. Finds the
// card on PCI, maps its register BAR uncached, sets up legacy RX/TX descriptor
// rings out of physical frames (reached through the HHDM), and moves raw
// Ethernet frames. Everything above Ethernet lives in net.c. See
// docs/networking.md for the register-level design.

#include <e1000.h>
#include <frames.h>
#include <paging.h>
#include <pci.h>
#include <mmio.h> // register access + dma_wmb before the TX doorbell
#include <idt.h> // register_interrupt_handler, irq_unmask/route, interrupt_frame
#include <acpi.h> // acpi_pci_route (_PRT)
#include <console.h>
#include <utils.h>

// PCI identity of the card this driver binds to.
#define E1000_VENDOR_INTEL 0x8086
#define E1000_DEVICE_82540EM 0x100E

// --- Register offsets (bytes into BAR0 MMIO) --------------------------------
#define REG_CTRL 0x0000
#define REG_STATUS 0x0008
#define REG_EERD 0x0014
#define REG_ICR 0x00C0
#define REG_IMS 0x00D0
#define REG_IMC 0x00D8
#define REG_RDTR 0x2820
#define REG_RCTL 0x0100
#define REG_TCTL 0x0400
#define REG_TIPG 0x0410
#define REG_RDBAL 0x2800
#define REG_RDBAH 0x2804
#define REG_RDLEN 0x2808
#define REG_RDH 0x2810
#define REG_RDT 0x2818
#define REG_TDBAL 0x3800
#define REG_TDBAH 0x3804
#define REG_TDLEN 0x3808
#define REG_TDH 0x3810
#define REG_TDT 0x3818
#define REG_RAL0 0x5400
#define REG_RAH0 0x5404
#define REG_MTA 0x5200

// CTRL bits
#define CTRL_SLU (1u << 6)  // set link up
#define CTRL_ASDE (1u << 5) // auto-speed detect enable
#define CTRL_RST (1u << 26) // device reset
// RCTL bits
#define RCTL_EN (1u << 1)     // receiver enable
#define RCTL_BAM (1u << 15)   // accept broadcast
#define RCTL_SECRC (1u << 26) // strip Ethernet CRC
#define RCTL_BSIZE_2048 0u    // (BSEX=0, SIZE=00) -> 2048-byte buffers
// TCTL bits
#define TCTL_EN (1u << 1)      // transmitter enable
#define TCTL_PSP (1u << 3)     // pad short packets
#define TCTL_CT (0x0F << 4)    // collision threshold = 15
#define TCTL_COLD (0x40 << 12) // collision distance = 64 (half-duplex)
// IPG timing packed as IPGT=10, IPGR1=8, IPGR2=6 (IEEE 802.3 recommended).
#define TIPG_IEEE8023 0x0060200A
// TX descriptor cmd bits
#define TXD_EOP (1u << 0)  // end of packet
#define TXD_IFCS (1u << 1) // insert FCS/CRC
#define TXD_RS (1u << 3)   // report status (sets DD when done)
#define TXD_STAT_DD (1u << 0)
// RX descriptor status bits
#define RXD_STAT_DD (1u << 0)  // descriptor done
#define RXD_STAT_EOP (1u << 1) // end of packet
// Interrupt cause / mask bits (ICR/IMS) — the receive-side causes we enable.
#define ICR_RXDMT0 (1u << 4) // RX descriptor ring below min threshold
#define ICR_RXO (1u << 6)    // receiver overrun
#define ICR_RXT0 (1u << 7)   // receiver timer (a packet was received)

#define N_RX 32
#define N_TX 8
#define RX_BUFSZ 2048

// Bytes of BAR0 register space to map (the e1000 register file fits well under
// this); the VA window is bump-allocated by iomap().
#define NICWIN_SZ 0x20000ull

// Legacy receive descriptor. The card owns it until it sets status.DD, then the
// driver reads the frame and hands the descriptor back (see e1000_rx_poll).
struct rx_desc {
    uint64_t addr;     // physical address of the buffer the card DMAs into
    uint16_t length;   // number of bytes the card wrote into that buffer
    uint16_t checksum; // hardware packet checksum (unused here)
    uint8_t status;    // status bits: RXD_STAT_DD (done), RXD_STAT_EOP
    uint8_t errors;    // receive error flags (unused here)
    uint16_t special;  // VLAN tag / special field (unused here)
} __attribute__((packed));

// Legacy transmit descriptor. The driver fills it and bumps TDT; the card
// transmits and, because cmd.RS is set, writes back status.DD when done.
struct tx_desc {
    uint64_t addr;    // physical address of the frame to transmit
    uint16_t length;  // frame length in bytes
    uint8_t cso;      // checksum offset (unused — no TX checksum offload)
    uint8_t cmd;      // command bits: TXD_EOP | TXD_IFCS | TXD_RS
    uint8_t status;   // written back by the card: TXD_STAT_DD when sent
    uint8_t css;      // checksum start (unused)
    uint16_t special; // VLAN tag / special field (unused)
} __attribute__((packed));

static volatile uint8_t* mmio; // mapped BAR0
static bool present;           // card found + configured
static uint8_t mac[6];

// PCI location + INTx receive-interrupt state.
static uint8_t pci_dev, irq_pin, irq_line;
static bool irq_on, irq_via_prt;
static volatile uint64_t rx_ints; // receive interrupts taken (diagnostic)

static volatile struct rx_desc* rx_ring; // N_RX descriptors
static volatile struct tx_desc* tx_ring; // N_TX descriptors
static uint8_t* rx_buf[N_RX];            // HHDM VA of each RX buffer
static uint32_t rx_cur;                  // next descriptor we expect
static int32_t rx_handed = -1; // slot handed out, recycled on the next poll
static uint32_t tx_cur;        // next descriptor to fill

static inline uint32_t reg_read(uint32_t off)
{
    return mmio_r32(mmio, off);
}

static inline void reg_write(uint32_t off, uint32_t val)
{
    mmio_w32(mmio, off, val);
}

// Read the MAC. On both QEMU and real hardware the card auto-loads receive
// address 0 (RAL0/RAH0) from the EEPROM at reset; RAH0.AV marks it valid. Fall
// back to reading EEPROM words 0..2 directly if it is not.
static void read_mac(void)
{
    uint32_t ral = reg_read(REG_RAL0);
    uint32_t rah = reg_read(REG_RAH0);
    if (rah & (1u << 31)) { // Address Valid
        mac[0] = (uint8_t)ral;
        mac[1] = (uint8_t)(ral >> 8);
        mac[2] = (uint8_t)(ral >> 16);
        mac[3] = (uint8_t)(ral >> 24);
        mac[4] = (uint8_t)rah;
        mac[5] = (uint8_t)(rah >> 8);
        return;
    }
    for (int i = 0; i < 3; i++) {
        reg_write(REG_EERD, ((uint32_t)i << 8) | 1u); // addr<<8 | START
        uint32_t v;
        while (!((v = reg_read(REG_EERD)) & (1u << 4))) { // wait DONE
        }
        uint16_t w = (uint16_t)(v >> 16);
        mac[2 * i] = (uint8_t)w;
        mac[2 * i + 1] = (uint8_t)(w >> 8);
    }
}

static void rings_init(void)
{
    // One frame each for the (small) descriptor rings.
    uintptr_t rx_ring_phys = frame_alloc();
    uintptr_t tx_ring_phys = frame_alloc();
    void* rx_va = phys_to_virt(rx_ring_phys);
    void* tx_va = phys_to_virt(tx_ring_phys);
    memset(rx_va, 0, PAGE_SZ); // zero before the card sees it (non-volatile)
    memset(tx_va, 0, PAGE_SZ);
    rx_ring = rx_va;
    tx_ring = tx_va;

    for (int i = 0; i < N_RX; i++) {
        uintptr_t p = frame_alloc();
        rx_buf[i] = phys_to_virt(p);
        rx_ring[i].addr = p;
        rx_ring[i].status = 0;
    }
    rx_cur = 0;
    tx_cur = 0;

    // Program the RX ring and enable the receiver.
    reg_write(REG_RDBAL, (uint32_t)rx_ring_phys);
    reg_write(REG_RDBAH, (uint32_t)(rx_ring_phys >> 32));
    reg_write(REG_RDLEN, N_RX * (uint32_t)sizeof(struct rx_desc));
    reg_write(REG_RDH, 0);
    reg_write(REG_RDT, N_RX - 1); // all descriptors owned by the card
    reg_write(REG_RCTL, RCTL_EN | RCTL_BAM | RCTL_SECRC | RCTL_BSIZE_2048);

    // Program the TX ring and enable the transmitter.
    reg_write(REG_TDBAL, (uint32_t)tx_ring_phys);
    reg_write(REG_TDBAH, (uint32_t)(tx_ring_phys >> 32));
    reg_write(REG_TDLEN, N_TX * (uint32_t)sizeof(struct tx_desc));
    reg_write(REG_TDH, 0);
    reg_write(REG_TDT, 0);
    reg_write(REG_TCTL, TCTL_EN | TCTL_PSP | TCTL_CT | TCTL_COLD);
    reg_write(REG_TIPG, TIPG_IEEE8023);
}

bool e1000_init(void)
{
    struct pci_addr a = pci_find(E1000_VENDOR_INTEL, E1000_DEVICE_82540EM);
    if (!a.found) {
        return false;
    }
    pci_enable_bus_master(a);
    pci_dev = a.dev;
    uint32_t intr = pci_read32(a.bus, a.dev, a.func, 0x3C);
    irq_line = (uint8_t)(intr & 0xFF);       // PCI Interrupt Line
    irq_pin = (uint8_t)((intr >> 8) & 0xFF); // PCI Interrupt Pin (1=INTA)

    mmio = iomap(pci_bar(a, 0), NICWIN_SZ, PAGEF_P | PAGEF_RW | PAGEF_UC);

    // Reset, then wait for it to self-clear.
    reg_write(REG_IMC, 0xFFFFFFFF); // mask all interrupts (we poll)
    reg_write(REG_CTRL, reg_read(REG_CTRL) | CTRL_RST);
    for (volatile int i = 0; i < 1000000 && (reg_read(REG_CTRL) & CTRL_RST);
         i++) {
    }
    reg_write(REG_IMC, 0xFFFFFFFF);
    (void)reg_read(REG_ICR); // clear pending causes

    read_mac();
    // Program receive address 0 so unicast frames to us are accepted.
    reg_write(REG_RAL0, (uint32_t)mac[0] | ((uint32_t)mac[1] << 8) |
                                ((uint32_t)mac[2] << 16) |
                                ((uint32_t)mac[3] << 24));
    reg_write(REG_RAH0,
              (uint32_t)mac[4] | ((uint32_t)mac[5] << 8) | (1u << 31));
    for (int i = 0; i < 128; i++) {
        reg_write(REG_MTA + i * 4, 0); // clear multicast filter
    }

    rings_init();

    // Bring the link up.
    reg_write(REG_CTRL, reg_read(REG_CTRL) | CTRL_SLU | CTRL_ASDE);

    present = true;
    return true;
}

void e1000_mac(uint8_t out[6])
{
    for (int i = 0; i < 6; i++) {
        out[i] = mac[i];
    }
}

bool e1000_tx(const void* frame, uint16_t len)
{
    if (!present) {
        return false;
    }
    // Copy into a fresh buffer owned for this descriptor slot.
    volatile struct tx_desc* d = &tx_ring[tx_cur];
    // Reuse a per-slot bounce buffer sized to a frame.
    static uint8_t* txbuf[N_TX];
    if (txbuf[tx_cur] == NULL) {
        txbuf[tx_cur] = phys_to_virt(frame_alloc());
    }
    if (len > RX_BUFSZ) {
        len = RX_BUFSZ;
    }
    memcpy(txbuf[tx_cur], frame, len);
    d->addr = (uintptr_t)txbuf[tx_cur] - hhdm_offset; // back to physical
    d->length = len;
    d->cso = 0;
    d->css = 0;
    d->special = 0;
    d->cmd = TXD_EOP | TXD_IFCS | TXD_RS;
    d->status = 0;

    tx_cur = (tx_cur + 1) % N_TX;
    // Make the descriptor writes above globally visible before we ring the
    // doorbell, so the card's DMA engine can never read a half-initialized
    // descriptor once it sees TDT advance (dma_wmb also covers a
    // write-combining ring mapping).
    dma_wmb();
    reg_write(REG_TDT,
              tx_cur); // doorbell: hand descriptor tx_cur-1 to the card

    // Wait for the card to write the descriptor back done (cmd.RS -> status.DD)
    // so the caller gets real confirmation the frame went out. TX is near-
    // instant here, so bound the spin and report a timeout as failure.
    for (int i = 0; i < 1000000; i++) {
        if (d->status & TXD_STAT_DD) {
            return true;
        }
    }
    return false;
}

// Return the descriptor from the previous call to the card. Deferring the
// recycle to the next poll keeps the frame we handed out valid (the card can't
// refill its buffer) until the caller asks for the next one.
static void rx_recycle_handed(void)
{
    if (rx_handed >= 0) {
        rx_ring[rx_handed].status = 0;
        reg_write(REG_RDT, (uint32_t)rx_handed);
        rx_handed = -1;
    }
}

bool e1000_rx_poll(struct e1000_frame* out)
{
    if (!present) {
        return false;
    }
    rx_recycle_handed();
    while (rx_ring[rx_cur].status & RXD_STAT_DD) {
        volatile struct rx_desc* d = &rx_ring[rx_cur];
        uint16_t len = d->length;
        bool eop = d->status & RXD_STAT_EOP;
        uint32_t idx = rx_cur;
        rx_cur = (rx_cur + 1) % N_RX;
        if (eop) {
            out->data = rx_buf[idx];
            out->len = len;
            rx_handed = (int32_t)idx; // recycled on the next call
            return true;
        }
        d->status = 0; // non-EOP fragment: drop and return it now
        reg_write(REG_RDT, idx);
    }
    return false;
}

// Receive ISR. Legacy INTx is level-triggered and shared, so reading ICR both
// tells us why we were called and de-asserts the line; a read of 0 means the
// interrupt was some other device on the shared GSI (a harmless no-op here). We
// deliberately do NOT run the protocol stack from interrupt context — that path
// also transmits (ARP/TCP acks) and touches BSP-only state that isn't reentrant
// against a foreground send. Instead we just count, and the frames are drained
// by net_poll() from the main thread (the interrupt wakes the idle `hlt`).
static void e1000_irq(struct interrupt_frame* f)
{
    (void)f;
    uint32_t icr = reg_read(REG_ICR); // read-to-clear
    if (icr & (ICR_RXT0 | ICR_RXO | ICR_RXDMT0)) {
        rx_ints++;
    }
}

void e1000_enable_irq(void)
{
    if (!present || irq_on) {
        return;
    }
    // Prefer the ACPI _PRT (correct GSI + level/active-low polarity for PCI
    // INTx); fall back to the firmware-programmed PCI Interrupt Line register.
    uint32_t gsi;
    uint16_t flags;
    if (irq_pin >= 1 && irq_pin <= 4 &&
        acpi_pci_route(pci_dev, (uint8_t)(irq_pin - 1), &gsi, &flags)) {
        irq_line = (uint8_t)gsi;
        irq_via_prt = true;
        register_interrupt_handler(32 + gsi, e1000_irq);
        irq_route_gsi(gsi, flags);
        irq_on = true;
    } else if (irq_line != 0 && irq_line != 0xFF) {
        register_interrupt_handler((uint32_t)(32 + irq_line), e1000_irq);
        irq_unmask(irq_line);
        irq_on = true;
    }
    if (!irq_on) {
        return;
    }
    reg_write(REG_RDTR, 0);  // no receive delay: interrupt per packet
    (void)reg_read(REG_ICR); // clear any stale causes
    reg_write(REG_IMS, ICR_RXT0 | ICR_RXO | ICR_RXDMT0); // unmask RX causes
    console_printf("juampiOS: e1000 rx irq gsi=%u (%s)\n", (unsigned)irq_line,
                   irq_via_prt ? "_PRT" : "Interrupt Line");
}

bool e1000_irq_driven(void)
{
    return irq_on;
}

uint64_t e1000_irq_count(void)
{
    return rx_ints;
}
