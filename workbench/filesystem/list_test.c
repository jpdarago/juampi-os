#include "list.h"

#ifdef TEST_FLAG

#include "memory.h"
#include <check.h>

START_TEST(test_buffer_create){
	void * mem = malloc(128);
	kmem_init(mem,128);

	list * bl = list_create();
	
	fail_unless(!bl->head,"head no es vacia");
	fail_unless(!bl->tail,"tail no es vacia");
	fail_unless(!bl->elements,"Se esperaba lista sin elementos");
	fail_unless(!bl->destroyer,"Habia destructor");
	fail_unless(!bl->creator,"Se esperaba sin creador");
	list_destroy(bl);
	free(mem);
} END_TEST

START_TEST(test_push){
	void * mem = malloc(128);
	kmem_init(mem,128);

	list * bl = list_create( );
	
	int * data = malloc(sizeof(int));
	*data = 5;

	uint index = list_add(bl,data);
	int * other_data = list_get(bl,0);
	fail_unless(index == 0, "Fallo la asignacion de indice");
	fail_unless(*other_data == 5, "No se obtuvieron los datos insertados");
	list_destroy(bl);
	free(mem);
} END_TEST

START_TEST(test_two_push){
	void * mem = malloc(128);
	kmem_init(mem,128);

	list * bl = list_create();
	
	int * data = malloc(sizeof(int));
	*data = 5;
	
	list_add(bl,data);
	int * other_data = malloc(sizeof(int));
	*other_data = 6;

	list_add(bl,other_data);
	
	data = list_get(bl,0);
	fail_unless(bl->elements == 2, "El conteo de elementos esta mal");
	fail_unless(*other_data == 6, "No se obtuvieron los datos insertados");
	fail_unless(*data == 5);
	list_destroy(bl);
	free(data); free(other_data);
	free(mem);
} END_TEST

START_TEST(test_delete){
	void * mem = malloc(128);
	kmem_init(mem,128);	
	list * bl = list_create();
	
	int * data = malloc(sizeof(int));
	*data = 5;
	
	list_add(bl,data);
	int * other_data = malloc(sizeof(int));
	*other_data = 6;

	list_add(bl,other_data);
	
	list_delete(bl,0);
	fail_unless(bl->elements == 1,"El conteo de elementos no es correcto");
	int * the_data = list_get(bl,1);
	fail_unless(*the_data == 6, "No se obtuvieron los datos insertados");
	list_destroy(bl);
	free(data); free(other_data);
	free(mem);
} END_TEST

TFun cases[] = {
	test_buffer_create,
	test_push,
	test_two_push,
	test_delete,
	NULL
};

Suite * fs_suite(void){
	Suite * s = suite_create("Lista de buffers");

	TCase * tc_core = tcase_create ("Core");
	for(int i = 0; cases[i]; i++)
		tcase_add_test(tc_core,cases[i]);

	suite_add_tcase(s,tc_core);

	return s;
}

int main(){	
	int number_failed;
	Suite * s = fs_suite();
	SRunner * sr = srunner_create(s);
	srunner_run_all(sr, CK_VERBOSE);
	number_failed = srunner_ntests_failed(sr);
	
	srunner_free(sr);

	return (number_failed == 0) 
		? EXIT_SUCCESS
		: EXIT_FAILURE;
	
	return 0;
}

#endif

#ifdef TEST_FLAG_MEM

int main(){
	int i, * data;
	int datas[] = {1,2,3,4,5,6};
	
	list * l = list_create();
	for(i = 0; i < 6; i++){
		data = malloc(sizeof(int));
		*data = datas[i];
		list_add(l,data);
	}	
	list_destroy(l);
	return 0;
}

#endif
