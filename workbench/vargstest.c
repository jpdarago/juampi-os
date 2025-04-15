#include "vargs.h"
#include <stdio.h>

void test(int d, ...){
	varg_list vargs;
	int * cosa = &d;

	printf("PROBANDO %d\n",*cosa);
	va_set(vargs,d);
	int i = 0;
	for(i = 0; i < d; i++){
		printf("%c\n",va_yield(vargs,char));
	}

	va_end(vargs);
}

int main(){
	test(5,'a','b','c','d','e');
	return 0;
}
