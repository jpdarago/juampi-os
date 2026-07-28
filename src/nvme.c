// Minimal polled NVMe driver (see nvme.h). Brings up the first NVMe controller
// on PCI, creates an admin and one I/O queue pair, IDENTIFYs namespace 1, and
// reads blocks by polling the completion queue's phase bit — no interrupts, no
// writes. Everything the controller DMAs to/from lives in fresh physical frames
// reached through the HHDM; caller buffers are serviced through a one-page
// bounce buffer, so they need not be physically contiguous.
//
// This is milestone 1 of the NVMe work: prove the queue/doorbell/PRP mechanics
// in QEMU. MSI-X interrupts, writes, and wiring ext2 onto the block device come
// in later milestones. BSP-only, like the other drivers.

#include <nvme.h>
#include <pci.h>
#include <paging.h>
#include <frames.h>
#include <ktime.h>
#include <utils.h>

// PCI class code for an NVMe controller: mass storage / NVM / NVMe.
#define PCI_CLASS_STORAGE 0x01
#define PCI_SUBCLASS_NVM 0x08
#define PCI_PROGIF_NVME 0x02

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

// IDENTIFY CONTROLLER: only the leading fields up to the model string are laid
// out (the buffer is a full 4 KiB page; the rest is unused here).
struct nvme_id_ctrl {
    uint16_t vid;
    uint16_t ssvid;
    char sn[20];
    char mn[40]; // model number, space-padded ASCII
    char fr[8];
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
// flight at a time), addressed through its two doorbell registers.
typedef struct {
    struct nvme_command* sq;             // submission ring (in a DMA frame)
    volatile struct nvme_completion* cq; // completion ring (in a DMA frame)
    volatile uint32_t* sq_tail_db;       // submission-queue tail doorbell
    volatile uint32_t* cq_head_db;       // completion-queue head doorbell
    uint16_t depth;
    uint16_t sq_tail;
    uint16_t cq_head;
    uint16_t cid;
    uint8_t phase; // completion phase we currently expect (starts at 1)
} nvme_queue;

static volatile uint8_t* regs; // iomap'd BAR0
static uint32_t db_stride;     // doorbell stride in bytes (from CAP.DSTRD)
static bool g_present;
static uint64_t g_blocks;
static uint32_t g_block_size;
static char g_model[41];

static nvme_queue admin_q;
static nvme_queue io_q;

static uint8_t* bounce_va; // one-page DMA bounce buffer
static uintptr_t bounce_pa;

static uint32_t reg32(uint32_t off)
{
    return *(volatile uint32_t*)(regs + off);
}
static void wreg32(uint32_t off, uint32_t v)
{
    *(volatile uint32_t*)(regs + off) = v;
}
static uint64_t reg64(uint32_t off)
{
    return *(volatile uint64_t*)(regs + off);
}
static void wreg64(uint32_t off, uint64_t v)
{
    *(volatile uint64_t*)(regs + off) = v;
}

// Doorbell register for queue `qid`: submission (is_cq=0) or completion (=1).
static volatile uint32_t* doorbell(uint32_t qid, int is_cq)
{
    uint32_t off = REG_DOORBELL + (2 * qid + (uint32_t)is_cq) * db_stride;
    return (volatile uint32_t*)(regs + off);
}

// Allocate a zeroed DMA frame; returns its HHDM virtual pointer and physical
// address. Queues and identify buffers fit in a single page.
static void* dma_frame(uintptr_t* pa_out)
{
    uintptr_t pa = frame_alloc();
    void* va = phys_to_virt(pa);
    memset(va, 0, PAGE_SZ);
    *pa_out = pa;
    return va;
}

// Submit `cmd` on `q` and busy-wait (bounded) for its completion. Returns the
// status code (0 = success) or -1 on timeout.
static int submit(nvme_queue* q, struct nvme_command* cmd)
{
    cmd->cid = q->cid++;
    q->sq[q->sq_tail] = *cmd;
    q->sq_tail = (uint16_t)((q->sq_tail + 1) % q->depth);
    *q->sq_tail_db = q->sq_tail;

    volatile struct nvme_completion* c = &q->cq[q->cq_head];
    uint64_t deadline = ktime_ms() + 5000;
    while ((c->status & 1) != q->phase) {
        if (ktime_ms() > deadline || (reg32(REG_CSTS) & CSTS_CFS)) {
            return -1;
        }
        __asm__ __volatile__("pause");
    }
    uint16_t status = (uint16_t)(c->status >> 1); // strip the phase bit
    q->cq_head = (uint16_t)((q->cq_head + 1) % q->depth);
    if (q->cq_head == 0) {
        q->phase ^= 1; // wrapped: the controller flips phase
    }
    *q->cq_head_db = q->cq_head;
    return status & 0x3FF; // SC | SCT
}

// Wait up to ~5 s for CSTS.RDY to reach `want`. Returns false on timeout.
static bool wait_ready(uint32_t want)
{
    uint64_t deadline = ktime_ms() + 5000;
    while ((reg32(REG_CSTS) & CSTS_RDY) != want) {
        if (ktime_ms() > deadline || (reg32(REG_CSTS) & CSTS_CFS)) {
            return false;
        }
        __asm__ __volatile__("pause");
    }
    return true;
}

// Point `q` at freshly allocated SQ/CQ frames and its doorbells.
static void queue_init(nvme_queue* q, uint32_t qid, uintptr_t* sq_pa,
                       uintptr_t* cq_pa)
{
    q->sq = dma_frame(sq_pa);
    q->cq = dma_frame(cq_pa);
    q->sq_tail_db = doorbell(qid, 0);
    q->cq_head_db = doorbell(qid, 1);
    q->depth = QUEUE_DEPTH;
    q->sq_tail = 0;
    q->cq_head = 0;
    q->cid = 0;
    q->phase = 1;
}

// Run IDENTIFY (`cns`, `nsid`) into a DMA page; returns the page VA or NULL.
static void* identify(uint32_t cns, uint32_t nsid, uintptr_t* pa)
{
    void* buf = dma_frame(pa);
    struct nvme_command cmd = {0};
    cmd.opcode = ADMIN_IDENTIFY;
    cmd.nsid = nsid;
    cmd.prp1 = *pa;
    cmd.cdw10 = cns;
    if (submit(&admin_q, &cmd) != 0) {
        return NULL;
    }
    return buf;
}

// Create the I/O completion then submission queue (order matters: the SQ names
// its CQ). Returns false on any controller error.
static bool create_io_queues(uintptr_t sq_pa, uintptr_t cq_pa)
{
    struct nvme_command cq = {0};
    cq.opcode = ADMIN_CREATE_IO_CQ;
    cq.prp1 = cq_pa;
    cq.cdw10 = ((QUEUE_DEPTH - 1) << 16) | IO_QID;
    cq.cdw11 = 1; // PC (physically contiguous), interrupts disabled
    if (submit(&admin_q, &cq) != 0) {
        return false;
    }

    struct nvme_command sq = {0};
    sq.opcode = ADMIN_CREATE_IO_SQ;
    sq.prp1 = sq_pa;
    sq.cdw10 = ((QUEUE_DEPTH - 1) << 16) | IO_QID;
    sq.cdw11 = (IO_QID << 16) | 1; // posts completions to CQ IO_QID; PC
    return submit(&admin_q, &sq) == 0;
}

void nvme_init(void)
{
    pci_addr a = pci_find_class(PCI_CLASS_STORAGE, PCI_SUBCLASS_NVM,
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
        return; // controller can't hold our queue depth (won't happen in QEMU)
    }

    // Reset: clear enable and wait for the controller to go not-ready.
    wreg32(REG_CC, reg32(REG_CC) & ~CC_EN);
    if (!wait_ready(0)) {
        return;
    }

    // Admin queue pair, then point the controller at it and re-enable.
    uintptr_t asq_pa, acq_pa;
    queue_init(&admin_q, 0, &asq_pa, &acq_pa);
    wreg32(REG_AQA, ((QUEUE_DEPTH - 1) << 16) | (QUEUE_DEPTH - 1));
    wreg64(REG_ASQ, asq_pa);
    wreg64(REG_ACQ, acq_pa);
    wreg32(REG_CC, CC_IOSQES | CC_IOCQES | CC_EN); // MPS=0 (4 KiB), NVM cmd set
    if (!wait_ready(CSTS_RDY)) {
        return;
    }

    // IDENTIFY the controller for the model string.
    uintptr_t pa;
    struct nvme_id_ctrl* ctrl = identify(CNS_CONTROLLER, 0, &pa);
    if (ctrl == NULL) {
        return;
    }
    memcpy(g_model, ctrl->mn, 40);
    g_model[40] = '\0';
    for (int i = 39; i >= 0 && g_model[i] == ' '; i--) {
        g_model[i] = '\0'; // trim the space padding
    }
    frame_free(pa);

    // IDENTIFY namespace 1 for its size and block size.
    struct nvme_id_ns* ns = identify(CNS_NAMESPACE, 1, &pa);
    if (ns == NULL) {
        return;
    }
    g_blocks = ns->nsze;
    uint32_t fmt = ns->lbaf[ns->flbas & 0xF];
    g_block_size = 1u << ((fmt >> 16) & 0xFF);
    frame_free(pa);
    if (g_block_size == 0 || g_block_size > PAGE_SZ) {
        return; // unsupported format for the one-page bounce path
    }

    // The single I/O queue pair, plus the bounce buffer reads land in.
    uintptr_t iosq_pa, iocq_pa;
    queue_init(&io_q, IO_QID, &iosq_pa, &iocq_pa);
    if (!create_io_queues(iosq_pa, iocq_pa)) {
        return;
    }
    bounce_va = dma_frame(&bounce_pa);
    g_present = true;
}

bool nvme_present(void)
{
    return g_present;
}
uint64_t nvme_blocks(void)
{
    return g_blocks;
}
uint32_t nvme_block_size(void)
{
    return g_block_size;
}
const char* nvme_model(void)
{
    return g_present ? g_model : "";
}

bool nvme_read(uint64_t lba, uint32_t count, void* buf)
{
    if (!g_present || count == 0) {
        return false;
    }
    uint32_t per_chunk = PAGE_SZ / g_block_size; // blocks that fit the bounce
    uint8_t* out = buf;
    while (count > 0) {
        uint32_t n = count < per_chunk ? count : per_chunk;
        struct nvme_command cmd = {0};
        cmd.opcode = IO_READ;
        cmd.nsid = 1;
        cmd.prp1 = bounce_pa; // one page, page-aligned: PRP1 alone suffices
        cmd.cdw10 = (uint32_t)lba;
        cmd.cdw11 = (uint32_t)(lba >> 32);
        cmd.cdw12 = n - 1; // NLB is 0-based
        if (submit(&io_q, &cmd) != 0) {
            return false;
        }
        memcpy(out, bounce_va, (size_t)n * g_block_size);
        out += (size_t)n * g_block_size;
        lba += n;
        count -= n;
    }
    return true;
}
