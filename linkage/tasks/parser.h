#ifndef __PARSER_H
#define __PARSER_H

#define MAXARGS 	16
#define MAXARGLEN	30

typedef struct argument{
	char str[MAXARGLEN];
} argument;

typedef struct argument_list {
	unsigned int length;
	argument list[MAXARGS];	
} argument_list;

argument_list * parse_arguments(const char * command);
const char * get_parse_error();

#endif
