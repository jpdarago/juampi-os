#ifndef __BLOCKDEV_H
#define __BLOCKDEV_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

// A 512-byte-sector block device: the storage abstraction the filesystem sits
// on, so ext2 (and future filesystems) never hardwire a particular driver.
// LBAs and sector counts are in 512-byte units. ATA is natively 512-byte; an
// NVMe namespace with 512-byte logical blocks maps 1:1. A device with no media
// reports sectors() == 0.
typedef struct blockdev {
    bool (*read)(uint64_t lba, uint32_t count, void* buf);
    bool (*write)(uint64_t lba, uint32_t count, const void* buf);
    uint64_t (*sectors)(
            void); // total addressable 512-byte sectors, 0 if absent
} blockdev;

#endif
