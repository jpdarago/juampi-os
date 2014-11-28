#ifndef __ERRORS_H
#define __ERRORS_H

#include <stdlib.h>

#define ETOOLONG	0
#define ETOOMANY 	1
#define EINVGRAM	2
#define EPROGLONG	3
#define EARGLONG	4
#define EMANYARGS	5
#define EINVCHAR	6
#define EINVEXPR	7

extern char * text_errors[];

void handle_error(int error);

#endif
