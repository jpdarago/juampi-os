#include "stdio.h"
#include "syscall_wrappers.h"
#include "utils.h"
#include "vargs.h"

void fail(const char * msg)
{
	scrn_print(18,0,msg);
	while(1);
}

int putchar(char c)
{
	return write(STDOUT,1,&c);	
}

int puts(const char * s)
{
	if(s == NULL)
		fail("Escribiendo string nula");
	return write(STDOUT,strlen(s),s);	
}

int printf(const char * msg, ...) 
{
	varg_list l;
	varg_set(l,msg);
	
	unsigned int i;
	char buffer[64];

	for(i = 0; msg[i]; i++) {
		switch(msg[i]) {
		case '%':
			i++;
			switch(msg[i]) {
			case '%':
				putchar(msg[i]);
				break;
			case 'x':
				num_to_str(varg_yield(l,unsigned int), 16, buffer);
				puts(buffer);
				break;
			case 'd':
				num_to_str(varg_yield(l,int), 10, buffer);
				puts(buffer);
				break;
			case 's':
				puts(varg_yield(l,char*));
				break;
			case 'c':
				putchar(varg_yield(l,char));
				break;
			}
			break;
		default:
			putchar(msg[i]);
			break;
		}
	}
	
	varg_end(l);
	return 0;
}
