#ifndef __FS_H
#define __FS_H

#include "types.h"
#include "klist.h"
#include "sem.h"
#include "rw_sem.h"

#define FS_MAXLEN 128

typedef struct inode inode;
typedef struct dir_entry dir_entry;
typedef struct super_block super_block;
typedef struct file_object file_object;

typedef enum fs_type {
	FS_FILE,
	FS_DIR,
	FS_CHARDEV,
	FS_BLOCKDEV,

	FS_ERROR = -1
} fs_type;

#define FILE_MAXLEN 30

typedef struct dirent {
	uint inode_number;
	char name[FILE_MAXLEN];
} dirent;

typedef struct fs_ops{
	int (*read)(file_object *, uint, void *);
	int (*write)(file_object *, uint, void *);
	int (*open)(inode *, file_object *);
	int (*close)(file_object *);
	int (*readdir)(file_object *, dirent *);
	int (*flush)(file_object *);
	int (*seek)(file_object *,uint);
} fs_ops;

typedef struct inode_ops {
	//Dado un nombre de archivo y un inodo de directorio
	//busca un archivo que tenga el nombre correspondiente
	inode * (*lookup)(inode *, char *);
	//Borra un inodo del disco
	int (*unlink)(inode *, char * name);
	//Crea una nueva entrada de directorio
	int (*mkdir)(inode *, char *);
	//Borra una entrada de directorio
	int	(*rmdir)(inode *, char *);
	//Ubica un inodo como subarchivo de un directorio
	int (*create)(inode *, char *);
} inode_ops;

typedef struct super_block_ops{
	//Crea una estructura en memoria correspondiente a un
	//inodo para este fileystem
	inode * (*alloc_inode)(super_block *);
	//Consigue un inodo libre en este filesystem
	inode * (*obtain_inode)(super_block *);
	//Lee los datos de un inodo desde el filesystem
	int (*read_inode) (inode*);
	//Escribe los datos del inodo a disco
	int (*write_inode)(inode*);
	//Borra un inodo de disco, destruyendo la memoria tambien
	int (*delete_inode)(inode*);
	//Destruye la estructura del inodo en memoria
	int (*release_inode)(inode*);
	//Flushea el super bloque a disco duro
	int (*write_super)(super_block *); 
	//Flushea todo el filesystem  a disco duro
	int (*sync_fs)(super_block *);
} super_block_ops;

typedef struct pipe {
	void * data; //Un buffer de size bytes
	uint offset;
	uint size;
} pipe;

typedef struct char_dev {
	ushort	major_number;
	ushort	minor_number;
	sem 	* access_lock;
} char_dev;

typedef struct block_dev {
	ushort 	major_number;
	ushort 	minor_number;
	sem		* access_lock;
} block_dev;

struct inode {
	uint 	inode_number;
	uint 	use_count;
	
	bool	is_dirty;
	
	uint 	file_size;

	fs_type	inode_type;	
	sem 	* lock;

	list_head open_ptr;

	super_block * super_block;	
	
	inode_ops const	* i_ops;
	fs_ops	  const	* f_ops;

	pipe 		* info_pipe; //Si es pipe
	char_dev 	* info_cdev; //Si es char device
	block_dev 	* info_bdev; //Si es block device
	void		* info_disk; //Si es archivo en disco (MINIX)
};

struct super_block{
	inode * root;	
	
	uint block_size;
	uint disk_size;
	uint max_file_size;
	uint magic;
	bool is_dirty;

	list_head open_inodes;

	void * fs_data;
	super_block_ops const * ops;
};

#define FS_RD 		1 
#define FS_WR	 	2
#define FS_RDWR		3

#define FS_RDBIT	1
#define FS_WRBIT	2

//Flags de apertura
#define FS_TRUNC	4	
#define FS_CREATE	8

struct file_object {
	inode 			* inode;
	uint 			access_mode;
	uint			flags;
	uint 			file_read_offset;
	uint			file_write_offset;
	fs_ops 	const 	* f_ops;
};

enum fs_error { 
	EINVOFF = 1,
	EPERMS,
	EINVTYPE,
	ENOSPACE,
	EINVFS,
	EINODES,
	ENODIR,
	ENOFILE,
	EFBUSY,
	EDIRNOTEMPTY,
	ENOINODE
};

#endif
