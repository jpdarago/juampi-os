#ifndef __KMODULE_H
#define __KMODULE_H

#include <stdint.h>
#include <stddef.h>

// Boot modules (files) loaded by Limine alongside the kernel — used to ship Lua
// scripts (init.lua and friends) into the image.
size_t kmodule_count(void);
const char* kmodule_path(size_t i);
const void* kmodule_data(size_t i, size_t* size);
// Find a module whose path ends with `name` (e.g. "init.lua"); NULL if none.
const void* kmodule_find(const char* name, size_t* size);

#endif
