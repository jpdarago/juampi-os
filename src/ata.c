#include <ata.h>
#include <ports.h>
#include <ktime.h>

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

// Drive/head register: bit 6 selects LBA mode, bit 4 selects the slave drive.
#define DRIVE_SLAVE_CHS 0xB0 // slave, for the CHS-style IDENTIFY select
#define DRIVE_SLAVE_LBA 0xF0 // slave, LBA mode (| top nibble of a 28-bit LBA)

static bool present;
static uint64_t sectors;

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
}

bool ata_present(void)
{
    return present;
}

uint64_t ata_sectors(void)
{
    return sectors;
}

bool ata_read(uint64_t lba, uint32_t count, void* buf)
{
    if (!present || count == 0) {
        return false;
    }
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
            uint16_t* w = (uint16_t*)out;
            for (int i = 0; i < 256; i++) {
                w[i] = inw(REG_DATA);
            }
            out += 512;
        }
        lba += chunk;
        count -= chunk;
    }
    return true;
}
