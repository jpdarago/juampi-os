#ifndef __NVME_H
#define __NVME_H

#include <stdint.h>
#include <stdbool.h>

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

// Read `count` logical blocks starting at LBA `lba` into `buf`. Returns false
// on timeout, controller error, or when no controller is present.
bool nvme_read(uint64_t lba, uint32_t count, void* buf);

#endif
