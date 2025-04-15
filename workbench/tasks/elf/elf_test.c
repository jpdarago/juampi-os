#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include "elf.h"
#include <string.h>

FILE * source;

void clean_up(){
	fclose(source);
}

const char * types[] = {
	"NULL", "LOAD", "DYNAMIC", "INTERP","NOTE",
	"SHLIB","PHDR","TLS","LOOS","HIOS","LOPROC","HIPROC"
};

char * permissions(uint flags) {
	char * res = strdup("   ");
	if(flags & 1) res[0] = 'X';
	if(flags & 2) res[1] = 'W';
	if(flags & 4) res[2] = 'R';
	return res;
}

int main(int argc, const char ** argv){
	source = stdin;
	if(argc > 1){ 
		source = fopen(argv[1],"r");
		atexit(clean_up);
	}else{
		printf("Te falta el argumento de archivo\n");
		exit(0);	
	}
	char * buffer = malloc(1024);
	int read = 0;

	while(!feof(source)){
		buffer = realloc(buffer,read+1024);
		read += fread(buffer+read,sizeof(char),1024,source);
	}

	elf_file * e = elf_exec_create(buffer);
	if(e == NULL){
		printf("Algo salio muy mal\n"); 
		exit(0);
	}
	printf("El punto de entrada es %x\n",elf_entry_point(e));
	for(uint i = 0; i < e->header->ph_entry_count; ++i){
		elf_segment * es = elf_get_segment(e,i);
		if(es == NULL){
			printf("El header %d es nulo\n",i);
			continue;
		}
		printf("HEADER DE PROGRAMA: \n");
		printf("\tNumero identificador de tipo: %d\n",es->type);
		printf("\tTipo: %s\n",types[es->type]);
		printf("\tTamanio: %d\n",es->file_size);
		printf("\tTamanio de imagen: %d\n",es->mem_size);
		printf("\tDireccion virtual: %x\n",es->virtual_address);
		printf("\tAtributos: %x\n", es->attributes);
		printf("\tAlineacion: %x\n",es->alignment);
		printf("\tFlags: %s\n",permissions(es->flags));
		elf_free_segment(es);
	}

	elf_destroy(e);	
}
