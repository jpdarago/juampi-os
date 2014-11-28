#ifndef __SHELL_H
#define __SHELL_H

#define MAX_LEN 		128
#define MAX_PROGRAMS 	16
#define MAX_FILENAME	32
#define MAX_ARGLEN		32
#define MAX_ARGS		16

typedef struct {
	char filename[1+MAX_FILENAME];
	char * arguments[MAX_ARGS+1];
} program;

extern program program_call[];
extern int programs;

#endif
