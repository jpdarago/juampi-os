#ifndef __FS_H
#define __FS_H

#include "types.h"
#include "klist.h"

// Maximum length of an absolute path
#define FS_MAXLEN 128
// Maximum length of a piece of an absolute path
#define FILE_MAXLEN 30

typedef struct inode inode;
typedef struct dir_entry dir_entry;
typedef struct super_block super_block;
typedef struct file_object file_object;

typedef enum fs_type {
    FS_FILE,
    FS_DIR,
    FS_CHARDEV,
    FS_BLOCKDEV,
    FS_ERROR = -1
} fs_type;

typedef struct dirent {
    uint inode_number;
    char name[FILE_MAXLEN];
} dirent;

typedef struct fs_ops {
    int (*read)(file_object *, uint, void *);
    int (*write)(file_object *, uint, void *);
    int (*open)(inode *, file_object *);
    int (*close)(file_object *);
    int (*readdir)(file_object *, dirent *);
    int (*flush)(file_object *);
    int (*seek)(file_object *,uint);
} fs_ops;

typedef struct inode_ops {
    // Given a file name and a directory inode
    // finds a file that has the corresponding name
    inode * (*lookup)(inode *, char *);
    // Deletes an inode from disk
    int (*unlink)(inode *, char * name);
    // Creates a new directory entry
    int (*mkdir)(inode *, char *);
    // Deletes a directory entry
    int (*rmdir)(inode *, char *);
    // Places an inode as a subfile of a directory
    int (*create)(inode *, char *);
} inode_ops;

typedef struct super_block_ops {
    // Creates an in-memory structure corresponding to an
    // inode for this filesystem
    inode * (*alloc_inode)(super_block *);
    // Gets a free inode in this filesystem
    inode * (*obtain_inode)(super_block *);
    // Reads the data of an inode from the filesystem
    int (*read_inode)(inode*);
    // Writes the inode data to disk
    int (*write_inode)(inode*);
    // Deletes an inode from disk, destroying the memory too
    int (*delete_inode)(inode*);
    // Destroys the inode structure in memory
    int (*release_inode)(inode*);
    // Flushes the super block to hard disk
    int (*write_super)(super_block *);
    // Flushes the whole filesystem  to hard disk
    int (*sync_fs)(super_block *);
} super_block_ops;

typedef struct pipe {
    void * data; // A buffer of size bytes
    uint offset;
    uint size;
} pipe;

typedef struct char_dev {
    ushort major_number;
    ushort minor_number;
    fs_ops  * ops;
} char_dev;

typedef struct block_dev {
    ushort major_number;
    ushort minor_number;
    fs_ops  * ops;
} block_dev;

struct inode {
    uint inode_number;
    uint use_count;

    bool is_dirty;

    uint file_size;

    fs_type inode_type;

    list_head open_ptr;

    super_block * super_block;

    inode_ops const * i_ops;
    fs_ops const * f_ops;

    pipe        * info_pipe; // If it is a pipe
    char_dev    * info_cdev; // If it is a char device
    block_dev   * info_bdev; // If it is a block device
    void        * info_disk; // If it is a file on disk (MINIX)
};

struct super_block {
    inode * root;

    uint block_size;
    uint disk_size;
    uint max_file_size;
    uint magic;
    bool is_dirty;

    list_head open_inodes;

    void * fs_data;
    super_block_ops const * ops;
};

#define FS_RD       1
#define FS_WR       2
#define FS_RDWR     (FS_RD | FS_WR)

#define FS_RDBIT    1
#define FS_WRBIT    2

// Opening flags
#define FS_TRUNC    4
#define FS_CREAT    8

struct file_object {
    inode           * inode;
    uint access_mode;
    uint flags;
    uint file_read_offset;
    uint file_write_offset;
    fs_ops const   * f_ops;
};

enum fs_error {
    EINVOFF = 1,
    EPERMS,
    EINVTYPE,
    ENOSPACE,
    EINVFS,
    EINODES,
    ENODIR,
    ENOFILE,
    EFBUSY,
    EDIRNOTEMPTY,
    EINVINODETYPE,
    ENODOUBLEOPEN,
    EINVINODE,
    EINVOP,
    EINVFD,
    ENOFDSPACE,
    EINVPATH,
    ETOOLONG
};

#endif
