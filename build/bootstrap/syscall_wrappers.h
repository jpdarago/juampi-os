#ifndef __SYSCALL_WRAPPERS
#define __SYSCALL_WRAPPERS

int fork();
int wait4(int pid);
int sleep();
int kill(int pid, int signal);
int scrn_print(int row, int col, const char * message);
int exit();
int open(char * pathname, unsigned int flags);
int read(int fd, unsigned int bytes, void * buffer);
int write(int fd,unsigned int bytes, const void * buffer);
int close(int fd);
int exec(char * filename, char ** arguments);

#define SIGINT  0
#define SIGKILL 1
#define SIGSTOP 2
#define SIGCONT 3

#define FS_RD 1
#define FS_WR 2
#define FS_RDWR (FS_RD | FS_WR)

#endif
