#ifndef __ATA_H
#define __ATA_H

#include <stdint.h>
#include <stdbool.h>
#include <blockdev.h>

// Polled ATA driver for a single target: the primary IDE channel's slave drive
// (legacy ports 0x1F0/0x3F6). It reads a data disk kept separate from the
// Limine boot disk (the boot disk is the primary master). Transfers use
// bus-master DMA when the controller exposes it (PIIX3 and friends), falling
// back to word-at-a-time PIO otherwise; either way completion is polled with a
// timeout, matching the kernel's other polling drivers (no interrupts).
void ata_init(void);
bool ata_present(void);
uint64_t ata_sectors(void); // total addressable 512-byte sectors (0 if absent)
bool ata_dma_active(void);  // true once bus-master DMA is enabled (else PIO)

// Read `count` 512-byte sectors starting at LBA `lba` into `buf`. Returns false
// on timeout, device error, or when no disk is present.
bool ata_read(uint64_t lba, uint32_t count, void* buf);

// Write `count` 512-byte sectors starting at LBA `lba` from `buf`, then flush
// the drive's write cache. Returns false on timeout/error or with no disk.
bool ata_write(uint64_t lba, uint32_t count, const void* buf);

// The ATA disk as a generic block device (for ext2_mount and friends). The
// driver's read/write/sectors already speak 512-byte sectors, so this is a thin
// wrapper. Valid after ata_init(); sectors() is 0 when no disk is attached.
const struct blockdev* ata_blockdev(void);

#endif
