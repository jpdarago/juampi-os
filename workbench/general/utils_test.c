#include <check.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "types.h"

extern void * my_memcpy(void * dst, void * src, int bytes);
extern void * my_memset(void * dst, uchar val, uint count);

#ifndef TIMER_FLAG 

START_TEST (test_memcpy) {
	char mem[] = "Esto es un string a copiar que no es muy largo";
	char buf[128];

	for(int i = 0; mem[i]; i++){
		my_memcpy(buf,mem,i);
		for(int j = 0; j < i; j++)
			fail_if(buf[j] != mem[j],
					"Se esperaba %c se obtuvo %c",mem[j],buf[j]);
		for(int j = 0; mem[j]; j++)
			buf[i] = '_';
	}
} END_TEST

START_TEST (test_memset) {
	uint MSIZE = 1025; char mem[MSIZE];
	for(int i = 0; i < MSIZE; i++){
		for(int j = 0; j < MSIZE; j++) 
			mem[j] = '_';
		my_memset(mem,'a',i);
		for(int j = 0; j < i; j++)
				fail_if(mem[j] != 'a');
		for(int j = i; j < MSIZE; j++)
				fail_if(mem[j] != '_');
	}
} END_TEST

START_TEST (test_memcpy_small) {
	uint from = 0xDEADBEEF;
	uint to;
	memcpy(&to,&from,sizeof(from));
	fail_unless(from == 0xDEADBEEF && to == 0xDEADBEEF);
} END_TEST

TFun tests[] = {
	test_memcpy,
	test_memset,
	test_memcpy_small,
	NULL
};

int main(){
	Suite * s = suite_create("Utilidades generales");

	TCase * tc_core = tcase_create("Core");
	for(int i = 0; tests[i]; i++)
		tcase_add_test(tc_core,tests[i]);
	 suite_add_tcase(s,tc_core);
	SRunner *sr = srunner_create(s);
	srunner_run_all(sr,CK_VERBOSE);
	int number_failed = srunner_ntests_failed(sr);
	srunner_free(sr);
	
	return (number_failed == 0) ? EXIT_SUCCESS : EXIT_FAILURE;
}

#else
typedef unsigned long long ticks;
static __inline__ ticks getticks(void)
{
	unsigned a, d;
	asm("cpuid");
	asm volatile("rdtsc" : "=a" (a), "=d" (d));
	return (((ticks)a) | (((ticks)d) << 32));
}

unsigned long long measure(void (*fn)(void *,int), void * array, int bytes){
	unsigned long long start = getticks();
	fn(array,bytes);
	return getticks()-start;
}

void my_memset_ms(void * memory, int bytes){
	my_memset(memory,1,bytes);
}

void * my_memset2(void * mem, unsigned char val, unsigned int count){
	unsigned int i; int * m = (int *) mem;
	unsigned int v = val | (val << 8) | (val << 16) | (val << 20); 
	for(i = 0; i+4 < count; i += 4){
		*m++ = v;
	}
	char * rm = (char *) mem;
	for(;i < count; i++) *rm++ = val; 
	return mem;
}

void memset_c_ms(void * memory, int bytes){
	my_memset2(memory,1,bytes);
}

void memset_ms(void * memory, int bytes){
	memset(memory,1,bytes);
}

int sizes[] = {100000,50000,10000,4096,4095,4,3,1024,512,511,16,20,0};

int main(){
	int i = 0;

	for(i = 0; sizes[i] > 0; i++){
		void * array = malloc(sizes[i]);
		printf("==== PARA UN ARREGLO DE %d BYTES ====\n",sizes[i]);
		
		ticks opt = measure(my_memset_ms,array,sizes[i]);
		ticks oth = measure(memset_c_ms,array,sizes[i]);
		ticks sopt = measure(memset_ms,array,sizes[i]);

		printf("optimizado = %lld, patron = %lld, basico = %lld, porcent =  %lf %% \n",
			opt,sopt,oth,(100.0*oth)/opt);
		free(array);
	}
}

#endif
