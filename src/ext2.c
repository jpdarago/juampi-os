#include <ext2.h>
#include <ata.h>
#include <memory.h>
#include <utils.h>

// ext2 read + write. On-disk layout is little-endian and x86-64 is too, so the
// packed structs below map straight onto the bytes read from disk. Writes are
// read-modify-write straight to disk (no cache, no journal); the primary
// superblock and group descriptors are kept consistent, but sparse_super
// backups are not (e2fsck -f refreshes them).

#define EXT2_MAGIC 0xEF53
#define EXT2_ROOT_INO 2

#define S_IFMT 0xF000
#define S_IFDIR 0x4000
#define S_IFREG 0x8000

#define INCOMPAT_FILETYPE 0x0002
#define RO_COMPAT_LARGE_FILE 0x0002

// Directory-entry file types (used when the fs has the filetype feature).
#define FT_REG 1
#define FT_DIR 2

struct superblock {
    uint32_t inodes_count;
    uint32_t blocks_count;
    uint32_t r_blocks_count;
    uint32_t free_blocks_count;
    uint32_t free_inodes_count;
    uint32_t first_data_block;
    uint32_t log_block_size;
    uint32_t log_frag_size;
    uint32_t blocks_per_group;
    uint32_t frags_per_group;
    uint32_t inodes_per_group;
    uint32_t mtime;
    uint32_t wtime;
    uint16_t mnt_count;
    uint16_t max_mnt_count;
    uint16_t magic;
    uint16_t state;
    uint16_t errors;
    uint16_t minor_rev_level;
    uint32_t lastcheck;
    uint32_t checkinterval;
    uint32_t creator_os;
    uint32_t rev_level;
    uint16_t def_resuid;
    uint16_t def_resgid;
    // EXT2_DYNAMIC_REV (rev_level >= 1) extension:
    uint32_t first_ino;
    uint16_t inode_size;
    uint16_t block_group_nr;
    uint32_t feature_compat;
    uint32_t feature_incompat;
    uint32_t feature_ro_compat;
} __attribute__((packed));

struct group_desc {
    uint32_t block_bitmap;
    uint32_t inode_bitmap;
    uint32_t inode_table;
    uint16_t free_blocks_count;
    uint16_t free_inodes_count;
    uint16_t used_dirs_count;
    uint16_t pad;
    uint32_t reserved[3];
} __attribute__((packed));

struct inode {
    uint16_t mode;
    uint16_t uid;
    uint32_t size; // low 32 bits
    uint32_t atime;
    uint32_t ctime;
    uint32_t mtime;
    uint32_t dtime;
    uint16_t gid;
    uint16_t links_count;
    uint32_t blocks;
    uint32_t flags;
    uint32_t osd1;
    uint32_t block[15]; // 0-11 direct, 12 single, 13 double, 14 triple indirect
    uint32_t generation;
    uint32_t file_acl;
    uint32_t dir_acl; // for regular files: high 32 bits of size (large_file)
} __attribute__((packed));

struct dirent {
    uint32_t inode;
    uint16_t rec_len;
    uint8_t name_len;
    uint8_t file_type;
    char name[]; // name_len bytes, unterminated
} __attribute__((packed));

static bool mounted;
static struct superblock sb;
static uint32_t block_size;
static uint32_t inode_size;
static uint32_t groups; // number of block groups
static bool has_filetype;

static allocator* mem(void)
{
    return &heap_default()->base;
}

// Read ext2 block `blk` (block_size bytes) into `buf`. Blocks map linearly onto
// 512-byte LBAs.
static bool read_block(uint32_t blk, void* buf)
{
    uint32_t spb = block_size / 512;
    return ata_read((uint64_t)blk * spb, spb, buf);
}

static bool read_group_desc(uint32_t group, struct group_desc* out)
{
    // The group descriptor table starts in the block after the superblock.
    uint32_t table = sb.first_data_block + 1;
    uint32_t byte = group * (uint32_t)sizeof(struct group_desc);
    uint8_t* blk = new (mem(), uint8_t, block_size);
    bool ok = read_block(table + byte / block_size, blk);
    if (ok) {
        memcpy(out, blk + byte % block_size, sizeof *out);
    }
    heap_free(heap_default(), blk);
    return ok;
}

static bool read_inode(uint32_t ino, struct inode* out)
{
    if (ino == 0) {
        return false;
    }
    uint32_t group = (ino - 1) / sb.inodes_per_group;
    uint32_t index = (ino - 1) % sb.inodes_per_group;
    struct group_desc gd;
    if (!read_group_desc(group, &gd)) {
        return false;
    }
    uint32_t byte = index * inode_size;
    uint8_t* blk = new (mem(), uint8_t, block_size);
    bool ok = read_block(gd.inode_table + byte / block_size, blk);
    if (ok) {
        memcpy(out, blk + byte % block_size, sizeof *out);
    }
    heap_free(heap_default(), blk);
    return ok;
}

// Map logical file block `n` to its absolute disk block (0 = sparse hole or
// out of range). Handles direct, single- and double-indirect blocks; triple
// indirect is unsupported (returns 0 — caps file support at ~64 MB with 1 KiB
// blocks, far beyond any script).
static uint32_t block_of(const struct inode* in, uint32_t n)
{
    uint32_t per = block_size / 4;
    if (n < 12) {
        return in->block[n];
    }
    n -= 12;
    uint32_t result = 0;
    uint32_t* tmp = new (mem(), uint32_t, per);
    if (n < per) {
        if (in->block[12]) {
            read_block(in->block[12], tmp);
            result = tmp[n];
        }
    } else {
        n -= per;
        if (n < per * per && in->block[13]) {
            read_block(in->block[13], tmp);
            uint32_t dbl = tmp[n / per];
            if (dbl) {
                read_block(dbl, tmp);
                result = tmp[n % per];
            }
        }
    }
    heap_free(heap_default(), tmp);
    return result;
}

static uint64_t inode_file_size(const struct inode* in)
{
    uint64_t size = in->size;
    if ((in->mode & S_IFMT) == S_IFREG &&
        (sb.feature_ro_compat & RO_COMPAT_LARGE_FILE)) {
        size |= (uint64_t)in->dir_acl << 32;
    }
    return size;
}

// Copy an inode's data into a fresh heap buffer of exactly `size` bytes.
static void* read_file(const struct inode* in, uint64_t size, size_t* out_size)
{
    uint8_t* out = new (mem(), uint8_t, size ? (ptrdiff_t)size : 1);
    uint8_t* block = new (mem(), uint8_t, block_size);
    uint8_t* p = out;
    uint64_t remaining = size;
    uint32_t bi = 0;
    while (remaining > 0) {
        uint32_t chunk =
                remaining < block_size ? (uint32_t)remaining : block_size;
        uint32_t phys = block_of(in, bi);
        if (phys == 0) {
            memset(p, 0, chunk); // sparse hole
        } else {
            read_block(phys, block);
            memcpy(p, block, chunk);
        }
        p += chunk;
        remaining -= chunk;
        bi++;
    }
    heap_free(heap_default(), block);
    if (out_size) {
        *out_size = (size_t)size;
    }
    return out;
}

// Callback over directory entries; returning true stops the walk early.
typedef bool (*dirent_fn)(void* ctx, uint32_t ino, uint8_t type,
                          const char* name, uint32_t namelen);

static void walk_dir(const struct inode* dir, dirent_fn fn, void* ctx)
{
    if ((dir->mode & S_IFMT) != S_IFDIR) {
        return;
    }
    uint64_t size = inode_file_size(dir);
    uint8_t* block = new (mem(), uint8_t, block_size);
    for (uint32_t bi = 0; (uint64_t)bi * block_size < size; bi++) {
        uint32_t phys = block_of(dir, bi);
        if (phys == 0 || !read_block(phys, block)) {
            continue;
        }
        uint32_t off = 0;
        while (off + 8 <= block_size) {
            struct dirent* de = (struct dirent*)(block + off);
            if (de->rec_len < 8) {
                break; // corrupt record; avoid an infinite loop
            }
            if (de->inode != 0) {
                uint32_t namelen = de->name_len;
                uint8_t type = 0;
                if (has_filetype) {
                    type = de->file_type;
                } else {
                    namelen |= (uint32_t)de->file_type << 8;
                }
                if (fn(ctx, de->inode, type, de->name, namelen)) {
                    heap_free(heap_default(), block);
                    return;
                }
            }
            off += de->rec_len;
        }
    }
    heap_free(heap_default(), block);
}

// --- Path resolution -------------------------------------------------------

struct lookup {
    const char* name;
    uint32_t namelen;
    uint32_t found; // resolved inode, 0 until matched
};

static bool str_eq(const char* a, const char* b, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++) {
        if (a[i] != b[i]) {
            return false;
        }
    }
    return true;
}

static bool match_entry(void* ctx, uint32_t ino, uint8_t type, const char* name,
                        uint32_t namelen)
{
    (void)type;
    struct lookup* l = ctx;
    if (namelen == l->namelen && str_eq(name, l->name, namelen)) {
        l->found = ino;
        return true;
    }
    return false;
}

// Resolve `path` to its inode, walking components from the root. Leading and
// repeated slashes are ignored. Returns false if any component is missing.
static bool resolve(const char* path, uint32_t* out_ino, struct inode* out)
{
    struct inode cur;
    if (!read_inode(EXT2_ROOT_INO, &cur)) {
        return false;
    }
    uint32_t ino = EXT2_ROOT_INO;
    const char* p = path;
    while (*p) {
        while (*p == '/') {
            p++;
        }
        if (!*p) {
            break;
        }
        const char* start = p;
        while (*p && *p != '/') {
            p++;
        }
        struct lookup l = {
                .name = start, .namelen = (uint32_t)(p - start), .found = 0};
        walk_dir(&cur, match_entry, &l);
        if (l.found == 0 || !read_inode(l.found, &cur)) {
            return false;
        }
        ino = l.found;
    }
    *out_ino = ino;
    *out = cur;
    return true;
}

// --- Public API ------------------------------------------------------------

bool ext2_mount(void)
{
    mounted = false;
    if (!ata_present()) {
        return false;
    }
    // The superblock sits at byte offset 1024 -> LBA 2, and is 1024 bytes.
    uint8_t buf[1024];
    if (!ata_read(2, 2, buf)) {
        return false;
    }
    memcpy(&sb, buf, sizeof sb);
    if (sb.magic != EXT2_MAGIC) {
        return false;
    }
    block_size = 1024u << sb.log_block_size;
    // rev 0 has no inode_size field and uses a fixed 128-byte inode.
    inode_size = (sb.rev_level >= 1 && sb.inode_size) ? sb.inode_size : 128;
    if (inode_size < sizeof(struct inode)) {
        inode_size = sizeof(struct inode);
    }
    has_filetype = (sb.feature_incompat & INCOMPAT_FILETYPE) != 0;
    groups = (sb.blocks_count - sb.first_data_block + sb.blocks_per_group - 1) /
             sb.blocks_per_group;
    mounted = true;
    return true;
}

bool ext2_mounted(void)
{
    return mounted;
}

void* ext2_read_path(const char* path, size_t* size)
{
    if (!mounted) {
        return NULL;
    }
    uint32_t ino;
    struct inode in;
    if (!resolve(path, &ino, &in)) {
        return NULL;
    }
    if ((in.mode & S_IFMT) != S_IFREG) {
        return NULL;
    }
    return read_file(&in, inode_file_size(&in), size);
}

bool ext2_stat_path(const char* path, ext2_stat* out)
{
    if (!mounted) {
        return false;
    }
    uint32_t ino;
    struct inode in;
    if (!resolve(path, &ino, &in)) {
        return false;
    }
    out->size = inode_file_size(&in);
    out->inode = ino;
    out->mode = in.mode;
    out->is_dir = (in.mode & S_IFMT) == S_IFDIR;
    return true;
}

struct emit_ctx {
    void (*emit)(void* ctx, const char* name, uint32_t inode, uint8_t type);
    void* ctx;
    int count;
};

static bool emit_entry(void* ctx, uint32_t ino, uint8_t type, const char* name,
                       uint32_t namelen)
{
    struct emit_ctx* e = ctx;
    // Copy the name out to a NUL-terminated stack buffer for the callback.
    char buf[256];
    if (namelen > sizeof(buf) - 1) {
        namelen = sizeof(buf) - 1;
    }
    memcpy(buf, name, namelen);
    buf[namelen] = '\0';
    e->emit(e->ctx, buf, ino, type);
    e->count++;
    return false;
}

int ext2_list(const char* path,
              void (*emit)(void* ctx, const char* name, uint32_t inode,
                           uint8_t type),
              void* ctx)
{
    if (!mounted) {
        return -1;
    }
    uint32_t ino;
    struct inode in;
    if (!resolve(path, &ino, &in)) {
        return -1;
    }
    if ((in.mode & S_IFMT) != S_IFDIR) {
        return -1;
    }
    struct emit_ctx e = {.emit = emit, .ctx = ctx, .count = 0};
    walk_dir(&in, emit_entry, &e);
    return e.count;
}

// --- Write support ---------------------------------------------------------
// Every operation is read-modify-write straight to disk (no cache, no journal),
// keeping the primary superblock, group descriptors, bitmaps, inode table, and
// directory data consistent.

static uint32_t roundup4(uint32_t x)
{
    return (x + 3) & ~3u;
}

static bool write_block(uint32_t blk, const void* buf)
{
    uint32_t spb = block_size / 512;
    return ata_write((uint64_t)blk * spb, spb, buf);
}

static bool write_group_desc(uint32_t group, const struct group_desc* gd)
{
    uint32_t table = sb.first_data_block + 1;
    uint32_t byte = group * (uint32_t)sizeof(struct group_desc);
    uint8_t* blk = new (mem(), uint8_t, block_size);
    bool ok = read_block(table + byte / block_size, blk);
    if (ok) {
        memcpy(blk + byte % block_size, gd, sizeof *gd);
        ok = write_block(table + byte / block_size, blk);
    }
    heap_free(heap_default(), blk);
    return ok;
}

static bool write_inode(uint32_t ino, const struct inode* in)
{
    uint32_t group = (ino - 1) / sb.inodes_per_group;
    uint32_t index = (ino - 1) % sb.inodes_per_group;
    struct group_desc gd;
    if (!read_group_desc(group, &gd)) {
        return false;
    }
    uint32_t byte = index * inode_size;
    uint32_t b = gd.inode_table + byte / block_size;
    uint8_t* blk = new (mem(), uint8_t, block_size);
    bool ok = read_block(b, blk);
    if (ok) {
        memcpy(blk + byte % block_size, in, sizeof *in);
        ok = write_block(b, blk);
    }
    heap_free(heap_default(), blk);
    return ok;
}

// Write the cached superblock back, preserving the bytes of the 1 KiB block we
// don't model.
static bool write_superblock(void)
{
    uint8_t buf[1024];
    if (!ata_read(2, 2, buf)) {
        return false;
    }
    memcpy(buf, &sb, sizeof sb);
    return ata_write(2, 2, buf);
}

// Set the first free bit (< nbits) in a bitmap block; return its index or -1.
static int bitmap_alloc(uint32_t bitmap_block, uint32_t nbits)
{
    uint8_t* bm = new (mem(), uint8_t, block_size);
    int found = -1;
    if (read_block(bitmap_block, bm)) {
        for (uint32_t i = 0; i < nbits; i++) {
            if (!(bm[i >> 3] & (1u << (i & 7)))) {
                bm[i >> 3] |= (uint8_t)(1u << (i & 7));
                found = (int)i;
                break;
            }
        }
        if (found >= 0) {
            write_block(bitmap_block, bm);
        }
    }
    heap_free(heap_default(), bm);
    return found;
}

static void bitmap_free(uint32_t bitmap_block, uint32_t bit)
{
    uint8_t* bm = new (mem(), uint8_t, block_size);
    if (read_block(bitmap_block, bm)) {
        bm[bit >> 3] &= (uint8_t)~(1u << (bit & 7));
        write_block(bitmap_block, bm);
    }
    heap_free(heap_default(), bm);
}

// Allocate a zeroed data block; 0 on failure.
static uint32_t alloc_block(void)
{
    for (uint32_t g = 0; g < groups; g++) {
        struct group_desc gd;
        if (!read_group_desc(g, &gd) || gd.free_blocks_count == 0) {
            continue;
        }
        int bit = bitmap_alloc(gd.block_bitmap, sb.blocks_per_group);
        if (bit < 0) {
            continue;
        }
        gd.free_blocks_count--;
        write_group_desc(g, &gd);
        sb.free_blocks_count--;
        write_superblock();
        uint32_t blk =
                sb.first_data_block + g * sb.blocks_per_group + (uint32_t)bit;
        uint8_t* zero = new (mem(), uint8_t, block_size);
        memset(zero, 0, block_size);
        write_block(blk, zero);
        heap_free(heap_default(), zero);
        return blk;
    }
    return 0;
}

static void free_block(uint32_t blk)
{
    if (blk < sb.first_data_block) {
        return;
    }
    uint32_t rel = blk - sb.first_data_block;
    uint32_t g = rel / sb.blocks_per_group;
    if (g >= groups) {
        return;
    }
    struct group_desc gd;
    if (!read_group_desc(g, &gd)) {
        return;
    }
    bitmap_free(gd.block_bitmap, rel % sb.blocks_per_group);
    gd.free_blocks_count++;
    write_group_desc(g, &gd);
    sb.free_blocks_count++;
    write_superblock();
}

static uint32_t alloc_inode(bool is_dir)
{
    for (uint32_t g = 0; g < groups; g++) {
        struct group_desc gd;
        if (!read_group_desc(g, &gd) || gd.free_inodes_count == 0) {
            continue;
        }
        int bit = bitmap_alloc(gd.inode_bitmap, sb.inodes_per_group);
        if (bit < 0) {
            continue;
        }
        gd.free_inodes_count--;
        if (is_dir) {
            gd.used_dirs_count++;
        }
        write_group_desc(g, &gd);
        sb.free_inodes_count--;
        write_superblock();
        return g * sb.inodes_per_group + (uint32_t)bit + 1;
    }
    return 0;
}

static void free_inode(uint32_t ino, bool was_dir)
{
    uint32_t g = (ino - 1) / sb.inodes_per_group;
    if (g >= groups) {
        return;
    }
    struct group_desc gd;
    if (!read_group_desc(g, &gd)) {
        return;
    }
    bitmap_free(gd.inode_bitmap, (ino - 1) % sb.inodes_per_group);
    gd.free_inodes_count++;
    if (was_dir && gd.used_dirs_count > 0) {
        gd.used_dirs_count--;
    }
    write_group_desc(g, &gd);
    sb.free_inodes_count++;
    write_superblock();
}

// Map logical block `n` of `in` to physical `blk`, allocating the single-
// indirect block on demand. False if beyond direct + single indirect.
static bool set_block_of(struct inode* in, uint32_t n, uint32_t blk)
{
    uint32_t per = block_size / 4;
    if (n < 12) {
        in->block[n] = blk;
        in->blocks += block_size / 512;
        return true;
    }
    n -= 12;
    if (n >= per) {
        return false;
    }
    if (in->block[12] == 0) {
        uint32_t ind = alloc_block();
        if (ind == 0) {
            return false;
        }
        in->block[12] = ind;
        in->blocks += block_size / 512; // the indirect block itself
    }
    uint32_t* tmp = new (mem(), uint32_t, per);
    bool ok = read_block(in->block[12], tmp);
    if (ok) {
        tmp[n] = blk;
        ok = write_block(in->block[12], tmp);
        in->blocks += block_size / 512;
    }
    heap_free(heap_default(), tmp);
    return ok;
}

// Free every data + metadata block of an inode (direct, single, and double
// indirect — double only ever appears on files mke2fs created).
static void free_all_blocks(struct inode* in)
{
    uint32_t per = block_size / 4;
    for (int i = 0; i < 12; i++) {
        if (in->block[i]) {
            free_block(in->block[i]);
        }
        in->block[i] = 0;
    }
    uint32_t* tmp = new (mem(), uint32_t, per);
    if (in->block[12]) {
        if (read_block(in->block[12], tmp)) {
            for (uint32_t i = 0; i < per; i++) {
                if (tmp[i]) {
                    free_block(tmp[i]);
                }
            }
        }
        free_block(in->block[12]);
        in->block[12] = 0;
    }
    if (in->block[13]) {
        uint32_t* l1 = new (mem(), uint32_t, per);
        if (read_block(in->block[13], l1)) {
            for (uint32_t i = 0; i < per; i++) {
                if (!l1[i]) {
                    continue;
                }
                if (read_block(l1[i], tmp)) {
                    for (uint32_t j = 0; j < per; j++) {
                        if (tmp[j]) {
                            free_block(tmp[j]);
                        }
                    }
                }
                free_block(l1[i]);
            }
        }
        heap_free(heap_default(), l1);
        free_block(in->block[13]);
        in->block[13] = 0;
    }
    heap_free(heap_default(), tmp);
    in->blocks = 0;
}

static uint32_t dir_find(struct inode* dir, const char* name, uint32_t namelen)
{
    struct lookup l = {.name = name, .namelen = namelen, .found = 0};
    walk_dir(dir, match_entry, &l);
    return l.found;
}

// Add an entry to directory `dir` (its inode `dir_ino`); grows the directory a
// block if needed (updating and rewriting the inode).
static bool dir_add(uint32_t dir_ino, struct inode* dir, const char* name,
                    uint32_t namelen, uint32_t child, uint8_t type)
{
    uint32_t need = roundup4(8 + namelen);
    uint8_t* block = new (mem(), uint8_t, block_size);
    uint64_t size = inode_file_size(dir);
    bool done = false;
    for (uint32_t bi = 0; (uint64_t)bi * block_size < size && !done; bi++) {
        uint32_t phys = block_of(dir, bi);
        if (phys == 0 || !read_block(phys, block)) {
            continue;
        }
        uint32_t off = 0;
        while (off + 8 <= block_size) {
            struct dirent* de = (struct dirent*)(block + off);
            if (de->rec_len < 8) {
                break;
            }
            uint32_t used = de->inode == 0 ? 0 : roundup4(8 + de->name_len);
            if (de->rec_len - used >= need) {
                uint32_t total = de->rec_len;
                struct dirent* nd;
                if (de->inode == 0) {
                    nd = de; // reuse the whole empty slot
                } else {
                    de->rec_len = (uint16_t)used;
                    nd = (struct dirent*)(block + off + used);
                    total -= used;
                }
                nd->inode = child;
                nd->rec_len = (uint16_t)total;
                nd->name_len = (uint8_t)namelen;
                nd->file_type = has_filetype ? type : 0;
                memcpy(nd->name, name, namelen);
                write_block(phys, block);
                done = true;
                break;
            }
            off += de->rec_len;
        }
    }
    if (!done) {
        uint32_t blk = alloc_block();
        if (blk != 0 && set_block_of(dir, (uint32_t)(size / block_size), blk)) {
            memset(block, 0, block_size);
            struct dirent* nd = (struct dirent*)block;
            nd->inode = child;
            nd->rec_len = (uint16_t)block_size;
            nd->name_len = (uint8_t)namelen;
            nd->file_type = has_filetype ? type : 0;
            memcpy(nd->name, name, namelen);
            write_block(blk, block);
            dir->size += block_size;
            write_inode(dir_ino, dir);
            done = true;
        }
    }
    heap_free(heap_default(), block);
    return done;
}

// Remove `name` from directory `dir`: absorb its record into the previous entry
// (or clear the inode field if it is first in its block).
static bool dir_remove(struct inode* dir, const char* name, uint32_t namelen)
{
    uint8_t* block = new (mem(), uint8_t, block_size);
    uint64_t size = inode_file_size(dir);
    bool done = false;
    for (uint32_t bi = 0; (uint64_t)bi * block_size < size && !done; bi++) {
        uint32_t phys = block_of(dir, bi);
        if (phys == 0 || !read_block(phys, block)) {
            continue;
        }
        uint32_t off = 0, prev = 0;
        bool have_prev = false;
        while (off + 8 <= block_size) {
            struct dirent* de = (struct dirent*)(block + off);
            if (de->rec_len < 8) {
                break;
            }
            if (de->inode != 0 && de->name_len == namelen &&
                str_eq(de->name, name, namelen)) {
                if (have_prev) {
                    ((struct dirent*)(block + prev))->rec_len += de->rec_len;
                } else {
                    de->inode = 0;
                }
                write_block(phys, block);
                done = true;
                break;
            }
            prev = off;
            have_prev = true;
            off += de->rec_len;
        }
    }
    heap_free(heap_default(), block);
    return done;
}

// Split `path` into its parent directory (must exist) and the final component.
static bool resolve_parent(const char* path, uint32_t* parent_ino,
                           struct inode* parent, const char** base,
                           uint32_t* baselen)
{
    size_t len = 0;
    while (path[len]) {
        len++;
    }
    while (len > 0 && path[len - 1] == '/') {
        len--; // ignore trailing slashes
    }
    if (len == 0) {
        return false; // empty or root
    }
    size_t slash = len;
    while (slash > 0 && path[slash - 1] != '/') {
        slash--;
    }
    *base = path + slash;
    *baselen = (uint32_t)(len - slash);
    if (*baselen == 0 || *baselen > 255) {
        return false;
    }
    char pbuf[256];
    if (slash == 0) {
        pbuf[0] = '/';
        pbuf[1] = '\0';
    } else {
        if (slash >= sizeof pbuf) {
            return false;
        }
        memcpy(pbuf, path, slash);
        pbuf[slash] = '\0';
    }
    if (!resolve(pbuf, parent_ino, parent)) {
        return false;
    }
    return (parent->mode & S_IFMT) == S_IFDIR;
}

bool ext2_write_file(const char* path, const void* data, size_t size)
{
    if (!mounted) {
        return false;
    }
    uint32_t parent_ino;
    struct inode parent;
    const char* base;
    uint32_t baselen;
    if (!resolve_parent(path, &parent_ino, &parent, &base, &baselen)) {
        return false;
    }

    uint32_t ino = dir_find(&parent, base, baselen);
    struct inode in;
    if (ino != 0) {
        if (!read_inode(ino, &in) || (in.mode & S_IFMT) != S_IFREG) {
            return false; // won't overwrite a directory
        }
        free_all_blocks(&in); // truncate to empty, then rewrite
        in.size = 0;
        in.dir_acl = 0;
    } else {
        ino = alloc_inode(false);
        if (ino == 0) {
            return false;
        }
        memset(&in, 0, sizeof in);
        in.mode = S_IFREG | 0644;
        in.links_count = 1;
        if (!dir_add(parent_ino, &parent, base, baselen, ino, FT_REG)) {
            free_inode(ino, false);
            return false;
        }
    }

    const uint8_t* p = data;
    uint32_t nblocks = (uint32_t)((size + block_size - 1) / block_size);
    uint8_t* buf = new (mem(), uint8_t, block_size);
    bool ok = true;
    for (uint32_t bi = 0; bi < nblocks; bi++) {
        uint32_t blk = alloc_block();
        if (blk == 0) {
            ok = false;
            break;
        }
        uint32_t chunk = (uint32_t)(size - (uint64_t)bi * block_size);
        if (chunk > block_size) {
            chunk = block_size;
        }
        memset(buf, 0, block_size);
        memcpy(buf, p + (uint64_t)bi * block_size, chunk);
        write_block(blk, buf);
        if (!set_block_of(&in, bi, blk)) {
            free_block(blk);
            ok = false;
            break;
        }
    }
    heap_free(heap_default(), buf);
    in.size = (uint32_t)size;
    write_inode(ino, &in);
    return ok;
}

bool ext2_mkdir(const char* path)
{
    if (!mounted) {
        return false;
    }
    uint32_t parent_ino;
    struct inode parent;
    const char* base;
    uint32_t baselen;
    if (!resolve_parent(path, &parent_ino, &parent, &base, &baselen)) {
        return false;
    }
    if (dir_find(&parent, base, baselen) != 0) {
        return false; // already exists
    }

    uint32_t ino = alloc_inode(true);
    if (ino == 0) {
        return false;
    }
    uint32_t blk = alloc_block();
    if (blk == 0) {
        free_inode(ino, true);
        return false;
    }

    // Initial directory block: "." -> self, ".." -> parent.
    uint8_t* block = new (mem(), uint8_t, block_size);
    memset(block, 0, block_size);
    struct dirent* dot = (struct dirent*)block;
    dot->inode = ino;
    dot->name_len = 1;
    dot->file_type = has_filetype ? FT_DIR : 0;
    dot->rec_len = (uint16_t)roundup4(8 + 1);
    dot->name[0] = '.';
    struct dirent* dd = (struct dirent*)(block + dot->rec_len);
    dd->inode = parent_ino;
    dd->name_len = 2;
    dd->file_type = has_filetype ? FT_DIR : 0;
    dd->rec_len = (uint16_t)(block_size - dot->rec_len);
    dd->name[0] = '.';
    dd->name[1] = '.';
    write_block(blk, block);
    heap_free(heap_default(), block);

    struct inode in;
    memset(&in, 0, sizeof in);
    in.mode = S_IFDIR | 0755;
    in.links_count = 2; // "." and the entry we add to the parent
    in.size = block_size;
    in.blocks = block_size / 512;
    in.block[0] = blk;
    write_inode(ino, &in);

    if (!dir_add(parent_ino, &parent, base, baselen, ino, FT_DIR)) {
        free_block(blk);
        free_inode(ino, true);
        return false;
    }
    parent.links_count++; // the new dir's ".." links back to the parent
    write_inode(parent_ino, &parent);
    return true;
}

static bool count_cb(void* ctx, uint32_t ino, uint8_t type, const char* name,
                     uint32_t namelen)
{
    (void)ino;
    (void)type;
    if (namelen == 1 && name[0] == '.') {
        return false;
    }
    if (namelen == 2 && name[0] == '.' && name[1] == '.') {
        return false;
    }
    *(int*)ctx += 1;
    return true; // a real entry: stop early
}

bool ext2_remove(const char* path)
{
    if (!mounted) {
        return false;
    }
    uint32_t parent_ino;
    struct inode parent;
    const char* base;
    uint32_t baselen;
    if (!resolve_parent(path, &parent_ino, &parent, &base, &baselen)) {
        return false;
    }
    uint32_t ino = dir_find(&parent, base, baselen);
    if (ino == 0) {
        return false;
    }
    struct inode in;
    if (!read_inode(ino, &in)) {
        return false;
    }
    bool is_dir = (in.mode & S_IFMT) == S_IFDIR;
    if (is_dir) {
        int n = 0;
        walk_dir(&in, count_cb, &n);
        if (n != 0) {
            return false; // directory not empty
        }
    }

    if (!dir_remove(&parent, base, baselen)) {
        return false;
    }

    if (is_dir) {
        free_all_blocks(&in);
        memset(&in, 0, sizeof in); // a zeroed inode is an unused inode
        write_inode(ino, &in);
        free_inode(ino, true);
        if (parent.links_count > 0) {
            parent.links_count--; // lose the removed dir's ".."
        }
        write_inode(parent_ino, &parent);
    } else {
        if (in.links_count > 0) {
            in.links_count--;
        }
        if (in.links_count == 0) {
            free_all_blocks(&in);
            memset(&in, 0, sizeof in);
            write_inode(ino, &in);
            free_inode(ino, false);
        } else {
            write_inode(ino, &in);
        }
    }
    return true;
}
