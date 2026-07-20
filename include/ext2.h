#ifndef __EXT2_H
#define __EXT2_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

// A small read-only ext2 reader over the ATA data disk (ata.h). Enough to load
// scripts and browse files from Lua: superblock parse, inode lookup, path
// resolution, whole-file reads, and directory listing. No writes.

typedef struct {
    uint64_t size;
    uint32_t inode;
    uint16_t mode; // raw ext2 i_mode (type + permission bits)
    bool is_dir;
} ext2_stat;

// Mount the filesystem: read and validate the superblock. Returns true on a
// valid ext2 fs. Safe to call with no disk attached (returns false); every
// other call then reports "not mounted" rather than touching the disk.
bool ext2_mount(void);
bool ext2_mounted(void);

// Read a whole regular file by path (leading '/' optional; resolved from the
// root). Returns a heap buffer of *size bytes that the caller must heap_free(),
// or NULL if the path is missing or not a regular file.
void* ext2_read_path(const char* path, size_t* size);

// Fill *out for `path`. Returns false if the path does not resolve.
bool ext2_stat_path(const char* path, ext2_stat* out);

// Enumerate the directory at `path`, calling emit() once per entry. `type` is
// the ext2 dirent file-type code (1=regular, 2=directory, ...) or 0 when the
// filesystem lacks the filetype feature. Returns the entry count, or -1 on
// error (not mounted, path missing, or not a directory).
int ext2_list(const char* path,
              void (*emit)(void* ctx, const char* name, uint32_t inode,
                           uint8_t type),
              void* ctx);

#endif
