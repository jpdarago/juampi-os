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
#include <dma.h>
#include <ktime.h>
#include <idt.h>      // register_interrupt_handler, interrupt_frame
#include <keyboard.h> // keyboard_inject: HID keyboard feeds the input ring
#include <utils.h>
#include <blockdev.h>

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
#define USBCMD_INTE (1u << 2) // master interrupt enable
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
#define IR_IMAN 0x00       // interrupter management
#define IR_ERSTSZ 0x08     // event ring segment table size
#define IR_ERSTBA 0x10     // ...base address (64-bit)
#define IR_ERDP 0x18       // event ring dequeue pointer (64-bit)
#define IMAN_IP (1u << 0)  // interrupt pending (write 1 to clear)
#define IMAN_IE (1u << 1)  // interrupt enable
#define ERDP_EHB (1u << 3) // event handler busy (write 1 to clear)

// A free IDT vector (32-47 have stubs) for MSI-X event delivery.
#define XHCI_VECTOR 46

// TRB types (control bits 15:10), flags, and completion codes we act on.
#define TRB_NORMAL 1
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
#define TRB_ISP (1u << 2)     // interrupt on short packet
#define TRB_IOC (1u << 5)     // interrupt on completion
#define TRB_IDT (1u << 6)     // immediate data (setup stage)
#define TRB_DIR_IN (1u << 16) // data/status stage: device-to-host
#define TRT_IN (3u << 16)     // setup stage: an IN data stage follows
#define TRB_CONFIGURE_ENDPOINT 12
#define TRB_EVALUATE_CONTEXT 13
#define TRB_RESET_ENDPOINT 14
#define TRB_SET_TR_DEQUEUE 16
#define EP_TYPE_CONTROL (4u << 3)
#define EP_TYPE_BULK_OUT (2u << 3)
#define EP_TYPE_BULK_IN (6u << 3)
#define EP_TYPE_INT_IN (7u << 3)
#define COMPLETION_CODE(status) (((status) >> 24) & 0xFF)
#define CC_SUCCESS 1
#define CC_STALL 6
#define CC_SHORT_PACKET 13
#define CC_TIMEOUT 0xFF // ours: no event arrived, not a controller code

// USB descriptor types and the mass-storage (Bulk-Only Transport) class codes.
#define DESC_DEVICE 1
#define DESC_CONFIG 2
#define DESC_INTERFACE 4
#define DESC_ENDPOINT 5
#define USB_CLASS_MSC 8
#define USB_CLASS_HUB 9
#define USB_CLASS_HID 3
#define MSC_SUBCLASS_SCSI 6
#define MSC_PROTO_BOT 0x50
#define HID_SUBCLASS_BOOT 1
#define HID_PROTO_KBD 1
#define HID_PROTO_MOUSE 2
#define EP_ATTR_BULK 2 // bmAttributes transfer type
#define EP_ATTR_INT 3  // interrupt endpoint

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

// A bulk endpoint: its transfer ring, its doorbell target (the device context
// index), and its max packet size.
typedef struct {
    ring r;
    uint32_t dci;
    uint16_t mps;
} usb_endpoint;

static volatile uint8_t* cap; // capability registers (BAR base)
static volatile uint8_t* op;  // operational registers
static volatile uint8_t* rt;  // runtime registers
static volatile uint32_t* db; // doorbell array
static bool g_present;

// Which init/enumeration step failed, for the boot report (NULL: no
// controller, or fine). Silent failure is expensive on real hardware.
static const char* fail_reason;
static uint32_t g_ports;
static uint32_t ctx_sz; // bytes per context entry (32 or 64, from HCCPARAMS1)

static ring cmd; // command ring

static uint64_t* dcbaa; // device context base array

// The event ring: the controller produces, we consume by cycle bit.
static struct {
    struct trb* trb;
    uintptr_t pa;
    uint32_t deq;   // consumer index
    uint32_t cycle; // consumer cycle state
} evt;

// MSI-X interrupt state for interrupter 0. When enabled, event-ring waits nap
// on hlt (woken by the interrupt) instead of busy-polling; consumption itself
// stays with the synchronous waiter — the ISR's only job is the wakeup.
static struct {
    bool enabled;            // MSI-X up; false = polled fallback
    volatile uint64_t count; // interrupts the ISR has taken
} evt_irq;

// An addressed USB device: its slot, where it sits in the topology (root port
// + the xHCI route string of hub hops below it), its speed and identity, and
// its control-endpoint transfer ring.
#define MAX_DEVICES 8
typedef struct {
    bool used;
    uint32_t slot;
    uint32_t speed;     // PORTSC-style speed code (FS=1 LS=2 HS=3 SS=4)
    uint32_t root_port; // 1-based root-hub port
    uint32_t route;     // hub-port nibbles below the root port (xHCI route)
    uint32_t depth;     // number of hubs above (0 = on a root port)
    uint16_t vid, pid;
    uint8_t usb_class; // bDeviceClass (9 = hub; 0 = classes per interface)
    ring ep0;
} usb_device;
static usb_device devices[MAX_DEVICES];
static uint32_t n_devices;

// The configured Bulk-Only-Transport mass-storage function (the first found).
static struct {
    bool ready;
    uint32_t slot;       // owning device's slot (for bulk doorbells)
    uint64_t blocks;     // capacity, in logical blocks
    uint32_t block_size; // bytes per logical block
    uint32_t tag;        // rolling CBW/CSW transaction tag
    usb_endpoint in, out;
    dma_buf cbw, csw; // Bulk-Only-Transport command/status wrappers
    dma_buf bounce;   // one-page staging buffer for data
} msc;

// A HID boot-protocol input device: its interrupt IN endpoint and the single
// in-flight report. Reports arrive asynchronously — hid_dispatch() consumes
// their transfer events wherever the event ring is drained (sync waits skip
// past them; xhci_poll() drains them from the idle loops).
typedef struct {
    bool present;
    uint32_t slot;
    usb_endpoint ep;           // interrupt IN
    dma_buf report;            // report buffer (one page; reports are <= 8 B)
    uint8_t last[8];           // previous keyboard report, for key-down diff
    volatile uint64_t reports; // reports received (test/diagnostic counter)
} hid_dev;
static hid_dev hid_kbd;

static bool hid_dispatch(const struct trb* ev);

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

static void ring_init(ring* r)
{
    dma_buf b = dma_page_alloc();
    r->trb = b.va;
    r->pa = b.pa;
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

// MSI-X handler: clear the interrupt-pending flag (RW1C; auto-cleared for
// MSI-X per spec, written defensively for quirky controllers) and count. The
// waiter consumes the event ring itself — the interrupt's job is to end its
// hlt nap.
static void xhci_irq(interrupt_frame* f)
{
    (void)f;
    w32(rt, IR0 + IR_IMAN, IMAN_IP | IMAN_IE);
    evt_irq.count++;
}

// Wait for the next event TRB (until the absolute `deadline`, in ktime_ms
// time) and copy it out, advancing the dequeue pointer and acknowledging it via
// ERDP. Returns false on timeout. When MSI-X is up the wait naps on `sti; hlt`
// with the ring checked under cli — the STI shadow makes enable-and-halt
// atomic, so an event landing between check and sleep still wakes us; the
// polled fallback spins on pause.
static bool next_event(struct trb* out, uint64_t deadline)
{
    if (evt_irq.enabled) {
        __asm__ __volatile__("cli");
    }
    for (;;) {
        volatile struct trb* e = &evt.trb[evt.deq];
        if ((e->control & TRB_CYCLE) == evt.cycle) {
            out->param = e->param;
            out->status = e->status;
            out->control = e->control;
            uint64_t consumed = evt.pa + (uint64_t)evt.deq * sizeof(struct trb);
            evt.deq++;
            if (evt.deq == RING_TRBS) {
                evt.deq = 0;
                evt.cycle ^= 1;
            }
            w64(rt, IR0 + IR_ERDP, consumed | ERDP_EHB);
            if (evt_irq.enabled) {
                __asm__ __volatile__("sti");
            }
            return true;
        }
        if (ktime_ms() > deadline) {
            if (evt_irq.enabled) {
                __asm__ __volatile__("sti");
            }
            return false;
        }
        if (evt_irq.enabled) {
            __asm__ __volatile__("sti; hlt; cli");
        } else {
            __asm__ __volatile__("pause");
        }
    }
}

// Drain events until one of `want_type` arrives (port-status-change and other
// events share the ring and must be skipped; asynchronous HID completions are
// dispatched, not lost). One deadline bounds the whole wait.
static bool wait_event(uint32_t want_type, struct trb* out, uint64_t timeout_ms)
{
    uint64_t deadline = ktime_ms() + timeout_ms;
    for (int i = 0; i < 64; i++) {
        if (!next_event(out, deadline)) {
            return false;
        }
        if (TRB_TYPE_OF(out->control) == want_type) {
            return true;
        }
        if (TRB_TYPE_OF(out->control) == TRB_TRANSFER_EVENT) {
            hid_dispatch(out); // a HID report landing mid-wait
        }
    }
    return false;
}

// Wait for the Transfer Event of (slot, dci) specifically — with HID endpoints
// live, a keypress completion can land during a bulk or control wait and must
// be dispatched rather than mistaken for the awaited completion.
static bool wait_transfer(uint32_t slot, uint32_t dci, struct trb* out,
                          uint64_t timeout_ms)
{
    uint64_t deadline = ktime_ms() + timeout_ms;
    for (int i = 0; i < 64; i++) {
        if (!next_event(out, deadline)) {
            return false;
        }
        if (TRB_TYPE_OF(out->control) == TRB_TRANSFER_EVENT) {
            uint32_t es = (out->control >> 24) & 0xFF;
            uint32_t ed = (out->control >> 16) & 0x1F;
            if (es == slot && ed == dci) {
                return true;
            }
            hid_dispatch(out);
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

// One EP0 IN control transfer on device `d`: SETUP / DATA / STATUS stages,
// then ring its EP0 doorbell and wait for the transfer to complete. `data_pa`
// receives `len` bytes. Returns false on timeout or a non-success completion.
static bool control_in(usb_device* d, uint64_t setup, uintptr_t data_pa,
                       uint16_t len)
{
    ring_push(&d->ep0, setup, 8, TRB_TYPE(TRB_SETUP_STAGE) | TRB_IDT | TRT_IN);
    ring_push(&d->ep0, data_pa, len, TRB_TYPE(TRB_DATA_STAGE) | TRB_DIR_IN);
    ring_push(&d->ep0, 0, 0, TRB_TYPE(TRB_STATUS_STAGE) | TRB_IOC);
    __asm__ __volatile__("" ::: "memory");
    db[d->slot] = 1; // EP0 doorbell (DCI 1)
    struct trb ev;
    if (!wait_transfer(d->slot, 1, &ev, 1000)) {
        return false;
    }
    uint8_t cc = COMPLETION_CODE(ev.status);
    return cc == CC_SUCCESS || cc == CC_SHORT_PACKET;
}

// A no-data control transfer (e.g. SET_CONFIGURATION): SETUP then a STATUS
// stage in the IN direction. Returns false on timeout/non-success.
static bool control_no_data(usb_device* d, uint64_t setup)
{
    ring_push(&d->ep0, setup, 8, TRB_TYPE(TRB_SETUP_STAGE) | TRB_IDT);
    ring_push(&d->ep0, 0, 0, TRB_TYPE(TRB_STATUS_STAGE) | TRB_DIR_IN | TRB_IOC);
    __asm__ __volatile__("" ::: "memory");
    db[d->slot] = 1;
    struct trb ev;
    if (!wait_transfer(d->slot, 1, &ev, 1000)) {
        return false;
    }
    uint8_t cc = COMPLETION_CODE(ev.status);
    return cc == CC_SUCCESS || cc == CC_SHORT_PACKET;
}

// GET_DESCRIPTOR(type<<8 | index) of `len` bytes into `buf_pa`.
static bool get_descriptor(usb_device* d, uint16_t value, uintptr_t buf_pa,
                           uint16_t len)
{
    uint64_t setup = (uint64_t)0x80 | ((uint64_t)0x06 << 8) |
                     ((uint64_t)value << 16) | ((uint64_t)len << 48);
    return control_in(d, setup, buf_pa, len);
}

// One bulk transfer on endpoint `ep`: a Normal TRB on its ring, ring its
// doorbell, wait for its Transfer Event. Interrupt on completion + short
// packet. Returns the completion code (CC_TIMEOUT if no event arrived), so
// callers can distinguish a STALL — which halts the endpoint and needs
// recovery — from other failures.
static uint8_t bulk_xfer(usb_endpoint* ep, uintptr_t data_pa, uint32_t len)
{
    ring_push(&ep->r, data_pa, len, TRB_TYPE(TRB_NORMAL) | TRB_IOC | TRB_ISP);
    __asm__ __volatile__("" ::: "memory");
    db[msc.slot] = ep->dci;
    struct trb ev;
    if (!wait_transfer(msc.slot, ep->dci, &ev, 2000)) {
        return CC_TIMEOUT;
    }
    return COMPLETION_CODE(ev.status);
}

static bool cc_ok(uint8_t cc)
{
    return cc == CC_SUCCESS || cc == CC_SHORT_PACKET;
}

// Reset a bulk ring to its pristine state (used after the controller stops an
// endpoint: its dequeue pointer is rewound to the ring base).
static void ring_reset(ring* r)
{
    memset(r->trb, 0, PAGE_SZ);
    struct trb* link = &r->trb[RING_TRBS - 1];
    link->param = r->pa;
    link->control = TRB_TYPE(TRB_LINK) | TRB_LINK_TC;
    r->enq = 0;
    r->cycle = 1;
}

// The addressed device owning `slot`, or NULL.
static usb_device* device_by_slot(uint32_t slot)
{
    for (uint32_t i = 0; i < n_devices; i++) {
        if (devices[i].used && devices[i].slot == slot) {
            return &devices[i];
        }
    }
    return NULL;
}

// Recover a halted (STALLed) bulk endpoint, per xHCI + USB: RESET_ENDPOINT
// moves it Halted -> Stopped, SET_TR_DEQUEUE rewinds its (reset) transfer
// ring, and CLEAR_FEATURE(ENDPOINT_HALT) tells the device to clear its halt
// and reset its data toggle. The endpoint is usable again afterwards.
static void recover_endpoint(usb_endpoint* ep)
{
    struct trb ev;
    run_command(0,
                TRB_TYPE(TRB_RESET_ENDPOINT) | (msc.slot << 24) |
                        (ep->dci << 16),
                &ev);
    ring_reset(&ep->r);
    run_command(ep->r.pa | 1, // new dequeue pointer | DCS
                TRB_TYPE(TRB_SET_TR_DEQUEUE) | (msc.slot << 24) |
                        (ep->dci << 16),
                &ev);
    // CLEAR_FEATURE(ENDPOINT_HALT): bmRequestType=0x02 (endpoint), bRequest=1,
    // wValue=0 (ENDPOINT_HALT), wIndex = the endpoint address.
    usb_device* d = device_by_slot(msc.slot);
    if (d != NULL) {
        uint64_t ep_addr = (ep->dci >> 1) | ((ep->dci & 1) << 7);
        control_no_data(d, (uint64_t)0x02 | ((uint64_t)0x01 << 8) |
                                   (ep_addr << 32));
    }
}

// One Bulk-Only-Transport command: send the 31-byte CBW wrapping the SCSI CDB,
// run the (optional) data phase, then read and check the 13-byte CSW.
static bool bot_command(const uint8_t* cdb, uint8_t cdb_len, bool dir_in,
                        uintptr_t data_pa, uint32_t data_len)
{
    msc.tag++;
    uint8_t* cbw = msc.cbw.va;
    memset(cbw, 0, 31);
    cbw[0] = 0x55; // dCBWSignature "USBC" (little-endian 0x43425355)
    cbw[1] = 0x53;
    cbw[2] = 0x42;
    cbw[3] = 0x43;
    memcpy(cbw + 4, &msc.tag, 4);   // dCBWTag
    memcpy(cbw + 8, &data_len, 4);  // dCBWDataTransferLength
    cbw[12] = dir_in ? 0x80 : 0x00; // bmCBWFlags
    cbw[13] = 0;                    // bCBWLUN
    cbw[14] = cdb_len;              // bCBWCBLength
    memcpy(cbw + 15, cdb, cdb_len); // CBWCB

    uint8_t cc = bulk_xfer(&msc.out, msc.cbw.pa, 31);
    if (!cc_ok(cc)) {
        if (cc == CC_STALL) {
            recover_endpoint(&msc.out);
        }
        return false;
    }
    if (data_len > 0) {
        usb_endpoint* ep = dir_in ? &msc.in : &msc.out;
        cc = bulk_xfer(ep, data_pa, data_len);
        if (!cc_ok(cc)) {
            if (cc != CC_STALL) {
                return false;
            }
            // A device that can't serve the data phase STALLs the endpoint but
            // still owes a CSW afterwards (BOT 6.7): recover and read it.
            recover_endpoint(ep);
        }
    }
    cc = bulk_xfer(&msc.in, msc.csw.pa, 13);
    if (cc == CC_STALL) {
        recover_endpoint(&msc.in);
        cc = bulk_xfer(&msc.in, msc.csw.pa, 13); // BOT 5.3.4: retry once
    }
    if (!cc_ok(cc)) {
        return false;
    }
    // dCSWSignature "USBS", dCSWTag echoing our dCBWTag (catches a desynced
    // command/status pairing), and bCSWStatus == 0 (command passed).
    const uint8_t* csw = msc.csw.va;
    uint32_t csw_tag;
    memcpy(&csw_tag, csw + 4, 4);
    return csw[0] == 0x55 && csw[1] == 0x53 && csw[2] == 0x42 &&
           csw[3] == 0x53 && csw_tag == msc.tag && csw[12] == 0;
}

// SCSI READ CAPACITY(10): last LBA + block size, both big-endian.
static bool scsi_read_capacity(void)
{
    uint8_t cdb[10] = {0x25};
    if (!bot_command(cdb, 10, true, msc.bounce.pa, 8)) {
        return false;
    }
    const uint8_t* d = msc.bounce.va;
    uint32_t last = ((uint32_t)d[0] << 24) | ((uint32_t)d[1] << 16) |
                    ((uint32_t)d[2] << 8) | d[3];
    msc.block_size = ((uint32_t)d[4] << 24) | ((uint32_t)d[5] << 16) |
                     ((uint32_t)d[6] << 8) | d[7];
    msc.blocks = (uint64_t)last + 1;
    return msc.block_size != 0;
}

// SCSI READ(10)/WRITE(10) of `blocks` logical blocks at `lba` to/from the
// bounce buffer (bounded to one page by the caller).
static bool scsi_rw(uint64_t lba, uint32_t blocks, bool write)
{
    uint8_t cdb[10] = {0};
    cdb[0] = write ? 0x2A : 0x28;
    cdb[2] = (uint8_t)(lba >> 24);
    cdb[3] = (uint8_t)(lba >> 16);
    cdb[4] = (uint8_t)(lba >> 8);
    cdb[5] = (uint8_t)lba;
    cdb[7] = (uint8_t)(blocks >> 8);
    cdb[8] = (uint8_t)blocks;
    return bot_command(cdb, 10, !write, msc.bounce.pa, blocks * msc.block_size);
}

// blockdev read/write: chunk the request through the one-page bounce buffer.
static bool msc_read(uint64_t lba, uint32_t count, void* buf)
{
    if (!msc.ready || msc.block_size == 0) {
        return false;
    }
    uint32_t per = PAGE_SZ / msc.block_size;
    uint8_t* out = buf;
    while (count > 0) {
        uint32_t n = count < per ? count : per;
        if (!scsi_rw(lba, n, false)) {
            return false;
        }
        memcpy(out, msc.bounce.va, (size_t)n * msc.block_size);
        out += (size_t)n * msc.block_size;
        lba += n;
        count -= n;
    }
    return true;
}
static bool msc_write(uint64_t lba, uint32_t count, const void* buf)
{
    if (!msc.ready || msc.block_size == 0) {
        return false;
    }
    uint32_t per = PAGE_SZ / msc.block_size;
    const uint8_t* in = buf;
    while (count > 0) {
        uint32_t n = count < per ? count : per;
        memcpy(msc.bounce.va, in, (size_t)n * msc.block_size);
        if (!scsi_rw(lba, n, true)) {
            return false;
        }
        in += (size_t)n * msc.block_size;
        lba += n;
        count -= n;
    }
    return true;
}
// 512-byte-sector count for the block-device view (0 unless 512-byte blocks).
static uint64_t msc_sectors(void)
{
    return (msc.ready && msc.block_size == 512) ? msc.blocks : 0;
}
static const blockdev msc_dev = {msc_read, msc_write, msc_sectors};

const blockdev* xhci_msc_blockdev(void)
{
    return &msc_dev;
}

// Configure the mass-storage interface of device `d` (first storage device
// wins): read the configuration descriptor, find the BOT interface and its two
// bulk endpoints, add them with CONFIGURE_ENDPOINT and select the
// configuration. Returns true when the device is ready for BOT.
static bool msc_setup(usb_device* d)
{
    if (msc.ready) {
        return false; // already bound to an earlier device
    }
    // Request a full page: the device returns min(wTotalLength, wLength), so
    // the walk below can never run past bytes that were actually transferred
    // (composite devices easily exceed the 255 bytes a short request returns).
    dma_buf cfg_mem = dma_page_alloc();
    uint8_t* cfg = cfg_mem.va;
    if (!get_descriptor(d, (uint16_t)(DESC_CONFIG << 8), cfg_mem.pa, PAGE_SZ)) {
        fail_reason = "config descriptor read failed";
        dma_page_free(cfg_mem);
        return false;
    }
    uint16_t total = (uint16_t)(cfg[2] | (cfg[3] << 8));
    uint8_t config_value = cfg[5];
    if (total > PAGE_SZ) {
        total = PAGE_SZ;
    }

    // Walk the descriptor list for the BOT interface and its bulk endpoints.
    bool in_msc = false, have_in = false, have_out = false;
    uint8_t in_ep = 0, out_ep = 0;
    for (uint32_t o = 0; o + 2 <= total;) {
        uint8_t blen = cfg[o], btype = cfg[o + 1];
        if (blen < 2) {
            break;
        }
        if (btype == DESC_INTERFACE) {
            in_msc = cfg[o + 5] == USB_CLASS_MSC &&
                     cfg[o + 6] == MSC_SUBCLASS_SCSI &&
                     cfg[o + 7] == MSC_PROTO_BOT;
        } else if (btype == DESC_ENDPOINT && in_msc) {
            uint8_t addr = cfg[o + 2];
            uint16_t mps = (uint16_t)(cfg[o + 4] | (cfg[o + 5] << 8));
            if ((cfg[o + 3] & 0x3) == EP_ATTR_BULK) {
                if (addr & 0x80) {
                    have_in = true;
                    in_ep = addr;
                    msc.in.mps = mps;
                } else {
                    have_out = true;
                    out_ep = addr;
                    msc.out.mps = mps;
                }
            }
        }
        o += blen;
    }
    dma_page_free(cfg_mem); // everything needed is extracted above
    if (!have_in || !have_out) {
        return false; // not a BOT mass-storage device — benign, no fail reason
    }
    // Doorbell context index for an endpoint: number*2 + (IN ? 1 : 0).
    msc.in.dci = (uint32_t)((in_ep & 0x0F) * 2 + 1);
    msc.out.dci = (uint32_t)((out_ep & 0x0F) * 2);
    ring_init(&msc.in.r);
    ring_init(&msc.out.r);

    // Input context: add the slot + both bulk endpoints, describe each
    // endpoint.
    dma_buf input_mem = dma_page_alloc();
    uint32_t* input = input_mem.va;
    uint32_t dwpc = ctx_sz / 4;
    uint32_t max_dci = msc.in.dci > msc.out.dci ? msc.in.dci : msc.out.dci;
    input[1] = 1u | (1u << msc.in.dci) | (1u << msc.out.dci); // A0 + endpoints
    uint32_t* slot_ctx = input + dwpc;
    slot_ctx[0] = d->route | (d->speed << 20) | (max_dci << 27);
    slot_ctx[1] = (d->root_port << 16);
    // The endpoint context for DCI n sits at input index n+1 (the input control
    // context occupies index 0, shifting the device context up by one).
    uint32_t* oc = input + (msc.out.dci + 1) * dwpc;
    oc[1] = EP_TYPE_BULK_OUT | (3u << 1) | ((uint32_t)msc.out.mps << 16);
    oc[2] = (uint32_t)(msc.out.r.pa | 1);
    oc[3] = (uint32_t)(msc.out.r.pa >> 32);
    oc[4] = msc.out.mps;
    uint32_t* ic = input + (msc.in.dci + 1) * dwpc;
    ic[1] = EP_TYPE_BULK_IN | (3u << 1) | ((uint32_t)msc.in.mps << 16);
    ic[2] = (uint32_t)(msc.in.r.pa | 1);
    ic[3] = (uint32_t)(msc.in.r.pa >> 32);
    ic[4] = msc.in.mps;

    struct trb ev;
    bool configured =
            run_command(input_mem.pa,
                        TRB_TYPE(TRB_CONFIGURE_ENDPOINT) | (d->slot << 24),
                        &ev) &&
            COMPLETION_CODE(ev.status) == CC_SUCCESS;
    // The input context is software-owned again once the command completes.
    dma_page_free(input_mem);
    if (!configured) {
        fail_reason = "CONFIGURE_ENDPOINT failed";
        return false;
    }
    // SET_CONFIGURATION(config_value): host-to-device standard request, no
    // data.
    uint64_t setup = (uint64_t)0x00 | ((uint64_t)0x09 << 8) |
                     ((uint64_t)config_value << 16);
    if (!control_no_data(d, setup)) {
        fail_reason = "SET_CONFIGURATION failed";
        return false;
    }
    msc.slot = d->slot;

    // Bulk-Only-Transport buffers, then read the capacity. A freshly attached
    // device may fail the first command with a unit-attention, so retry.
    msc.cbw = dma_page_alloc();
    msc.csw = dma_page_alloc();
    msc.bounce = dma_page_alloc();
    bool cap_ok = false;
    for (int i = 0; i < 4 && !cap_ok; i++) {
        cap_ok = scsi_read_capacity();
    }
    if (!cap_ok) {
        fail_reason = "READ CAPACITY failed";
        dma_page_free(msc.cbw);
        dma_page_free(msc.csw);
        dma_page_free(msc.bounce);
        return false;
    }
    msc.ready = true;
    return true;
}

// --- HID boot-protocol keyboard ---------------------------------------------

// HID usage -> ASCII for usages 0x04..0x38 (letters, digits, and the
// punctuation of the boot keyboard), unshifted and shifted.
static const char hid_keys[] = "abcdefghijklmnopqrstuvwxyz"
                               "1234567890"
                               "\n\x1b\b\t -=[]\\#;'`,./";
static const char hid_keys_shift[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
                                     "!@#$%^&*()"
                                     "\n\x1b\b\t _+{}|~:\"~<>?";

// Post the next interrupt-IN TRB so the device can deliver a report.
static void hid_post(hid_dev* h)
{
    ring_push(&h->ep.r, h->report.pa, 8,
              TRB_TYPE(TRB_NORMAL) | TRB_IOC | TRB_ISP);
    __asm__ __volatile__("" ::: "memory");
    db[h->slot] = h->ep.dci;
}

// Translate one 8-byte boot keyboard report ([modifiers][rsvd][keys x6]) into
// key-down characters for the shared input ring. A usage present now but not
// in the previous report is a fresh press; boot keyboards handle repeat on
// change only (host-side typematic repeat is not implemented).
static void hid_kbd_report(const uint8_t* r)
{
    bool shifted = (r[0] & 0x22) != 0; // left/right shift
    bool ctrl = (r[0] & 0x11) != 0;    // left/right control
    for (int i = 2; i < 8; i++) {
        uint8_t u = r[i];
        if (u < 4) {
            continue; // no key / error rollover
        }
        bool held = false;
        for (int j = 2; j < 8; j++) {
            held |= hid_kbd.last[j] == u;
        }
        if (held) {
            continue;
        }
        if (u >= 0x4F && u <= 0x52) { // right/left/down/up arrows
            keyboard_inject_seq("CDBA"[u - 0x4F]);
            continue;
        }
        if (u - 4 < (uint8_t)sizeof(hid_keys)) {
            char c = (shifted ? hid_keys_shift : hid_keys)[u - 4];
            if (ctrl && c >= 'a' && c <= 'z') {
                c &= 0x1F; // control characters, as the PS/2 path does
            }
            if (c != 0) {
                keyboard_inject(c);
            }
        }
    }
    memcpy(hid_kbd.last, r, 8);
}

// Consume an asynchronous transfer event if it belongs to a HID endpoint:
// handle the report and re-arm the endpoint. Returns false for events that
// belong to someone else (the synchronous waiters).
static bool hid_dispatch(const struct trb* ev)
{
    uint32_t slot = (ev->control >> 24) & 0xFF;
    uint32_t dci = (ev->control >> 16) & 0x1F;
    if (hid_kbd.present && slot == hid_kbd.slot && dci == hid_kbd.ep.dci) {
        hid_kbd_report(hid_kbd.report.va);
        hid_kbd.reports++;
        hid_post(&hid_kbd);
        return true;
    }
    return false;
}

// Configure the boot-protocol HID interface with `proto` (1=keyboard) on
// device `d` into `h`: find its interrupt IN endpoint, bring it up, select the
// configuration, switch the interface to the fixed boot report format, and arm
// the first report.
static bool hid_setup(usb_device* d, uint8_t proto, hid_dev* h)
{
    if (h->present) {
        return false; // already bound to an earlier device
    }
    dma_buf cfg_mem = dma_page_alloc();
    uint8_t* cfg = cfg_mem.va;
    if (!get_descriptor(d, (uint16_t)(DESC_CONFIG << 8), cfg_mem.pa, PAGE_SZ)) {
        dma_page_free(cfg_mem);
        return false;
    }
    uint16_t total = (uint16_t)(cfg[2] | (cfg[3] << 8));
    uint8_t config_value = cfg[5];
    if (total > PAGE_SZ) {
        total = PAGE_SZ;
    }
    bool in_hid = false, found = false;
    uint8_t iface = 0, ep_addr = 0, interval = 10;
    uint16_t mps = 8;
    for (uint32_t o = 0; o + 2 <= total;) {
        uint8_t blen = cfg[o], btype = cfg[o + 1];
        if (blen < 2) {
            break;
        }
        if (btype == DESC_INTERFACE) {
            in_hid = cfg[o + 5] == USB_CLASS_HID &&
                     cfg[o + 6] == HID_SUBCLASS_BOOT && cfg[o + 7] == proto;
            if (in_hid) {
                iface = cfg[o + 2];
            }
        } else if (btype == DESC_ENDPOINT && in_hid && !found) {
            if ((cfg[o + 3] & 0x3) == EP_ATTR_INT && (cfg[o + 2] & 0x80)) {
                ep_addr = cfg[o + 2];
                mps = (uint16_t)(cfg[o + 4] | (cfg[o + 5] << 8));
                interval = cfg[o + 6];
                found = true;
            }
        }
        o += blen;
    }
    dma_page_free(cfg_mem);
    if (!found) {
        return false; // no boot-protocol interface with this protocol
    }

    h->ep.dci = (uint32_t)((ep_addr & 0x0F) * 2 + 1);
    h->ep.mps = mps;
    ring_init(&h->ep.r);

    // xHCI interval: LS/FS express bInterval in ms (field = 2^(v-3) ms >= it);
    // HS/SS in 2^(bInterval-1) microframes (field = bInterval - 1).
    uint32_t ival;
    if (d->speed == 3 || d->speed == 4) {
        ival = interval > 0 ? (uint32_t)interval - 1 : 0;
    } else {
        ival = 3;
        while ((1u << (ival - 3)) < interval && ival < 10) {
            ival++;
        }
    }

    dma_buf input_mem = dma_page_alloc();
    uint32_t* input = input_mem.va;
    uint32_t dwpc = ctx_sz / 4;
    input[1] = 1u | (1u << h->ep.dci); // A0 (slot) + the endpoint
    uint32_t* slot_ctx = input + dwpc;
    slot_ctx[0] = d->route | (d->speed << 20) | (h->ep.dci << 27);
    slot_ctx[1] = (d->root_port << 16);
    uint32_t* ec = input + (h->ep.dci + 1) * dwpc;
    ec[0] = ival << 16;
    ec[1] = EP_TYPE_INT_IN | (3u << 1) | ((uint32_t)mps << 16);
    ec[2] = (uint32_t)(h->ep.r.pa | 1);
    ec[3] = (uint32_t)(h->ep.r.pa >> 32);
    ec[4] = mps;

    struct trb ev;
    bool ok = run_command(input_mem.pa,
                          TRB_TYPE(TRB_CONFIGURE_ENDPOINT) | (d->slot << 24),
                          &ev) &&
              COMPLETION_CODE(ev.status) == CC_SUCCESS;
    dma_page_free(input_mem);
    if (!ok) {
        return false;
    }
    if (!control_no_data(d, (uint64_t)0x00 | ((uint64_t)0x09 << 8) |
                                    ((uint64_t)config_value << 16))) {
        return false;
    }
    // SET_PROTOCOL(boot) then SET_IDLE(0): the fixed boot report format, sent
    // only when the state changes (which suits a poll-and-repost consumer).
    control_no_data(d, (uint64_t)0x21 | ((uint64_t)0x0B << 8) |
                               ((uint64_t)iface << 32));
    control_no_data(d, (uint64_t)0x21 | ((uint64_t)0x0A << 8) |
                               ((uint64_t)iface << 32));

    h->slot = d->slot;
    h->report = dma_page_alloc();
    h->present = true;
    hid_post(h);
    return true;
}

static void enumerate_device(uint32_t root_port, uint32_t speed, uint32_t route,
                             uint32_t depth);

static void delay_ms(uint64_t ms)
{
    uint64_t end = ktime_ms() + ms;
    while (ktime_ms() < end) {
        __asm__ __volatile__("pause");
    }
}

// Read a hub port's 32-bit status word (low 16 status, high 16 change bits)
// into `scratch`. Returns 0 on failure (reads as disconnected).
static uint32_t hub_port_status(usb_device* d, uint32_t port, dma_buf scratch)
{
    // GET_STATUS, class, port recipient: bmRequestType 0xA3.
    if (!control_in(d,
                    (uint64_t)0xA3 | ((uint64_t)port << 32) |
                            ((uint64_t)4 << 48),
                    scratch.pa, 4)) {
        return 0;
    }
    uint32_t st;
    memcpy(&st, scratch.va, 4);
    return st;
}

// SET_FEATURE (set=true) or CLEAR_FEATURE on a hub port (class request).
static bool hub_port_feature(usb_device* d, uint32_t port, uint32_t feature,
                             bool set)
{
    return control_no_data(
            d, (uint64_t)0x23 | ((uint64_t)(set ? 0x03 : 0x01) << 8) |
                       ((uint64_t)feature << 16) | ((uint64_t)port << 32));
}

// Hub port features (USB 2.0 11.24.2).
#define PORT_RESET 4
#define PORT_POWER 8
#define C_PORT_RESET 20
#define PS_CONNECTED (1u << 0)
#define PS_ENABLED (1u << 1)
#define PS_LOW_SPEED (1u << 9)
#define PS_HIGH_SPEED (1u << 10)
#define PS_C_RESET (1u << 20)

// Bring up a hub (class 9) and enumerate the devices behind it: select its
// configuration, power every port, then per connected port reset it (via hub
// class requests — the hub performs the signalling) and enumerate the child
// with the route string extended by this hop's port number.
static void hub_setup(usb_device* d)
{
    dma_buf scratch = dma_page_alloc();
    uint8_t* buf = scratch.va;
    if (!get_descriptor(d, (uint16_t)(DESC_CONFIG << 8), scratch.pa, 9)) {
        dma_page_free(scratch);
        return;
    }
    uint8_t config_value = buf[5];
    if (!control_no_data(d, (uint64_t)0x00 | ((uint64_t)0x09 << 8) |
                                    ((uint64_t)config_value << 16))) {
        dma_page_free(scratch);
        return;
    }

    // Hub descriptor (class type 0x29): bNbrPorts at byte 2. One route nibble
    // per hop caps a hub at 15 addressable ports.
    if (!control_in(d,
                    (uint64_t)0xA0 | ((uint64_t)0x06 << 8) |
                            ((uint64_t)0x2900 << 16) | ((uint64_t)9 << 48),
                    scratch.pa, 9)) {
        dma_page_free(scratch);
        return;
    }
    uint32_t nports = buf[2] < 15 ? buf[2] : 15;

    for (uint32_t p = 1; p <= nports; p++) {
        hub_port_feature(d, p, PORT_POWER, true);
    }
    delay_ms(100); // power-good time

    for (uint32_t p = 1; p <= nports; p++) {
        uint32_t st = hub_port_status(d, p, scratch);
        if (!(st & PS_CONNECTED)) {
            continue;
        }
        hub_port_feature(d, p, PORT_RESET, true);
        uint64_t dl = ktime_ms() + 500;
        do {
            st = hub_port_status(d, p, scratch);
        } while (!(st & PS_C_RESET) && ktime_ms() < dl);
        hub_port_feature(d, p, C_PORT_RESET, false);
        st = hub_port_status(d, p, scratch);
        if (!(st & PS_ENABLED)) {
            continue;
        }
        uint32_t speed = (st & PS_LOW_SPEED) ? 2 : (st & PS_HIGH_SPEED) ? 3 : 1;
        enumerate_device(d->root_port, speed, d->route | (p << (4 * d->depth)),
                         d->depth + 1);
    }
    dma_page_free(scratch);
}

// Address the device at (root_port, route) and read its descriptors; then hand
// it to its class driver — hubs recurse, the first BOT storage device becomes
// the msc function. FS devices get the textbook EP0 dance: address with the
// default max packet size, read the descriptor's first 8 bytes, and
// EVALUATE_CONTEXT the real bMaxPacketSize0 before reading the rest.
static void enumerate_device(uint32_t root_port, uint32_t speed, uint32_t route,
                             uint32_t depth)
{
    if (n_devices >= MAX_DEVICES) {
        return;
    }
    struct trb ev;
    if (!run_command(0, TRB_TYPE(TRB_ENABLE_SLOT), &ev) ||
        COMPLETION_CODE(ev.status) != CC_SUCCESS) {
        fail_reason = "ENABLE_SLOT failed";
        return;
    }
    usb_device* d = &devices[n_devices];
    memset(d, 0, sizeof *d);
    d->slot = (ev.control >> 24) & 0xFF;
    d->speed = speed;
    d->root_port = root_port;
    d->route = route;
    d->depth = depth;

    // Output device context (controller-written); publish it in the DCBAA.
    dcbaa[d->slot] = dma_page_alloc().pa;

    // Input context: enable the slot + EP0, describe the topology and the
    // control endpoint at the speed-default max packet size.
    dma_buf input_mem = dma_page_alloc();
    uint32_t* input = input_mem.va;
    uint32_t dwpc = ctx_sz / 4;           // dwords per context entry
    input[1] = 0x3;                       // add flags: A0 (slot) + A1 (EP0)
    uint32_t* slot_ctx = input + dwpc;    // slot context (index 1)
    uint32_t* ep0_ctx = input + 2 * dwpc; // EP0 context (index 2)
    slot_ctx[0] = route | (speed << 20) | (1u << 27);
    slot_ctx[1] = (root_port << 16);

    ring_init(&d->ep0);
    uint16_t mps = ep0_mps(speed);
    ep0_ctx[1] = EP_TYPE_CONTROL | (3u << 1) | ((uint32_t)mps << 16); // +CErr=3
    ep0_ctx[2] = (uint32_t)(d->ep0.pa | 1); // TR dequeue pointer low | DCS
    ep0_ctx[3] = (uint32_t)(d->ep0.pa >> 32);
    ep0_ctx[4] = 8; // average TRB length

    bool addressed =
            run_command(input_mem.pa,
                        TRB_TYPE(TRB_ADDRESS_DEVICE) | (d->slot << 24), &ev) &&
            COMPLETION_CODE(ev.status) == CC_SUCCESS;
    if (!addressed) {
        fail_reason = "ADDRESS_DEVICE failed";
        dma_page_free(input_mem);
        return;
    }

    // First 8 bytes of the device descriptor carry bMaxPacketSize0 (byte 7).
    // Only full-speed varies (8/16/32/64); LS/HS/SS are fixed by spec.
    dma_buf desc = dma_page_alloc();
    uint8_t* buf = desc.va;
    if (!get_descriptor(d, (uint16_t)(DESC_DEVICE << 8), desc.pa, 8)) {
        fail_reason = "GET_DESCRIPTOR failed";
        goto out;
    }
    if (speed == 1 && buf[7] >= 8 && buf[7] != mps) {
        memset(input, 0, PAGE_SZ);
        input[1] = 0x2; // add flag: A1 (EP0) — only its max packet size counts
        uint32_t* e0 = input + 2 * dwpc;
        e0[1] = EP_TYPE_CONTROL | (3u << 1) | ((uint32_t)buf[7] << 16);
        run_command(input_mem.pa,
                    TRB_TYPE(TRB_EVALUATE_CONTEXT) | (d->slot << 24), &ev);
    }

    if (!get_descriptor(d, (uint16_t)(DESC_DEVICE << 8), desc.pa, 18)) {
        fail_reason = "GET_DESCRIPTOR failed";
        goto out;
    }
    d->usb_class = buf[4];
    d->vid = (uint16_t)(buf[8] | (buf[9] << 8));
    d->pid = (uint16_t)(buf[10] | (buf[11] << 8));
    d->used = true;
    n_devices++;

    if (d->usb_class == USB_CLASS_HUB) {
        hub_setup(d);
    } else {
        // Class functions live in the interfaces; each probe binds its first
        // match and no-ops otherwise.
        msc_setup(d);
        hid_setup(d, HID_PROTO_KBD, &hid_kbd);
    }

out:
    dma_page_free(input_mem);
    dma_page_free(desc);
}

// Reset every connected root port and enumerate what answers (devices behind
// hubs are reached recursively via hub_setup).
static void enumerate_root_ports(void)
{
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
            enumerate_device(p, PORTSC_SPEED(sc), 0, 0);
        }
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
    uint32_t slots = hcs1 & 0xFF;
    g_ports = (hcs1 >> 24) & 0xFF;
    ctx_sz = (r32(cap, CAP_HCCPARAMS1) & (1u << 2)) ? 64 : 32;

    // Wait for ready, then halt and reset the controller.
    uint64_t deadline = ktime_ms() + 1000;
    while (r32(op, OP_USBSTS) & USBSTS_CNR) {
        if (ktime_ms() > deadline) {
            fail_reason = "controller-not-ready timeout";
            return;
        }
    }
    w32(op, OP_USBCMD, r32(op, OP_USBCMD) & ~USBCMD_RUN);
    deadline = ktime_ms() + 1000;
    while (!(r32(op, OP_USBSTS) & USBSTS_HCH)) {
        if (ktime_ms() > deadline) {
            fail_reason = "halt timeout";
            return;
        }
    }
    w32(op, OP_USBCMD, USBCMD_HCRST);
    deadline = ktime_ms() + 1000;
    while ((r32(op, OP_USBCMD) & USBCMD_HCRST) ||
           (r32(op, OP_USBSTS) & USBSTS_CNR)) {
        if (ktime_ms() > deadline) {
            fail_reason = "reset timeout";
            return;
        }
    }

    // Advertise all device slots and hand the controller its context array.
    w32(op, OP_CONFIG, slots);
    dma_buf dcbaa_mem = dma_page_alloc();
    dcbaa = dcbaa_mem.va;
    w64(op, OP_DCBAAP, dcbaa_mem.pa);

    // Command ring.
    ring_init(&cmd);
    w64(op, OP_CRCR, cmd.pa | CRCR_RCS);

    // Event ring: one segment described by a one-entry ERST. Program size and
    // dequeue pointer before the base address (which arms the interrupter).
    dma_buf erst_mem = dma_page_alloc();
    struct erst_entry* erst = erst_mem.va;
    dma_buf evt_mem = dma_page_alloc();
    evt.trb = evt_mem.va;
    evt.pa = evt_mem.pa;
    evt.deq = 0;
    evt.cycle = 1;
    erst[0].base = evt.pa;
    erst[0].size = RING_TRBS;
    w32(rt, IR0 + IR_ERSTSZ, 1);
    w64(rt, IR0 + IR_ERDP, evt.pa | ERDP_EHB);
    w64(rt, IR0 + IR_ERSTBA, erst_mem.pa);

    // MSI-X: deliver interrupter-0 events as interrupts so event waits can
    // sleep on hlt; without it the waits fall back to polling.
    evt_irq.enabled = pci_msix_setup(a, XHCI_VECTOR);
    if (evt_irq.enabled) {
        register_interrupt_handler(XHCI_VECTOR, xhci_irq);
        w32(rt, IR0 + IR_IMAN, IMAN_IP | IMAN_IE); // enable interrupter 0
    }

    // Run, then confirm the ring machinery with a NO-OP command round-trip.
    w32(op, OP_USBCMD, USBCMD_RUN | (evt_irq.enabled ? USBCMD_INTE : 0));
    deadline = ktime_ms() + 1000;
    while (r32(op, OP_USBSTS) & USBSTS_HCH) {
        if (ktime_ms() > deadline) {
            fail_reason = "run timeout";
            goto fail;
        }
    }
    struct trb ev;
    if (!run_command(0, TRB_TYPE(TRB_NO_OP_CMD), &ev) ||
        COMPLETION_CODE(ev.status) != CC_SUCCESS) {
        fail_reason = "NO-OP command failed";
        goto fail;
    }
    g_present = true;

    enumerate_root_ports(); // every root port, recursing through hubs
    return;

fail:
    // Halt the controller (so it stops referencing the rings/DCBAA) before
    // returning their frames — a failed init must not leak or leave live DMA.
    w32(op, OP_USBCMD, r32(op, OP_USBCMD) & ~USBCMD_RUN);
    deadline = ktime_ms() + 1000;
    while (!(r32(op, OP_USBSTS) & USBSTS_HCH) && ktime_ms() < deadline) {
    }
    dma_page_free(dcbaa_mem);
    dma_page_free(erst_mem);
    dma_page_free(evt_mem);
    frame_free(cmd.pa);
}

bool xhci_present(void)
{
    return g_present;
}
uint32_t xhci_ports(void)
{
    return g_present ? g_ports : 0;
}
uint32_t xhci_device_count(void)
{
    return n_devices;
}
bool xhci_device_info(uint32_t i, uint16_t* vid, uint16_t* pid,
                      uint8_t* usb_class)
{
    if (i >= n_devices || !devices[i].used) {
        return false;
    }
    *vid = devices[i].vid;
    *pid = devices[i].pid;
    *usb_class = devices[i].usb_class;
    return true;
}
bool xhci_msc_ready(void)
{
    return msc.ready;
}
uint64_t xhci_msc_blocks(void)
{
    return msc.blocks;
}
uint32_t xhci_msc_block_size(void)
{
    return msc.block_size;
}
const char* xhci_fail_reason(void)
{
    return fail_reason;
}
bool xhci_irq_driven(void)
{
    return evt_irq.enabled;
}
uint64_t xhci_irq_count(void)
{
    return evt_irq.count;
}

// Non-blocking event drain, called from the kernel's idle loops: HID report
// completions are handled (and their endpoints re-armed); anything else
// pending outside a synchronous wait is noise (e.g. port changes) and is
// discarded.
void xhci_poll(void)
{
    if (!g_present) {
        return;
    }
    struct trb ev;
    while (next_event(&ev, 0)) {
        if (TRB_TYPE_OF(ev.control) == TRB_TRANSFER_EVENT) {
            hid_dispatch(&ev);
        }
    }
}

bool xhci_kbd_present(void)
{
    return hid_kbd.present;
}
uint64_t xhci_kbd_reports(void)
{
    return hid_kbd.reports;
}
