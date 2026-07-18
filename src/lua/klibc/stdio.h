#ifndef KLIBC_STDIO_H
#define KLIBC_STDIO_H
// Freestanding <stdio.h> for the vendored Lua. Formatting comes from the
// vendored printf; all "file" output is routed to the kernel console, so
// stdout/stderr are opaque non-NULL sentinels and the FILE* argument is ignored.
#include <stddef.h>
#include <stdarg.h>
#include <printf/printf.h> // printf/sprintf/snprintf/vsnprintf (standard names)

typedef struct __FILE FILE;
extern FILE* stdout;
extern FILE* stderr;
extern FILE* stdin;

#define EOF (-1)
#define BUFSIZ 512

// printf/sprintf/snprintf/vsnprintf/vprintf come from <printf/printf.h> above.
size_t fwrite(const void* p, size_t sz, size_t n, FILE* f);
int fputs(const char* s, FILE* f);
int fputc(int c, FILE* f);
int fflush(FILE* f);
int fprintf(FILE* f, const char* fmt, ...);
int vfprintf(FILE* f, const char* fmt, va_list ap);

// File input is not supported (no disk-backed Lua loading); these stubs exist
// only so lauxlib's luaL_loadfile path compiles and links. They always "fail".
FILE* fopen(const char* path, const char* mode);
FILE* freopen(const char* path, const char* mode, FILE* f);
int fclose(FILE* f);
size_t fread(void* p, size_t sz, size_t n, FILE* f);
int getc(FILE* f);
int ungetc(int c, FILE* f);
int feof(FILE* f);
int ferror(FILE* f);
#endif
