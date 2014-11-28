#ifndef __SYSCALL_WRAPPERS
#define __SYSCALL_WRAPPERS

int fork();
int wait4(int pid);
int sleep();
int kill(int pid, int signal);
int scrn_print(int row, int col, const char * message);
int exit();
int open(const char * pathname, unsigned int flags);
int read(int fd, unsigned int bytes, void * buffer);
int write(int fd,unsigned int bytes, const void * buffer);
int close(int fd);
int exec(const char * filename, char ** arguments);
void get_cwd(char *);
int set_cwd(const char *);
int get_pid();

#define FILE_MAXLEN 30

typedef struct dirent{
	unsigned int inode_number;
	char name[FILE_MAXLEN];
} dirent;

int readdir(int fd, dirent * d);

typedef struct date {
	unsigned int second;
	unsigned int minute;
	unsigned int hour;
	unsigned int day;
	unsigned int month;
	unsigned int year;
} date;

void gettime(date * d);

#define SIGINT 	0
#define SIGKILL 1
#define SIGSTOP	2
#define SIGCONT	3

#define FS_RD 1
#define FS_WR 2
#define FS_TRUNC 4
#define FS_CREAT 8

#define FS_RDWR (FS_RD | FS_WR)

#endif
