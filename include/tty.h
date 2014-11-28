#ifndef __TTY_H
#define __TTY_H

#include <types.h>
#include <vfs.h>

#define TTY_MAJOR 0
#define TTY_MINOR 0

int read_tty(file_object * o, uint, void *);
int write_tty(file_object * o, uint, void *);
int open_tty(inode * ino, file_object * obj);
int close_tty(file_object * f);

extern fs_ops tty_fs_ops;

#endif
