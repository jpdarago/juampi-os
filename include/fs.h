#ifndef __FS_SYSCALLS_H
#define __FS_SYSCALLS_H

#include <types.h>
#include <vfs.h>

void init_disk_super_block();
super_block * get_disk_super_block();

int invalid_path(const char * dir);
int do_open(char * pathname, uint flags);
int do_read(int fd, int bytes, void * buffer);
int do_write(int fd, int bytes, void * buffer);
int do_readdir(int fd, dirent * d);
int do_close(int fd);
int do_mkdir(char * pathname);
int do_unlink(char * pathname);
int do_rmdir(char * pathname);
uint get_file_size(int fd);

//Cantidad maxima de file descriptors por proceso
#define MAX_FDS 16

#endif
