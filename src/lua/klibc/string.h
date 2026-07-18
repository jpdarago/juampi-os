#ifndef KLIBC_STRING_H
#define KLIBC_STRING_H
// Freestanding <string.h> for the vendored Lua. Implemented in klibc.c
// (plus memset/memcpy from the kernel's utils.c).
#include <stddef.h>
void* memset(void* d, int c, size_t n);
void* memcpy(void* d, const void* s, size_t n);
void* memmove(void* d, const void* s, size_t n);
int memcmp(const void* a, const void* b, size_t n);
void* memchr(const void* s, int c, size_t n);
size_t strlen(const char* s);
int strcmp(const char* a, const char* b);
int strncmp(const char* a, const char* b, size_t n);
int strcoll(const char* a, const char* b);
char* strcpy(char* d, const char* s);
char* strchr(const char* s, int c);
char* strpbrk(const char* s, const char* set);
size_t strspn(const char* s, const char* set);
char* strstr(const char* h, const char* n);
char* strerror(int e);
#endif
