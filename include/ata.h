#ifndef __ATA_H
#define __ATA_H

#include <stdint.h>
#include <stdbool.h>

// Minimal polled ATA PIO driver for a single target: the primary IDE channel's
// slave drive (legacy ports 0x1F0/0x3F6). It reads a data disk kept separate
// from the Limine boot disk (the boot disk is the primary master). No DMA and
// no interrupts — reads spin on the status register with a timeout, matching
// the kernel's other polling drivers.
void ata_init(void);
bool ata_present(void);
uint64_t ata_sectors(void); // total addressable 512-byte sectors (0 if absent)

// Read `count` 512-byte sectors starting at LBA `lba` into `buf`. Returns false
// on timeout, device error, or when no disk is present.
bool ata_read(uint64_t lba, uint32_t count, void* buf);

// Write `count` 512-byte sectors starting at LBA `lba` from `buf`, then flush
// the drive's write cache. Returns false on timeout/error or with no disk.
bool ata_write(uint64_t lba, uint32_t count, const void* buf);

#endif
