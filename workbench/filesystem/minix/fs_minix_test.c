#include "fs_minix.h"
#include "hdd.h"
#include "buffer_cache.h"

#include <check.h>
#include <stdlib.h>
#include <stdio.h>

static uint NEXT_INODE = 261;

START_TEST (test_minix_super_block)
{
	super_block b;
	fs_minix_init(&b);

	fail_if(b.block_size != 1024);
	fail_if(b.magic != MINIX_MAGIC);
	fail_if(b.is_dirty);
	fail_if(b.max_file_size != 268966912);
	fail_if(((minix_fs_data*)(b.fs_data))->data_zone_start != 173);
	fail_if(((minix_fs_data*)(b.fs_data))->number_inodes != 5376);
	fail_if(b.disk_size != 16515072,
	        "Se esperaba %d se obtuvo %d",16515072,b.disk_size);
	fail_if(b.root->inode_number != MINIX_ROOT_INODE);
	fail_if(b.root->inode_type != FS_DIR);
} END_TEST

START_TEST (test_minix_write_super_block)
{
	super_block b;
	fs_minix_init(&b);

	minix_fs_data * mfd = b.fs_data;
	bitset * ibm = &mfd->inode_bitmap;
	uint next = bitset_search(ibm);
	fail_if(next != NEXT_INODE-1,
	        "Proximo inodo libre incorrecto. Se esperaba "
	        "%d se obtuvo %d",NEXT_INODE,next);

	bitset_set(ibm,next);
	fail_unless(bitset_search(ibm) == next+1);
	b.ops->write_super(&b);
	fs_minix_init(&b);

	fail_if(bitset_search(ibm) != next+1,
	        "Se esperaba %d se obtuvo %d",next+1,bitset_search(ibm));
} END_TEST

START_TEST (test_minix_read_inode)
{
	super_block b;
	fs_minix_init(&b);

	inode * ino = b.ops->alloc_inode(&b);
	ino->inode_number = 6;
	b.ops->read_inode(ino);

	fail_unless(ino->file_size == 3300, "File Size incorrecto. "
	            "Se esperaba %d se obtuvo %d\n",11,ino->file_size);
	fail_unless(ino->use_count == 1, "Use count incorrecto. "
	            "Se esperaba %d se obtuvo %d\n",0,ino->use_count);
	fail_unless(ino->inode_type == FS_FILE, "Tipo de archivo incorrecto. "
	            "Se esperaba FILE (%d) se obtuvo %d\n",FS_FILE,ino->inode_type);
}
END_TEST

START_TEST (test_minix_delete_inode)
{
	super_block bd;
	super_block * b = &bd;
	fs_minix_init(&bd);

	inode * i = b->ops->alloc_inode(b);
	i->inode_number = 6;
	fail_unless(!b->ops->delete_inode(i));
	minix_fs_data * mfd = b->fs_data;
	fail_unless(!bitset_get(&mfd->inode_bitmap,5));
}
END_TEST

START_TEST (test_minix_lookup_inode)
{
	super_block bd;
	super_block * b = &bd;
	fs_minix_init(&bd);

	inode * root = b->root;
	inode * res = root->i_ops->lookup(root,"prueba1.txt");

	fail_unless(res->inode_number == 6);
	fail_unless(res->file_size == 3300);
	fail_unless(res->inode_type == FS_FILE);
}
END_TEST

START_TEST (test_minix_lookup_inode_dir)
{
	super_block bd;
	super_block * b = &bd;
	fs_minix_init(&bd);

	inode * root = b->root;
	inode * res = root->i_ops->lookup(root,"docs");

	fail_unless(res->inode_number == 3);
	fail_unless(res->inode_type == FS_DIR);
	fail_unless(res->file_size == 128,
		"Se esperaba %d se obtuvo %d\n",0,res->file_size);
}
END_TEST

START_TEST (test_minix_lookup_inode_non_existent)
{
	super_block bd;
	super_block * b = &bd;
	fs_minix_init(&bd);

	inode * root = b->root;
	inode * res = root->i_ops->lookup(root,"NOEXISTE");

	fail_unless(res == NULL);
}
END_TEST

START_TEST (test_minix_lookup_far_inode)
{
	super_block bd;
	fs_minix_init(&bd);

	char * filenames[] = {"1.txt","2.txt","10.txt",
		"225.txt","249.txt",NULL };
	int inodes[] = {163,43,170,247,249};

	inode * root = bd.root;
	fail_unless(root != NULL);	
	inode * res = root->i_ops->lookup(root,"testDir");
	fail_unless(res && res->inode_type == FS_DIR);

	for(int i = 0; filenames[i]; i++) {
		inode * ino = root->i_ops->lookup(res,filenames[i]);
		fail_unless(ino != NULL, "No se encontro el archivo %s",filenames[i]);
		fail_unless(ino->inode_number == inodes[i], 
			"inodo incorrecto,se esperaba %d se obtuvo %d",
			inodes[i],ino->inode_number);
	}
}
END_TEST

START_TEST (test_minix_mkdir_inode)
{
	super_block bd;
	fs_minix_init(&bd);

	inode * root = bd.root;
	int res = root->i_ops->mkdir(root,"games");
	buffers_flush_all();
	fail_unless(res == 0,"No se pudo crear el inodo para games");
	inode * games = root->i_ops->lookup(root,"games");
	fail_unless(games != NULL);
	
	minix_inode * mi = root->info_disk;

	minix_dir_entry entries[MINIX_DIR_ENTRIES];
	minix_hdd_read(mi->zones[0],1,&entries);
	ushort inum = entries[7].inode;
	char * name = entries[7].name;

	fail_unless(inum == NEXT_INODE,"Se esperaba %d" 
		" se obtuvo %d",NEXT_INODE,inum);
	fail_unless(games->inode_number == inum, "Inodo invalido "
		"se esperaba %d se obtuvo %d\n",games->inode_number,inum);	
	fail_unless(!strcmp(name,"games"),"Nombre invalido para "
		"inodo %d. Se esperaba games se obtuvo %s\n",inum,name);
	fail_unless(games->inode_type == FS_DIR,"games no es un "
		"directorio");
	fail_unless(games->file_size == 64); 
}
END_TEST

START_TEST (test_minix_rmdir) { 
	super_block bd;
	fs_minix_init(&bd);

	inode * root = bd.root;
	inode * bin = root->i_ops->lookup(root,"bin");
	fail_unless(bin && bin->inode_number == 2);
	fail_unless(!root->i_ops->rmdir(root,"bin"));	
	buffers_flush_all();
	fail_unless(root->i_ops->lookup(root,"bin") == NULL);
} END_TEST

START_TEST (test_minix_read) {
	super_block bd;
	fs_minix_init(&bd);

	inode * root = bd.root;
	inode * prueba1 = root->i_ops->lookup(root,"prueba1.txt");
	fail_unless(prueba1 && prueba1->inode_number == 6);
	
	char buffer[513];
	file_object f = { .access_mode = FS_RD };
	
	prueba1->f_ops->open(prueba1,&f);
		
	int read = f.f_ops->read(&f,11,buffer);
	fail_unless(read == 11,"Se esperaba leer 11 bytes" 
		" se leyo %d\n",read);	
	buffer[read] = '\0';	
	fail_unless(!strcmp(buffer,"Hola mundo\n"),
		"Se esperaba 'Hola Mundo' se obtuvo '%s'\n",buffer);
} END_TEST

START_TEST (test_minix_write) {
	super_block bd;
	fs_minix_init(&bd);

	inode * root = bd.root;
	inode * prueba1 = root->i_ops->lookup(root,"prueba1.txt");	
	fail_unless(prueba1 && prueba1->inode_number == 6);

	char buffer[513]; char data[] = "Hola mundo!!\n";

	file_object f = { .access_mode = FS_RD };
	file_object f2 = { .access_mode = FS_WR };
	 
	prueba1->f_ops->open(prueba1,&f);
	prueba1->f_ops->open(prueba1,&f2);

	int res = prueba1->f_ops->write(&f2,strlen(data),data);
	fail_unless(res == strlen(data),"Se esperaba %d" 
		" se obtuvo %d",strlen(data),res);
	fail_unless(f2.file_write_offset == prueba1->file_size);	

	f.f_ops->seek(&f,prueba1->file_size-11-strlen(data));
	res = prueba1->f_ops->read(&f,11+strlen(data),buffer);
	fail_unless(res == 11+strlen(data),"Se esperaba %d" 
		" se obtuvo %d",11+strlen(data),res);	

	buffer[res] = '\0';
	char * compare = "Hola mundo\nHola mundo!!\n";
	fail_unless(!strcmp(buffer,compare),
		"Se esperaba <%s> se obtuvo <%s>\n",buffer,compare);	
} END_TEST

START_TEST (test_minix_readdir){
	super_block bd;
	fs_minix_init(&bd);

	inode * root = bd.root;
	char * entries[] = { ".","..", "bin", "docs", 
		"prueba1.txt", "testDir", "dev" };
	int inodes[] = { 1, 1, 2, 3, 6, 7, 258 };
	int i = 0;
	
	fail_unless(root->f_ops->readdir != NULL);
	file_object f = { .access_mode = FS_RD };	
	root->f_ops->open(root,&f);
	dirent d;

	while(root->f_ops->readdir(&f,&d) == sizeof(d)){
		fail_unless(d.inode_number == inodes[i], "Se esperaba %d "
			"se obtuvo %d", inodes[i],d.inode_number );
		fail_unless(!strcmp(d.name,entries[i]), "Se esperaba %s "
			"se obtuvo %s", d.name,entries[i]);
		i++;	
	}
	fail_unless(i == 7,"Se esperaba 7 se obtuvo %d\n",i);
} END_TEST

START_TEST (test_minix_create) {
	super_block bd; fs_minix_init(&bd);

	inode * root = bd.root;
	int res = root->i_ops->create(root,"doom.txt");
	fail_unless(!res,"Se esperaba 0 se obtuvo %d\n",res);
	inode * ino = root->i_ops->lookup(root,"doom.txt");
	
	fail_unless(ino != NULL);
	fail_unless(ino->inode_number == NEXT_INODE,"Se esperaba "
		"%d se obtuvo %d\n",NEXT_INODE,ino->inode_number);
	fail_unless(ino->inode_type == FS_FILE,"Se esperaba 0"
		"se obtuvo %d\n",ino->inode_type,FS_FILE);
	fail_unless(ino->file_size == 0,"Se esperaba 0" 
		" se obtuvo %d\n",ino->file_size);
} END_TEST

START_TEST (test_minix_create_and_write) {
	super_block bd; fs_minix_init(&bd);

	inode * root = bd.root;
	int res = root->i_ops->create(root,"doom.txt");
	fail_unless(!res,"Se esperaba 0 se obtuvo %d\n",res);
	inode * ino = root->i_ops->lookup(root,"doom.txt");
	fail_unless(ino != NULL);

	file_object f = { .access_mode = FS_RD };
	file_object f2 = { .access_mode = FS_WR };

	ino->f_ops->open(ino,&f2);
	ino->f_ops->open(ino,&f);
	char * data = "This is not real. This is but a surreal "
		"conjure by a madman";
	int written = f2.f_ops->write(&f2,strlen(data)+1,data);
	fail_unless(written == strlen(data)+1);
	char buf[512];

	f.f_ops->seek(&f,0);
	int read = f.f_ops->read(&f,strlen(data)+1,buf);

	fail_unless(read == strlen(data)+1);
	fail_unless(!strcmp(buf,data));
} END_TEST

START_TEST (test_minix_read_from_subdir) {
	super_block bd; fs_minix_init(&bd);

	inode * root = bd.root;
	inode * docs = root->i_ops->lookup(root,"docs");
	inode * prueba2 = root->i_ops->lookup(docs,"prueba2.txt");
	
	file_object f = { .access_mode = FS_RD };
	char buf[512];char * data = "Esto es documentacion\n";
	prueba2->f_ops->open(prueba2,&f);
	int res = f.f_ops->read(&f,512,buf);	
	buf[res] = '\0';
	fail_unless(res == strlen(data));
	fail_unless(!strcmp(buf,data));
} END_TEST

START_TEST (test_minix_unlink_file) {
	super_block bd; fs_minix_init(&bd);

	inode * root = bd.root;
	int res = root->i_ops->unlink(root,"prueba1.txt");		
	fail_unless(!res,"Fallo borrar: %d\n",res);
	buffers_flush_all(); hdd_output("hdd_out.img");	
	fail_unless(root->i_ops->lookup(root,"prueba1.txt") == NULL);

} END_TEST

START_TEST (test_minix_read_several) {
	super_block bd; fs_minix_init(&bd);

	inode * root = bd.root;
	inode * prueba1 = root->i_ops->lookup(root,"prueba1.txt");
	
	file_object f = { .access_mode = FS_RD };
	char buf[2048], realdata[2048];
	
	FILE * real = fopen("testimg/prueba1.txt","r");
	fread(realdata, 1, 2048, real);

	prueba1->f_ops->open(prueba1,&f);
	int res = f.f_ops->read(&f,2048,buf);
	
	fail_unless(res == 2048,
		"Se esperaba 2048 se obtuvo %d\n",res);	
	fail_unless(!memcmp(buf,realdata,2048));	
} END_TEST

START_TEST (test_minix_read_major_minor) {
	super_block bd; fs_minix_init(&bd);
	inode * root = bd.root;
	inode * dev = root->i_ops->lookup(root,"dev");
	fail_if(!dev);
	inode * tty = dev->i_ops->lookup(dev,"tty");
	fail_if(!tty);
	fail_if(tty->inode_number != 259);
	char_dev * cd = tty->info_cdev;
	fail_if(!cd);
	fail_if(cd->major_number != 0xDE,"Se esperaba %x se obtuvo %x",
		0xDE,cd->major_number);
	fail_if(cd->minor_number != 0xAD,"Se esperaba %x se obtuvo %x",
		0xAD,cd->minor_number);
} END_TEST

START_TEST (test_minix_read_and_write_append) {
	super_block bd; fs_minix_init(&bd);
	inode * root = bd.root;
	inode * docs = root->i_ops->lookup(root,"docs");
	fail_if(docs == NULL);	
	inode * prueba2 = docs->i_ops->lookup(docs,"prueba2.txt");
	fail_if(prueba2 == NULL);
	file_object f = { .access_mode = FS_RDWR };
	char buf1[2048],buf2[2048];
	strcpy(buf2,"Esto tambien\n");	
	prueba2->f_ops->open(prueba2,&f);
	int written = f.f_ops->write(&f,strlen(buf2),buf2);	
	fail_if(written != strlen(buf2));
	char * new_contents = "Esto es documentacion\nEsto tambien\n";
	int read = f.f_ops->read(&f,strlen(new_contents),buf1);
	fail_unless(read == strlen(new_contents));
	fail_unless(!strcmp(buf1,new_contents));	
} END_TEST

START_TEST (test_minix_read_and_write_trunc) {
	super_block bd; fs_minix_init(&bd);
	inode * root = bd.root;
	inode * docs = root->i_ops->lookup(root,"docs");
	fail_if(docs == NULL);	
	inode * prueba2 = docs->i_ops->lookup(docs,"prueba2.txt");
	fail_if(prueba2 == NULL);
	file_object f = { .access_mode = FS_RDWR, .flags = FS_TRUNC };
	char buf1[2048],buf2[2048];
	strcpy(buf2,"Esto seria documentacion\n");	
	prueba2->f_ops->open(prueba2,&f);
	int written = f.f_ops->write(&f,strlen(buf2),buf2);	
	fail_if(written != strlen(buf2));
	char * new_contents = "Esto seria documentacion";
	int read = f.f_ops->read(&f,strlen(new_contents),buf1);
	fail_unless(read == strlen(new_contents));
	fail_unless(!strcmp(buf1,new_contents));	
} END_TEST

START_TEST (test_minix_open_two_files) {
	super_block bd; fs_minix_init(&bd);
	inode * root = bd.root;
	inode * docs = root->i_ops->lookup(root,"docs");	
	inode * prueba2 = docs->i_ops->lookup(docs,"prueba2.txt");
	inode * prueba3 = docs->i_ops->lookup(docs,"prueba3.txt");
	fail_if(prueba2 == NULL); fail_if(prueba3 == NULL);
	file_object f1 = { .access_mode = FS_RD };
	file_object f2 = { .access_mode = FS_RD }; 
	char buf1[2048],buf2[2048],buf3[2048];
	strcpy(buf2,"Esto es documentacion\n");
	strcpy(buf3,"Esto es mas documentacion\n");
	prueba2->f_ops->open(prueba2,&f1);
	prueba3->f_ops->open(prueba3,&f2);
	int read1 = f1.f_ops->read(&f1,strlen(buf2),buf1);
	fail_if(read1 != strlen(buf2));
	buf1[read1] = '\0';
	fail_unless(!strcmp(buf1,buf2));
	int read2 = f2.f_ops->read(&f2,strlen(buf3),buf1);
	fail_if(read2 != strlen(buf3));
	buf1[read2] = '\0';
	fail_unless(!strcmp(buf1,buf3));
} END_TEST

START_TEST (test_minix_flush_file) {
	super_block bd;
	fs_minix_init(&bd);

	inode * root = bd.root;
	inode * prueba1 = root->i_ops->lookup(root,"prueba1.txt");	
	fail_unless(prueba1 && prueba1->inode_number == 6);

	char buffer[513]; char data[] = "Hola mundo!!\n";

	file_object f = { .access_mode = FS_RD };
	file_object f2 = { .access_mode = FS_WR };
	 
	prueba1->f_ops->open(prueba1,&f);
	prueba1->f_ops->open(prueba1,&f2);

	int res = prueba1->f_ops->write(&f2,strlen(data),data);
	fail_unless(res == strlen(data),"Se esperaba %d" 
		" se obtuvo %d",strlen(data),res);
	fail_unless(f2.file_write_offset == prueba1->file_size);	

	f.f_ops->seek(&f,prueba1->file_size-11-strlen(data));
	res = prueba1->f_ops->read(&f,11+strlen(data),buffer);
	fail_unless(res == 11+strlen(data),"Se esperaba %d" 
		" se obtuvo %d",11+strlen(data),res);
	buffer[res] = '\0';
	char * compare = "Hola mundo\nHola mundo!!\n";
	fail_unless(!strcmp(buffer,compare),
		"Se esperaba <%s> se obtuvo <%s>\n",buffer,compare);	
	
	buffers_flush_all();
	
	hdd_output(	"hdd_output.img");
	hdd_init(	"hdd_output.img");
	
	f.f_ops->seek(&f,prueba1->file_size-11-strlen(data));
	fail_unless(f.f_ops->read(&f,1000,buffer) == strlen(compare));
	fail_unless(!strcmp(buffer,compare),
		"Se esperaba <%s> se obtuvo <%s>\n",buffer,compare);	
} END_TEST

START_TEST (test_minix_create_and_ls) {
	super_block bd; fs_minix_init(&bd);

	inode * root = bd.root;
	int res = root->i_ops->create(root,"doom.txt");
	fail_unless(!res,"Se esperaba 0 se obtuvo %d\n",res);
	
	char * entries[] = { ".","..", "bin", "docs", 
		"prueba1.txt", "testDir", "dev", "doom.txt" };
	int inodes[] = { 1, 1, 2, 3, 6, 7, 258, NEXT_INODE };
	int i = 0;
	
	fail_unless(root->f_ops->readdir != NULL);
	file_object f = { .access_mode = FS_RD };	
	root->f_ops->open(root,&f);
	dirent d;
	
	while(root->f_ops->readdir(&f,&d) == sizeof(d)){
		fail_unless(d.inode_number == inodes[i], "Se esperaba %d "
			"se obtuvo %d", inodes[i],d.inode_number );
		fail_unless(!strcmp(d.name,entries[i]), "Se esperaba %s "
			"se obtuvo %s", d.name,entries[i]);
		i++;	
	}

	fail_unless(i == 8,"Se esperaba 8 se obtuvo %d\n",i);
	buffers_flush_all();
} END_TEST

START_TEST (test_minix_close) {
	super_block bd;
	fs_minix_init(&bd);

	inode * root = bd.root;
	inode * prueba1 = root->i_ops->lookup(root,"prueba1.txt");
	fail_unless(prueba1 && prueba1->inode_number == 6);
	
	char buffer[513];
	file_object f = { .access_mode = FS_RD };
	
	prueba1->f_ops->open(prueba1,&f);
		
	int read = f.f_ops->read(&f,11,buffer);
	fail_unless(read == 11,"Se esperaba leer 11 bytes" 
		" se leyo %d\n",read);	
	buffer[read] = '\0';	
	fail_unless(!strcmp(buffer,"Hola mundo\n"),
		"Se esperaba 'Hola Mundo' se obtuvo '%s'\n",buffer);
	
	memset(buffer,0,sizeof(buffer));
	f.f_ops->close(&f);
	prueba1 = root->i_ops->lookup(root,"prueba1.txt");
	file_object f2 = { .access_mode = FS_RD };
	prueba1->f_ops->open(prueba1,&f2);		
	read = f2.f_ops->read(&f2,11,buffer);
	fail_unless(read == 11,"Se esperaba leer 11 bytes" 
		" se leyo %d\n",read);	
	buffer[read] = '\0';	
	fail_unless(!strcmp(buffer,"Hola mundo\n"),
		"Se esperaba 'Hola Mundo' se obtuvo '%s'\n",buffer);

} END_TEST

TFun tests[] = {
	test_minix_super_block,
	test_minix_write_super_block,
	test_minix_read_inode,
	test_minix_delete_inode,
	test_minix_lookup_inode,
	test_minix_lookup_inode_dir,
	test_minix_lookup_inode_non_existent,
	test_minix_lookup_far_inode,
	test_minix_mkdir_inode,
	test_minix_rmdir,
	test_minix_read,
	test_minix_write,
	test_minix_readdir,
	test_minix_create,
	test_minix_create_and_write,
	test_minix_read_from_subdir,
	test_minix_unlink_file,
	test_minix_read_several,
	test_minix_read_major_minor,
	test_minix_read_and_write_append,
	test_minix_read_and_write_trunc,
	test_minix_open_two_files,
	test_minix_flush_file,
	test_minix_create_and_ls,
	test_minix_close,
	NULL
};

int main()
{
	hdd_init("hdd.img");
	Suite * s = suite_create("Minix File System");
	
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
