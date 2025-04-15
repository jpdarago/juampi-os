#ifndef __DEV_TABLE_H
#define __DEV_TABLE_H

#include <types.h>
#include <vfs.h>

fs_ops      * get_fops_table(ushort major, ushort minor);
inode_ops   * get_iops_table(ushort major, ushort minor);

#endif
