// Intel 82540EM ("e1000") NIC driver — poll-driven, no interrupts. Finds the
// card on PCI, maps its register BAR uncached, sets up legacy RX/TX descriptor
// rings out of physical frames (reached through the HHDM), and moves raw
// Ethernet frames. Everything above Ethernet lives in net.c. See
// docs/networking.md for the register-level design.

#include <e1000.h>
#include <frames.h>
#include <paging.h>
#include <pci.h>
#include <utils.h>

// --- Register offsets (bytes into BAR0 MMIO) --------------------------------
#define REG_CTRL 0x0000
#define REG_STATUS 0x0008
#define REG_EERD 0x0014
#define REG_ICR 0x00C0
#define REG_IMC 0x00D8
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
#define TCTL_EN (1u << 1)  // transmitter enable
#define TCTL_PSP (1u << 3) // pad short packets
// TX descriptor cmd bits
#define TXD_EOP (1u << 0)  // end of packet
#define TXD_IFCS (1u << 1) // insert FCS/CRC
#define TXD_RS (1u << 3)   // report status (sets DD when done)
#define TXD_STAT_DD (1u << 0)
// RX descriptor status bits
#define RXD_STAT_DD (1u << 0)  // descriptor done
#define RXD_STAT_EOP (1u << 1) // end of packet

#define N_RX 32
#define N_TX 8
#define RX_BUFSZ 2048

// Register window: a private higher-half VA clear of the framebuffer window
// (gfx.c's FBWIN at ...0000000, 16 MiB). 128 KiB covers the e1000 BAR0.
#define NICWIN_VA 0xffffe00002000000ull
#define NICWIN_SZ 0x20000ull

// Legacy receive descriptor. The card owns it until it sets status.DD, then the
// driver reads the frame and hands the descriptor back (see e1000_rx_poll).
typedef struct __attribute__((packed)) {
    uint64_t addr;     // physical address of the buffer the card DMAs into
    uint16_t length;   // number of bytes the card wrote into that buffer
    uint16_t checksum; // hardware packet checksum (unused here)
    uint8_t status;    // status bits: RXD_STAT_DD (done), RXD_STAT_EOP
    uint8_t errors;    // receive error flags (unused here)
    uint16_t special;  // VLAN tag / special field (unused here)
} rx_desc;

// Legacy transmit descriptor. The driver fills it and bumps TDT; the card
// transmits and, because cmd.RS is set, writes back status.DD when done.
typedef struct __attribute__((packed)) {
    uint64_t addr;    // physical address of the frame to transmit
    uint16_t length;  // frame length in bytes
    uint8_t cso;      // checksum offset (unused — no TX checksum offload)
    uint8_t cmd;      // command bits: TXD_EOP | TXD_IFCS | TXD_RS
    uint8_t status;   // written back by the card: TXD_STAT_DD when sent
    uint8_t css;      // checksum start (unused)
    uint16_t special; // VLAN tag / special field (unused)
} tx_desc;

static volatile uint8_t* mmio; // mapped BAR0
static bool present;           // card found + configured
static uint8_t mac[6];

static volatile rx_desc* rx_ring; // N_RX descriptors
static volatile tx_desc* tx_ring; // N_TX descriptors
static uint8_t* rx_buf[N_RX];     // HHDM VA of each RX buffer
static uint32_t rx_cur;           // next descriptor we expect
static int32_t rx_handed = -1;    // slot handed out, recycled on the next poll
static uint32_t tx_cur;           // next descriptor to fill

static inline uint32_t reg_read(uint32_t off)
{
    return *(volatile uint32_t*)(mmio + off);
}

static inline void reg_write(uint32_t off, uint32_t val)
{
    *(volatile uint32_t*)(mmio + off) = val;
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
    reg_write(REG_RDLEN, N_RX * (uint32_t)sizeof(rx_desc));
    reg_write(REG_RDH, 0);
    reg_write(REG_RDT, N_RX - 1); // all descriptors owned by the card
    reg_write(REG_RCTL, RCTL_EN | RCTL_BAM | RCTL_SECRC | RCTL_BSIZE_2048);

    // Program the TX ring and enable the transmitter.
    reg_write(REG_TDBAL, (uint32_t)tx_ring_phys);
    reg_write(REG_TDBAH, (uint32_t)(tx_ring_phys >> 32));
    reg_write(REG_TDLEN, N_TX * (uint32_t)sizeof(tx_desc));
    reg_write(REG_TDH, 0);
    reg_write(REG_TDT, 0);
    reg_write(REG_TCTL, TCTL_EN | TCTL_PSP | (0x0F << 4) | (0x40 << 12));
    reg_write(REG_TIPG, 0x0060200A);
}

bool e1000_init(void)
{
    pci_addr a = pci_find(0x8086, 0x100E); // Intel 82540EM
    if (!a.found) {
        return false;
    }
    pci_enable_bus_master(a);

    uintptr_t bar0 = pci_bar(a, 0);
    for (uint64_t off = 0; off < NICWIN_SZ; off += PAGE_SZ) {
        map_page(kernel_dir, NICWIN_VA + off, bar0 + off,
                 PAGEF_P | PAGEF_RW | PAGEF_UC);
    }
    mmio = (volatile uint8_t*)NICWIN_VA;

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

bool e1000_present(void)
{
    return present;
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
    volatile tx_desc* d = &tx_ring[tx_cur];
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
    // descriptor once it sees TDT advance. (x86 keeps stores in program order,
    // so this is conservative here, but it documents the required ordering and
    // stays correct if the ring is ever mapped write-combining.)
    __asm__ __volatile__("mfence" ::: "memory");
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

bool e1000_rx_poll(e1000_frame* out)
{
    if (!present) {
        return false;
    }
    rx_recycle_handed();
    while (rx_ring[rx_cur].status & RXD_STAT_DD) {
        volatile rx_desc* d = &rx_ring[rx_cur];
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
