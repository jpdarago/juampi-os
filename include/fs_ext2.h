#ifndef __FS_EXT2_H
#define __FS_EXT2_H

#include <vfs.h>

// ext2 driver (read + write). The image is built with 1024-byte blocks so it
// lines up with the buffer cache (which works in 1024-byte units), and small
// enough to avoid the extent-tree/64bit features that only ext4 needs.
#define EXT2_BLOCK_SIZE 1024
#define EXT2_MAGIC 0xEF53
#define EXT2_ROOT_INO 2
#define EXT2_N_BLOCKS 15 // 12 direct + single/double/triple indirect
#define EXT2_PPB (EXT2_BLOCK_SIZE / 4) // block pointers per block

// File-type bits in the inode mode field.
#define EXT2_S_IFMT 0xF000
#define EXT2_S_IFCHR 0x2000
#define EXT2_S_IFDIR 0x4000
#define EXT2_S_IFBLK 0x6000
#define EXT2_S_IFREG 0x8000

// On-disk superblock (only the leading fields we use; it lives at byte 1024).
typedef struct {
    uint32 inodes_count;
    uint32 blocks_count;
    uint32 r_blocks_count;
    uint32 free_blocks_count;
    uint32 free_inodes_count;
    uint32 first_data_block;
    uint32 log_block_size;
    uint32 log_frag_size;
    uint32 blocks_per_group;
    uint32 frags_per_group;
    uint32 inodes_per_group;
    uint32 mtime;
    uint32 wtime;
    uint16 mnt_count;
    uint16 max_mnt_count;
    uint16 magic;
    uint16 state;
    uint16 errors;
    uint16 minor_rev_level;
    uint32 lastcheck;
    uint32 checkinterval;
    uint32 creator_os;
    uint32 rev_level;
    uint16 def_resuid;
    uint16 def_resgid;
    uint32 first_ino;
    uint16 inode_size;
} __attribute__((__packed__)) ext2_super_block;

// On-disk block group descriptor (32 bytes).
typedef struct {
    uint32 block_bitmap;
    uint32 inode_bitmap;
    uint32 inode_table;
    uint16 free_blocks_count;
    uint16 free_inodes_count;
    uint16 used_dirs_count;
    uint16 pad;
    uint32 reserved[3];
} __attribute__((__packed__)) ext2_group_desc;

// On-disk inode (128 bytes).
typedef struct {
    uint16 mode;
    uint16 uid;
    uint32 size;
    uint32 atime;
    uint32 ctime;
    uint32 mtime;
    uint32 dtime;
    uint16 gid;
    uint16 links_count;
    uint32 blocks;
    uint32 flags;
    uint32 osd1;
    uint32 block[EXT2_N_BLOCKS];
    uint32 generation;
    uint32 file_acl;
    uint32 dir_acl;
    uint32 faddr;
    uint8 osd2[12];
} __attribute__((__packed__)) ext2_inode;

// On-disk directory entry header (the name follows, name_len bytes).
typedef struct {
    uint32 inode;
    uint16 rec_len;
    uint8 name_len;
    uint8 file_type;
} __attribute__((__packed__)) ext2_dir_entry_head;

// ext2 directory entry file_type values (when the filetype feature is on).
#define EXT2_FT_REG_FILE 1
#define EXT2_FT_DIR 2
#define EXT2_FT_CHRDEV 3

// In-memory per-mount metadata (stored in super_block->fs_data).
typedef struct {
    uint32 block_size;
    uint32 inode_size;
    uint32 inodes_per_group;
    uint32 blocks_per_group;
    uint32 first_data_block;
    uint32 inodes_count;
    uint32 groups_count;
    uint32 gd_block;    // block where the group descriptor table starts
    uint32 gd_blocks;   // how many blocks the group descriptor table spans
    uint32 free_blocks; // superblock free block count (kept in sync on alloc)
    uint32 free_inodes; // superblock free inode count
    ext2_group_desc* groups;
} ext2_fs_data;

int fs_ext2_init(super_block* block);

#endif
