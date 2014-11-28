#include "syscall_wrappers.h"
#include "stdio.h"
#include "utils.h"

int main(int argc, const char * argv[])
{
	date d;
	gettime(&d);
	printf("Hoy es %d/%d/%d %d:%d:%d\n",
		d.day,d.month,d.year,d.hour,d.minute,d.second);
	exit();
	return 0;
}
