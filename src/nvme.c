// Minimal NVMe driver (see nvme.h). Brings up the first NVMe controller on PCI,
// creates an admin and one I/O queue pair, IDENTIFYs namespace 1, and reads
// logical blocks. Everything the controller DMAs to/from lives in fresh
// physical frames reached through the HHDM; caller buffers are serviced through
// a one-page bounce buffer, so they need not be physically contiguous.
//
// I/O completions are delivered by MSI-X — the controller writes the LAPIC
// message address programmed into its vector table, raising an interrupt on a
// free vector; the handler drains the I/O completion queue and wakes the reader
// (which naps on hlt instead of busy-polling). Admin commands (init-time only)
// stay polled, and the whole driver falls back to polling if the controller
// advertises no MSI-X.
//
// Reads and writes are both supported; nvme_blockdev() exposes the namespace as
// a generic blockdev so ext2 can mount on NVMe (see blockdev.h). BSP-only, like
// the other drivers.

#include <nvme.h>
#include <pci.h>
#include <paging.h>
#include <frames.h>
#include <dma.h>
#include <mmio.h> // register access + doorbell (dma_wmb) helpers
#include <ktime.h>
#include <apic.h> // lapic_id for the MSI-X message address
#include <idt.h>  // register_interrupt_handler, interrupt_frame
#include <utils.h>

// PCI class code for an NVMe controller: mass storage / NVM / NVMe.
#define PCI_CLASS_STORAGE 0x01
#define PCI_SUBCLASS_NVM 0x08
#define PCI_PROGIF_NVME 0x02

// A free IDT vector (32-47 have stubs) for MSI-X completion delivery.
#define NVME_VECTOR 45

// Controller register offsets (bytes into BAR0).
#define REG_CAP 0x00  // capabilities (64-bit)
#define REG_VS 0x08   // version
#define REG_CC 0x14   // controller configuration
#define REG_CSTS 0x1C // controller status
#define REG_AQA 0x24  // admin queue attributes
#define REG_ASQ 0x28  // admin submission queue base (64-bit)
#define REG_ACQ 0x30  // admin completion queue base (64-bit)
#define REG_DOORBELL 0x1000

#define CC_EN (1u << 0)      // enable
#define CC_IOSQES (6u << 16) // I/O SQ entry size = 2^6 = 64 bytes
#define CC_IOCQES (4u << 20) // I/O CQ entry size = 2^4 = 16 bytes
#define CSTS_RDY (1u << 0)   // ready
#define CSTS_CFS (1u << 1)   // controller fatal status

// Admin opcodes.
#define ADMIN_CREATE_IO_SQ 0x01
#define ADMIN_CREATE_IO_CQ 0x05
#define ADMIN_IDENTIFY 0x06
// I/O opcodes.
#define IO_WRITE 0x01
#define IO_READ 0x02

#define CNS_NAMESPACE 0x00 // IDENTIFY: this namespace
#define CNS_CONTROLLER 0x01

#define IO_QID 1        // the single I/O queue pair we create
#define QUEUE_DEPTH 64u // entries per queue (SQ = 4 KiB, CQ = 1 KiB)

// A 64-byte NVMe submission-queue command (common format).
struct nvme_command {
    uint8_t opcode;
    uint8_t flags;
    uint16_t cid;
    uint32_t nsid;
    uint64_t rsvd2;
    uint64_t mptr;
    uint64_t prp1;
    uint64_t prp2;
    uint32_t cdw10;
    uint32_t cdw11;
    uint32_t cdw12;
    uint32_t cdw13;
    uint32_t cdw14;
    uint32_t cdw15;
} __attribute__((packed));

// A 16-byte completion-queue entry. `status` holds the phase bit (bit 0) then
// the status code / type; a non-zero status>>1 is an error.
struct nvme_completion {
    uint32_t result;
    uint32_t rsvd;
    uint16_t sq_head;
    uint16_t sq_id;
    uint16_t cid;
    uint16_t status;
} __attribute__((packed));

// IDENTIFY CONTROLLER: only the fields through MDTS are laid out (the buffer
// is a full 4 KiB page; the rest is unused here).
struct nvme_id_ctrl {
    uint16_t vid;
    uint16_t ssvid;
    char sn[20];
    char mn[40]; // model number, space-padded ASCII
    char fr[8];
    uint8_t rab;
    uint8_t ieee[3];
    uint8_t cmic;
    uint8_t mdts; // max transfer = 2^mdts pages (0 = no limit)
} __attribute__((packed));

// IDENTIFY NAMESPACE: size, the in-use LBA format index, and the format table.
struct nvme_id_ns {
    uint64_t nsze; // total logical blocks
    uint64_t ncap;
    uint64_t nuse;
    uint8_t nsfeat;
    uint8_t nlbaf;
    uint8_t flbas; // bits 3:0 = current LBA format index
    uint8_t mc;
    uint8_t dpc;
    uint8_t dps;
    uint8_t rsvd30[98];
    uint32_t lbaf[16]; // each: bits 23:16 = LBA data size as a power of two
} __attribute__((packed));

// A submission/completion queue pair driven synchronously (one command in
// flight at a time), addressed through its two doorbell registers. The ring
// physical addresses live here too, so creating the queue needs no side
// channel to hand them to the controller.
struct nvme_queue {
    struct nvme_command* sq;             // submission ring (in a DMA frame)
    volatile struct nvme_completion* cq; // completion ring (in a DMA frame)
    uintptr_t sq_pa, cq_pa;              // ring physical addresses
    volatile uint32_t* sq_tail_db;       // submission-queue tail doorbell
    volatile uint32_t* cq_head_db;       // completion-queue head doorbell
    uint16_t depth;
    uint16_t sq_tail;
    uint16_t cq_head;
    uint16_t cid;
    uint8_t phase; // completion phase we currently expect (starts at 1)
};

static volatile uint8_t* regs; // iomap'd BAR0
static uint32_t db_stride;     // doorbell stride in bytes (from CAP.DSTRD)

// Which init step failed, for the boot report (NULL: no controller, or fine).
// Silent init failure is cheap in QEMU but expensive on real hardware.
static const char* fail_reason;

// The namespace this driver serves (namespace 1 of the first controller).
static struct {
    bool present;
    uint64_t blocks;
    uint32_t block_size;
    uint32_t max_cmd_blocks; // per-command cap (PRP list size + MDTS)
    char model[41];
} ns;

static struct nvme_queue admin_q;
static struct nvme_queue io_q;

static struct dma_buf bounce; // one-page staging buffer (fallback path)
static struct dma_buf
        prp_list; // one page of PRP entries for multi-page transfers

// MSI-X interrupt state. When the controller advertises MSI-X, I/O completions
// arrive as interrupts instead of being polled: the ISR drains the I/O CQ and
// signals the reader. `done`/`status` describe the single command in flight —
// the synchronous submit path is what makes one flag sufficient.
static struct {
    bool enabled;                  // MSI-X up; false = polled fallback
    volatile bool done;            // set by the ISR when the command posts
    volatile int status;           // that completion's status code
    volatile uint64_t completions; // total completions the ISR has handled
} io_irq;

static uint32_t reg32(uint32_t off)
{
    return mmio_r32(regs, off);
}
static void wreg32(uint32_t off, uint32_t v)
{
    mmio_w32(regs, off, v);
}
static uint64_t reg64(uint32_t off)
{
    return mmio_r64(regs, off);
}
static void wreg64(uint32_t off, uint64_t v)
{
    mmio_w64(regs, off, v);
}

// Doorbell register for queue `qid`: submission (is_cq=0) or completion (=1).
static volatile uint32_t* doorbell(uint32_t qid, int is_cq)
{
    uint32_t off = REG_DOORBELL + (2 * qid + (uint32_t)is_cq) * db_stride;
    return (volatile uint32_t*)(regs + off);
}

// Submit `cmd` on `q` and busy-wait (bounded) for its completion. Returns the
// status code (0 = success) or -1 on timeout.
static int submit(struct nvme_queue* q, struct nvme_command* cmd)
{
    cmd->cid = q->cid++;
    q->sq[q->sq_tail] = *cmd;
    q->sq_tail = (uint16_t)((q->sq_tail + 1) % q->depth);
    // Hand the entry to the controller: the SQ writes must be visible before
    // the doorbell store (mmio_doorbell orders them).
    mmio_doorbell32(q->sq_tail_db, q->sq_tail);

    volatile struct nvme_completion* c = &q->cq[q->cq_head];
    uint64_t deadline = ktime_ms() + 5000;
    while ((c->status & 1) != q->phase) {
        if (ktime_ms() > deadline || (reg32(REG_CSTS) & CSTS_CFS)) {
            return -1;
        }
        cpu_relax();
    }
    uint16_t status = (uint16_t)(c->status >> 1); // strip the phase bit
    q->cq_head = (uint16_t)((q->cq_head + 1) % q->depth);
    if (q->cq_head == 0) {
        q->phase ^= 1; // wrapped: the controller flips phase
    }
    *q->cq_head_db = q->cq_head;
    return status & 0x3FF; // SC | SCT
}

// MSI-X completion handler: drain every pending I/O completion, ring the CQ
// head doorbell, and signal the waiting reader. Runs with interrupts off;
// interrupt_dispatch EOIs the LAPIC after it returns (NVME_VECTOR is 32-47).
static void nvme_irq(struct interrupt_frame* f)
{
    (void)f;
    volatile struct nvme_completion* c = &io_q.cq[io_q.cq_head];
    while ((c->status & 1) == io_q.phase) {
        io_irq.status = (c->status >> 1) & 0x3FF;
        io_q.cq_head = (uint16_t)((io_q.cq_head + 1) % io_q.depth);
        if (io_q.cq_head == 0) {
            io_q.phase ^= 1;
        }
        *io_q.cq_head_db = io_q.cq_head;
        io_irq.completions++;
        io_irq.done = true;
        c = &io_q.cq[io_q.cq_head];
    }
}

// Submit one command on the I/O queue and block until nvme_irq signals it
// (bounded). The io_done check runs with interrupts masked and re-enables via
// `sti; hlt` — the STI shadow makes enable-and-halt atomic and a pending
// completion wakes hlt immediately, so the wakeup cannot be lost between the
// check and the sleep. Returns the status or -1.
static int submit_io_irq(struct nvme_command* cmd)
{
    cmd->cid = io_q.cid++;
    io_q.sq[io_q.sq_tail] = *cmd;
    io_q.sq_tail = (uint16_t)((io_q.sq_tail + 1) % io_q.depth);
    io_irq.done = false;
    io_irq.status = -1;
    // Hand the entry over: the SQ entry + flag stores must precede the
    // doorbell.
    mmio_doorbell32(io_q.sq_tail_db, io_q.sq_tail);

    uint64_t deadline = ktime_ms() + 5000;
    __asm__ __volatile__("cli");
    while (!io_irq.done) {
        if (ktime_ms() > deadline || (reg32(REG_CSTS) & CSTS_CFS)) {
            __asm__ __volatile__("sti");
            return -1;
        }
        __asm__ __volatile__("sti; hlt; cli");
    }
    __asm__ __volatile__("sti");
    return io_irq.status;
}

// One command on the I/O queue: interrupt-driven when MSI-X is up, else polled.
static int submit_io(struct nvme_command* cmd)
{
    return io_irq.enabled ? submit_io_irq(cmd) : submit(&io_q, cmd);
}

// Enable MSI-X delivery of I/O completions to NVME_VECTOR on this core.
// Returns false (polled fallback) if the controller advertises no MSI-X.
static bool msix_setup(struct pci_addr a)
{
    if (!pci_msix_setup(a, NVME_VECTOR)) {
        return false;
    }
    register_interrupt_handler(NVME_VECTOR, nvme_irq);
    return true;
}

// Wait up to ~5 s for CSTS.RDY to reach `want`. Returns false on timeout.
static bool wait_ready(uint32_t want)
{
    uint64_t deadline = ktime_ms() + 5000;
    while ((reg32(REG_CSTS) & CSTS_RDY) != want) {
        if (ktime_ms() > deadline || (reg32(REG_CSTS) & CSTS_CFS)) {
            return false;
        }
        cpu_relax();
    }
    return true;
}

// Point `q` at freshly allocated SQ/CQ frames and its doorbells.
static void queue_init(struct nvme_queue* q, uint32_t qid)
{
    struct dma_buf sq = dma_page_alloc();
    struct dma_buf cq = dma_page_alloc();
    q->sq = sq.va;
    q->sq_pa = sq.pa;
    q->cq = cq.va;
    q->cq_pa = cq.pa;
    q->sq_tail_db = doorbell(qid, 0);
    q->cq_head_db = doorbell(qid, 1);
    q->depth = QUEUE_DEPTH;
    q->sq_tail = 0;
    q->cq_head = 0;
    q->cid = 0;
    q->phase = 1;
}

// Run IDENTIFY (`cns`, `nsid`) into a DMA page; returns the page ({NULL, 0} on
// failure). The caller frees it with dma_page_free() when done.
static struct dma_buf identify(uint32_t cns, uint32_t nsid)
{
    struct dma_buf buf = dma_page_alloc();
    struct nvme_command cmd = {0};
    cmd.opcode = ADMIN_IDENTIFY;
    cmd.nsid = nsid;
    cmd.prp1 = buf.pa;
    cmd.cdw10 = cns;
    if (submit(&admin_q, &cmd) != 0) {
        dma_page_free(buf);
        return (struct dma_buf){0};
    }
    return buf;
}

// Create the I/O completion then submission queue (order matters: the SQ names
// its CQ). Returns false on any controller error.
static bool create_io_queues(void)
{
    struct nvme_command cq = {0};
    cq.opcode = ADMIN_CREATE_IO_CQ;
    cq.prp1 = io_q.cq_pa;
    cq.cdw10 = ((QUEUE_DEPTH - 1) << 16) | IO_QID;
    // PC (bit 0). With MSI-X up, also enable interrupts (IEN bit 1) and point
    // at vector table entry 0 (IV, bits 31:16); otherwise the CQ is polled.
    cq.cdw11 = io_irq.enabled ? 0x3u : 0x1u;
    if (submit(&admin_q, &cq) != 0) {
        return false;
    }

    struct nvme_command sq = {0};
    sq.opcode = ADMIN_CREATE_IO_SQ;
    sq.prp1 = io_q.sq_pa;
    sq.cdw10 = ((QUEUE_DEPTH - 1) << 16) | IO_QID;
    sq.cdw11 = (IO_QID << 16) | 1; // posts completions to CQ IO_QID; PC
    return submit(&admin_q, &sq) == 0;
}

void nvme_init(void)
{
    struct pci_addr a = pci_find_class(PCI_CLASS_STORAGE, PCI_SUBCLASS_NVM,
                                       PCI_PROGIF_NVME);
    if (!a.found) {
        return;
    }
    pci_enable_bus_master(a);
    regs = iomap(pci_bar64(a, 0), 0x2000, PAGEF_P | PAGEF_RW | PAGEF_UC);

    uint64_t cap = reg64(REG_CAP);
    db_stride = 4u << ((cap >> 32) & 0xF); // CAP.DSTRD
    uint32_t mqes = (uint32_t)(cap & 0xFFFF) + 1;
    if (QUEUE_DEPTH > mqes) {
        fail_reason = "queue depth over CAP.MQES";
        return;
    }

    // Reset: clear enable and wait for the controller to go not-ready.
    wreg32(REG_CC, reg32(REG_CC) & ~CC_EN);
    if (!wait_ready(0)) {
        fail_reason = "reset timeout";
        return;
    }

    // Admin queue pair, then point the controller at it and re-enable.
    queue_init(&admin_q, 0);
    wreg32(REG_AQA, ((QUEUE_DEPTH - 1) << 16) | (QUEUE_DEPTH - 1));
    wreg64(REG_ASQ, admin_q.sq_pa);
    wreg64(REG_ACQ, admin_q.cq_pa);
    wreg32(REG_CC, CC_IOSQES | CC_IOCQES | CC_EN); // MPS=0 (4 KiB), NVM cmd set
    if (!wait_ready(CSTS_RDY)) {
        fail_reason = "enable timeout";
        goto fail;
    }

    // IDENTIFY the controller for the model string and its transfer limit.
    uint8_t mdts;
    {
        struct dma_buf id = identify(CNS_CONTROLLER, 0);
        if (id.va == NULL) {
            fail_reason = "IDENTIFY controller failed";
            goto fail;
        }
        struct nvme_id_ctrl* ctrl = id.va;
        memcpy(ns.model, ctrl->mn, 40);
        ns.model[40] = '\0';
        for (int i = 39; i >= 0 && ns.model[i] == ' '; i--) {
            ns.model[i] = '\0'; // trim the space padding
        }
        mdts = ctrl->mdts;
        dma_page_free(id);
    }

    // IDENTIFY namespace 1 for its size and block size.
    {
        struct dma_buf id = identify(CNS_NAMESPACE, 1);
        if (id.va == NULL) {
            fail_reason = "IDENTIFY namespace failed";
            goto fail;
        }
        struct nvme_id_ns* idns = id.va;
        ns.blocks = idns->nsze;
        uint32_t fmt = idns->lbaf[idns->flbas & 0xF];
        ns.block_size = 1u << ((fmt >> 16) & 0xFF);
        dma_page_free(id);
    }
    if (ns.block_size == 0 || ns.block_size > PAGE_SZ) {
        fail_reason = "unsupported LBA format";
        goto fail;
    }

    // Per-command transfer cap: one PRP-list page covers a partial first page
    // plus 512 list entries (2 MiB), further limited by the controller's MDTS
    // (2^mdts pages; 0 = no limit).
    {
        uint32_t max_bytes = 512u * PAGE_SZ;
        if (mdts != 0 && mdts < 9) {
            max_bytes = (1u << mdts) * PAGE_SZ;
        }
        ns.max_cmd_blocks = max_bytes / ns.block_size;
    }

    // Set up MSI-X (if advertised) before creating the I/O CQ, so the CQ can be
    // created with interrupts enabled; otherwise the queue stays polled.
    io_irq.enabled = msix_setup(a);

    // The single I/O queue pair, the fallback bounce buffer, and the PRP list
    // used by multi-page direct transfers.
    queue_init(&io_q, IO_QID);
    if (!create_io_queues()) {
        fail_reason = "CREATE I/O queue failed";
        frame_free(io_q.sq_pa);
        frame_free(io_q.cq_pa);
        goto fail;
    }
    bounce = dma_page_alloc();
    prp_list = dma_page_alloc();
    ns.present = true;
    return;

fail:
    // Disable the controller (so it stops referencing the admin queues) before
    // returning their frames — a failed init must not leak or leave live DMA.
    wreg32(REG_CC, reg32(REG_CC) & ~CC_EN);
    wait_ready(0);
    frame_free(admin_q.sq_pa);
    frame_free(admin_q.cq_pa);
}

bool nvme_present(void)
{
    return ns.present;
}
uint64_t nvme_blocks(void)
{
    return ns.blocks;
}
uint32_t nvme_block_size(void)
{
    return ns.block_size;
}
const char* nvme_model(void)
{
    return ns.present ? ns.model : "";
}
bool nvme_irq_driven(void)
{
    return io_irq.enabled;
}
uint64_t nvme_irq_count(void)
{
    return io_irq.completions;
}
const char* nvme_fail_reason(void)
{
    return fail_reason;
}

// Point the command's data pointers straight at the caller's buffer: PRP1
// takes the first page (any offset), then either one more page in PRP2 or a
// list of page-aligned entries in the persistent PRP-list page. NVMe's PRP
// scheme is exactly a page-granular scatter list, so a virtually-contiguous
// kernel buffer needs no physical contiguity — each page is looked up
// individually. Returns false if any page is unmapped (caller falls back to
// the bounce buffer).
static bool build_prps(struct nvme_command* cmd, const uint8_t* va, size_t len)
{
    uintptr_t pa = physical_address(kernel_dir, (uintptr_t)va);
    if (pa == (uintptr_t)-1) {
        return false;
    }
    cmd->prp1 = pa;
    size_t first = PAGE_SZ - ((uintptr_t)va & (PAGE_SZ - 1));
    if (len <= first) {
        cmd->prp2 = 0;
        return true;
    }
    const uint8_t* p = va + first; // page-aligned remainder
    size_t pages = (len - first + PAGE_SZ - 1) / PAGE_SZ;
    if (pages == 1) {
        uintptr_t pa2 = physical_address(kernel_dir, (uintptr_t)p);
        if (pa2 == (uintptr_t)-1) {
            return false;
        }
        cmd->prp2 = pa2;
        return true;
    }
    uint64_t* list = prp_list.va;
    for (size_t i = 0; i < pages; i++) {
        uintptr_t e = physical_address(kernel_dir, (uintptr_t)p + i * PAGE_SZ);
        if (e == (uintptr_t)-1) {
            return false;
        }
        list[i] = e;
    }
    cmd->prp2 = prp_list.pa;
    return true;
}

// Shared read/write engine. The fast path DMAs directly to/from the caller's
// buffer via PRPs (up to ns.max_cmd_blocks per command); if a page of the
// buffer is not kernel-mapped, that chunk is staged through the one-page
// bounce buffer instead.
static bool nvme_rw(uint64_t lba, uint32_t count, uint8_t* buf, bool write)
{
    if (!ns.present || count == 0) {
        return false;
    }
    while (count > 0) {
        uint32_t n = count < ns.max_cmd_blocks ? count : ns.max_cmd_blocks;
        struct nvme_command cmd = {0};
        cmd.opcode = write ? IO_WRITE : IO_READ;
        cmd.nsid = 1;
        bool direct = build_prps(&cmd, buf, (size_t)n * ns.block_size);
        if (!direct) {
            uint32_t per = PAGE_SZ / ns.block_size;
            n = n < per ? n : per;
            if (write) {
                memcpy(bounce.va, buf, (size_t)n * ns.block_size);
            }
            cmd.prp1 = bounce.pa;
            cmd.prp2 = 0;
        }
        cmd.cdw10 = (uint32_t)lba;
        cmd.cdw11 = (uint32_t)(lba >> 32);
        cmd.cdw12 = n - 1; // NLB is 0-based
        if (submit_io(&cmd) != 0) {
            return false;
        }
        if (!direct && !write) {
            memcpy(buf, bounce.va, (size_t)n * ns.block_size);
        }
        buf += (size_t)n * ns.block_size;
        lba += n;
        count -= n;
    }
    return true;
}

bool nvme_read(uint64_t lba, uint32_t count, void* buf)
{
    return nvme_rw(lba, count, buf, false);
}

bool nvme_write(uint64_t lba, uint32_t count, const void* buf)
{
    // The engine only reads from `buf` on the write path; the cast lets one
    // implementation serve both directions.
    return nvme_rw(lba, count, (uint8_t*)(uintptr_t)buf, true);
}

// The block-device view (blockdev.h): 512-byte sectors. Only namespaces with a
// 512-byte logical block map 1:1, so a differently-formatted namespace reports
// zero sectors (unusable as a block device) rather than corrupting addressing.
static uint64_t nvme_bd_sectors(void)
{
    return (ns.present && ns.block_size == 512) ? ns.blocks : 0;
}
static const struct blockdev nvme_dev = {nvme_read, nvme_write,
                                         nvme_bd_sectors};

const struct blockdev* nvme_blockdev(void)
{
    return &nvme_dev;
}
