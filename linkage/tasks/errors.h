#ifndef __ERRORS_H
#define __ERRORS_H

#define ETOOLONG 	1
#define EINVCMD		2
#define EINVARGLEN	3
#define ETOOMANY	4
#define EINVCHAR	5
#define EINVEXPR	6

char * error_message(int error);

#endif
