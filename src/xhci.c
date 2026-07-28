// Minimal xHCI USB host-controller driver (see xhci.h). Milestone 1: bring the
// controller up and enumerate the attached device to its descriptor. xHCI is
// driven by TRB (Transfer Request Block) rings much like NVMe's queues, but
// with several ring types — a command ring (software -> controller), an event
// ring (controller -> software, consumed by a cycle bit), and per-endpoint
// transfer rings — plus a device-context array the controller DMAs.
//
// Bring-up sets up the command + event rings and the DCBAA, runs the
// controller, and round-trips a NO-OP command. Enumeration then resets the
// connected port, ENABLE_SLOTs it, ADDRESS_DEVICEs it (building the slot +
// control-endpoint contexts), and runs one EP0 control transfer
// (GET_DESCRIPTOR) to read the device's vendor/product/class. Poll-driven,
// BSP-only; MSI-X, bulk endpoints and a class driver come later.
//
// All controller-visible memory (rings, ERST, DCBAA, contexts, buffers) is
// fresh physical frames reached through the HHDM; frames are page-aligned,
// which satisfies every xHCI alignment requirement.

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
#define CAP_HCCPARAMS1 0x10
#define CAP_DBOFF 0x14  // doorbell array offset
#define CAP_RTSOFF 0x18 // runtime register space offset

// Operational registers (BAR base + CAPLENGTH).
#define OP_USBCMD 0x00
#define OP_USBSTS 0x04
#define OP_CRCR 0x18   // command ring control (64-bit)
#define OP_DCBAAP 0x30 // device context base array pointer (64-bit)
#define OP_CONFIG 0x38
#define OP_PORTSC(p) (0x400 + 0x10 * ((p) - 1)) // p is 1-based

#define USBCMD_RUN (1u << 0)
#define USBCMD_HCRST (1u << 1)
#define USBSTS_HCH (1u << 0)  // HC halted
#define USBSTS_CNR (1u << 11) // controller not ready
#define CRCR_RCS (1u << 0)    // ring cycle state

#define PORTSC_CCS (1u << 0) // current connect status
#define PORTSC_PED (1u << 1) // port enabled/disabled
#define PORTSC_PR (1u << 4)  // port reset
#define PORTSC_SPEED(v) (((v) >> 10) & 0xF)
#define PORTSC_RW1C 0x00FE0000u // change bits: write 0 to leave them untouched

// Runtime registers: interrupter 0 lives at RTSOFF + 0x20.
#define IR0 0x20
#define IR_ERSTSZ 0x08     // event ring segment table size
#define IR_ERSTBA 0x10     // ...base address (64-bit)
#define IR_ERDP 0x18       // event ring dequeue pointer (64-bit)
#define ERDP_EHB (1u << 3) // event handler busy (write 1 to clear)

// TRB types (control bits 15:10), flags, and completion codes we act on.
#define TRB_LINK 6
#define TRB_SETUP_STAGE 2
#define TRB_DATA_STAGE 3
#define TRB_STATUS_STAGE 4
#define TRB_ENABLE_SLOT 9
#define TRB_ADDRESS_DEVICE 11
#define TRB_NO_OP_CMD 23
#define TRB_TRANSFER_EVENT 32
#define TRB_CMD_COMPLETION 33
#define TRB_TYPE(t) ((uint32_t)(t) << 10)
#define TRB_TYPE_OF(ctrl) (((ctrl) >> 10) & 0x3F)
#define TRB_CYCLE (1u << 0)
#define TRB_LINK_TC (1u << 1) // link: toggle cycle
#define TRB_IOC (1u << 5)     // interrupt on completion
#define TRB_IDT (1u << 6)     // immediate data (setup stage)
#define TRB_DIR_IN (1u << 16) // data/status stage: device-to-host
#define TRT_IN (3u << 16)     // setup stage: an IN data stage follows
#define EP_TYPE_CONTROL (4u << 3)
#define COMPLETION_CODE(status) (((status) >> 24) & 0xFF)
#define CC_SUCCESS 1
#define CC_SHORT_PACKET 13

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

// A producer ring (command or transfer): a page of TRBs whose last slot is a
// LINK back to the start, plus the producer index and cycle state.
typedef struct {
    struct trb* trb;
    uintptr_t pa;
    uint32_t enq;
    uint32_t cycle;
} ring;

static volatile uint8_t* cap; // capability registers (BAR base)
static volatile uint8_t* op;  // operational registers
static volatile uint8_t* rt;  // runtime registers
static volatile uint32_t* db; // doorbell array
static bool g_present;
static uint32_t g_ports;
static uint32_t g_slots;
static uint32_t ctx_sz; // bytes per context entry (32 or 64, from HCCPARAMS1)

static ring cmd; // command ring
static ring ep0; // the enumerated device's control endpoint ring

static uint64_t* dcbaa; // device context base array
static uintptr_t dcbaa_pa;

static struct trb* evt_ring;
static uintptr_t evt_ring_pa;
static uint32_t evt_deq;   // consumer index
static uint32_t evt_cycle; // consumer cycle state

// Enumerated device (milestone 1 stops at the device descriptor).
static bool g_dev_found;
static uint16_t g_vid, g_pid;
static uint8_t g_class;

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

static void ring_init(ring* r)
{
    r->trb = dma_page(&r->pa);
    struct trb* link = &r->trb[RING_TRBS - 1];
    link->param = r->pa; // LINK back to the start
    link->control = TRB_TYPE(TRB_LINK) | TRB_LINK_TC;
    r->enq = 0;
    r->cycle = 1;
}

// Enqueue a TRB (cycle bit filled from the producer state); on reaching the
// trailing LINK slot, arm it with the current cycle and wrap.
static void ring_push(ring* r, uint64_t param, uint32_t status,
                      uint32_t control)
{
    struct trb* t = &r->trb[r->enq];
    t->param = param;
    t->status = status;
    t->control = (control & ~TRB_CYCLE) | r->cycle;
    r->enq++;
    if (r->enq == RING_TRBS - 1) {
        struct trb* link = &r->trb[RING_TRBS - 1];
        link->control = (link->control & ~TRB_CYCLE) | r->cycle;
        r->enq = 0;
        r->cycle ^= 1;
    }
}

// Wait (bounded) for the next event TRB and copy it out, advancing the dequeue
// pointer and acknowledging it via ERDP. Returns false on timeout.
static bool next_event(struct trb* out, uint64_t timeout_ms)
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

// Drain events until one of `want_type` arrives (port-status-change and other
// events share the ring and must be skipped). Returns false on timeout.
static bool wait_event(uint32_t want_type, struct trb* out, uint64_t timeout_ms)
{
    for (int i = 0; i < 64; i++) {
        if (!next_event(out, timeout_ms)) {
            return false;
        }
        if (TRB_TYPE_OF(out->control) == want_type) {
            return true;
        }
    }
    return false;
}

// Issue a command and wait for its Command Completion Event. Returns false on
// timeout/wrong event; the caller reads slot id / completion code from `*ev`.
static bool run_command(uint64_t param, uint32_t control, struct trb* ev)
{
    ring_push(&cmd, param, 0, control);
    __asm__ __volatile__("" ::: "memory");
    db[0] = 0; // command doorbell (slot 0, target 0)
    return wait_event(TRB_CMD_COMPLETION, ev, 1000);
}

// EP0 max packet size for the port speed (SS=512, HS=64, LS=8, FS=64).
static uint16_t ep0_mps(uint32_t speed)
{
    switch (speed) {
    case 4:
        return 512;
    case 2:
        return 8;
    default:
        return 64;
    }
}

// One EP0 IN control transfer: SETUP / DATA / STATUS stages, then ring the
// slot's EP0 doorbell and wait for the transfer to complete. `data_pa` receives
// `len` bytes. Returns false on timeout or a non-success completion.
static bool control_in(uint32_t slot, uint64_t setup, uintptr_t data_pa,
                       uint16_t len)
{
    ring_push(&ep0, setup, 8, TRB_TYPE(TRB_SETUP_STAGE) | TRB_IDT | TRT_IN);
    ring_push(&ep0, data_pa, len, TRB_TYPE(TRB_DATA_STAGE) | TRB_DIR_IN);
    ring_push(&ep0, 0, 0, TRB_TYPE(TRB_STATUS_STAGE) | TRB_IOC);
    __asm__ __volatile__("" ::: "memory");
    db[slot] = 1; // EP0 doorbell (DCI 1)
    struct trb ev;
    if (!wait_event(TRB_TRANSFER_EVENT, &ev, 1000)) {
        return false;
    }
    uint8_t cc = COMPLETION_CODE(ev.status);
    return cc == CC_SUCCESS || cc == CC_SHORT_PACKET;
}

// Reset the first connected root port and address the device on it, then read
// its device descriptor. Stores vendor/product/class on success.
static void enumerate(void)
{
    uint32_t port = 0, speed = 0;
    for (uint32_t p = 1; p <= g_ports; p++) {
        uint32_t sc = r32(op, OP_PORTSC(p));
        if (!(sc & PORTSC_CCS)) {
            continue;
        }
        if (!(sc & PORTSC_PED)) {
            w32(op, OP_PORTSC(p), (sc & ~PORTSC_RW1C) | PORTSC_PR);
            uint64_t dl = ktime_ms() + 500;
            do {
                sc = r32(op, OP_PORTSC(p));
            } while (!(sc & PORTSC_PED) && ktime_ms() < dl);
        }
        if (sc & PORTSC_PED) {
            port = p;
            speed = PORTSC_SPEED(sc);
            break;
        }
    }
    if (port == 0) {
        return; // nothing connected/enabled
    }

    struct trb ev;
    if (!run_command(0, TRB_TYPE(TRB_ENABLE_SLOT), &ev) ||
        COMPLETION_CODE(ev.status) != CC_SUCCESS) {
        return;
    }
    uint32_t slot = (ev.control >> 24) & 0xFF;

    // Output device context (controller-written); publish it in the DCBAA.
    uintptr_t devctx_pa;
    (void)dma_page(&devctx_pa);
    dcbaa[slot] = devctx_pa;

    // Input context: enable the slot + EP0, describe the port and control EP.
    uintptr_t input_pa;
    uint32_t* input = dma_page(&input_pa);
    uint32_t dwpc = ctx_sz / 4;               // dwords per context entry
    input[1] = 0x3;                           // add flags: A0 (slot) + A1 (EP0)
    uint32_t* slot_ctx = input + dwpc;        // slot context (index 1)
    uint32_t* ep0_ctx = input + 2 * dwpc;     // EP0 context (index 2)
    slot_ctx[0] = (speed << 20) | (1u << 27); // speed, context entries = 1
    slot_ctx[1] = (port << 16);               // root hub port number

    ring_init(&ep0);
    uint16_t mps = ep0_mps(speed);
    ep0_ctx[1] = EP_TYPE_CONTROL | (3u << 1) | ((uint32_t)mps << 16); // +CErr=3
    ep0_ctx[2] = (uint32_t)(ep0.pa | 1); // TR dequeue pointer low | DCS
    ep0_ctx[3] = (uint32_t)(ep0.pa >> 32);
    ep0_ctx[4] = 8; // average TRB length

    if (!run_command(input_pa, TRB_TYPE(TRB_ADDRESS_DEVICE) | (slot << 24),
                     &ev) ||
        COMPLETION_CODE(ev.status) != CC_SUCCESS) {
        return;
    }

    // GET_DESCRIPTOR (device): bmRequestType=0x80, bRequest=6, wValue=0x0100,
    // wLength=18. The setup packet's 8 bytes travel as the TRB's immediate
    // data.
    uintptr_t buf_pa;
    uint8_t* buf = dma_page(&buf_pa);
    uint64_t setup = (uint64_t)0x80 | ((uint64_t)0x06 << 8) |
                     ((uint64_t)0x0100 << 16) | ((uint64_t)18 << 48);
    if (!control_in(slot, setup, buf_pa, 18)) {
        return;
    }
    g_class = buf[4];
    g_vid = (uint16_t)(buf[8] | (buf[9] << 8));
    g_pid = (uint16_t)(buf[10] | (buf[11] << 8));
    g_dev_found = true;
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
    ctx_sz = (r32(cap, CAP_HCCPARAMS1) & (1u << 2)) ? 64 : 32;

    // Wait for ready, then halt and reset the controller.
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
    dcbaa = dma_page(&dcbaa_pa);
    w64(op, OP_DCBAAP, dcbaa_pa);

    // Command ring.
    ring_init(&cmd);
    w64(op, OP_CRCR, cmd.pa | CRCR_RCS);

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
    struct trb ev;
    if (!run_command(0, TRB_TYPE(TRB_NO_OP_CMD), &ev) ||
        COMPLETION_CODE(ev.status) != CC_SUCCESS) {
        return;
    }
    g_present = true;

    enumerate(); // milestone 1: read the attached device's descriptor
}

bool xhci_present(void)
{
    return g_present;
}
uint32_t xhci_ports(void)
{
    return g_present ? g_ports : 0;
}
bool xhci_device_found(void)
{
    return g_dev_found;
}
uint16_t xhci_vid(void)
{
    return g_vid;
}
uint16_t xhci_pid(void)
{
    return g_pid;
}
uint8_t xhci_class(void)
{
    return g_class;
}
