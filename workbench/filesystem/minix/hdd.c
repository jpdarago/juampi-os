#include "hdd.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <assert.h>

#define SIZE_IN_SECTORS 32*1024

char HDD[SIZE_IN_SECTORS*ATA_SECTSIZE];

//Inicializar y detectar discos
void hdd_init(char * HDD_SRC)
{
	//Este kludge es para no leer a disco en cada test
	FILE * f = fopen(HDD_SRC,"r");
	if(f == NULL) assert(false);
	struct stat st;
	stat(HDD_SRC, &st);
	uint size = st.st_size;

	if(sizeof(HDD) < size) size = sizeof(HDD);
	fread(HDD,sizeof(uchar),size,f);

	fclose(f);
}

//Leer (usando LBA 28) una cantidad de bytes de un disco a un buffer
int hdd_read(uint address, uint sectors, void * buffer)
{
	memcpy(	buffer,
	        HDD+address*ATA_SECTSIZE,
	        sectors*ATA_SECTSIZE*sizeof(uchar));
	return sectors*ATA_SECTSIZE;
}

//Escribir (usando LBA 28) una cantidad de bytes de un disco a un buffer
int hdd_write(uint address,uint sectors, void * buffer)
{
	memcpy(	HDD+address*ATA_SECTSIZE,
	        buffer,
	        sectors*ATA_SECTSIZE*sizeof(uchar));
	return sectors*ATA_SECTSIZE;
}

//Backupea el disco en un img para debuggear
void hdd_output(char * filename)
{
	FILE * f = fopen(filename,"w");
	fwrite(HDD,1,sizeof(HDD),f);
	fclose(f);
}
