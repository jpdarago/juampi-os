#include <dev_table.h>
#include <tty.h>

#define MAX_MAJOR 16
#define MAX_MINOR 16

fs_ops * fs_table[MAX_MAJOR][MAX_MINOR] = {
    [TTY_MAJOR] = {
        [TTY_MINOR] = &tty_fs_ops
    }
};

inode_ops * iops_table[MAX_MAJOR][MAX_MINOR];

void check_dev_numbers(ushort major, ushort minor)
{
    if(minor >= MAX_MINOR)
        kernel_panic("Minor number %d invalido",minor);
    if(major >= MAX_MAJOR)
        kernel_panic("Major number %d invalido",major);
}

fs_ops* get_fops_table(ushort major, ushort minor)
{
    check_dev_numbers(major,minor);
    fs_ops * res = fs_table[major][minor];
    return res;
}

inode_ops* get_iops_table(ushort major, ushort minor)
{
    check_dev_numbers(major,minor);
    inode_ops * res = iops_table[major][minor];
    return res;
}
