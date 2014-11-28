#ifndef __UTILS_H
#define __UTILS_H

void memcpy(void *dest, void *src, unsigned int count);
void memset(void *dest, unsigned char val, unsigned int count);

unsigned int strlen(const char *str);

void strcpy(char * dst, const char * src);
void strcat(char * dst, const char * src);

int memcmp(void * , void * , unsigned int );
int strcmp(char *, char *);

void num_to_str(unsigned int, unsigned int, char *);

void strncpy(char *,char *,unsigned int);

#define CEIL(a,b) ((a)+(b)-1)/(b)
#define NULL ((void*)0)
#define BOCHSBREAK __asm__ __volatile__ ("xchg %%ebx,%%ebx")

typedef enum { false, true } bool;

#endif

