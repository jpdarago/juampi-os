#ifndef __NVME_H
#define __NVME_H

#include <stdint.h>
#include <stdbool.h>
#include <blockdev.h>

// Minimal polled NVMe driver: brings up the first NVMe controller found on PCI,
// identifies namespace 1, and reads logical blocks by polling the completion
// queue (no interrupts, no writes — see docs for the milestone plan). Mirrors
// the ata.h shape so callers look the same. BSP-only, like the other drivers.
void nvme_init(void);
bool nvme_present(void);

// Total addressable logical blocks on namespace 1, and the block size in bytes
// (0 if no controller/namespace). Unlike ATA these are not fixed at 512.
uint64_t nvme_blocks(void);
uint32_t nvme_block_size(void);

// NUL-terminated controller model string (IDENTIFY CONTROLLER), or "" if
// absent. Points at driver-owned storage; do not free.
const char* nvme_model(void);

// Whether I/O completions are delivered via MSI-X interrupts (true) or polled
// (false, the fallback when the controller advertises no MSI-X), and the count
// of completions the interrupt handler has serviced (0 in polled mode).
bool nvme_irq_driven(void);
uint64_t nvme_irq_count(void);

// Which init step failed, or NULL (no controller found, or init succeeded).
// Surfaced in the boot report so a real-hardware failure isn't silent.
const char* nvme_fail_reason(void);

// Read/write `count` logical blocks starting at LBA `lba`. Returns false on
// timeout, controller error, or when no controller is present.
bool nvme_read(uint64_t lba, uint32_t count, void* buf);
bool nvme_write(uint64_t lba, uint32_t count, const void* buf);

// The namespace as a generic block device (blockdev.h), for ext2_mount. Only a
// 512-byte-logical-block namespace maps onto the interface's 512-byte sectors;
// otherwise its sectors() is 0 and the mount fails cleanly.
const struct blockdev* nvme_blockdev(void);

#endif
