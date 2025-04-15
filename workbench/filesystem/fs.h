#ifndef __FS_H
#define __FS_H

#include "types.h"
#include "hdd.h"

//Inicializa el sistema de archivos
//en el disco primario
void fs_init();

typedef struct {
	char * filename;
	uint permissions;
	uint file_size;
} fs_fdata;

typedef struct {
	uint node_index;
	uint offset_read;
	uint offset_wrote;
	fs_fdata * data;
} fs_file;

typedef int fs_descr;
typedef int fs_descr_dir;

#define fs_error(x) ((x) < 0)

//Abrir un archivo (no directorio, archivo). Devuelve NULL si no pudo
fs_descr fs_open(char * filename,char * mode);
//Cerrar un archivo ya abierto. Devuelve -1 en caso de error, 0 si esta todo bien.
int fs_close(fs_descr file);

//Leer una cantidad de datos de un archivo a un buffer. 
//Devuelve la cantidad de datos leidos, -1 en caso de error.
int fs_read(fs_descr file, uint bytes, void * buffer);
//Escribir una cantidad de datos a un buffer
//Devuelve la cantidad de datos escritos, -1 en caso de error.
int fs_write(fs_descr file, uint bytes, void * buffer);

//Obtener datos del archivo
fs_fdata * fs_stat(char * filename);
//Borrar un archivo. Regresa 0 si tuvo exito -1 en caso de error
int fs_unlink(char * filename);

typedef enum { FS_FILE, FS_DIR } fs_type;

typedef struct{
	char * dirname;
	uint permissions;
} fs_ddata;

//Abrir un directorio. Devuelve NULL si falla.
fs_descr_dir fs_opendir(char * dirname, char * mode);
//Cerrar un directorio. Devuelve -1 en caso de error, 0 si esta todo bien.
int fs_closedir(fs_descr_dir dir);
//Devuelve la siguiente entrada del directorio, NULL si falla.
char * fs_readdir(fs_descr_dir dir);

#endif
