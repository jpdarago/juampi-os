#include <bcache.h>

#include <alloc.h>
#include <string.h>

// Write-back LRU block cache (see bcache.h). Each slot holds one block_size
// buffer. Slots live on two intrusive lists at once: a hash bucket chain keyed
// by block number (O(1) lookup) and a global LRU list (most-recently-used at
// the head, eviction victim at the tail). A miss reuses the LRU tail, writing
// it back first if it is dirty.

// Blocks prefetched after a miss. Small on purpose: read-ahead helps sequential
// metadata (directory blocks, neighbouring inodes) without evicting the hot
// working set to chase speculative blocks.
#define READAHEAD 3

struct bcache_slot {
    uint32_t blk;
    bool valid;
    bool dirty;
    uint8_t* data;
    struct bcache_slot* lru_prev;
    struct bcache_slot* lru_next;
    struct bcache_slot* hash_next;
};

struct bcache {
    const struct blockdev* dev;
    struct heap_allocator* heap;
    uint32_t block_size;
    uint32_t spb; // 512-byte sectors per cache block
    uint32_t nslots;
    struct bcache_slot* slots;
    struct bcache_slot** buckets; // nbuckets heads
    uint32_t nbuckets;            // power of two, so & (nbuckets-1) selects
    struct bcache_slot* lru_head;
    struct bcache_slot* lru_tail;
    struct bcache_stats stats;
};

static struct allocator* mem(struct bcache* c)
{
    return &c->heap->base;
}

static uint32_t bucket_of(struct bcache* c, uint32_t blk)
{
    return blk & (c->nbuckets - 1);
}

// --- intrusive list plumbing ------------------------------------------------

static void lru_unlink(struct bcache* c, struct bcache_slot* s)
{
    if (s->lru_prev) {
        s->lru_prev->lru_next = s->lru_next;
    } else {
        c->lru_head = s->lru_next;
    }
    if (s->lru_next) {
        s->lru_next->lru_prev = s->lru_prev;
    } else {
        c->lru_tail = s->lru_prev;
    }
    s->lru_prev = s->lru_next = NULL;
}

static void lru_push_front(struct bcache* c, struct bcache_slot* s)
{
    s->lru_prev = NULL;
    s->lru_next = c->lru_head;
    if (c->lru_head) {
        c->lru_head->lru_prev = s;
    }
    c->lru_head = s;
    if (!c->lru_tail) {
        c->lru_tail = s;
    }
}

static void lru_push_back(struct bcache* c, struct bcache_slot* s)
{
    s->lru_next = NULL;
    s->lru_prev = c->lru_tail;
    if (c->lru_tail) {
        c->lru_tail->lru_next = s;
    }
    c->lru_tail = s;
    if (!c->lru_head) {
        c->lru_head = s;
    }
}

static void lru_touch(struct bcache* c, struct bcache_slot* s)
{
    if (c->lru_head == s) {
        return;
    }
    lru_unlink(c, s);
    lru_push_front(c, s);
}

static void hash_insert(struct bcache* c, struct bcache_slot* s)
{
    uint32_t b = bucket_of(c, s->blk);
    s->hash_next = c->buckets[b];
    c->buckets[b] = s;
}

static void hash_remove(struct bcache* c, struct bcache_slot* s)
{
    struct bcache_slot** pp = &c->buckets[bucket_of(c, s->blk)];
    while (*pp && *pp != s) {
        pp = &(*pp)->hash_next;
    }
    if (*pp) {
        *pp = s->hash_next;
    }
    s->hash_next = NULL;
}

static struct bcache_slot* lookup(struct bcache* c, uint32_t blk)
{
    for (struct bcache_slot* s = c->buckets[bucket_of(c, blk)]; s;
         s = s->hash_next) {
        if (s->valid && s->blk == blk) {
            return s;
        }
    }
    return NULL;
}

// --- device I/O for a single cache block ------------------------------------

static bool dev_read_block(struct bcache* c, uint32_t blk, void* buf)
{
    return c->dev->read((uint64_t)blk * c->spb, c->spb, buf);
}

static bool dev_write_block(struct bcache* c, uint32_t blk, const void* buf)
{
    return c->dev->write((uint64_t)blk * c->spb, c->spb, buf);
}

// Flush a dirty slot to disk; on success it is clean again.
static bool flush_slot(struct bcache* c, struct bcache_slot* s)
{
    if (!s->valid || !s->dirty) {
        return true;
    }
    if (!dev_write_block(c, s->blk, s->data)) {
        return false;
    }
    s->dirty = false;
    c->stats.writebacks++;
    return true;
}

// Claim a slot for `blk`: reuse the LRU tail, writing it back and evicting its
// old contents first. The returned slot is at the LRU front, rehashed to `blk`,
// still marked invalid (caller fills ->data then sets valid).
static struct bcache_slot* claim(struct bcache* c, uint32_t blk)
{
    struct bcache_slot* s = c->lru_tail;
    if (s->valid) {
        flush_slot(c, s); // best-effort: keeps data if the write fails
        hash_remove(c, s);
        c->stats.evictions++;
    }
    s->valid = false;
    s->dirty = false;
    s->blk = blk;
    hash_insert(c, s);
    lru_touch(c, s);
    return s;
}

// Fetch `blk` into the cache without read-ahead (returns the resident slot, or
// NULL on device error).
static struct bcache_slot* fill(struct bcache* c, uint32_t blk)
{
    struct bcache_slot* s = lookup(c, blk);
    if (s) {
        c->stats.hits++;
        lru_touch(c, s);
        return s;
    }
    c->stats.misses++;
    s = claim(c, blk);
    if (!dev_read_block(c, blk, s->data)) {
        // Drop the half-initialised slot so a retry re-reads it.
        hash_remove(c, s);
        s->blk = 0;
        return NULL;
    }
    s->valid = true;
    return s;
}

// Prefetch up to READAHEAD blocks after `blk`, but never force a write-back for
// speculation: stop as soon as the next victim slot is dirty.
static void readahead(struct bcache* c, uint32_t blk)
{
    uint64_t total = c->dev->sectors() / c->spb;
    for (uint32_t i = 1; i <= READAHEAD; i++) {
        uint32_t next = blk + i;
        if (total && next >= total) {
            break;
        }
        if (lookup(c, next)) {
            continue;
        }
        if (c->lru_tail->valid && c->lru_tail->dirty) {
            break;
        }
        if (!fill(c, next)) {
            break;
        }
    }
}

// --- public API -------------------------------------------------------------

struct bcache* bcache_create(const struct blockdev* dev, uint32_t block_size,
                             uint32_t nslots, struct heap_allocator* heap)
{
    if (!dev || block_size == 0 || block_size % 512 != 0 || nslots == 0) {
        return NULL;
    }
    struct bcache* c = new (&heap->base, struct bcache, 1);
    c->dev = dev;
    c->heap = heap;
    c->block_size = block_size;
    c->spb = block_size / 512;
    c->nslots = nslots;
    c->slots = new (mem(c), struct bcache_slot, nslots);

    // Round the bucket count up to a power of two >= nslots so bucket_of() is a
    // mask, keeping chains short (load factor <= 1).
    uint32_t nb = 1;
    while (nb < nslots) {
        nb <<= 1;
    }
    c->nbuckets = nb;
    c->buckets = new (mem(c), struct bcache_slot*, nb);

    // Seed the LRU list with every slot (all invalid). claim() reuses from the
    // tail, so the first nslots misses consume these before any eviction.
    for (uint32_t i = 0; i < nslots; i++) {
        struct bcache_slot* s = &c->slots[i];
        s->data = new (mem(c), uint8_t, block_size);
        s->valid = s->dirty = false;
        lru_push_front(c, s);
    }
    return c;
}

void bcache_destroy(struct bcache* c)
{
    if (!c) {
        return;
    }
    bcache_sync(c);
    for (uint32_t i = 0; i < c->nslots; i++) {
        heap_free(c->heap, c->slots[i].data);
    }
    heap_free(c->heap, c->buckets);
    heap_free(c->heap, c->slots);
    heap_free(c->heap, c);
}

bool bcache_read(struct bcache* c, uint32_t blk, void* buf)
{
    struct bcache_slot* s = lookup(c, blk);
    if (s) {
        c->stats.hits++;
        lru_touch(c, s);
        memcpy(buf, s->data, c->block_size);
        return true;
    }
    s = fill(c, blk);
    if (!s) {
        return false;
    }
    memcpy(buf, s->data, c->block_size);
    readahead(c, blk);
    return true;
}

bool bcache_write(struct bcache* c, uint32_t blk, const void* buf)
{
    struct bcache_slot* s = lookup(c, blk);
    if (!s) {
        // Overwriting a whole block: claim a slot and fill it from `buf`
        // without a read (the old disk contents are about to be replaced
        // wholesale).
        s = claim(c, blk);
        s->valid = true;
    } else {
        lru_touch(c, s);
    }
    memcpy(s->data, buf, c->block_size);
    s->dirty = true;
    return true;
}

bool bcache_read_run(struct bcache* c, uint32_t blk, uint32_t n, void* buf)
{
    if (n == 0) {
        return true;
    }
    // Fast path: if nothing in the range is resident, none of it can be dirty,
    // so one big device read is both correct and cheapest.
    bool any_resident = false;
    for (uint32_t i = 0; i < n; i++) {
        if (lookup(c, blk + i)) {
            any_resident = true;
            break;
        }
    }
    if (!any_resident) {
        return c->dev->read((uint64_t)blk * c->spb, n * c->spb, buf);
    }
    // Slow path: serve resident (possibly dirty) blocks from the cache.
    uint8_t* p = buf;
    for (uint32_t i = 0; i < n; i++) {
        if (!bcache_read(c, blk + i, p)) {
            return false;
        }
        p += c->block_size;
    }
    return true;
}

bool bcache_sync(struct bcache* c)
{
    bool ok = true;
    for (uint32_t i = 0; i < c->nslots; i++) {
        if (!flush_slot(c, &c->slots[i])) {
            ok = false;
        }
    }
    return ok;
}

void bcache_forget(struct bcache* c, uint32_t blk)
{
    struct bcache_slot* s = lookup(c, blk);
    if (!s) {
        return;
    }
    flush_slot(c, s);
    hash_remove(c, s);
    s->valid = false;
    s->dirty = false;
    lru_unlink(c, s);
    lru_push_back(c, s); // to the eviction end: reuse first, contents are stale
}

void bcache_get_stats(const struct bcache* c, struct bcache_stats* out)
{
    *out = c->stats;
}
