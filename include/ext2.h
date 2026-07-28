#ifndef __EXT2_H
#define __EXT2_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <memory.h>
#include <blockdev.h>

// A small ext2 driver over a generic block device (blockdev.h) — the ATA data
// disk today, any 512-byte-sector device tomorrow. Enough to load scripts and
// browse files from Lua: superblock parse, inode lookup, path resolution,
// whole-file reads, directory listing, and read-modify-write updates.

typedef struct {
    uint64_t size;
    uint32_t inode;
    uint16_t mode; // raw ext2 i_mode (type + permission bits)
    bool is_dir;
} ext2_stat;

// Mount the filesystem on block device `dev`: read and validate the superblock.
// `heap` is the fs's scratch/result allocator (injected here so the module
// never calls heap_default()). Returns true on a valid ext2 fs. Safe to call
// with a NULL or empty device (returns false); every other call then reports
// "not mounted".
bool ext2_mount(const blockdev* dev, heap_allocator* heap);
bool ext2_mounted(void);

// Read a whole regular file by path (leading '/' optional; resolved from the
// root). Returns a buffer of *size bytes that the caller must release with
// ext2_free(), or NULL if the path is missing or not a regular file.
void* ext2_read_path(const char* path, size_t* size);

// Release a buffer returned by ext2_read_path.
void ext2_free(void* p);

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

// --- Write (read-modify-write, no journal) ---------------------------------

// Create or overwrite a regular file with `size` bytes from `data` (the parent
// directory must exist). Supports direct + single-indirect blocks (~268 KiB at
// 1 KiB blocks). Returns false on error / out of space.
bool ext2_write_file(const char* path, const void* data, size_t size);
// Create a directory (its parent must exist and the name must be free).
bool ext2_mkdir(const char* path);
// Delete a regular file, or an empty directory. Returns false if missing, if a
// directory is non-empty, or on error.
bool ext2_remove(const char* path);

#endif
