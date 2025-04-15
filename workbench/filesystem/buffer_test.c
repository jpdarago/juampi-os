#include "buffer.h"
#include "memory.h"

#ifdef TEST_FLAG

#include <check.h>
#include <string.h>
#include <stdio.h>
#include <assert.h>

START_TEST(test_buffer_create){
	fs_hdd_buffer * bf = buffer_create(0,1,);
	
	fail_unless(bf->top == ATA_SECTSIZE);
	fail_unless(bf->cluster == 0);
	fail_unless(bf->cluster_size == 1);
	fail_unless(bf->consumed == 0);

	buffer_destroy(bf);
} END_TEST

START_TEST(test_buffer_use){
	hdd_init("test_buffer.dat");

	fs_hdd_buffer * bf = buffer_create(0,1,);

	char * dump = malloc(ATA_SECTSIZE);
	char * res = "THE CAKE IS A LIE";

	buffer_read(bf,strlen(res)+1,dump);
	dump[strlen(res)] = '\0';
	fail_unless(!strcmp(res,dump),
		"Se esperaba %s se obtuvo %s en la lectura del buffer",res,dump);

	char * res2 = "THE CAKE WAS A LIE";

	buffer_read(bf,strlen(res2),dump);
	dump[strlen(res2)] = '\0';
	
	fail_unless(!strcmp(res2,dump),
		"Se esperaba %s se obtuvo %s en la lectura del buffer",
		res2,dump);

	buffer_destroy(bf);
	free(dump);
} END_TEST

START_TEST(test_buffer_reset){
	hdd_init("test_buffer.dat");

	fs_hdd_buffer * bf = buffer_create(0,1,);

	char * dump = malloc(ATA_SECTSIZE);
	char * res = "THE CAKE IS A LIE ";

	buffer_read(bf,strlen(res),dump);
	fail_unless(!strncmp(res,dump,strlen(res)),
		"Se esperaba %s se obtuvo %s.",res,dump);
	buffer_reset(bf,0,0);
	buffer_read(bf,strlen(res),dump);
	fail_unless(!strncmp(res,dump,strlen(res)),
		"Se esperaba %s se obtuvo %s.",res,dump);

	free(dump);
	buffer_destroy(bf);
} END_TEST

START_TEST(test_buffer_read_beyond_sector){
	hdd_init("test_buffer.dat");

	fs_hdd_buffer * bf = buffer_create(0,1,);
	void * dump1, * dump2;

	dump1 = malloc(ATA_SECTSIZE+1);
	dump2 = malloc(ATA_SECTSIZE+1);

	FILE * f = fopen("test_buffer.dat","r");
	int bytes = fread(dump1,sizeof(char),ATA_SECTSIZE+1,f);
	assert(bytes == ATA_SECTSIZE+1);
	buffer_read(bf,ATA_SECTSIZE+1,dump2);
	
	fail_unless(bf->cluster == 1,"Cluster incorrecto");	
	fail_unless(bf->consumed == 1, "Lectura incorrecta");
	
	fail_unless(!memcmp(dump1,dump2,ATA_SECTSIZE+1),
		"No hubo lectura igual");
	fclose(f);
	
	buffer_destroy(bf);
	free(dump1); free(dump2);
} END_TEST

START_TEST (test_buffer_write){
	hdd_init("test_buffer.dat");

	fs_hdd_buffer * bf = buffer_create(0,1,);
	
	char data[] = "Datos a copiar";
	char original[ATA_SECTSIZE];
	
	memcpy(original,HDD,ATA_SECTSIZE);
	memcpy(original,data,strlen(data));

	buffer_write(bf,strlen(data),data);
	
	fail_unless(!memcmp(HDD,original,ATA_SECTSIZE),
		"Los datos no se escribieron satisfactoriamente");
	
	buffer_destroy(bf);
} END_TEST

START_TEST (test_buffer_write_beyond_sector) {
	hdd_init("test_buffer.dat");
	fs_hdd_buffer * bf = buffer_create(0,1,);
	
	char data[2*ATA_SECTSIZE+1],*copy="DATOS NUEVOS ";
	int copy_len = strlen(copy), data_len = 2*ATA_SECTSIZE;
	for(int i = 0; i+copy_len < data_len; i += copy_len)
		memcpy(data+i,copy,strlen(copy));
	data[data_len] = '\0';

	buffer_write(bf,data_len,data);
	
	fail_unless(!memcmp(HDD,data,data_len),
		"No se escribieron los datos satisfactoriamente");

	buffer_destroy(bf);	
} END_TEST

START_TEST (test_buffer_write_with_offset) {
	hdd_init("test_buffer.dat");
	fs_hdd_buffer * bf = buffer_create(0,1,);
	
	buffer_reset(bf,0,20);
	
	char data[] = "Datos a copiar"; 
	char original[ATA_SECTSIZE];
	
	memcpy(original,HDD,ATA_SECTSIZE);
	memcpy(original+20,data,strlen(data));
	
	buffer_write(bf,strlen(data),data);
	fail_unless(!memcmp(HDD,original,ATA_SECTSIZE),
			"Los datos no se escribieron satisfactoriamente");

	buffer_destroy(bf);
} END_TEST

START_TEST(test_buffer_write_border){
	hdd_init("test_buffer.dat");
	fs_hdd_buffer * bf = buffer_create(0,1,);

	buffer_reset(bf,0,ATA_SECTSIZE-20);
	char data[21],dump[21],res[21];
	
	memset(data,'A',sizeof(data));
	memset(res,0,sizeof(res));

	data[20] = '\0';

	memset(res,'A',strlen(data));
	memset(dump,0,sizeof(dump));
	
	buffer_write(bf,20,data);
	memset(data,'B',strlen(data));
	buffer_write(bf,20,data);
	
	buffer_reset(bf,0,ATA_SECTSIZE-20);
	buffer_read(bf,20,dump);

	fail_unless(!strcmp(res,dump),
			"No se escribio bien el buffer. "
			"Se esperaba %s se obtuvo %s",
				res,dump);
	
	buffer_read(bf,20,dump); 
	memset(res,'B',strlen(res));
	
	fail_unless(!strcmp(res,dump),
			"No se escribio bien el buffer. "
			"Se esperaba %s se obtuvo %s",
				res,dump);
	
	buffer_destroy(bf);
} END_TEST

START_TEST (test_buffer_read_reverse) {
	hdd_init("test_buffer.dat");

	fs_hdd_buffer * bf = buffer_create(0,1,);
	buffer_reset(bf,0,ATA_SECTSIZE);
	char res[1000] = {0},buffer[1000] = {0};
	hdd_read(0,1,res);
	buffer_read_reverse(bf,ATA_SECTSIZE,buffer);
	fail_unless(!memcmp(res,buffer,ATA_SECTSIZE),
			"No se leyo bien el buffer. "
			"Se esperaba %s se obtuvo %s",
				res,buffer);
	buffer_destroy(bf);
} END_TEST

START_TEST (test_assigned_buffer) {
	hdd_init("test_buffer.dat");
	char mem[512];

	fs_hdd_buffer * bf = buffer_create(0,1, .buffer = mem );

	char * dump = malloc(ATA_SECTSIZE);
	char * res = "THE CAKE IS A LIE";

	buffer_read(bf,strlen(res)+1,dump);
	dump[strlen(res)] = '\0';
	fail_unless(!strcmp(res,dump),
		"Se esperaba %s se obtuvo %s en la lectura del buffer",res,dump);

	char * res2 = "THE CAKE WAS A LIE";

	buffer_read(bf,strlen(res2),dump);
	dump[strlen(res2)] = '\0';
	
	fail_unless(!strcmp(res2,dump),
		"Se esperaba %s se obtuvo %s en la lectura del buffer",
		res2,dump);

	buffer_destroy(bf);
	free(dump);
} END_TEST

Suite * fs_suite(void){
	Suite * s = suite_create("Buffer de disco duro");

	TCase * tc_core = tcase_create ("Core");

	tcase_add_test(tc_core,test_buffer_create);
	tcase_add_test(tc_core,test_buffer_use);
	tcase_add_test(tc_core,test_buffer_reset);
	tcase_add_test(tc_core,test_buffer_read_beyond_sector);
	tcase_add_test(tc_core,test_buffer_write);
	tcase_add_test(tc_core,test_buffer_write_beyond_sector);
	tcase_add_test(tc_core,test_buffer_write_with_offset);
	tcase_add_test(tc_core,test_buffer_write_border);
	tcase_add_test(tc_core,test_buffer_read_reverse);
	tcase_add_test(tc_core,test_assigned_buffer);
	suite_add_tcase(s,tc_core);

	return s;
}

int main(){	
	int number_failed;

	void * mem = malloc(1024*1024);
	kmem_init(mem,1024*1024);


	Suite * s = fs_suite();
	SRunner * sr = srunner_create(s);
	srunner_run_all(sr, CK_NORMAL);
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

	return 0;
}

#endif
