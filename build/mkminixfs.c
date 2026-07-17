// mkminixfs — populate a Minix v1 (30-char, magic 0x138F) filesystem image
// entirely from userspace: no loopback mount, no sudo. Run mkfs.minix first to
// lay down an empty filesystem, then feed this tool a manifest on stdin:
//
//   D <path>                    make a directory
//   F <path> <hostfile>         copy a host file into the image
//   C <path> <major> <minor>    create a character-device node
//
// Directories are assumed to fit in a single zone (32 entries); files may use
// the 7 direct zones plus one single-indirect block (up to ~519 KB). That
// covers this OS's task binaries and docs. Assumes a little-endian build host
// (x86/arm64), which matches the target.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#define BS 1024
#define NAMELEN 30
#define DIRENT_SZ 32
#define INODE_SZ 32
#define NZONES 7
#define PTRS_PER_ZONE 512
#define ROOT_INODE 1
#define MINIX_MAGIC 0x138F

#define IFDIR 0040000
#define IFREG 0100000
#define IFCHR 0020000

#pragma pack(push, 1)
typedef struct {
    uint16_t ninodes, nzones, imap_blocks, zmap_blocks, firstdatazone, log_zone;
    uint32_t max_size;
    uint16_t magic;
} superblock;

typedef struct {
    uint16_t mode, uid;
    uint32_t size, time;
    uint8_t gid, nlinks;
    uint16_t zones[NZONES];
    uint16_t indirect, double_indirect;
} minode; // 32 bytes on disk

typedef struct {
    uint16_t inode;
    char name[NAMELEN];
} dirent32; // 32 bytes on disk
#pragma pack(pop)

static FILE* img;
static superblock sb;
static uint32_t inode_table_block;

static void die(const char* msg)
{
    fprintf(stderr, "mkminixfs: %s\n", msg);
    exit(1);
}

static void rd(void* buf, long off, size_t n)
{
    if (fseek(img, off, SEEK_SET) || fread(buf, 1, n, img) != n) {
        die("read failed");
    }
}

static void wr(const void* buf, long off, size_t n)
{
    if (fseek(img, off, SEEK_SET) || fwrite(buf, 1, n, img) != n) {
        die("write failed");
    }
}

static void read_inode(uint16_t ino, minode* out)
{
    rd(out, (long)inode_table_block * BS + (long)(ino - 1) * INODE_SZ,
       sizeof(*out));
}

static void write_inode(uint16_t ino, const minode* in)
{
    wr(in, (long)inode_table_block * BS + (long)(ino - 1) * INODE_SZ,
       sizeof(*in));
}

static int bit_get(uint32_t start_block, uint32_t bit)
{
    uint8_t byte;
    rd(&byte, (long)start_block * BS + bit / 8, 1);
    return (byte >> (bit % 8)) & 1;
}

static void bit_set(uint32_t start_block, uint32_t bit)
{
    uint8_t byte;
    long off = (long)start_block * BS + bit / 8;
    rd(&byte, off, 1);
    byte |= (1 << (bit % 8));
    wr(&byte, off, 1);
}

static uint32_t zmap_block(void)
{
    return 2 + sb.imap_blocks;
}

static uint16_t alloc_inode(void)
{
    for (uint32_t b = 1; b <= sb.ninodes; b++) {
        if (!bit_get(2, b)) {
            bit_set(2, b);
            return (uint16_t)b;
        }
    }
    die("out of inodes");
    return 0;
}

// Allocates a data zone, zeroes it, and returns its absolute block number.
static uint16_t alloc_zone(void)
{
    static uint8_t zero[BS];
    uint32_t maxbit = sb.nzones - sb.firstdatazone + 1;
    for (uint32_t b = 1; b <= maxbit; b++) {
        if (!bit_get(zmap_block(), b)) {
            bit_set(zmap_block(), b);
            uint16_t zone = (uint16_t)(sb.firstdatazone + b - 1);
            wr(zero, (long)zone * BS, BS);
            return zone;
        }
    }
    die("out of zones");
    return 0;
}

static uint16_t dir_lookup(uint16_t dir, const char* name)
{
    minode di;
    read_inode(dir, &di);
    for (int z = 0; z < NZONES && di.zones[z]; z++) {
        dirent32 ents[BS / DIRENT_SZ];
        rd(ents, (long)di.zones[z] * BS, BS);
        for (unsigned i = 0; i < BS / DIRENT_SZ; i++) {
            if (ents[i].inode && strncmp(ents[i].name, name, NAMELEN) == 0) {
                return ents[i].inode;
            }
        }
    }
    return 0;
}

// Appends a (child, name) entry to a single-zone directory, growing its size.
static void dir_add(uint16_t dir, uint16_t child, const char* name)
{
    minode di;
    read_inode(dir, &di);
    if (!di.zones[0]) {
        di.zones[0] = alloc_zone();
    }
    dirent32 ents[BS / DIRENT_SZ];
    long zoff = (long)di.zones[0] * BS;
    rd(ents, zoff, BS);
    for (unsigned i = 0; i < BS / DIRENT_SZ; i++) {
        if (ents[i].inode == 0) {
            dirent32 e;
            e.inode = child;
            memset(e.name, 0, NAMELEN);
            memcpy(e.name, name, strnlen(name, NAMELEN));
            wr(&e, zoff + (long)i * DIRENT_SZ, DIRENT_SZ);
            uint32_t used = (i + 1) * DIRENT_SZ;
            if (used > di.size) {
                di.size = used;
            }
            write_inode(dir, &di);
            return;
        }
    }
    die("directory full (needs more than one zone)");
}

static uint16_t make_dir(uint16_t parent, const char* name)
{
    uint16_t ino = alloc_inode();
    uint16_t zone = alloc_zone();
    minode di;
    memset(&di, 0, sizeof(di));
    di.mode = IFDIR | 0755;
    di.nlinks = 2;
    di.size = 2 * DIRENT_SZ;
    di.zones[0] = zone;
    write_inode(ino, &di);

    dirent32 e;
    memset(&e, 0, sizeof(e));
    e.inode = ino;
    strncpy(e.name, ".", NAMELEN);
    wr(&e, (long)zone * BS, DIRENT_SZ);
    memset(&e, 0, sizeof(e));
    e.inode = parent;
    strncpy(e.name, "..", NAMELEN);
    wr(&e, (long)zone * BS + DIRENT_SZ, DIRENT_SZ);

    dir_add(parent, ino, name);
    minode pi;
    read_inode(parent, &pi);
    pi.nlinks++;
    write_inode(parent, &pi);
    return ino;
}

static uint16_t make_file(uint16_t parent, const char* name,
                          const uint8_t* data, uint32_t size)
{
    uint16_t ino = alloc_inode();
    minode fi;
    memset(&fi, 0, sizeof(fi));
    fi.mode = IFREG | 0644;
    fi.nlinks = 1;
    fi.size = size;

    uint32_t nzones = (size + BS - 1) / BS;
    uint32_t zi = 0;
    for (; zi < nzones && zi < NZONES; zi++) {
        uint16_t z = alloc_zone();
        fi.zones[zi] = z;
        uint32_t off = zi * BS;
        uint32_t n = (size - off < BS) ? size - off : BS;
        wr(data + off, (long)z * BS, n);
    }
    if (nzones > NZONES) {
        uint16_t ind = alloc_zone();
        fi.indirect = ind;
        uint16_t ptrs[PTRS_PER_ZONE];
        memset(ptrs, 0, sizeof(ptrs));
        for (uint32_t k = 0; zi < nzones; zi++, k++) {
            if (k >= PTRS_PER_ZONE) {
                die("file too big (needs double indirect)");
            }
            uint16_t z = alloc_zone();
            ptrs[k] = z;
            uint32_t off = zi * BS;
            uint32_t n = (size - off < BS) ? size - off : BS;
            wr(data + off, (long)z * BS, n);
        }
        wr(ptrs, (long)ind * BS, BS);
    }
    write_inode(ino, &fi);
    dir_add(parent, ino, name);
    return ino;
}

static uint16_t make_char(uint16_t parent, const char* name, int major,
                          int minor)
{
    uint16_t ino = alloc_inode();
    minode ci;
    memset(&ci, 0, sizeof(ci));
    ci.mode = IFCHR | 0666;
    ci.nlinks = 1;
    // The kernel decodes major = zones[0] & 0xFF, minor = zones[0] >> 8.
    ci.zones[0] = (uint16_t)(((minor & 0xFF) << 8) | (major & 0xFF));
    write_inode(ino, &ci);
    dir_add(parent, ino, name);
    return ino;
}

// Resolves the parent directory of an absolute path and copies the final
// component into leaf[]. Every intermediate directory must already exist.
static uint16_t resolve_parent(const char* path, char* leaf)
{
    if (path[0] != '/') {
        die("paths must be absolute");
    }
    uint16_t cur = ROOT_INODE;
    const char* p = path + 1;
    for (;;) {
        const char* slash = strchr(p, '/');
        if (!slash) {
            size_t l = strnlen(p, NAMELEN);
            memcpy(leaf, p, l);
            leaf[l] = '\0';
            return cur;
        }
        char comp[NAMELEN + 1];
        size_t len = (size_t)(slash - p);
        if (len > NAMELEN) {
            len = NAMELEN;
        }
        memcpy(comp, p, len);
        comp[len] = '\0';
        cur = dir_lookup(cur, comp);
        if (!cur) {
            die("parent directory does not exist");
        }
        p = slash + 1;
    }
}

static uint8_t* read_host_file(const char* path, uint32_t* out_size)
{
    FILE* f = fopen(path, "rb");
    if (!f) {
        die("cannot open host file");
    }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t* buf = malloc(sz ? sz : 1);
    if (sz && fread(buf, 1, sz, f) != (size_t)sz) {
        die("cannot read host file");
    }
    fclose(f);
    *out_size = (uint32_t)sz;
    return buf;
}

int main(int argc, char** argv)
{
    if (argc != 2) {
        fprintf(stderr, "usage: %s <image>   (manifest on stdin)\n", argv[0]);
        return 1;
    }
    img = fopen(argv[1], "r+b");
    if (!img) {
        die("cannot open image");
    }
    rd(&sb, 1024, sizeof(sb));
    if (sb.magic != MINIX_MAGIC) {
        die("not a Minix v1 (0x138F) filesystem");
    }
    inode_table_block = 2 + sb.imap_blocks + sb.zmap_blocks;

    char line[8192];
    while (fgets(line, sizeof(line), stdin)) {
        char op;
        char path[4096];
        char rest[4096];
        int n = sscanf(line, " %c %4095s %4095[^\n]", &op, path, rest);
        if (n < 2 || op == '#') {
            continue;
        }
        char leaf[NAMELEN + 1];
        uint16_t parent = resolve_parent(path, leaf);
        if (op == 'D') {
            make_dir(parent, leaf);
        } else if (op == 'F') {
            uint32_t size;
            uint8_t* data = read_host_file(rest, &size);
            make_file(parent, leaf, data, size);
            free(data);
        } else if (op == 'C') {
            int major = 0, minor = 0;
            sscanf(rest, "%d %d", &major, &minor);
            make_char(parent, leaf, major, minor);
        } else {
            die("unknown manifest op");
        }
    }
    fclose(img);
    return 0;
}
