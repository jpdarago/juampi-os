#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#include "memory.h"
#include "fs.h"
#include "hdd.h"
#include "buffer.h"
#include "list.h"
#include "types.h"
#include "exception.h"

//TODO: Agregar mucho mucho error checking por falta de espacio y
//similares. Considerar el tema concurrencia de procesos.

/*
 * Documentacion de FAT 16 tomada de OSDEV y de:
 * http://www.maverick-os.dk/FileSystemFormats/FAT16_FileSystem.html
*/

//Tabla FAT 16 buffereada en memoria
static ushort * FAT;

//Bit que indica si hay que bajar la FAT a disco
static bool fat16_fatdirty;

//Longitud maxima de cualquier pedazo de directorio
#define FS_DIRPART_MAXLEN	64

//Longitud maxima de un nombre de archivo
#define FS_PATH_MAXLEN 255

//Valores especiales de una entrada de la FAT 16
#define FAT16_FAT_FREE 		0x0
#define FAT16_FAT_BADSECTOR 	0xFFF7
#define FAT16_FAT_EOF 		0xFFFF

#define FAT16_FAT_INVCLUSTER 	0x0

//Estructura del bootsector de FAT 16
typedef struct {
	uchar	bootcode[3];
	char	osname[8];
	ushort	sector_size; //En bytes
	uchar	cluster_size;//En sectores
	ushort	reserved_sectors;
	uchar 	fat_copies;
	ushort	root_entries;
	ushort	small_sectors;
	uchar	media_descriptor;
	ushort	fat_size;//En sectores
	ushort	track_size;//En sectores
	ushort	head_count;
	uint	hidden_sectors;
	uint	large_sectors;
	uchar	drive_num;
	uchar	__reserved;
	uchar	extended_boot;
	uint	vol_serial;
	char	vol_label[11];
	char	fsys_type[8];
	uchar	bootstrap_code[448];
	ushort	boot_signature;
} __attribute__((__packed__)) fat16_bootsector;

//Longitudes de nombres de FAT (sin VFAT)
#define FAT16_BASENAME_SIZE 8
#define FAT16_EXT_SIZE 3
#define FAT16_FILENAME_SIZE 11
//Caracter de no uso
#define FAT16_DELETED_TAG 0xE5

//Estructura de un directorio de FAT 16
typedef struct{
	char 	filename[FAT16_BASENAME_SIZE];
	char 	extension[FAT16_EXT_SIZE];
	uchar 	attr;
	uchar 	__reserved_NT;
	uchar	creation_stamp;
	ushort 	creation_time;
	ushort 	creation_date;
	ushort 	last_access_date;
	ushort 	__reserved_FAT32;
	ushort	last_write_time;
	ushort	last_write_date;
	ushort 	starting_cluster;
	uint	file_size;
} __attribute__((__packed__)) fat16_direntry;

//Bits para comparar los atributos de un directorio de FAT 16
#define FAT16_ATTR_RDONLY	(1 << 0)
#define FAT16_ATTR_HIDDEN	(1 << 1)
#define FAT16_ATTR_SYS		(1 << 2)
#define FAT16_ATTR_VOL		(1 << 3)
#define FAT16_ATTR_DIR		(1 << 4)
#define FAT16_ATTR_ACH		(1 << 5)

#define FAT16_LONG_ENTRY	0xF

//Estructura interna almacenada 
//en la lista de archivos abiertos
typedef struct{
	//Estructura con datos del archivo
	fs_file * fd;
	//Cluster de inicio del archivo
	uint current_cluster;
	//Buffer de datos para lectura
	fs_hdd_buffer * read_buffer;
	//Buffer de datos para escritura
	fs_hdd_buffer * write_buffer;
	//Condiciones de apertura del archivo
	uint open_mode;
	//Cluster y offset de la entrada en el directorio
	uint dir_cluster,dir_offset;
} fs_file_struct;

//Elimina un nodo de archivo abierto de la lista enlazada
static void fs_delete_node(void * data)
{
	fs_file_struct * f = data;
	kmem_free(f->fd->data->filename);
	kmem_free(f->fd->data);

	buffer_destroy(f->read_buffer);
	buffer_destroy(f->write_buffer);
}

//Valores de inicializacion del disco
static uint 
	//Sector donde comienza la FAT
	fat16_fatstart,
	//Cantidad de FATS que tiene la particion
	fat16_fatcount,
	//Tamaño en sectores de la FAT
	fat16_fatsize,
	//Sector donde empieza el rootdir
	fat16_rootdir,
	//Sector donde empieza la region de datos
	fat16_dataregion,
	//Tamaño en sectores de un cluster
	fat16_clustersize,
	//Entradas como maximo del directorio root
	fat16_rootdirentries,
	//Sectores reservados por la particion
	fat16_reserved_sectors,
	//Comienzo de la particion
	fat16_partition_begin,
	//Sectores escondidos sin uso
	fat16_hidden_sectors,
	//Cluster de inicio del directorio root
	fat16_rootdir_cluster,
	//Tamaño del cluster en bytes
	fat16_cluster_bytesize,
	//Offset de comienzo de los clusters en FAT
	fat16_datastart_cluster,
	//Cantidad de entradas de la FAT
	fat16_fatentries;

//Nombre del disco
char fat16_drivename[16];
//Media descriptor
uchar fat16_media_descriptor;

//Trimea un nombre padeado
static void fat16_trim(char * str, int len)
{
	uint i;
	for(i = len-1; i >= 1; i--){ 
		if(str[i] == ' ' && str[i-1] != ' '){
			break;
		}
	}
	str[i] = '\0';
}

//Obtener la entrada de un directorio dado el cluster
static fat16_direntry fat16_getentry(uint cluster, uint offset)
{
	char buffer[ATA_SECTSIZE*fat16_clustersize*sizeof(char)];
	fat16_direntry result;

	hdd_read(cluster*fat16_clustersize,fat16_clustersize,buffer);

	memcpy(&result,buffer+offset,sizeof(result));
	return result;
}

//Lista de buffers de archivos abiertos
static list * opened_files;

//Carga los valores del bootsector
static void fat16_load_bootsector(){
	fat16_bootsector bootsect;
	hdd_read(0x0,1,&bootsect);

	memcpy(fat16_drivename,&bootsect.vol_label,11);
	fat16_trim(fat16_drivename,11);

	fat16_partition_begin 	= 0;
	fat16_clustersize 		= bootsect.cluster_size; 
	fat16_rootdirentries	= bootsect.root_entries;
	fat16_fatcount 			= bootsect.fat_copies;
	fat16_reserved_sectors	= bootsect.reserved_sectors;
	fat16_fatsize			= bootsect.fat_size;
	fat16_hidden_sectors	= bootsect.hidden_sectors;
	fat16_media_descriptor	= bootsect.media_descriptor;

	fat16_cluster_bytesize	= ATA_SECTSIZE*fat16_clustersize;
}

//Calcula las posiciones de las regiones
static void fat16_calculate_regions(){
	fat16_fatstart	 = fat16_partition_begin + 
		fat16_reserved_sectors*fat16_clustersize;
	fat16_rootdir 	 = fat16_fatstart + fat16_fatcount*fat16_fatsize;
	fat16_dataregion = fat16_rootdir + 
		(fat16_rootdirentries*sizeof(fat16_direntry))/
			(fat16_clustersize*ATA_SECTSIZE);	

	//El rootdir es el primer cluster en la data region
	//Por lo tanto, uno anterior es el comienzo de la data region.
	fat16_datastart_cluster = fat16_rootdir - fat16_clustersize;
	fat16_rootdir_cluster = 
		fat16_partition_begin + fat16_reserved_sectors +
		(fat16_fatcount*fat16_fatsize)/fat16_clustersize;	
}

void fat16_initialize_fat_table(){
	fat16_fatentries = (fat16_fatsize*ATA_SECTSIZE)/sizeof(ushort);
	uint bytes = fat16_fatsize*ATA_SECTSIZE*sizeof(uchar);
	FAT = kmem_alloc(bytes);
	if(FAT == NULL) kernel_panic("No hay memoria para la FAT");
	if(hdd_read(fat16_fatstart,fat16_fatsize,FAT) != bytes)
			kernel_panic("No se leyo bien la de disco FAT\n");
	fat16_fatdirty = false;	
}

//Inicializa el sistema de archivos
void fs_init()
{
	fat16_load_bootsector();
	fat16_calculate_regions();
	fat16_initialize_fat_table();

	opened_files = list_create( .destroyer = fs_delete_node );
	if(!opened_files) 
		kernel_panic("No se pudo crear lista de archivos");
}

//Asegura la consistencia de la FAT
static void fat16_flush_fat(){
	if(fat16_fatdirty){
		fat16_fatdirty = false;
		uint bytes = fat16_fatsize*ATA_SECTSIZE;
		if(hdd_write(fat16_fatstart,fat16_fatsize,FAT) != bytes)
			kernel_panic("No se pudo flushear la FAT");
	}
}

//Devuelve el siguiente cluster en la cadena FAT
static uint fat16_next_cluster(uint current_cluster)
{
	uint res = FAT[current_cluster-fat16_datastart_cluster];
	if(res == FAT16_FAT_EOF){ 
		return FAT16_FAT_INVCLUSTER;
	}
	return res+fat16_datastart_cluster;
}

//Consigue y marca un cluster libre donde continuar escribiendo
//Si se lo invoca con cero es solo para conseguir un cluster libre
static uint fat16_search_cluster(uint cluster)
{
	int entry = cluster-fat16_datastart_cluster;
	if(entry < 0) entry = 0;

	uint fat16_entries = (fat16_fatsize*ATA_SECTSIZE)/16;
	FAT[entry] = FAT[0]; 
	for(int i = 2; i < fat16_entries; i++){
		if(FAT[i] == FAT16_FAT_FREE){
			if(cluster){ 
				FAT[entry] = i;
				FAT[i] = FAT16_FAT_EOF;
				fat16_fatdirty = 1;
			}
			return i + fat16_datastart_cluster;
		}
	}
	return FAT16_FAT_INVCLUSTER;
}

//Entrada de VFAT 16 de archivo
typedef struct {
	uchar order;
	char name_1[10];
	uchar attr;
	uchar type;
	uchar checksum;
	char name_2[12];
	ushort __zero;
	char name_3[4];
} __attribute__((__packed__)) fat16_long_entry;

#define LAST_ENTRY(l) ((l).order & (1 << 6))
#define IS_LONG(d) ((((d).attr) & 0xF) == 0xF)

//Devuelve el resultado de concatenar dos strings con posible realocacion
//Asume que alcanza el espacio en orig
static void str_append_start(char * orig, char * new)
{
	char temp[FS_PATH_MAXLEN+1];

	int lo = strlen(orig),ln = strlen(new);
	if(ln + lo > FS_PATH_MAXLEN) return;
	
	memcpy(temp,new,ln);
	memcpy(temp+ln,orig,lo);
	temp[lo+ln] = '\0';
	strcpy(orig,temp);
}

//Bundle con los datos que nos interesa sobre el nombre
typedef struct {
	fat16_direntry entry;
	uint cluster,offset;
} fat16_dirdata;

//Pega el nombre de la entrada en lf a name_buffer, asignando mas espacio si es
//necesario
static void fat16_append_entry(char * name_buffer, fat16_long_entry * lf){
	char str[14]; int i;

	for(i=0;i<5;i++) str[i]   = lf->name_1[2*i];		
	for(i=0;i<6;i++) str[5+i] = lf->name_2[2*i];
	for(i=0;i<2;i++) str[11+i]= lf->name_3[2*i];
	str[13] = '\0';
			
	//TODO: Ubicar bien segun posicion
	str_append_start(name_buffer,str);	
}

static void fat16_build_simple_entry(char * name_buffer, fat16_direntry * d){
	if(d->attr & FAT16_ATTR_VOL) return;
	//No habia entrada VFAT antes.
	//Usamos la entrada normal.
	fat16_trim(d->filename,8);
	fat16_trim(d->extension,3);
		
	strcpy(name_buffer,d->filename);
	if(d->extension[0] != '\0')
		strcat(name_buffer,".");
	strcat(name_buffer,d->extension);	
}	

//Busca un directorio dado por direntry, empezando por current_cluster, por el nombre
static fat16_dirdata fat16_search(uint current_cluster, char * name)
{
	fat16_direntry d, res;
	
	uint cluster,offset; cluster = offset = 0;
	memset(&res,0,sizeof(res));
	
	char buffered_cluster[fat16_cluster_bytesize];
	fs_hdd_buffer * bf = buffer_create(
					current_cluster,
					fat16_clustersize,
					.next_cluster = fat16_next_cluster,
					.buffer = buffered_cluster);

	uint next_entry = buffer_read(bf,sizeof(d),&d);
	char name_buffer[FS_PATH_MAXLEN+1];

	for(;next_entry > 0;next_entry = buffer_read(bf,sizeof(d),&d),offset++){	
		if(d.filename[0] == 0x00) break;
		if(d.filename[0] == FAT16_DELETED_TAG) 
			continue;
			
		name_buffer[0] = '\0';
		
		for(;IS_LONG(d);next_entry = buffer_read(bf,sizeof(d),&d)){
			fat16_long_entry lf; memcpy(&lf,&d,sizeof(lf));
			fat16_append_entry(name_buffer,&lf);
			offset++;
		}
	
		if(!name_buffer[0]){
			fat16_build_simple_entry(name_buffer,&d);
		}
		
		if(!strcmp(name_buffer,name)){
			res = d; 
			cluster = bf->cluster;
			offset %= (fat16_cluster_bytesize/sizeof(fat16_direntry));
			break;
		}
	}

	buffer_destroy(bf);
	return (fat16_dirdata) 
		{.cluster = cluster, .offset = offset, .entry = res };
}

//Consigue el siguiente pedazo de archivo
static uint fat16_filename_next_part(char * filename,char name_buffer[64])
{
	uint i = 0,j = 0;
	for(;j < FS_DIRPART_MAXLEN && filename[i] && filename[i] != '/'; i++,j++)
		name_buffer[j] = filename[i];
	name_buffer[j] = '\0';
	if(filename[i] == '/') i++;
	return i;
}

//Indices de modos de apertura de archivo
#define FS_READ		1
#define FS_WRITE	2
#define FS_APPEND	4
#define FS_MODIFY	(FS_WRITE | FS_APPEND)

//Devuelve un bitset con el modo de apertura del archivo
uint fs_get_open_mode(char * mode)
{
	uint res = 0;
	for(;*mode;mode++){
		switch(*mode){
			case 'r': res |= FS_READ; break;
			case 'w': res |= FS_WRITE; break;
			case 'a': res |= FS_APPEND; break;
		}
	}
	return res;
}

//Devuelve el ultimo cluster de la cadena empezando
//en starting_cluster
static uint fat16_last_cluster(uint starting_cluster)
{
	if(starting_cluster == fat16_datastart_cluster)
		return starting_cluster;

	uint next_cluster, cluster = starting_cluster;
	while((next_cluster = fat16_next_cluster(cluster))){
		cluster = next_cluster;
	}
	return cluster;
}

//Registra el nuevo archivo y devuelve un puntero al usuario.
static fs_descr 
fs_register_new_file(uint starting_cluster, 
			uint file_size, 
			char * filename, 
			uint permissions,
			uint open_mode,
			uint dir_cluster, 
			uint dir_offset)
{
	fs_file * file_data = kmem_alloc(sizeof(fs_file));
	
	file_data->node_index = opened_files->elements;
	file_data->offset_read = file_data->offset_wrote = 0;

	//TODO: Inicializar file_data->data 
	//con los permisos y filename y eso
	file_data->data = kmem_alloc(sizeof(fs_fdata));	
	
	char * filedup = kmem_alloc(strlen(filename)+1);
	strcpy(filedup,filename);

	file_data->data->filename = filedup;
	file_data->data->file_size = file_size;
	file_data->data->permissions = permissions;
	
	uint cluster = starting_cluster;		
	if(open_mode & FS_APPEND){
		cluster = fat16_last_cluster(starting_cluster);	
	}

	fs_file_struct * f = kmem_alloc(sizeof(fs_file_struct));
	f->read_buffer = f->write_buffer = NULL;

	f->open_mode = open_mode;
	f->current_cluster = cluster;
		
	f->fd = file_data;	
	f->dir_cluster = dir_cluster, f->dir_offset = dir_offset;	

	return list_add(opened_files,f);
}

//Formatea los resto % 13 o 13 caracteres del nombre
//Devuelve el nombre para continuar o NULL
static int fat16_format_name(fat16_long_entry * e, char * s, int len)
{	
	int chars = len % 13;
	if(chars == 0) chars = 13;
	
	char str[14]; str[chars] = '\0';
	for(int i = len-chars,j = 0;i < len;i++,j++)
		str[j] = s[i];
	
	str[13] = '\0';
	for(int i = 0; i < chars; i++){
		char * buf = e->name_1 + 2*i;
		
		if(i >= 5) 	buf = e->name_2 + 2*(i - 5);
		if(i >= 5+6)	buf = e->name_3 + 2*(i - 5 - 6);
		
		*buf = str[i]; *(buf+1) = 0x00;
	}

	return len-chars;
}

static fat16_dirdata
fs_write_entry_to_disk(	char * name, 
			uint attributes,
			fs_hdd_buffer * bf,
			uint starting_cluster)
{
	int l = strlen(name);
	int long_entries = (l + 13)/(2+5+6);

	char namecopy[FS_DIRPART_MAXLEN]; 
	strcpy(namecopy,name);

	//Necesitamos contar el caracter nulo al empezar
	int ml = l+1;
	for(int i = long_entries;i >= 1;i--){
		fat16_long_entry le = {0};

		memset(&le.name_1,0xFF,sizeof(le.name_1));
		memset(&le.name_2,0xFF,sizeof(le.name_2));
		memset(&le.name_3,0xFF,sizeof(le.name_3));

		ml = fat16_format_name(&le,namecopy,ml);
		if(!namecopy[0]) break;
		
		le.attr = FAT16_LONG_ENTRY;
		le.type = 0;
		 
		//TODO: Calcular el checksum posta
		le.checksum = 0;
		le.order = i & 0x1F;
		if(i == long_entries){ 
			le.order |= 1 << 6;
		}
		
		buffer_write(bf,sizeof(le),&le);
	}
	fat16_direntry r = {{0}};;
	strcpy(namecopy,name);
	for(int i = 0; i < l; i++){
		char c = namecopy[i]; 	
		if(c >= 'a' && c <= 'z')
			namecopy[i] += 'A'-'a';
	}
	int i;
	for(i = 0; i < l && i < FAT16_FILENAME_SIZE && namecopy[i] != '.';i++){
		char c = namecopy[i];
		r.filename[i] = c;
	}
	for(; i < FAT16_FILENAME_SIZE; i++) r.filename[i] = ' ';
	for(i = 0;namecopy[i] && namecopy[i] != '.';i++);
	if(namecopy[i] == '.')
		memcpy(&r.extension,namecopy+i+1,FAT16_EXT_SIZE);
	
	r.attr = attributes;
	r.file_size = 0;

	uint offset = bf->consumed/sizeof(fat16_direntry), 
	     cluster = bf->cluster;
	
	if(r.attr & FAT16_ATTR_DIR){
		//Crear un nuevo directorio requiere crear
		//dos entradas nuevas
		uint dir_cluster = fat16_search_cluster(0);
		r.starting_cluster = dir_cluster - fat16_datastart_cluster;

		fat16_direntry buf[fat16_cluster_bytesize];
		memset(buf,0,sizeof(buf));
		
		//Primera entrada: El propio directorio
		memcpy(buf[0].filename,".              ",8);
		memcpy(buf[0].extension,"              ",3);
		buf[0].attr = attributes;
		buf[0].starting_cluster = r.starting_cluster;
		
		//Segunda entrada: El directorio padre
		memcpy(buf[1].filename,"..             ",8);
		memcpy(buf[1].extension,"              ",3);
		buf[1].attr = attributes; //TODO: ???
		buf[1].starting_cluster = starting_cluster;

		hdd_write(dir_cluster,fat16_clustersize,buf);
	}else r.starting_cluster = 0x0000;
	
	//TODO: Rellenar todos los demas 
	//campos (fechas y eso)
	
	buffer_write(bf,sizeof(r),&r);	

	return (fat16_dirdata) 
		{.cluster = cluster, .offset = offset, .entry = r };
}

//Regresa el proximo cluster de la cadena o asigna uno nuevo
//si no encuentra suficiente espacio.
static uint
fat16_next_or_new_cluster(uint current_cluster)
{
	uint cluster = fat16_next_cluster(current_cluster);
	if(cluster == FAT16_FAT_INVCLUSTER)
		//TODO: Error checking
		return fat16_search_cluster(current_cluster);
	return cluster;
}

//Crea una nueva entrada de directorio en 
//el directorio que empieza en starting_cluster
static fat16_dirdata
fs_create_entry(uint starting_cluster,char * name, uint attributes)
{
	if(starting_cluster == FAT16_FAT_INVCLUSTER)
		//TODO: Error checking
		starting_cluster = fat16_search_cluster(0);

	int l = strlen(name);
	
	fat16_dirdata res; memset(&res,0,sizeof(res));
	if(l > FS_DIRPART_MAXLEN) return res;

	char buffered_cluster[fat16_cluster_bytesize];
	fs_hdd_buffer * bf = buffer_create(
				starting_cluster,
				fat16_clustersize,
				.next_cluster = fat16_next_or_new_cluster,
				.buffer = buffered_cluster);
	
	fat16_direntry d;
	uint dirents = sizeof(fat16_direntry),
	     read = buffer_read(bf,dirents,&d); 
	
	int long_entries = (l+13)/(5+6+2);
	for(;read; read = buffer_read(bf,dirents,&d)){
		bool usable = true;

		uint previous_cluster = bf->cluster,
		     previous_offset  = bf->consumed;

		for(int e = 1+long_entries; e; e--){
			if(d.attr == 0xFFFF || 
			  (d.filename[0] != 0x00 && d.filename[0] != 0xE5)){
				usable = false;
				break;
			}else buffer_read(bf,dirents,&d);
		}
		if(usable){
			buffer_reset(bf,previous_cluster,previous_offset-dirents);
			//TODO: Si se escribe un directorio hay que agregarle el . y ..
			//como entradas simples
			res = fs_write_entry_to_disk(name,attributes,
						bf,starting_cluster);
			goto fin;
		}
	}
fin:
	buffer_destroy(bf);
	return res;
}

static fs_descr fs_open_do(char * filename, char * mode, uint current_cluster)
{
	uint i = 0, open_mode = fs_get_open_mode(mode);
	do{
		char name_buffer[FS_DIRPART_MAXLEN];
		i += fat16_filename_next_part(filename+i,name_buffer);
		
		fat16_dirdata d = fat16_search(current_cluster,name_buffer);
		
		if(d.entry.filename[0] == 0x00){ 
			if(open_mode & FS_WRITE){
				//Creamos la entrada de directorio
				//TODO: Error checking
				int attrs = 0x0;
				if(filename[i]) attrs |= FAT16_ATTR_DIR;
				d = fs_create_entry(current_cluster,
							name_buffer,
							attrs);
			}else return -1;
		}
		fat16_direntry entry = d.entry;
		uint cluster = entry.starting_cluster+fat16_datastart_cluster;
		if(!filename[i]){
			//Habria que registrar un error si no es archivo
			if(entry.attr & FAT16_ATTR_DIR){ 
				return -1;
			}
			#define MODIFY(o) ((o) & FS_MODIFY)
			if((entry.attr & FAT16_ATTR_RDONLY) && MODIFY(open_mode)){
				return -1;
			}
			//Es un archivo. Hay que crear un archivo
			return fs_register_new_file(	cluster,
							entry.file_size,
							filename,
							entry.attr & 0xF,
							open_mode,
							d.cluster,
							d.offset);
		}else{
			//Habria que registrar un error si no es directorio
			if(~entry.attr & FAT16_ATTR_DIR) return -1;
			current_cluster = cluster;
			
		}
	}while(filename[i]);
	return -1;
}

//Abrir un archivo (no directorio, archivo). Devuelve NULL si no pudo
//TODO: Usar distintos modos de acceso al archivo.
fs_descr fs_open(char * filename,char * mode)
{
	if(strlen(filename) > FS_PATH_MAXLEN) return -1;
	if(filename[0] == '/') filename++;
	fs_descr res =  fs_open_do(filename,mode,fat16_rootdir_cluster);
	
	fat16_flush_fat();
	return res;
}

//Cerrar un archivo ya abierto. Devuelve -1 en caso de error, 0 si esta todo bien.
int fs_close(fs_descr file_desc)
{
	//TODO: Si no encuentra el archivo, que devuelva -1?
	list_delete(opened_files,file_desc);
	
	fat16_flush_fat();
	return 0;
}

//Leer una cantidad de datos de un archivo a un buffer. 
//Devuelve la cantidad de datos leidos, -1 en caso de error.
int fs_read(fs_descr file_desc, uint bytes, void * buffer)
{
	fs_file_struct * node = list_get(opened_files,file_desc);
	if(node == NULL || !(node->open_mode & FS_READ)) return -1;
	//Archivo vacio, nada que hacer
	if(node->current_cluster == fat16_datastart_cluster) return 0;

	if(!node->read_buffer){
		node->read_buffer = buffer_create(
					node->current_cluster,
					fat16_clustersize,
					.next_cluster = fat16_next_cluster);
	}

	uint remaining = node->fd->data->file_size - node->fd->offset_read;
	uint bytes_to_read = (bytes > remaining) ? remaining : bytes;	
	
	uint bytes_read = buffer_read(node->read_buffer,bytes_to_read,buffer);
	node->fd->offset_read += bytes_read;
	
	return bytes_read;
}

//Escribir una cantidad de datos de un buffer a disco
//Devuelve la cantidad de datos escritos, -1 en caso de error.
int fs_write(fs_descr file_desc, uint bytes, void * buffer)
{
	fs_file_struct * node = list_get(opened_files,file_desc);
	if(node == NULL || !(node->open_mode & (FS_APPEND | FS_WRITE))){
		return -1;
	}
		
	if(node->current_cluster == fat16_datastart_cluster){
		node->current_cluster = fat16_search_cluster(0);
	}

	uint file_size = node->fd->data->file_size;
	if(!node->write_buffer){
		node->write_buffer = buffer_create(
					node->current_cluster,
					fat16_clustersize,
					.next_cluster = fat16_next_or_new_cluster);
	
		if(node->open_mode & FS_APPEND){
			//TODO: Hace 2 accesos a disco, debiera hacer 1
			buffer_reset(node->write_buffer,node->current_cluster,
					file_size % fat16_cluster_bytesize);	
		}else file_size = 0;
	}
	
	uint write_cluster = node->write_buffer->cluster;
	uint bytes_written = buffer_write(node->write_buffer,bytes,buffer);
	if(node->read_buffer && node->read_buffer->cluster == write_cluster){
		//TODO: Determinar si es necesario flushear el buffer
		//de lectura, no flushearlo siempre
		buffer_refresh(node->read_buffer);
	}
	
	//Actualizar la entrada de directorio del archivo
	//con el nuevo tamaño del mismo
	if(node->open_mode & FS_WRITE){ 
		file_size = 0;
	}
	
	uint new_size = file_size + bytes_written;
	node->fd->data->file_size = new_size;
	
	fat16_direntry 
		dir_buffer[fat16_cluster_bytesize/sizeof(fat16_direntry)];
	
	uint dir_sector = node->dir_cluster*fat16_clustersize;
	
	hdd_read(dir_sector,fat16_clustersize,dir_buffer);
	fat16_direntry * dir = &dir_buffer[node->dir_offset];
	if(!dir->starting_cluster){
		//No habia inicio para el cluster
		dir->starting_cluster = 
			node->current_cluster - fat16_datastart_cluster; 
	}
	dir->file_size = new_size;
	hdd_write(dir_sector,fat16_clustersize,dir_buffer);
		
	fat16_flush_fat();
	return bytes_written;
}

//Obtener datos del archivo
fs_fdata * fs_stat(char * filename)
{
	return NULL;
}

//Determina el cluster del que vino anteriormente en la FAT
static uint fat16_prev_cluster(uint cluster)
{
	if(cluster < fat16_datastart_cluster) 
		return FAT16_FAT_INVCLUSTER;
	int entry = cluster-fat16_datastart_cluster;
	for(int i = 0; i < fat16_fatentries; i++){
		if(FAT[i] == entry){
			return i+fat16_datastart_cluster;
		}
	}
	return FAT16_FAT_INVCLUSTER;
}

//Libera las entradas de directorio asociadas a un archivo que esta en
//el cluster dir_cluster con offset interno dir_offset
static int fs_clear_direntries(uint dir_cluster, uint dir_offset){
	//Liberamos la entrada en el directorio	
	fat16_long_entry buffer[fat16_cluster_bytesize];
	hdd_read(dir_cluster,fat16_clustersize,buffer);
	
	uint cluster = dir_cluster;
	fat16_long_entry * le = buffer+dir_offset;
	bool stop = false;

	do{
		if(IS_LONG(*le) && LAST_ENTRY(*le)) 
			stop = true;
		memset(le,0,sizeof(fat16_long_entry));
		//Por compatibilidad, podriamos tener entradas de FAT 16
		//simples (las que no tienen entradas largas previamente)
		if(!IS_LONG(*le)) stop = true;

		if(le == buffer){
			hdd_write(cluster,fat16_clustersize,buffer);
			uint prev = fat16_prev_cluster(cluster);
			if(prev == FAT16_FAT_INVCLUSTER) return -1;
			cluster = prev;
			hdd_read(prev,fat16_clustersize,buffer);
		}else le--;
	}while(!stop);

	uint bytes = fat16_clustersize*ATA_SECTSIZE;
	if(hdd_write(cluster,fat16_clustersize,buffer) != bytes) 
		kernel_panic("No se flusheo bien al limpiar entradas de directorio");
	return 0;
}

//Libera los recursos asociados a este archivo
static int fs_clear(uint data_cluster, uint dir_cluster, uint dir_offset)
{
	fat16_fatdirty = 1;

	//Liberamos las entradas en la FAT
	for(uint c = data_cluster,tmp; c < FAT16_FAT_INVCLUSTER; c = tmp){
		tmp = FAT[c];
		FAT[c] = FAT16_FAT_FREE;
	}

	fs_clear_direntries(dir_cluster,dir_offset);
	fat16_flush_fat();
	return 0;
}

//Borrar un archivo
int fs_unlink(char * filename)
{
	if(strlen(filename) > FS_PATH_MAXLEN) return -1;

	if(filename[0] == '/') filename++;
	int i = 0; uint current_cluster = fat16_rootdir_cluster;
	do{
		char name_buffer[FS_DIRPART_MAXLEN];
		i += fat16_filename_next_part(filename+i,name_buffer);
		
		fat16_dirdata d = fat16_search(current_cluster,name_buffer);
		if(d.entry.filename[0] == 0x00) return -1;

		fat16_direntry entry = d.entry;
		uint cluster = entry.starting_cluster+fat16_datastart_cluster;
		if(!filename[i]){
			//Habria que registrar un error si no es archivo
			if((entry.attr & FAT16_ATTR_DIR) ||
			   (entry.attr & FAT16_ATTR_RDONLY)){ 
				return -1;
			}
			return fs_clear(d.entry.starting_cluster,
					d.cluster,d.offset);	
		}else{
			current_cluster = cluster;
		}
	}while(filename[i]);

	return -1;
}

//Abrir un directorio. Devuelve NULL si falla.
fs_descr_dir fs_opendir(char * filename, char * mode)
{
	return -1;
}
//Cerrar un directorio. Devuelve -1 en caso de error, 0 si esta todo bien.
int fs_closedir(fs_descr_dir dir)
{
	return -1;
}
//Devuelve el nombre del siguiente subdirectorio entrada del directorio, NULL si falla.
char * fs_readdir(fs_descr_dir dir)
{
	return NULL;
}

//Destruye el sistema de archivos
static void fs_destroy()
{
	list_destroy(opened_files);
	kmem_free(FAT);
}

#ifdef TEST_FLAG	
	#include "fs_test.c"
#endif
