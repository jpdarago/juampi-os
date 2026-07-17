// ext2 driver: implements the VFS filesystem ops against the ext2 on-disk
// format (superblock, block groups, inodes, directory entries, indirect
// blocks).

#include <fs_ext2.h>
#include <buffer_cache.h>
#include <memory.h>
#include <dev_table.h>
#include <exception.h>
#include <utils.h>

// The buffer cache works in 1024-byte blocks, so an ext2 block (also 1024)
// maps one-to-one onto a cache block.
static void ext2_read_block(uint32 block, void* buf)
{
    buffered_read_several(block, 1, buf);
}

// --- inode reading ----------------------------------------------------------

static const inode_ops ext2_default_inode_ops = {0};
static const fs_ops ext2_default_file_ops = {0};
static const inode_ops ext2_dir_inode_ops;
static const fs_ops ext2_file_file_ops;
static const fs_ops ext2_dir_file_ops;

static int ext2_init_file_type(inode* data, uint16 mode)
{
    ext2_inode* ei = data->info_disk;
    ushort dev, major, minor;
    switch (mode & EXT2_S_IFMT) {
    case EXT2_S_IFREG:
        data->inode_type = FS_FILE;
        data->i_ops = &ext2_default_inode_ops;
        data->f_ops = &ext2_file_file_ops;
        break;
    case EXT2_S_IFDIR:
        data->inode_type = FS_DIR;
        data->i_ops = &ext2_dir_inode_ops;
        data->f_ops = &ext2_dir_file_ops;
        break;
    case EXT2_S_IFCHR:
        data->inode_type = FS_CHARDEV;
        data->info_cdev = kmalloc(sizeof(char_dev));
        // Same device encoding the kernel expects elsewhere: major in the low
        // byte, minor in the high byte of the first block pointer.
        dev = ei->block[0];
        major = dev & 0xFF;
        minor = (dev >> 8) & 0xFF;
        data->info_cdev->major_number = major;
        data->info_cdev->minor_number = minor;
        data->f_ops = get_fops_table(major, minor);
        data->i_ops = get_iops_table(major, minor);
        break;
    default:
        return -1;
    }
    return 0;
}

static int ext2_read_inode(inode* data)
{
    ext2_fs_data* fs = data->super_block->fs_data;
    uint32 ino = data->inode_number;
    uint32 group = (ino - 1) / fs->inodes_per_group;
    uint32 index = (ino - 1) % fs->inodes_per_group;
    uint32 byte_off = index * fs->inode_size;
    uint32 block = fs->groups[group].inode_table + byte_off / fs->block_size;
    uint32 in_block = byte_off % fs->block_size;

    ext2_inode* ei = data->info_disk;
    buffered_read(block, in_block, sizeof(ext2_inode), ei);

    data->file_size = ei->size;
    data->is_dirty = false;
    if (ext2_init_file_type(data, ei->mode))
        return -1;
    return 0;
}

// --- file data (block mapping) ----------------------------------------------

// Maps the file-relative block index to an absolute block number, following the
// direct, single- and double-indirect pointers. Returns 0 for a hole.
static uint32 ext2_bmap(ext2_inode* ei, uint32 index)
{
    if (index < 12)
        return ei->block[index];
    index -= 12;
    if (index < EXT2_PPB) {
        if (!ei->block[12])
            return 0;
        uint32 ptrs[EXT2_PPB];
        ext2_read_block(ei->block[12], ptrs);
        return ptrs[index];
    }
    index -= EXT2_PPB;
    if (index < (uint32)EXT2_PPB * EXT2_PPB) {
        if (!ei->block[13])
            return 0;
        uint32 ptrs[EXT2_PPB];
        ext2_read_block(ei->block[13], ptrs);
        uint32 mid = ptrs[index / EXT2_PPB];
        if (!mid)
            return 0;
        ext2_read_block(mid, ptrs);
        return ptrs[index % EXT2_PPB];
    }
    return 0; // triple indirect: unsupported (files here never get this big)
}

static int ext2_read_data(inode* ino, uint offset, uint bytes, void* _buf)
{
    char* buf = _buf;
    ext2_inode* ei = ino->info_disk;
    uint bs = EXT2_BLOCK_SIZE;
    uint processed = 0;
    while (bytes > 0) {
        uint32 phys = ext2_bmap(ei, offset / bs);
        uint in_block = offset % bs;
        uint chunk = bs - in_block;
        if (chunk > bytes)
            chunk = bytes;
        if (phys == 0) {
            memset(buf, 0, chunk); // sparse hole
        } else {
            buffered_read(phys, in_block, chunk, buf);
        }
        offset += chunk;
        bytes -= chunk;
        buf += chunk;
        processed += chunk;
    }
    return processed;
}

// --- write support ----------------------------------------------------------

// A dir entry occupies 8 header bytes + the name, rounded up to 4 bytes.
static uint32 ext2_dirent_size(uint name_len)
{
    return (8 + name_len + 3) & ~3u;
}

static void ext2_write_block(uint32 block, void* buf)
{
    buffered_write_several(block, 1, 0, buf);
}

static void ext2_flush_gdt(ext2_fs_data* fs)
{
    buffered_write_several(fs->gd_block, fs->gd_blocks, 0, fs->groups);
}

// The superblock's free-block/free-inode counts live at bytes 12 and 16 of the
// superblock, i.e. block 1 (with 1024-byte blocks) at those offsets.
static void ext2_flush_super(super_block* s)
{
    ext2_fs_data* fs = s->fs_data;
    buffered_write(1, 12, 4, 0, &fs->free_blocks);
    buffered_write(1, 16, 4, 0, &fs->free_inodes);
}

// Allocates and zeroes a data block, returning its absolute number (0 = full).
static uint32 ext2_alloc_block(super_block* s)
{
    ext2_fs_data* fs = s->fs_data;
    uint8 bm[EXT2_BLOCK_SIZE];
    for (uint32 g = 0; g < fs->groups_count; g++) {
        if (fs->groups[g].free_blocks_count == 0)
            continue;
        ext2_read_block(fs->groups[g].block_bitmap, bm);
        for (uint32 b = 0; b < fs->blocks_per_group; b++) {
            if (bm[b >> 3] & (1 << (b & 7)))
                continue;
            bm[b >> 3] |= (1 << (b & 7));
            ext2_write_block(fs->groups[g].block_bitmap, bm);
            fs->groups[g].free_blocks_count--;
            fs->free_blocks--;
            ext2_flush_gdt(fs);
            ext2_flush_super(s);
            uint32 block = fs->first_data_block + g * fs->blocks_per_group + b;
            uint8 zero[EXT2_BLOCK_SIZE];
            memset(zero, 0, EXT2_BLOCK_SIZE);
            ext2_write_block(block, zero);
            return block;
        }
    }
    return 0;
}

static void ext2_free_block(super_block* s, uint32 block)
{
    ext2_fs_data* fs = s->fs_data;
    uint32 rel = block - fs->first_data_block;
    uint32 g = rel / fs->blocks_per_group;
    uint32 b = rel % fs->blocks_per_group;
    uint8 bm[EXT2_BLOCK_SIZE];
    ext2_read_block(fs->groups[g].block_bitmap, bm);
    bm[b >> 3] &= ~(1 << (b & 7));
    ext2_write_block(fs->groups[g].block_bitmap, bm);
    fs->groups[g].free_blocks_count++;
    fs->free_blocks++;
    ext2_flush_gdt(fs);
    ext2_flush_super(s);
}

// Allocates a free inode number (1-based), updating the bitmaps/counts.
static uint32 ext2_alloc_inode_num(super_block* s, bool is_dir)
{
    ext2_fs_data* fs = s->fs_data;
    uint8 bm[EXT2_BLOCK_SIZE];
    for (uint32 g = 0; g < fs->groups_count; g++) {
        if (fs->groups[g].free_inodes_count == 0)
            continue;
        ext2_read_block(fs->groups[g].inode_bitmap, bm);
        for (uint32 b = 0; b < fs->inodes_per_group; b++) {
            if (bm[b >> 3] & (1 << (b & 7)))
                continue;
            bm[b >> 3] |= (1 << (b & 7));
            ext2_write_block(fs->groups[g].inode_bitmap, bm);
            fs->groups[g].free_inodes_count--;
            if (is_dir)
                fs->groups[g].used_dirs_count++;
            fs->free_inodes--;
            ext2_flush_gdt(fs);
            ext2_flush_super(s);
            return g * fs->inodes_per_group + b + 1;
        }
    }
    return 0;
}

// Like ext2_bmap but allocates the data (and indirect) block if missing. Sets
// *changed when it touches the inode's direct pointers or i_blocks count.
static uint32 ext2_bmap_alloc(super_block* s, ext2_inode* ei, uint32 index,
                              bool* changed)
{
    uint32 per_sector = EXT2_BLOCK_SIZE / 512;
    if (index < 12) {
        if (!ei->block[index]) {
            ei->block[index] = ext2_alloc_block(s);
            ei->blocks += per_sector;
            *changed = true;
        }
        return ei->block[index];
    }
    index -= 12;
    if (index < EXT2_PPB) {
        if (!ei->block[12]) {
            ei->block[12] = ext2_alloc_block(s);
            ei->blocks += per_sector;
            *changed = true;
        }
        uint32 ptrs[EXT2_PPB];
        ext2_read_block(ei->block[12], ptrs);
        if (!ptrs[index]) {
            ptrs[index] = ext2_alloc_block(s);
            ei->blocks += per_sector;
            *changed = true;
            ext2_write_block(ei->block[12], ptrs);
        }
        return ptrs[index];
    }
    return 0; // double indirect writes unsupported
}

static int ext2_write_data(inode* ino, uint offset, uint bytes, void* _buf)
{
    char* buf = _buf;
    ext2_inode* ei = ino->info_disk;
    super_block* s = ino->super_block;
    uint processed = 0;
    bool changed = false;
    while (bytes > 0) {
        uint32 phys =
                ext2_bmap_alloc(s, ei, offset / EXT2_BLOCK_SIZE, &changed);
        if (phys == 0)
            break;
        uint in_block = offset % EXT2_BLOCK_SIZE;
        uint chunk = EXT2_BLOCK_SIZE - in_block;
        if (chunk > bytes)
            chunk = bytes;
        buffered_write(phys, in_block, chunk, ino->inode_number, buf);
        offset += chunk;
        bytes -= chunk;
        buf += chunk;
        processed += chunk;
    }
    if (offset > ino->file_size) {
        ino->file_size = offset;
        changed = true;
    }
    if (changed) {
        ei->size = ino->file_size;
        ino->is_dirty = true;
    }
    return processed;
}

// Inserts a (child_ino, name) entry into a directory, splitting slack out of an
// existing entry or extending the directory by a block.
static int ext2_dir_add(inode* dir, uint32 child_ino, char* name, uint8 ftype)
{
    ext2_inode* dei = dir->info_disk;
    super_block* s = dir->super_block;
    uint name_len = strlen(name);
    uint need = ext2_dirent_size(name_len);
    uint8 blk[EXT2_BLOCK_SIZE];

    for (uint off = 0; off < dir->file_size; off += EXT2_BLOCK_SIZE) {
        bool changed = false;
        uint32 phys = ext2_bmap_alloc(s, dei, off / EXT2_BLOCK_SIZE, &changed);
        ext2_read_block(phys, blk);
        uint p = 0;
        while (p < EXT2_BLOCK_SIZE) {
            ext2_dir_entry_head* de = (ext2_dir_entry_head*)(blk + p);
            uint rl = de->rec_len;
            if (rl < 8)
                break;
            uint actual = de->inode ? ext2_dirent_size(de->name_len) : 0;
            if (rl - actual >= need) {
                ext2_dir_entry_head* nde;
                uint newrl;
                if (de->inode == 0) {
                    nde = de;
                    newrl = rl;
                } else {
                    de->rec_len = actual;
                    nde = (ext2_dir_entry_head*)(blk + p + actual);
                    newrl = rl - actual;
                }
                nde->inode = child_ino;
                nde->rec_len = newrl;
                nde->name_len = name_len;
                nde->file_type = ftype;
                memcpy((char*)nde + 8, name, name_len);
                ext2_write_block(phys, blk);
                return 0;
            }
            p += rl;
        }
    }

    // No room: give the directory another block.
    bool changed = false;
    uint32 phys =
            ext2_bmap_alloc(s, dei, dir->file_size / EXT2_BLOCK_SIZE, &changed);
    if (phys == 0)
        return -ENOSPACE;
    memset(blk, 0, EXT2_BLOCK_SIZE);
    ext2_dir_entry_head* nde = (ext2_dir_entry_head*)blk;
    nde->inode = child_ino;
    nde->rec_len = EXT2_BLOCK_SIZE;
    nde->name_len = name_len;
    nde->file_type = ftype;
    memcpy((char*)blk + 8, name, name_len);
    ext2_write_block(phys, blk);
    dir->file_size += EXT2_BLOCK_SIZE;
    dei->size = dir->file_size;
    dir->is_dirty = true;
    return 0;
}

// Frees a file's data blocks and resets it to zero length.
static void ext2_truncate(inode* ino)
{
    ext2_inode* ei = ino->info_disk;
    super_block* s = ino->super_block;
    for (int i = 0; i < 12; i++) {
        if (ei->block[i]) {
            ext2_free_block(s, ei->block[i]);
            ei->block[i] = 0;
        }
    }
    if (ei->block[12]) {
        uint32 ptrs[EXT2_PPB];
        ext2_read_block(ei->block[12], ptrs);
        for (uint32 i = 0; i < EXT2_PPB; i++)
            if (ptrs[i])
                ext2_free_block(s, ptrs[i]);
        ext2_free_block(s, ei->block[12]);
        ei->block[12] = 0;
    }
    ei->blocks = 0;
    ei->size = 0;
    ino->file_size = 0;
    ino->is_dirty = true;
}

static int ext2_create(inode* parent, char* name)
{
    super_block* s = parent->super_block;
    uint32 ino = ext2_alloc_inode_num(s, false);
    if (!ino)
        return -EINODES;

    inode* ni = s->ops->alloc_inode(s);
    ni->inode_number = ino;
    ni->file_size = 0;
    ni->inode_type = FS_FILE;
    ext2_inode* ei = ni->info_disk;
    memset(ei, 0, sizeof(*ei));
    ei->mode = EXT2_S_IFREG | 0644;
    ei->links_count = 1;
    ni->is_dirty = true;
    s->ops->write_inode(ni);
    s->ops->release_inode(ni);

    int r = ext2_dir_add(parent, ino, name, EXT2_FT_REG_FILE);
    parent->super_block->ops->write_inode(parent);
    buffers_flush_all();
    return r;
}

static int ext2_mkdir(inode* parent, char* name)
{
    super_block* s = parent->super_block;
    uint32 ino = ext2_alloc_inode_num(s, true);
    if (!ino)
        return -EINODES;
    uint32 dblock = ext2_alloc_block(s);
    if (!dblock)
        return -ENOSPACE;

    // The new directory's single block holds "." and "..".
    uint8 blk[EXT2_BLOCK_SIZE];
    memset(blk, 0, EXT2_BLOCK_SIZE);
    ext2_dir_entry_head* dot = (ext2_dir_entry_head*)blk;
    dot->inode = ino;
    dot->rec_len = ext2_dirent_size(1);
    dot->name_len = 1;
    dot->file_type = EXT2_FT_DIR;
    blk[8] = '.';
    ext2_dir_entry_head* dotdot = (ext2_dir_entry_head*)(blk + dot->rec_len);
    dotdot->inode = parent->inode_number;
    dotdot->rec_len = EXT2_BLOCK_SIZE - dot->rec_len;
    dotdot->name_len = 2;
    dotdot->file_type = EXT2_FT_DIR;
    ((char*)dotdot)[8] = '.';
    ((char*)dotdot)[9] = '.';
    ext2_write_block(dblock, blk);

    inode* ni = s->ops->alloc_inode(s);
    ni->inode_number = ino;
    ni->file_size = EXT2_BLOCK_SIZE;
    ni->inode_type = FS_DIR;
    ext2_inode* ei = ni->info_disk;
    memset(ei, 0, sizeof(*ei));
    ei->mode = EXT2_S_IFDIR | 0755;
    ei->links_count = 2; // "." and the entry we are about to add
    ei->block[0] = dblock;
    ei->blocks = EXT2_BLOCK_SIZE / 512;
    ni->is_dirty = true;
    s->ops->write_inode(ni);
    s->ops->release_inode(ni);

    int r = ext2_dir_add(parent, ino, name, EXT2_FT_DIR);
    // The new "..": bump the parent's link count.
    ext2_inode* pei = parent->info_disk;
    pei->links_count++;
    parent->is_dirty = true;
    s->ops->write_inode(parent);
    buffers_flush_all();
    return r;
}

static int ext2_flush(file_object* f)
{
    if (f->inode->is_dirty)
        f->inode->super_block->ops->write_inode(f->inode);
    buffers_flush_all();
    return 0;
}

// --- file operations --------------------------------------------------------

static int ext2_open_file(inode* ino, file_object* file)
{
    if (ino == NULL)
        return -EINVINODE;
    file->inode = ino;
    file->f_ops = ino->f_ops;
    file->file_read_offset = 0;
    if (file->access_mode & FS_WR) {
        if (ino->inode_type == FS_DIR)
            return -EINVTYPE;
        file->file_write_offset = ino->file_size; // append by default
        if (file->flags & FS_TRUNC) {
            file->file_write_offset = 0;
            ext2_truncate(ino);
        }
    }
    return 0;
}

static int ext2_read_file(file_object* file, uint bytes, void* buffer)
{
    if (!(file->access_mode & FS_RDBIT))
        return -EPERMS;
    uint offset = file->file_read_offset;
    uint remaining = file->inode->file_size - offset;
    if (bytes > remaining)
        bytes = remaining;
    int read = ext2_read_data(file->inode, offset, bytes, buffer);
    file->file_read_offset += read;
    return read;
}

static int ext2_write_file(file_object* file, uint bytes, void* buffer)
{
    if (!(file->access_mode & FS_WRBIT))
        return -EPERMS;
    int written = ext2_write_data(file->inode, file->file_write_offset, bytes,
                                  buffer);
    if (written > 0)
        file->file_write_offset += written;
    return written;
}

static int ext2_close_file(file_object* f)
{
    f->inode->super_block->ops->release_inode(f->inode);
    return 0;
}

static int ext2_seek(file_object* f, uint offset)
{
    f->file_read_offset = offset;
    f->file_write_offset = offset;
    return 0;
}

static int ext2_readdir(file_object* f, dirent* out)
{
    for (;;) {
        uint off = f->file_read_offset;
        if (off >= f->inode->file_size)
            return 0;
        ext2_dir_entry_head de;
        if (ext2_read_data(f->inode, off, sizeof(de), &de) != sizeof(de))
            return 0;
        if (de.rec_len == 0)
            return 0;
        f->file_read_offset += de.rec_len;
        if (de.inode != 0 && de.name_len != 0) {
            uint nl = de.name_len;
            if (nl > FILE_MAXLEN - 1)
                nl = FILE_MAXLEN - 1;
            ext2_read_data(f->inode, off + sizeof(de), nl, out->name);
            out->name[nl] = '\0';
            out->inode_number = de.inode;
            return sizeof(dirent);
        }
    }
}

static const fs_ops ext2_file_file_ops = {.read = ext2_read_file,
                                          .write = ext2_write_file,
                                          .open = ext2_open_file,
                                          .close = ext2_close_file,
                                          .flush = ext2_flush,
                                          .seek = ext2_seek};

static const fs_ops ext2_dir_file_ops = {
        .open = ext2_open_file,
        .close = ext2_close_file,
        .readdir = ext2_readdir,
};

// --- directory lookup -------------------------------------------------------

static inode* ext2_lookup(inode* start, char* name)
{
    if (start == NULL || start->inode_type != FS_DIR)
        return NULL;

    uint size = start->file_size;
    char nbuf[256];
    for (uint off = 0; off < size;) {
        ext2_dir_entry_head de;
        if (ext2_read_data(start, off, sizeof(de), &de) != sizeof(de))
            break;
        if (de.rec_len == 0)
            break;
        if (de.inode != 0) {
            uint nl = de.name_len;
            ext2_read_data(start, off + sizeof(de), nl, nbuf);
            nbuf[nl] = '\0';
            if (!strcmp(nbuf, name)) {
                super_block* s = start->super_block;
                inode* r = s->ops->alloc_inode(s);
                r->inode_number = de.inode;
                s->ops->read_inode(r);
                return r;
            }
        }
        off += de.rec_len;
    }
    return NULL;
}

static int ext2_ro_dirop(inode* i, char* n)
{
    return -EPERMS;
}

static const inode_ops ext2_dir_inode_ops = {.lookup = ext2_lookup,
                                             .mkdir = ext2_mkdir,
                                             .rmdir = ext2_ro_dirop,
                                             .create = ext2_create,
                                             .unlink = ext2_ro_dirop};

// --- super block / inode lifecycle ------------------------------------------

static inode* ext2_find_free_inode(super_block* s)
{
    list_head* ptr;
    list_for_each(ptr, &s->open_inodes)
    {
        inode* res = list_entry(ptr, inode, open_ptr);
        if (!res->use_count) {
            res->use_count++;
            return res;
        }
    }
    return NULL;
}

static inode* ext2_alloc_inode(super_block* s)
{
    inode* ino = ext2_find_free_inode(s);
    if (ino == NULL) {
        ino = kmalloc(sizeof(inode));
        memset(ino, 0, sizeof(inode));
        ino->use_count = 1;
        ino->super_block = s;
        list_add(&ino->open_ptr, &s->open_inodes);
    }
    ino->i_ops = &ext2_default_inode_ops;
    ino->f_ops = &ext2_default_file_ops;

    ext2_inode* ei = kmalloc(sizeof(ext2_inode));
    memset(ei, 0, sizeof(ext2_inode));
    ino->info_disk = ei;
    return ino;
}

static int ext2_release_inode(inode* data)
{
    if (data == data->super_block->root)
        return 0;
    data->use_count--;
    if (data->use_count > 0)
        return 0;
    kfree(data->info_disk);
    kfree(data->info_cdev);
    kfree(data->info_bdev);
    kfree(data->info_pipe);
    data->info_disk = data->info_cdev = NULL;
    data->info_bdev = NULL;
    data->info_pipe = NULL;
    return 0;
}

static int ext2_write_inode(inode* data)
{
    ext2_fs_data* fs = data->super_block->fs_data;
    uint32 ino = data->inode_number;
    uint32 group = (ino - 1) / fs->inodes_per_group;
    uint32 index = (ino - 1) % fs->inodes_per_group;
    uint32 byte_off = index * fs->inode_size;
    uint32 block = fs->groups[group].inode_table + byte_off / fs->block_size;
    uint32 in_block = byte_off % fs->block_size;

    ext2_inode* ei = data->info_disk;
    ei->size = data->file_size;
    buffered_write(block, in_block, sizeof(ext2_inode), ino, ei);
    data->is_dirty = false;
    return 0;
}

static int ext2_write_super(super_block* s)
{
    ext2_flush_super(s);
    buffers_flush_all();
    return 0;
}

static int ext2_delete_inode(inode* data)
{
    return -EPERMS;
}

static inode* ext2_obtain_inode(super_block* s)
{
    return NULL; // no allocation on a read-only filesystem
}

static const super_block_ops ext2_super_block_ops = {
        .read_inode = ext2_read_inode,
        .write_inode = ext2_write_inode,
        .delete_inode = ext2_delete_inode,
        .alloc_inode = ext2_alloc_inode,
        .release_inode = ext2_release_inode,
        .write_super = ext2_write_super,
        .obtain_inode = ext2_obtain_inode};

// --- mount ------------------------------------------------------------------

int fs_ext2_init(super_block* block)
{
    buffer_cache_init();

    // The superblock lives at byte offset 1024, i.e. block 1 with 1024-byte
    // blocks. Read the whole block and copy out the fields we use.
    char buf[EXT2_BLOCK_SIZE];
    ext2_read_block(1, buf);
    ext2_super_block sb;
    memcpy(&sb, buf, sizeof(sb));

    if (sb.magic != EXT2_MAGIC)
        kernel_panic("Invalid ext2 superblock magic");
    if ((1024u << sb.log_block_size) != EXT2_BLOCK_SIZE)
        kernel_panic("ext2: only 1024-byte blocks are supported");

    INIT_LIST_HEAD(&block->open_inodes);
    block->magic = sb.magic;
    block->block_size = EXT2_BLOCK_SIZE;
    block->disk_size = sb.blocks_count * EXT2_BLOCK_SIZE;
    block->max_file_size = 0x7FFFFFFF;
    block->is_dirty = false;

    ext2_fs_data* fs = kmalloc(sizeof(ext2_fs_data));
    fs->block_size = EXT2_BLOCK_SIZE;
    fs->inode_size = (sb.rev_level >= 1) ? sb.inode_size : 128;
    fs->inodes_per_group = sb.inodes_per_group;
    fs->blocks_per_group = sb.blocks_per_group;
    fs->first_data_block = sb.first_data_block;
    fs->inodes_count = sb.inodes_count;
    fs->free_blocks = sb.free_blocks_count;
    fs->free_inodes = sb.free_inodes_count;
    fs->groups_count =
            (sb.blocks_count - sb.first_data_block + sb.blocks_per_group - 1) /
            sb.blocks_per_group;

    // The group descriptor table starts in the block after the superblock.
    fs->gd_block = sb.first_data_block + 1;
    fs->gd_blocks =
            (fs->groups_count * sizeof(ext2_group_desc) + EXT2_BLOCK_SIZE - 1) /
            EXT2_BLOCK_SIZE;
    fs->groups = kmalloc(fs->gd_blocks * EXT2_BLOCK_SIZE);
    buffered_read_several(fs->gd_block, fs->gd_blocks, fs->groups);
    block->fs_data = fs;

    block->ops = &ext2_super_block_ops;

    block->root = block->ops->alloc_inode(block);
    if (block->root == NULL)
        kernel_panic("ext2: could not allocate root inode");
    block->root->inode_number = EXT2_ROOT_INO;
    block->ops->read_inode(block->root);

    return 0;
}
