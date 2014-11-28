#include <stdio.h>

void kernel_panic(char * message){
	printf("KERNEL PANIC: %s\n",message);
	while(1);
}
