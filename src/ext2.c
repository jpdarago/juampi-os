#include <ext2.h>
#include <ata.h>
#include <memory.h>
#include <utils.h>

// Read-only ext2. On-disk layout is little-endian and x86-64 is too, so the
// packed structs below map straight onto the bytes read from disk.

#define EXT2_MAGIC 0xEF53
#define EXT2_ROOT_INO 2

#define S_IFMT 0xF000
#define S_IFDIR 0x4000
#define S_IFREG 0x8000

#define INCOMPAT_FILETYPE 0x0002
#define RO_COMPAT_LARGE_FILE 0x0002

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
