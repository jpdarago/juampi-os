#include "syscall_wrappers.h"
#include "stdio.h"
#include "parser.h"
#include "utils.h"

int main(int argc, char * argv[])
{
	for(int i = 1; i < argc; i++)
		printf("%s ",argv[i]);
	putchar('\n');

	exit();
	return 0;
}
