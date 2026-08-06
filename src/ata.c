#include <ata.h>
#include <ports.h>
#include <ktime.h>
#include <pci.h>
#include <dma.h>
#include <paging.h>
#include <console.h>
#include <utils.h> // memcpy

// Primary IDE channel, legacy port layout. We only ever touch the slave drive
// (unit 1); the master (unit 0) is the Limine boot disk and is left alone.
#define ATA_IO 0x1F0
#define ATA_CTRL 0x3F6

#define REG_DATA (ATA_IO + 0)
#define REG_SECCOUNT (ATA_IO + 2)
#define REG_LBA0 (ATA_IO + 3)
#define REG_LBA1 (ATA_IO + 4)
#define REG_LBA2 (ATA_IO + 5)
#define REG_DRIVE (ATA_IO + 6)
#define REG_STATUS (ATA_IO + 7)
#define REG_COMMAND (ATA_IO + 7)

#define ST_ERR 0x01
#define ST_DRQ 0x08
#define ST_BSY 0x80

#define CMD_IDENTIFY 0xEC
#define CMD_READ_PIO 0x20
#define CMD_WRITE_PIO 0x30
#define CMD_FLUSH 0xE7

// Drive/head register: bit 6 selects LBA mode, bit 4 selects the slave drive.
#define DRIVE_SLAVE_CHS 0xB0   // slave, for the CHS-style IDENTIFY select
#define DRIVE_SLAVE_LBA 0xF0   // slave, LBA mode (| top nibble of a 28-bit LBA)
#define DRIVE_SLAVE_LBA48 0x50 // slave, LBA mode, for 48-bit addressing

// --- Bus-master IDE (UDMA) --------------------------------------------------
// PIO pushes every word through the CPU (a port access per word — one VM exit
// each under KVM). Bus mastering instead lets the IDE controller DMA straight
// to/from memory while the CPU waits on a status bit. The controller is a PCI
// function (class 0x01/0x01) whose BAR4 is a 16-byte I/O block: the Bus Master
// IDE (BMIDE) registers, one 8-byte set per channel. We drive the primary
// channel, where our data disk is the slave. Requires a controller that exposes
// BMIDE — the i440fx/PIIX3 (`-machine pc`) does; q35's AHCI does not, so on q35
// (and the NVMe-only XPS) the driver stays on PIO and NVMe carries real I/O.
#define BM_CMD 0    // bit0 start/stop, bit3 direction (1 = memory->disk)
#define BM_STATUS 2 // bit0 active, bit1 error, bit2 irq (write 1 to clear)
#define BM_PRDT 4   // 32-bit physical address of the PRD table

#define BM_START 0x01
#define BM_WRITE 0x08 // direction bit in BM_CMD (set = disk write)
#define BM_ERROR 0x02
#define BM_ACTIVE 0x01

#define CMD_READ_DMA_EXT 0x25
#define CMD_WRITE_DMA_EXT 0x35

// A Physical Region Descriptor: one span of the transfer buffer. `count` is a
// byte count (0 encodes 64 KiB); bit 15 of `flags` marks the final entry (EOT).
struct prd {
    uint32_t addr;
    uint16_t count;
    uint16_t flags;
} __attribute__((packed));

#define PRD_EOT 0x8000
#define PRDS_PER_TABLE (PAGE_SZ / sizeof(struct prd))

// Cap one DMA command so its PRD table fits in a page and the transfer stays
// modest: 256 sectors = 128 KiB, at most 33 page-granular descriptors.
#define DMA_MAX_SECTORS 256

static bool present;
static uint64_t sectors;

static bool g_dma_ok;             // BMIDE discovered and enabled
static uint16_t g_bm_base;        // primary-channel BMIDE I/O base (from BAR4)
static struct dma_buf g_prdt_buf; // one page holding the PRD table
static struct dma_buf g_bounce_buf; // staging for buffers we can't DMA in place

// Concurrency: this state — the single PRD table, the bounce page, and the one
// set of BMIDE + drive registers — is shared and unsynchronised, and a
// concurrent second caller would corrupt an in-flight transfer. That is safe
// because disk I/O is single-threaded by construction: it is issued only from
// the BSP's shell interpreter. ata_init() runs at boot before smp_init() starts
// any application processor, and the parallel Lua workers on those cores get no
// filesystem library (open_worker_libs in lua_thread.c exposes only base/table/
// string/math/coroutine/thread/mem). The whole storage stack relies on this —
// ext2.c, bcache.c, and nvme.c are lock-free for the same reason. If the
// filesystem is ever handed to worker cores, add ONE lock at the ext2/blockdev
// boundary (serialising ata + nvme + the cache together), not a per-driver lock
// here — a lock in this file alone would leave ext2 and the cache still racing.

static void ata_dma_init(void); // defined below; called at the end of ata_init

// Read the alternate-status register a few times (~400ns) to let the drive's
// status settle after a select or command, as the ATA spec requires.
static void settle(void)
{
    for (int i = 0; i < 4; i++) {
        (void)inb(ATA_CTRL);
    }
}

// Spin until BSY clears (and, if want_drq, DRQ sets), bounded by a timeout so a
// missing or wedged drive can't hang boot. Returns false on timeout or error.
static bool wait_ready(bool want_drq)
{
    uint64_t deadline = ktime_ms() + 1000; // 1s is ample under QEMU
    for (;;) {
        uint8_t st = inb(REG_STATUS);
        if (st & ST_ERR) {
            return false;
        }
        if (!(st & ST_BSY) && (!want_drq || (st & ST_DRQ))) {
            return true;
        }
        if (ktime_ms() > deadline) {
            return false;
        }
    }
}

void ata_init(void)
{
    present = false;
    sectors = 0;

    // We poll, so disable this channel's interrupts (nIEN, control bit 1) to
    // keep IRQ14 quiet.
    outb(ATA_CTRL, 0x02);

    // Select the slave and IDENTIFY it.
    outb(REG_DRIVE, DRIVE_SLAVE_CHS);
    settle();
    outb(REG_SECCOUNT, 0);
    outb(REG_LBA0, 0);
    outb(REG_LBA1, 0);
    outb(REG_LBA2, 0);
    outb(REG_COMMAND, CMD_IDENTIFY);

    uint8_t st = inb(REG_STATUS);
    if (st == 0 || st == 0xFF) {
        return; // nothing on this select (status 0 / floating bus)
    }
    if (!wait_ready(true)) {
        return;
    }

    uint16_t id[256];
    for (int i = 0; i < 256; i++) {
        id[i] = inw(REG_DATA);
    }

    // Total sectors: LBA48 count (words 100-103) if present, else LBA28
    // (60-61).
    uint64_t lba48 = (uint64_t)id[100] | ((uint64_t)id[101] << 16) |
                     ((uint64_t)id[102] << 32) | ((uint64_t)id[103] << 48);
    uint32_t lba28 = (uint32_t)id[60] | ((uint32_t)id[61] << 16);
    sectors = lba48 ? lba48 : lba28;
    present = sectors > 0;

    if (present) {
        ata_dma_init(); // upgrade to bus-master DMA if the controller offers it
    }
}

bool ata_present(void)
{
    return present;
}

uint64_t ata_sectors(void)
{
    return sectors;
}

static bool ata_pio_read(uint64_t lba, uint32_t count, void* buf)
{
    uint8_t* out = (uint8_t*)buf;
    while (count > 0) {
        // One command transfers up to 256 sectors (a count of 0 means 256).
        uint32_t chunk = count > 256 ? 256 : count;

        if (!wait_ready(false)) {
            return false;
        }
        outb(REG_DRIVE, DRIVE_SLAVE_LBA | (uint8_t)((lba >> 24) & 0x0F));
        settle();
        outb(REG_SECCOUNT, (uint8_t)(chunk & 0xFF));
        outb(REG_LBA0, (uint8_t)(lba & 0xFF));
        outb(REG_LBA1, (uint8_t)((lba >> 8) & 0xFF));
        outb(REG_LBA2, (uint8_t)((lba >> 16) & 0xFF));
        outb(REG_COMMAND, CMD_READ_PIO);

        for (uint32_t s = 0; s < chunk; s++) {
            if (!wait_ready(true)) {
                return false;
            }
            // Transfer the whole 256-word sector with a single string-I/O
            // instruction. A per-word inw() loop is one port read (one VM exit
            // under KVM) per word — ~256x more exits, which dominated large
            // reads like the Doom WAD.
            uint32_t words = 256;
            __asm__ __volatile__("cld; rep insw"
                                 : "+D"(out), "+c"(words)
                                 : "d"((uint16_t)REG_DATA)
                                 : "memory");
        }
        lba += chunk;
        count -= chunk;
    }
    return true;
}

static bool ata_pio_write(uint64_t lba, uint32_t count, const void* buf)
{
    const uint8_t* in = (const uint8_t*)buf;
    while (count > 0) {
        uint32_t chunk = count > 256 ? 256 : count;

        if (!wait_ready(false)) {
            return false;
        }
        outb(REG_DRIVE, DRIVE_SLAVE_LBA | (uint8_t)((lba >> 24) & 0x0F));
        settle();
        outb(REG_SECCOUNT, (uint8_t)(chunk & 0xFF));
        outb(REG_LBA0, (uint8_t)(lba & 0xFF));
        outb(REG_LBA1, (uint8_t)((lba >> 8) & 0xFF));
        outb(REG_LBA2, (uint8_t)((lba >> 16) & 0xFF));
        outb(REG_COMMAND, CMD_WRITE_PIO);

        for (uint32_t s = 0; s < chunk; s++) {
            if (!wait_ready(true)) {
                return false;
            }
            // One string-I/O instruction per sector (see ata_read).
            uint32_t words = 256;
            __asm__ __volatile__("cld; rep outsw"
                                 : "+S"(in), "+c"(words)
                                 : "d"((uint16_t)REG_DATA)
                                 : "memory");
        }
        lba += chunk;
        count -= chunk;
    }
    // Flush the drive's write cache so the data is durable on disk.
    if (!wait_ready(false)) {
        return false;
    }
    outb(REG_COMMAND, CMD_FLUSH);
    wait_ready(false);
    return true;
}

// Describe `bytes` of `buf` in the PRD table as page-granular spans. A span
// stays within one page, so it never crosses the 64 KiB boundary a PRD may not
// straddle. Returns the entry count, or 0 if a page is unmapped or lies above
// 4 GiB (a PRD address is 32-bit) — the caller then stages through the bounce
// page instead.
static int build_prdt(const uint8_t* buf, uint32_t bytes)
{
    struct prd* t = g_prdt_buf.va;
    uint32_t off = 0;
    int n = 0;
    while (off < bytes) {
        if (n >= (int)PRDS_PER_TABLE) {
            return 0;
        }
        uintptr_t va = (uintptr_t)buf + off;
        uintptr_t pa = physical_address(kernel_dir, va);
        if (pa == (uintptr_t)-1 || pa >= 0x100000000ull) {
            return 0;
        }
        uint32_t chunk =
                PAGE_SZ - (uint32_t)(va & (PAGE_SZ - 1)); // to page end
        if (chunk > bytes - off) {
            chunk = bytes - off;
        }
        t[n].addr = (uint32_t)pa;
        t[n].count = (uint16_t)chunk; // 1..4096, never the 0 that means 64 KiB
        t[n].flags = 0;
        n++;
        off += chunk;
    }
    t[n - 1].flags = PRD_EOT;
    return n;
}

// One bus-master transfer of up to DMA_MAX_SECTORS sectors. Returns the number
// of sectors transferred (it may clamp `count` when bouncing), or 0 on error.
static uint32_t ata_dma_chunk(uint64_t lba, uint32_t count, uint8_t* buf,
                              bool write)
{
    uint32_t bytes = count * 512;
    bool bounced = false;
    if (build_prdt(buf, bytes) == 0) {
        // Fall back to the bounce page: at most one page of sectors per
        // command.
        if (count > PAGE_SZ / 512) {
            count = PAGE_SZ / 512;
            bytes = count * 512;
        }
        if (write) {
            memcpy(g_bounce_buf.va, buf, bytes);
        }
        struct prd* t = g_prdt_buf.va;
        t[0].addr = (uint32_t)g_bounce_buf.pa;
        t[0].count = (uint16_t)bytes;
        t[0].flags = PRD_EOT;
        bounced = true;
    }

    // Point the engine at the PRDT, set direction, and clear stale status bits.
    outb(g_bm_base + BM_CMD, write ? BM_WRITE : 0);
    outl(g_bm_base + BM_PRDT, (uint32_t)g_prdt_buf.pa);
    outb(g_bm_base + BM_STATUS, inb(g_bm_base + BM_STATUS) | (BM_ERROR | 0x04));

    // Program the drive for LBA48: select the slave, then the high-order LBA
    // bytes, then the low-order bytes (the standard two-write sequence).
    if (!wait_ready(false)) {
        return 0;
    }
    outb(REG_DRIVE, DRIVE_SLAVE_LBA48);
    settle();
    outb(REG_SECCOUNT, (uint8_t)((count >> 8) & 0xFF));
    outb(REG_LBA0, (uint8_t)((lba >> 24) & 0xFF));
    outb(REG_LBA1, (uint8_t)((lba >> 32) & 0xFF));
    outb(REG_LBA2, (uint8_t)((lba >> 40) & 0xFF));
    outb(REG_SECCOUNT, (uint8_t)(count & 0xFF));
    outb(REG_LBA0, (uint8_t)(lba & 0xFF));
    outb(REG_LBA1, (uint8_t)((lba >> 8) & 0xFF));
    outb(REG_LBA2, (uint8_t)((lba >> 16) & 0xFF));
    outb(REG_COMMAND, write ? CMD_WRITE_DMA_EXT : CMD_READ_DMA_EXT);

    // Start the engine. The channel interrupt stays masked (nIEN, set in
    // ata_init), so completion is detected by the active bit clearing rather
    // than an IRQ — the engine drops it once the PRD list is drained.
    outb(g_bm_base + BM_CMD, (write ? BM_WRITE : 0) | BM_START);
    uint64_t deadline = ktime_ms() + 5000;
    uint8_t bmst;
    for (;;) {
        bmst = inb(g_bm_base + BM_STATUS);
        if ((bmst & BM_ERROR) || !(bmst & BM_ACTIVE)) {
            break;
        }
        if (ktime_ms() > deadline) {
            break;
        }
    }
    outb(g_bm_base + BM_CMD, write ? BM_WRITE : 0);        // stop
    outb(g_bm_base + BM_STATUS, bmst | (BM_ERROR | 0x04)); // clear latches

    if ((bmst & BM_ERROR) || (bmst & BM_ACTIVE) || !wait_ready(false)) {
        return 0; // controller error, short transfer, or drive never settled
    }
    if (inb(REG_STATUS) & ST_ERR) {
        return 0;
    }
    if (!write && bounced) {
        memcpy(buf, g_bounce_buf.va, bytes);
    }
    return count;
}

static bool ata_dma_rw(uint64_t lba, uint32_t count, uint8_t* buf, bool write)
{
    while (count > 0) {
        uint32_t chunk = count > DMA_MAX_SECTORS ? DMA_MAX_SECTORS : count;
        uint32_t done = ata_dma_chunk(lba, chunk, buf, write);
        if (done == 0) {
            return false;
        }
        buf += (uint64_t)done * 512;
        lba += done;
        count -= done;
    }
    if (write) {
        // Flush the drive's write cache so the data is durable, as PIO does.
        if (!wait_ready(false)) {
            return false;
        }
        outb(REG_DRIVE, DRIVE_SLAVE_LBA48);
        settle();
        outb(REG_COMMAND, CMD_FLUSH);
        wait_ready(false);
    }
    return true;
}

// Discover the bus-master IDE controller and enable DMA. Left disabled (PIO
// only) if no BMIDE-capable IDE function is present, e.g. on q35.
static void ata_dma_init(void)
{
    g_dma_ok = false;
    // Class 0x01 (mass storage) / subclass 0x01 (IDE) / prog-IF 0x80 (both
    // channels legacy-wired and bus-master capable), which is what PIIX3
    // reports.
    struct pci_addr a = pci_find_class(0x01, 0x01, 0x80);
    if (!a.found) {
        return;
    }
    g_bm_base = (uint16_t)pci_bar(a, 4);
    if (g_bm_base == 0) {
        return; // no BMIDE register block
    }
    pci_enable_bus_master(a);
    g_prdt_buf = dma_page_alloc();
    g_bounce_buf = dma_page_alloc();
    g_dma_ok = true;
}

// Read/write dispatch: bus-master DMA when available (no silent PIO fallback,
// so a DMA fault surfaces instead of hiding), else the PIO path.
bool ata_read(uint64_t lba, uint32_t count, void* buf)
{
    if (!present || count == 0) {
        return false;
    }
    return g_dma_ok ? ata_dma_rw(lba, count, buf, false)
                    : ata_pio_read(lba, count, buf);
}

bool ata_write(uint64_t lba, uint32_t count, const void* buf)
{
    if (!present || count == 0) {
        return false;
    }
    return g_dma_ok ? ata_dma_rw(lba, count, (uint8_t*)(uintptr_t)buf, true)
                    : ata_pio_write(lba, count, buf);
}

bool ata_dma_active(void)
{
    return g_dma_ok;
}

// The block-device view: the driver already speaks 512-byte sectors, so the
// vtable points straight at its read/write/sectors.
static const struct blockdev ata_dev = {ata_read, ata_write, ata_sectors};

const struct blockdev* ata_blockdev(void)
{
    return &ata_dev;
}
