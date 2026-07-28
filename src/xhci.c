// Minimal xHCI USB host-controller driver (see xhci.h). Milestone 1: bring the
// controller up and prove its ring machinery. xHCI is driven by TRB (Transfer
// Request Block) rings much like NVMe's queues, but with three ring types — a
// command ring (software -> controller), an event ring (controller -> software,
// consumed by a cycle bit), and per-endpoint transfer rings — plus a device
// context array the controller DMAs. Here we set up the command + event rings
// and the DCBAA, run the controller, and round-trip a NO-OP command to confirm
// doorbells, cycle bits and event consumption all work. Poll-driven, BSP-only.
//
// All controller-visible memory (rings, ERST, DCBAA, scratchpad) is fresh
// physical frames reached through the HHDM; frames are page-aligned, which
// satisfies every xHCI alignment requirement.

#include <xhci.h>
#include <pci.h>
#include <paging.h>
#include <frames.h>
#include <ktime.h>
#include <utils.h>

// PCI class for an xHCI controller: serial bus / USB / XHCI programming iface.
#define PCI_CLASS_SERIAL 0x0C
#define PCI_SUBCLASS_USB 0x03
#define PCI_PROGIF_XHCI 0x30

// Capability registers (at the BAR base).
#define CAP_CAPLENGTH 0x00 // u8: length -> operational registers
#define CAP_HCSPARAMS1 0x04
#define CAP_HCSPARAMS2 0x08
#define CAP_HCCPARAMS1 0x10
#define CAP_DBOFF 0x14  // doorbell array offset
#define CAP_RTSOFF 0x18 // runtime register space offset

// Operational registers (BAR base + CAPLENGTH).
#define OP_USBCMD 0x00
#define OP_USBSTS 0x04
#define OP_CRCR 0x18   // command ring control (64-bit)
#define OP_DCBAAP 0x30 // device context base array pointer (64-bit)
#define OP_CONFIG 0x38

#define USBCMD_RUN (1u << 0)
#define USBCMD_HCRST (1u << 1)
#define USBSTS_HCH (1u << 0)  // HC halted
#define USBSTS_CNR (1u << 11) // controller not ready
#define CRCR_RCS (1u << 0)    // ring cycle state

// Runtime registers: interrupter 0 lives at RTSOFF + 0x20.
#define IR0 0x20
#define IR_ERSTSZ 0x08     // event ring segment table size
#define IR_ERSTBA 0x10     // ...base address (64-bit)
#define IR_ERDP 0x18       // event ring dequeue pointer (64-bit)
#define ERDP_EHB (1u << 3) // event handler busy (write 1 to clear)

// TRB types (control bits 15:10) and completion codes we act on.
#define TRB_LINK 6
#define TRB_NO_OP_CMD 23
#define TRB_CMD_COMPLETION 33 // event
#define TRB_TYPE(t) ((uint32_t)(t) << 10)
#define TRB_TYPE_OF(ctrl) (((ctrl) >> 10) & 0x3F)
#define TRB_CYCLE (1u << 0)
#define TRB_LINK_TC (1u << 1) // link: toggle cycle
#define COMPLETION_CODE(status) (((status) >> 24) & 0xFF)
#define CC_SUCCESS 1

#define RING_TRBS 256 // one page of 16-byte TRBs

// A Transfer Request Block: 16 bytes, the unit of every xHCI ring.
struct trb {
    uint64_t param;
    uint32_t status;
    uint32_t control;
} __attribute__((packed));

// One Event Ring Segment Table entry: {ring base, size}.
struct erst_entry {
    uint64_t base;
    uint32_t size;
    uint32_t rsvd;
} __attribute__((packed));

static volatile uint8_t* cap; // capability registers (BAR base)
static volatile uint8_t* op;  // operational registers
static volatile uint8_t* rt;  // runtime registers
static volatile uint32_t* db; // doorbell array
static bool g_present;
static uint32_t g_ports;
static uint32_t g_slots;
static bool g_csz64; // 64-byte contexts (else 32) — for enumeration later

static struct trb* cmd_ring;
static uintptr_t cmd_ring_pa;
static uint32_t cmd_enq;   // producer index
static uint32_t cmd_cycle; // producer cycle state

static struct trb* evt_ring;
static uintptr_t evt_ring_pa;
static uint32_t evt_deq;   // consumer index
static uint32_t evt_cycle; // consumer cycle state

static uint32_t r32(volatile uint8_t* base, uint32_t o)
{
    return *(volatile uint32_t*)(base + o);
}
static void w32(volatile uint8_t* base, uint32_t o, uint32_t v)
{
    *(volatile uint32_t*)(base + o) = v;
}
static void w64(volatile uint8_t* base, uint32_t o, uint64_t v)
{
    *(volatile uint64_t*)(base + o) = v;
}

static void* dma_page(uintptr_t* pa)
{
    uintptr_t p = frame_alloc();
    void* v = phys_to_virt(p);
    memset(v, 0, PAGE_SZ);
    *pa = p;
    return v;
}

// Enqueue a command TRB (cycle bit filled in from the producer state) and,
// when the ring wraps, follow the trailing LINK TRB back to the start.
static void cmd_enqueue(uint64_t param, uint32_t status, uint32_t control)
{
    struct trb* t = &cmd_ring[cmd_enq];
    t->param = param;
    t->status = status;
    t->control = (control & ~TRB_CYCLE) | cmd_cycle;
    cmd_enq++;
    if (cmd_enq == RING_TRBS - 1) { // the last slot is the LINK TRB
        struct trb* link = &cmd_ring[RING_TRBS - 1];
        link->control = (link->control & ~TRB_CYCLE) | cmd_cycle;
        cmd_enq = 0;
        cmd_cycle ^= 1;
    }
}

// Ring the command doorbell (slot 0, target 0) after a barrier so the TRB
// stores are visible before the controller reads them.
static void ring_command_doorbell(void)
{
    __asm__ __volatile__("" ::: "memory");
    db[0] = 0;
}

// Wait (bounded) for the next event TRB, copy it out, advance the dequeue
// pointer and acknowledge it via ERDP. Returns false on timeout.
static bool poll_event(struct trb* out, uint64_t timeout_ms)
{
    uint64_t deadline = ktime_ms() + timeout_ms;
    for (;;) {
        volatile struct trb* e = &evt_ring[evt_deq];
        if ((e->control & TRB_CYCLE) == evt_cycle) {
            out->param = e->param;
            out->status = e->status;
            out->control = e->control;
            uint64_t consumed =
                    evt_ring_pa + (uint64_t)evt_deq * sizeof(struct trb);
            evt_deq++;
            if (evt_deq == RING_TRBS) {
                evt_deq = 0;
                evt_cycle ^= 1;
            }
            w64(rt, IR0 + IR_ERDP, consumed | ERDP_EHB);
            return true;
        }
        if (ktime_ms() > deadline) {
            return false;
        }
        __asm__ __volatile__("pause");
    }
}

void xhci_init(void)
{
    pci_addr a =
            pci_find_class(PCI_CLASS_SERIAL, PCI_SUBCLASS_USB, PCI_PROGIF_XHCI);
    if (!a.found) {
        return;
    }
    pci_enable_bus_master(a);
    cap = iomap(pci_bar64(a, 0), 0x10000, PAGEF_P | PAGEF_RW | PAGEF_UC);

    uint8_t caplen = *(volatile uint8_t*)(cap + CAP_CAPLENGTH);
    op = cap + caplen;
    rt = cap + (r32(cap, CAP_RTSOFF) & ~0x1Fu);
    db = (volatile uint32_t*)(cap + (r32(cap, CAP_DBOFF) & ~0x3u));

    uint32_t hcs1 = r32(cap, CAP_HCSPARAMS1);
    g_slots = hcs1 & 0xFF;
    g_ports = (hcs1 >> 24) & 0xFF;
    g_csz64 = (r32(cap, CAP_HCCPARAMS1) & (1u << 2)) != 0;

    // Wait for the controller to report ready, then halt and reset it.
    uint64_t deadline = ktime_ms() + 1000;
    while (r32(op, OP_USBSTS) & USBSTS_CNR) {
        if (ktime_ms() > deadline) {
            return;
        }
    }
    w32(op, OP_USBCMD, r32(op, OP_USBCMD) & ~USBCMD_RUN);
    deadline = ktime_ms() + 1000;
    while (!(r32(op, OP_USBSTS) & USBSTS_HCH)) {
        if (ktime_ms() > deadline) {
            return;
        }
    }
    w32(op, OP_USBCMD, USBCMD_HCRST);
    deadline = ktime_ms() + 1000;
    while ((r32(op, OP_USBCMD) & USBCMD_HCRST) ||
           (r32(op, OP_USBSTS) & USBSTS_CNR)) {
        if (ktime_ms() > deadline) {
            return;
        }
    }

    // Advertise all device slots and hand the controller its context array.
    w32(op, OP_CONFIG, g_slots);
    uintptr_t dcbaa_pa;
    (void)dma_page(&dcbaa_pa); // scratchpad handling comes with enumeration
    w64(op, OP_DCBAAP, dcbaa_pa);

    // Command ring: a page of TRBs whose last slot is a LINK back to the start.
    cmd_ring = dma_page(&cmd_ring_pa);
    struct trb* link = &cmd_ring[RING_TRBS - 1];
    link->param = cmd_ring_pa;
    link->control = TRB_TYPE(TRB_LINK) | TRB_LINK_TC;
    cmd_enq = 0;
    cmd_cycle = 1;
    w64(op, OP_CRCR, cmd_ring_pa | CRCR_RCS);

    // Event ring: one segment described by a one-entry ERST. Program size and
    // dequeue pointer before the base address (which arms the interrupter).
    uintptr_t erst_pa;
    struct erst_entry* erst = dma_page(&erst_pa);
    evt_ring = dma_page(&evt_ring_pa);
    evt_deq = 0;
    evt_cycle = 1;
    erst[0].base = evt_ring_pa;
    erst[0].size = RING_TRBS;
    w32(rt, IR0 + IR_ERSTSZ, 1);
    w64(rt, IR0 + IR_ERDP, evt_ring_pa | ERDP_EHB);
    w64(rt, IR0 + IR_ERSTBA, erst_pa);

    // Run, then confirm the ring machinery with a NO-OP command round-trip.
    w32(op, OP_USBCMD, USBCMD_RUN);
    deadline = ktime_ms() + 1000;
    while (r32(op, OP_USBSTS) & USBSTS_HCH) {
        if (ktime_ms() > deadline) {
            return;
        }
    }

    cmd_enqueue(0, 0, TRB_TYPE(TRB_NO_OP_CMD));
    ring_command_doorbell();
    struct trb ev;
    if (!poll_event(&ev, 1000)) {
        return; // no completion event: controller not usable
    }
    if (TRB_TYPE_OF(ev.control) != TRB_CMD_COMPLETION ||
        COMPLETION_CODE(ev.status) != CC_SUCCESS) {
        return;
    }
    g_present = true;
}

bool xhci_present(void)
{
    return g_present;
}
uint32_t xhci_ports(void)
{
    return g_present ? g_ports : 0;
}
