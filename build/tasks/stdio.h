#ifndef __STDIO_H
#define __STDIO_H

#define STDIN   0
#define STDOUT  1

#define FS_MAXLEN 128

void fail(const char * msg);
int putchar(char c);
int puts(const char * s);
int printf(const char * fmt, ...);

#endif
