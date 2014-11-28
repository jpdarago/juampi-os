#ifndef __FS_MINIX
#define __FS_MINIX

#include <vfs.h>
#include <bitset.h>

#define MINIX_BLOCK_SIZE 				1024
#define MINIX_BLOCK_BSIZE				2

#define	MINIX_SUPERBLOCK_SIZE			1024
#define MINIX_SUPERBLOCK_BSIZE			2
#define MINIX_SUPERBLOCK_START			1024
#define MINIX_SUPERBLOCK_START_BLOCK	0x2

#define MINIX_INODEBMAP_STARTB			2
#define MINIX_ZONEBMAP_STARTB			3
#define	MINIX_INODES_PER_BLOCK			32

#define MINIX_MAGIC					0x138F		

#define MINIX_IFSOCK	0140000
#define MINIX_IFLNK		0120000
#define MINIX_IFREG		0100000
#define MINIX_IFBLK		0060000
#define MINIX_IFDIR		0040000
#define MINIX_IFCHR		0020000
#define MINIX_IFIFO		0010000

#define MINIX_ROOT_INODE 1
#define MINIX_TYPE_ONLY(x)	((x) & ~0xFFF) 

typedef struct {
	ushort 	number_inodes;
	ushort 	data_zones;
	ushort 	inode_map_bsize;
	ushort 	zone_map_bsize;
	ushort 	data_zone_start;
	ushort 	data_zone_size_log;
	uint	max_file_size;
	ushort	magic;
} minix_super_block;

#define MINIX_DIR_MAXLEN 30 

typedef struct {
	ushort inode;	
	char name[MINIX_DIR_MAXLEN];
} __attribute__((__packed__)) minix_dir_entry;

#define MINIX_ZONES 7
//Cantidad de entradas de directorio por bloque
#define MINIX_DIR_ENTRIES 32
//Cantidad de punteros a bloques en un bloque MINIX
#define MINIX_BLOCK_PTRS 512
//Consideracion por las entradas default
#define MINIX_DEF_DIRSIZE 64

typedef struct {
	ushort	mode;
	ushort	uid;
	uint	size;
	uint	time;
	uchar	gid;
	uchar	nlinks;
	ushort	zones[MINIX_ZONES];
	ushort 	indirect_block;
	ushort 	doubly_indirect_block;
} __attribute__((__packed__)) minix_inode;

typedef struct {
	uint	number_inodes;
	uint	inode_entries_start;
	uint	data_zone_start;

	bitset 	inode_bitmap;
	bitset 	zone_bitmap;
	
	uint 	inode_bitmap_start_block;
	uint	inode_bitmap_blocks;

	uint 	zone_bitmap_start_block;
	uint	zone_bitmap_blocks;
} minix_fs_data;

typedef struct {
	uint block, offset;
} disk_position;

int fs_minix_init(super_block * block);

#endif
