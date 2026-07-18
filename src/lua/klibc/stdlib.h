#ifndef KLIBC_STDLIB_H
#define KLIBC_STDLIB_H
// Freestanding <stdlib.h> for the vendored Lua. malloc/realloc/free are backed
// by the kernel heap (klibc.c); strtod does string->double.
#include <stddef.h>
void* malloc(size_t n);
void* calloc(size_t n, size_t sz);
void* realloc(void* p, size_t n);
void free(void* p);
double strtod(const char* s, char** end);
long strtol(const char* s, char** end, int base);
static inline int abs(int x) { return x < 0 ? -x : x; }
void abort(void);
void exit(int code);
char* getenv(const char* name);
#define EXIT_SUCCESS 0
#define EXIT_FAILURE 1
#endif
