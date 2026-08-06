#ifndef __BCACHE_H
#define __BCACHE_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <blockdev.h>
#include <memory.h>

// A write-back LRU block cache over a blockdev (blockdev.h). It sits between a
// filesystem and its device so the metadata a filesystem re-reads constantly —
// group descriptors, inode tables, indirect blocks, directory blocks — is read
// from disk once and served from RAM on every repeat, and writes are buffered
// dirty and flushed in batches instead of one synchronous device write each.
//
// Cache units are `block_size` bytes (the filesystem's block size), mapped onto
// the device's 512-byte LBAs linearly: cache block `b` is device sector
// `b * (block_size / 512)`. The cache is device-agnostic, so any filesystem
// (ext2 today, others later) gets the same benefit.

struct bcache;

// Create a cache of `nslots` blocks of `block_size` bytes over `dev`, drawing
// all memory (control structures and the block buffers) from `heap`.
// `block_size` must be a positive multiple of 512. Returns NULL on bad args.
struct bcache* bcache_create(const struct blockdev* dev, uint32_t block_size,
                             uint32_t nslots, struct heap_allocator* heap);

// Flush and release a cache created by bcache_create.
void bcache_destroy(struct bcache* c);

// Read cache block `blk` (index in block_size units) into `buf`. On a miss the
// block is loaded and a short run of following blocks is prefetched
// (read-ahead) so sequential metadata scans — directory blocks, adjacent
// inode-table entries — turn into cache hits.
bool bcache_read(struct bcache* c, uint32_t blk, void* buf);

// Write cache block `blk` from `buf`. The block is marked dirty and kept in the
// cache (write-back); the device is not touched until eviction or bcache_sync.
bool bcache_write(struct bcache* c, uint32_t blk, const void* buf);

// Read `n` consecutive cache blocks starting at `blk` into `buf`. When none of
// the range is resident this streams straight from the device in one transfer —
// the fast path for whole-file reads, which are already coalesced by the
// caller. If any block in the range is resident it falls back to per-block
// reads so a freshly-written (still-dirty) block is served from the cache, not
// stale disk.
bool bcache_read_run(struct bcache* c, uint32_t blk, uint32_t n, void* buf);

// Write every dirty block back to the device. Returns false if any write fails
// (the block stays dirty so a later sync can retry it).
bool bcache_sync(struct bcache* c);

// Drop `blk` from the cache. If it is dirty it is written back first. Used when
// the filesystem frees a block on disk, so a later reuse can't read stale data.
void bcache_forget(struct bcache* c, uint32_t blk);

// Hit/miss counters, for the performance lab (docs/perf-lab.md) and tests.
struct bcache_stats {
    uint64_t hits;       // reads served without touching the device
    uint64_t misses;     // reads that had to fetch from the device
    uint64_t writebacks; // dirty blocks flushed to the device
    uint64_t evictions;  // resident blocks displaced to make room
};
void bcache_get_stats(const struct bcache* c, struct bcache_stats* out);

#endif
