#include <hdd.h>
#include <exception.h>
#include <scrn.h>

static uchar ata_read_stable(void)
{
    uchar status;
    // The register is read 5 times to generate a
    // 400 ns delay according to the specification
    status = inb(ATA_PRIMARY_COMSTAT);
    status = inb(ATA_PRIMARY_COMSTAT);
    status = inb(ATA_PRIMARY_COMSTAT);
    status = inb(ATA_PRIMARY_COMSTAT);
    status = inb(ATA_PRIMARY_COMSTAT);
    return status;
}

static void ata_reset(void)
{
    uchar status;
    // Keep nIEN set the whole time: this driver polls, so the drive must not
    // raise IRQ14. Leaving interrupts enabled here makes QEMU assert IRQ14,
    // which has no handler ("Unknown interrupt 0x2E").
    outb(ATA_PRIMARY_DEVCONTROL, ATA_CTRL_SRST | ATA_CTRL_NIEN);
    status = ata_read_stable();
    if (status & ATA_STATUS_BSY) {
        kernel_panic("ERROR: ATA RESET FAILED\n");
    }
    outb(ATA_PRIMARY_DEVCONTROL, ATA_CTRL_NIEN);
    status = ata_read_stable();
    if (status & ATA_STATUS_BSY) {
        kernel_panic("ERROR: ATA RESET FAILED\n");
    }
}

static void hdd_setup_lba(uint lba_address, uint sectors, uchar command)
{
    // Select the master drive (LBA mode) with LBA[27:24] FIRST, then let it
    // settle before polling status. The status register reflects the selected
    // drive, so polling RDY before selecting reads 0x00 on QEMU and spins
    // forever. Keep the drive interrupt masked (nIEN) since we poll.
    outb(ATA_PRIMARY_DRIVE, 0xE0 | ((lba_address >> 24) & 0x0F));
    outb(ATA_PRIMARY_DEVCONTROL, ATA_CTRL_NIEN);
    ata_read_stable(); // ~400ns settle after the drive select

    while ((inb(ATA_PRIMARY_COMSTAT) & ATA_STATUS_BSY) == ATA_STATUS_BSY)
        ;
    while ((inb(ATA_PRIMARY_COMSTAT) & ATA_STATUS_RDY) != ATA_STATUS_RDY)
        ;

    outb(ATA_PRIMARY_ERR, 0x00);
    outb(ATA_PRIMARY_SECTORCOUNT, sectors);
    outb(ATA_PRIMARY_LBALOW, (uchar)lba_address);
    outb(ATA_PRIMARY_LBAMID, (uchar)(lba_address >> 8));
    outb(ATA_PRIMARY_LBAHIGH, (uchar)(lba_address >> 16));
    outb(ATA_PRIMARY_COMSTAT, command);
}

static void hdd_ata_wait(void)
{
    volatile uchar status;
    while (((status = inb(ATA_PRIMARY_COMSTAT)) & ATA_STATUS_BSY) ==
           ATA_STATUS_BSY)
        ;
    return;
}

void hdd_read(uint lba_address, uint sectors, void* _buffer)
{
    ushort* buffer = _buffer;
    hdd_setup_lba(lba_address, sectors, ATA_COMM_READ_LBA28);
    uint eflags = irq_cli();
    uint i, j, buffer_pos = 0;
    for (i = 0; i < sectors; i++) {
        hdd_ata_wait();
        for (j = 0; j < 256; j++) {
            buffer[buffer_pos] = inw(ATA_PRIMARY_DATA);
            buffer_pos++;
        }
        ata_read_stable();
    }
    irq_sti(eflags);
}

void hdd_write(uint lba_address, uint sectors, void* _buffer)
{
    ushort* buffer = _buffer;
    hdd_setup_lba(lba_address, sectors, ATA_COMM_WRITE_LBA28);
    uint i, j, buffer_pos = 0;
    uint eflags = irq_cli();
    for (i = 0; i < sectors; i++) {
        hdd_ata_wait();
        for (j = 0; j < 256; j++) {
            outw(ATA_PRIMARY_DATA, buffer[buffer_pos]);
            buffer_pos++;
        }

        hdd_ata_wait();
        outb(ATA_PRIMARY_COMSTAT, ATA_COMM_FLUSHCACHE);
    }
    hdd_ata_wait();
    irq_sti(eflags);
}

void hdd_init()
{
    // Reset the disks
    ata_reset();
    outb(ATA_PRIMARY_LBALOW, 0xAA);
    if (inb(ATA_PRIMARY_LBALOW) == 0xAA) {
        scrn_printf("ATA MASTER DISK PRESENT!\n");
    } else {
        kernel_panic("No disk was detected.");
    }
}
