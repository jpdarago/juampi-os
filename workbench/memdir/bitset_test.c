#include <check.h>
#include <stdio.h>
#include <stdlib.h>

#include "bitset.h"

START_TEST(test_create_bitset) {
	unsigned int * s = malloc(15*sizeof(int));
	bitset b;
	
	bitset_init(&b,s,15);
	
	fail_unless(b.start == s);
	fail_unless(b.size == 15);
	
	free(s);
} END_TEST

START_TEST(test_set_clear_bitset){
	unsigned int * s = malloc(15*sizeof(int));
	bitset b; bitset_init(&b,s,15);
	
	unsigned int bit = 8*32+7;
	fail_if(b.start[8] & (1 << 7));
	bitset_set(&b,bit);
	fail_unless(b.start[8] & (1 << 7));
	bitset_clear(&b,bit);
	fail_if(b.start[8] & (1 << 7) );
	
	free(s);
} END_TEST

START_TEST(test_find_bitset){
	unsigned int * s = malloc(15*sizeof(int));
	bitset b; bitset_init(&b,s,15);
	fail_unless(bitset_search(&b) == 0,
		"Se esperaba 0 se obtuvo %d", bitset_search(&b));
	for(int i = 0; i < 4; i++)
		bitset_set(&b,i);
	fail_unless(bitset_search(&b) == 4,
		"Se esperaba %d se obtuvo %d",4,bitset_search(&b));
	free(s);
} END_TEST

TFun tests[] = {
	test_create_bitset,
	test_set_clear_bitset,
	test_find_bitset,
	NULL
};

int main(){
	Suite * s = suite_create("BITSET");

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
