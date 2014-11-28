#include <check.h>
#include <stdlib.h>
#include <string.h>

void * read_file(char * filename, int bytes){
	FILE * f = fopen(filename,"r");
	char * buffer = malloc(1+bytes);
	fread(buffer,1,bytes,f);
	fclose(f);
	buffer[bytes] = '\0';
	return buffer;
}

//Total de memoria: 256 kb
#define TOTAL_MEM 128*1024

START_TEST (test_fat16_initialize) {
	hdd_init("hdd.img");
	void * memory_buffer = malloc(TOTAL_MEM);
	kmem_init(memory_buffer,TOTAL_MEM);

	fail_unless(sizeof(fat16_direntry) == 32*sizeof(char),
		"La entrada del directorio tiene tamaño invalido."
		"Se obtuvo %d se esperaba %d",
			sizeof(fat16_direntry),32*sizeof(char));

	fail_unless(sizeof(fat16_long_entry) == sizeof(fat16_direntry),
		"La entrada larga de directorio tiene tamaño invalido.");

	fs_init();
	fail_unless(fat16_fatcount == 2,
		"La cantidad de fats es incorrecta");
	fail_unless(fat16_rootdirentries == 16,
		"La cantidad de entradas del directorio "
		"raiz es incorrecta");
	fail_unless(fat16_reserved_sectors == 1,
		"La cantidad de sectores reservados "
		"es incorrecta");
	fail_unless(fat16_clustersize == 1,
		"La cantidad de sectores por cluster "
		"es incorrecta");
	fail_unless(fat16_hidden_sectors == 1,
		"La cantidad de sectores escondidos "
		"es incorrecta, se esperaba 1 se obtuvo %d", fat16_hidden_sectors);
	fail_unless(fat16_fatsize == 128,
		"La cantidad de sectores por FAT "
		"es incorrecta: se esperaba 128 se obtuvo %d", fat16_fatsize);
	fail_unless(!strcmp(fat16_drivename,"TESTDRV"),
		"El identificador de disco se leyo mal");
	
	fail_unless(fat16_fatstart != 0x200,
		"El comienzo de la fat es invalido: %d",fat16_fatstart);
	
	fail_unless(fat16_rootdir != 0x20200,
		"El comienzo del directorio raiz es invalido: %d",fat16_rootdir);

	fail_unless(fat16_dataregion != 0x20600,
		"El comienzo del data sector es invalido: %d",fat16_dataregion);

	uchar fat_start = 0xFF00 | fat16_media_descriptor;
	uchar fat_real_start = ((ushort*) FAT)[0];
	fail_unless(fat_real_start == fat_start,
		"El comienzo de la FAT es incorrecto, se esperaba %x se obtuvo %x",
		fat_start,fat_real_start);
	
	fail_unless(((ushort*)FAT)[1] == 0xFFFF,
		"La segunda entrada de la FAT es incorrecta");

	fat16_direntry rootdir = fat16_getentry(fat16_rootdir,0);

	char rootdir_name[12];
	memcpy(rootdir_name,rootdir.filename,8);
	memcpy(rootdir_name+8,rootdir.extension,3);
	fat16_trim(rootdir_name,11);

	fail_unless(!strcmp(rootdir_name,fat16_drivename), 
		"El directorio raiz no tiene como primer entrada el volume set %s.",rootdir_name);

	fail_unless(fat16_rootdir_cluster == fat16_rootdir,
		"El cluster del directorio raiz fue hallado incorrectamente."
	"Se esperaba %d se obtuvo %d",
		fat16_rootdir_cluster, fat16_rootdir);
	
	fs_destroy();
	free(memory_buffer);
}END_TEST

START_TEST (test_fat16_read_file_in_rootdir) {
	hdd_init("hdd.img");
	
	void * memory_buffer = malloc(TOTAL_MEM);
	kmem_init(memory_buffer,TOTAL_MEM);

	fs_init();

	fs_descr f = fs_open("/prueba.txt","r");
	
	fail_if(fs_error(f),
		"El archivo no se abrio correctamente");
	
	char buffer[5];
	
	fs_read(f,4,buffer);
	buffer[4] = '\0';

	fail_unless(!memcmp(buffer,"HOLA",4),
		"La lectura no se realizo correctamente. " 
		"Se esperaba HOLA se obtuvo %s.",buffer);
	fail_if(fs_close(f) == -1,
		"El archivo no se cerro correctamente");	
	
	fs_destroy(); 
	free(memory_buffer);
}END_TEST

START_TEST (test_fat16_open_file_not_exists) {
	hdd_init("hdd.img");
	
	void * memory_buffer = malloc(TOTAL_MEM);
	kmem_init(memory_buffer,TOTAL_MEM);

	fs_init();

	fs_descr f = fs_open("/este_archivo_no_existe.txt","r");

	fail_unless(fs_error(f),
		"No se deberia poder abrir ese archivo");
	
	fs_destroy(); 
	free(memory_buffer);
}END_TEST

START_TEST (test_fat16_read_file_in_subdir) {
	hdd_init("hdd.img");

	void * memory_buffer = malloc(TOTAL_MEM);
	kmem_init(memory_buffer,TOTAL_MEM);

	fs_init();

	fs_descr f = fs_open("/dir2/subdir/c.txt","r");
	fail_if(fs_error(f),
		"El archivo no se abrio correctamente");
	
	char buffer[64];
	char * res = "ESTE ARCHIVO ESTA RE ESCONDIDO WACHIN"; 
	
	fs_read(f,strlen(res),buffer);
	buffer[strlen(res)] = '\0';
	fail_unless(!memcmp(buffer,res,strlen(res)),
		"El archivo no se leyo correctamente."
		"Se esperaba <%s> se obtuvo <%s>",res,buffer);

	fail_if(fs_close(f) == -1,
		"El archivo no se cerro correctamente");
	
	fs_destroy(); 
	free(memory_buffer);
}END_TEST

START_TEST (test_fat16_read_rdonly_file) {
	hdd_init("hdd.img");

	void * memory_buffer = malloc(TOTAL_MEM);
	kmem_init(memory_buffer,TOTAL_MEM);

	fs_init();
	fail_unless(fs_open("/dir1/archivoBloqueado.txt","w") == -1,
		"Se pudo abrir un archivo de solo lectura");
	fs_destroy(); 

	free(memory_buffer);
}END_TEST

START_TEST (test_fat16_read_long_file) {
	hdd_init("hdd.img");

	void * memory_buffer = malloc(TOTAL_MEM);
	kmem_init(memory_buffer,TOTAL_MEM);

	fs_init();

	fs_descr f = fs_open("/dir2/archivito.txt","r");
	fail_if(fs_error(f),"El archivo no se abrio correctamente");

	char buffer[ATA_SECTSIZE+2];
	char * res = read_file("testimg/dir2/archivito.txt",ATA_SECTSIZE+1);
	
	fs_read(f,ATA_SECTSIZE+1,buffer);
	buffer[strlen(res)] = '\0';
	
	fail_unless(!memcmp(buffer,res,ATA_SECTSIZE+1),
			"La lectura no se realizo correctamente."
			"Se esperaba: <%s> se obtuvo <%s>",res,buffer);

	fail_if(fs_close(f) == -1, "El archivo no se cerro correctamente");
	
	free(res);
	fs_destroy(); 

	free(memory_buffer);
}END_TEST

START_TEST (test_fat16_read_close_file) {
	hdd_init("hdd.img");

	void * memory_buffer = malloc(TOTAL_MEM);
	kmem_init(memory_buffer,TOTAL_MEM);

	fs_init();
	fs_descr f = fs_open("/dir2/archivito.txt","r");
	fs_close(f);
	
	fail_unless(fs_read(f,1,NULL) == -1,
		"El archivo no se cerro correctamente");
	fs_destroy(); 
	
	free(memory_buffer);
}END_TEST

START_TEST (test_fat16_read_longfilename_file){
	hdd_init("hdd.img");

	void * memory_buffer = malloc(TOTAL_MEM);
	kmem_init(memory_buffer,TOTAL_MEM);

	fs_init();

	fs_descr f = fs_open("/dir1/archivoConNombreMuyMuyMuyMuyLargo.txt","r");
	fail_if(fs_error(f),"No se abrio bien el archivo");

	char buffer[50];
	char * res = read_file("testimg/dir1/archivoConNombreMuyMuyMuyMuyLargo.txt",12);

	fs_read(f,strlen(res),buffer);
	buffer[strlen(res)] = '\0';

	fail_unless(!memcmp(buffer,res,strlen(res)+1),"La lectura no se realizo correctamente");
	free(res);
	fs_destroy(); 

	free(memory_buffer);
}END_TEST

START_TEST (test_fat16_read_longdir){
	hdd_init("hdd.img");

	void * memory_buffer = malloc(TOTAL_MEM);
	kmem_init(memory_buffer,TOTAL_MEM);

	fs_init();

	fs_descr f = fs_open("/dir3/30.txt","r");
	fail_if(fs_error(f), "No se abrio bien el archivo");

	char buffer[50], * res = "Archivo 30";
	fs_read(f,strlen(res),buffer);
	buffer[strlen(res)] = '\0';

	fail_unless(!memcmp(buffer,res,strlen(res)+1),
			"La lectura no se realizo correctamente");
	fs_destroy();
	free(memory_buffer);	
}END_TEST

START_TEST (test_read_several_files) {
	hdd_init("hdd.img");

	void * memory_buffer = malloc(TOTAL_MEM);
	kmem_init(memory_buffer,TOTAL_MEM);

	fs_init();
	
	fs_descr f1 = fs_open("/prueba.txt","r");
	fs_descr f2 = fs_open("/dir1/a.txt","r");

	fail_if(fs_error(f1),"No se abrio bien el archivo"); 
	fail_if(fs_error(f2), "No se abrio bien el archivo");
	
	char buffer1[1024], buffer2[1024];
	memset(buffer1,0,sizeof(buffer1));
	memset(buffer2,0,sizeof(buffer2));
	fs_read(f1,1000,buffer1);
	fs_read(f2,1000,buffer2);

	char * res1 = "HOLA\n";
	char * res2 = "EJEMPLO DE ARCHIVO\n";
	
	fail_unless(!memcmp(buffer1,res1,strlen(res1)),
			"No se leyo bien el archivo prueba. "
			"Se esperaba %s se obtuvo %s",res1,buffer1);
	fail_unless(!memcmp(buffer2,res2,strlen(res2)),
			"No se leyo bien el archivo a");

	fs_destroy();
	free(memory_buffer);
} END_TEST

START_TEST (test_fat16_read_emptyfile) {
	hdd_init("hdd.img");

	void * memory_buffer = malloc(TOTAL_MEM);
	kmem_init(memory_buffer,TOTAL_MEM);

	fs_init();

	fs_descr f = fs_open("/dir1/vacio.txt","r");
	fail_if(fs_error(f), "No se abrio bien el archivo");

	fail_unless(!fs_read(f,1000,NULL),
			"Cantidad de bytes leidos incorrecta");
	fs_destroy(); 
	free(memory_buffer);
}END_TEST

START_TEST (test_fat16_two_reads){
	hdd_init("hdd.img");
	
	void * memory_buffer = malloc(TOTAL_MEM);
	kmem_init(memory_buffer,TOTAL_MEM);

	fs_init();
	
	fs_descr f = fs_open("/dir1/a.txt","r");
	fail_if(fs_error(f),"No se abrio bien el archivo");

	char buffer[50];
	memset(buffer,0,sizeof(buffer));
	fs_read(f,4,buffer);
	fs_read(f,1000,buffer+4);

	char * res = "EJEMPLO DE ARCHIVO\n";
	fail_unless(!strcmp(buffer,res),
			"No se leyo bien el archivo. "
			"Se esperaba %s se obtuvo %s.",res,buffer);

	fs_close(f);
	fs_destroy(); 
	free(memory_buffer);
}END_TEST

START_TEST (test_fat16_read_up_a_dir){
	hdd_init("hdd.img");

	void * memory_buffer = malloc(TOTAL_MEM);
	kmem_init(memory_buffer,TOTAL_MEM);

	fs_init();

	fs_descr f = fs_open("/dir2/subdir/../ejemplo.extension","r");
	fail_if(fs_error(f),"No se pudo abrir el archivo");
	
	char buffer[100];
	memset(buffer,0,sizeof(buffer));
	char * res = "Este es un ejemplo de archivo\n";
	fs_read(f,1000,buffer);
	
	fail_unless(!strcmp(buffer,res),
		    "No se leyo bien el archivo. "
		    "Se esperaba %s se obtuvo %s ",res,buffer);

	fs_close(f);
	fs_destroy(); 
	free(memory_buffer);
}END_TEST

START_TEST (test_fat16_cant_write_read_file){
	hdd_init("hdd.img");

	void * memory_buffer = malloc(TOTAL_MEM);
	kmem_init(memory_buffer,TOTAL_MEM);

	fs_init();

	fs_descr f = fs_open("/dir1/a.txt","r");
	fail_unless(fs_write(f,0,NULL) == -1,
			"Se escribio un archivo abierto para leer");
	fs_destroy(); 
	free(memory_buffer);
}END_TEST

START_TEST (test_fat16_append){
	hdd_init("hdd.img");

	void * memory_buffer = malloc(TOTAL_MEM);
	kmem_init(memory_buffer,TOTAL_MEM);

	fs_init();

	fs_descr f = fs_open("/dir1/a.txt","ra");
	fail_if(fs_error(f), "No se abrio bien el archivo");

	char * buffer = "ESTA LINEA SE LA AGREGO YO";
	uint written = fs_write(f,strlen(buffer),buffer);
	fail_unless(written == strlen(buffer),
			"No se devolvio el conteo de bytes correcto");

	char content[1000];
	memset(content,0,sizeof(content));
	fs_read(f,1000,content);
	char * expectation = "EJEMPLO DE ARCHIVO\nESTA LINEA SE LA AGREGO YO";

	fail_unless(!strcmp(content,expectation),
			"No se escribio bien el archivo."
			"Se esperaba %s se obtuvo %s.",expectation,content);
	fs_close(f);
	fs_destroy(); 

	free(memory_buffer);
}END_TEST

START_TEST (test_fat16_append_beyond_sector) {
	hdd_init("hdd.img");

	void * memory_buffer = malloc(TOTAL_MEM);
	kmem_init(memory_buffer,TOTAL_MEM);

	fs_init();
	
	fs_descr f = fs_open("/dir1/vacio.txt","ra");
	fail_if(fs_error(f), "No se abrio bien el archivo");
	
	char * buffer = read_file("archivoGrande.txt",ATA_SECTSIZE+1);
	char * buffer2= strdup(buffer);

	fs_write(f,strlen(buffer),buffer);
	fs_read(f,strlen(buffer),buffer2);

	fail_unless(!strcmp(buffer2,buffer),
			"No se escribio bien el archivo");

	free(buffer); free(buffer2);
	fs_close(f);
	
	fs_destroy(); 
	free(memory_buffer);
}END_TEST

START_TEST (test_fat16_append_then_read) {
	hdd_init("hdd.img");

	void * memory_buffer = malloc(TOTAL_MEM);
	kmem_init(memory_buffer,TOTAL_MEM);

	fs_init();
	
	fs_descr f = fs_open("/dir1/vacio.txt","a");
	fail_if(fs_error(f), "No se abrio bien el archivo");
	
	char * buffer = read_file("archivoGrande.txt",ATA_SECTSIZE+1);
	char * buffer2= strdup(buffer);

	fs_write(f,strlen(buffer),buffer);
	fs_descr f2 = fs_open("/dir1/vacio.txt","r");
	fs_read(f2,strlen(buffer),buffer2);

	fail_unless(!strcmp(buffer2,buffer),
			"No se escribio bien el archivo");

	free(buffer); free(buffer2);
	fs_close(f); fs_close(f2);
	fs_destroy(); 

	free(memory_buffer);
}END_TEST

START_TEST (test_fat16_create_new_file) { 
	hdd_init("hdd.img");

	void * memory_buffer = malloc(TOTAL_MEM);
	kmem_init(memory_buffer,TOTAL_MEM);

	fs_init();

	fs_descr f = fs_open("/nuevo.txt","w");
	fail_if(fs_error(f), "No se creo bien el archivo");

	char * buffer = "Este es el contenido del archivo";
	char buffer2[1000];
	memset(buffer2,0,sizeof(buffer2));
	fs_write(f,strlen(buffer),buffer);

	fs_descr f2 = fs_open("/nuevo.txt","r");
	fail_if(fs_error(f2), "No se abrio bien el archivo");

	fs_read(f2,strlen(buffer),buffer2);
	buffer2[strlen(buffer)] = '\0';

	fail_unless(!strcmp(buffer,buffer2),
			"No se escribio bien el archivo");

	fs_close(f); fs_close(f2);
	fs_destroy(); 

	free(memory_buffer);
}END_TEST

START_TEST (test_fat16_create_new_file_in_subdir) {
	hdd_init("hdd.img");

	void * memory_buffer = malloc(TOTAL_MEM);
	kmem_init(memory_buffer,TOTAL_MEM);

	fs_init();

	fs_descr f = fs_open("/dir1/stuff.txt","w");
	fail_if(fs_error(f), "No se creo bien el archivo");

	char * buffer = "Este es el contenido del archivo";
	char buffer2[1000];
	memset(buffer2,0,sizeof(buffer2));

	fs_write(f,strlen(buffer),buffer);

	fs_descr f2 = fs_open("/dir1/stuff.txt","r");
	fail_if(fs_error(f2), "No se abrio bien el archivo");

	fs_read(f2,strlen(buffer),buffer2);
	buffer2[strlen(buffer)] = '\0';

	fail_unless(!strcmp(buffer,buffer2),
			"No se escribio bien el archivo. "
			"Se esperaba %s se obtuvo %s.",
		  		buffer,buffer2);

	fs_close(f); fs_close(f2);
	fs_destroy(); 

	free(memory_buffer);
}END_TEST

START_TEST (test_fat16_write_existing_file) {
	hdd_init("hdd.img");

	void * memory_buffer = malloc(TOTAL_MEM);
	kmem_init(memory_buffer,TOTAL_MEM);

	fs_init();

	fs_descr f = fs_open("/dir1/a.txt","rw");
	char * content = read_file("testimg/dir1/a.txt",19);

	char buffer[1000] = {0};
	fs_read(f,1000,buffer);

	fail_unless(!strcmp(buffer,content),
			"No se leyo bien el archivo. "
			"Se esperaba <%s> se obtuvo <%s>",
				content,buffer);
	
	free(content);
	content = "EL NUEVO CONTENIDO DEL ARCHIVO";
	
	fs_write(f,strlen(content),content);
	
	fs_descr f2 = fs_open("/dir1/a.txt","r");
	memset(buffer,0,sizeof(buffer));
	fs_read(f2,1000,buffer);
	
	fail_unless(!strcmp(buffer,content),
		"No se leyo bien el archivo. "
		"Se esperaba %s se obtuvo %s.",
			buffer,content);

	fs_destroy();
	free(memory_buffer);
} END_TEST

START_TEST (test_fat16_write_existing_file_beyond_sector) {
	hdd_init("hdd.img");

	void * memory_buffer = malloc(TOTAL_MEM);
	kmem_init(memory_buffer,TOTAL_MEM);

	fs_init();
	
	fs_descr f = fs_open("/dir1/a.txt","w");
	char * content = malloc(1+4*ATA_SECTSIZE);

	memset(content,'a',4*ATA_SECTSIZE);
	content[4*ATA_SECTSIZE] = '\0';
	
	fs_write(f,strlen(content),content);
	
	fs_descr f2 = fs_open("/dir1/a.txt","r");
	char * buffer = malloc(1+4*ATA_SECTSIZE);

	fs_read(f2,4*ATA_SECTSIZE,buffer);
	buffer[4*ATA_SECTSIZE] = '\0';

	fail_unless(!strcmp(buffer,content),
		"No se escribio bien el archivo");

	free(content); free(buffer);
	fs_destroy();

	free(memory_buffer);
} END_TEST

START_TEST (test_fat16_create_new_file_in_new_subdir) {
	hdd_init("hdd.img");
	
	void * memory_buffer = malloc(TOTAL_MEM);
	kmem_init(memory_buffer,TOTAL_MEM);
	
	fs_init();

	fs_descr f = fs_open("/dir4/stuff.txt","w");
	fail_if(fs_error(f), "No se creo bien el archivo");

	char * buffer = "Este es el contenido del archivo";
	char buffer2[1000];

	memset(buffer2,0,sizeof(buffer2));
	fs_write(f,strlen(buffer),buffer);
	
	fs_descr f2 = fs_open("/dir4/stuff.txt","r");
	fail_if(fs_error(f2), "No se abrio bien el archivo");

	fs_read(f2,strlen(buffer),buffer2);
	buffer2[strlen(buffer)] = '\0';

	fail_unless(!strcmp(buffer,buffer2),
			"No se escribio bien el archivo");

	fs_close(f); fs_close(f2);	
	fs_destroy(); 

	free(memory_buffer);
}END_TEST

START_TEST (test_fat16_create_new_file_in_double_subdir){
	hdd_init("hdd.img");

	void * memory_buffer = malloc(TOTAL_MEM);
	kmem_init(memory_buffer,TOTAL_MEM);

	fs_init();
	fs_descr f = fs_open("/dir4/subdir/stuff.txt","w");
	fail_if(fs_error(f), "No se creo bien el archivo");

	char * buffer = "Este es el contenido del archivo";
	char buffer2[1000];

	memset(buffer2,0,sizeof(buffer2));
	fs_write(f,strlen(buffer),buffer);
	
	fs_descr f2 = fs_open("/dir4/subdir/stuff.txt","r");
	fail_if(fs_error(f2), "No se abrio bien el archivo");

	fs_read(f2,strlen(buffer),buffer2);
	buffer2[strlen(buffer)] = '\0';

	fail_unless(!strcmp(buffer,buffer2),
			"No se escribio bien el archivo");

	fs_close(f); fs_close(f2);	
	fs_destroy(); 

	free(memory_buffer);
}END_TEST

START_TEST (test_fat16_unlink_file) {
	hdd_init("hdd.img");
	
	void * memory_buffer = malloc(TOTAL_MEM);
	kmem_init(memory_buffer,TOTAL_MEM);
	
	fs_init();
	
	fail_if(fs_open("/prueba.txt","r") == -1,
			"No se pudo abrir un archivo existente");

	fail_unless(fs_unlink("/prueba.txt") != -1,
			"No se elimino bien el archivo");
	fail_unless(fs_open("/prueba.txt","r") == -1,
			"Se pudo abrir un archivo borrado");
	fail_unless(fs_open("/dir1/a.txt","r") != -1,
			"No se pudo abrir un archivo no borrado");
	fs_destroy(); 

	free(memory_buffer);
}END_TEST

START_TEST (test_fat16_unlink_file_in_subdir) {
	hdd_init("hdd.img");
	
	void * memory_buffer = malloc(TOTAL_MEM);
	kmem_init(memory_buffer,TOTAL_MEM);
	
	fs_init();
	
	fail_if(fs_open("/dir1/a.txt","r") == -1,
			"No se pudo abrir un archivo existente");

	fail_unless(fs_unlink("/dir1/a.txt") != -1,
			"No se elimino bien el archivo");
	fail_unless(fs_open("/dir1/a.txt","r") == -1,
			"Se pudo abrir un archivo borrado");
	fail_if(fs_open("/dir1/b.txt","r") == -1,
			"No se pudo abrir un archivo no borrado");
	fs_destroy(); 
	
	free(memory_buffer);
}END_TEST

Suite * fs_suite(void){
	Suite * s = suite_create("Filesystem FAT 16");

	TCase * tc_core = tcase_create ("Core");

	tcase_add_test(tc_core,test_fat16_initialize);
	tcase_add_test(tc_core,test_fat16_read_file_in_rootdir);
	tcase_add_test(tc_core,test_fat16_read_file_in_subdir);
	tcase_add_test(tc_core,test_fat16_open_file_not_exists);
	tcase_add_test(tc_core,test_fat16_read_long_file);
	tcase_add_test(tc_core,test_fat16_read_rdonly_file);
	tcase_add_test(tc_core,test_fat16_read_longfilename_file);
	tcase_add_test(tc_core,test_fat16_two_reads);
	tcase_add_test(tc_core,test_fat16_read_emptyfile);
	tcase_add_test(tc_core,test_fat16_read_up_a_dir);
	tcase_add_test(tc_core,test_fat16_cant_write_read_file);
	tcase_add_test(tc_core,test_fat16_read_close_file);
	tcase_add_test(tc_core,test_fat16_read_longdir);
	tcase_add_test(tc_core,test_fat16_append);
	tcase_add_test(tc_core,test_fat16_append_beyond_sector);
	tcase_add_test(tc_core,test_fat16_append_then_read);
	tcase_add_test(tc_core,test_fat16_create_new_file);
	tcase_add_test(tc_core,test_fat16_create_new_file_in_double_subdir);	
	tcase_add_test(tc_core,test_fat16_create_new_file_in_new_subdir);
	tcase_add_test(tc_core,test_fat16_create_new_file_in_subdir);
	tcase_add_test(tc_core,test_fat16_write_existing_file_beyond_sector);
	tcase_add_test(tc_core,test_fat16_unlink_file);
	tcase_add_test(tc_core,test_fat16_unlink_file_in_subdir);
	tcase_add_test(tc_core,test_read_several_files);
	tcase_add_test(tc_core,test_fat16_write_existing_file);
	suite_add_tcase(s,tc_core);

	return s;
}

int main(){	
	int number_failed;
	
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

