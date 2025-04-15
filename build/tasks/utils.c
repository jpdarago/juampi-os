#include "utils.h"

unsigned int strlen(const char* str)
{
    unsigned int i = 0;
    while(str[i] != '\0') {
        i++;
    }
    return i;
}

void strcpy(char* dst, const char* src)
{
    unsigned int i;
    for(i = 0; src[i] != '\0'; i++) {
        dst[i] = src[i];
    }
    dst[i] = '\0';
}

void strcat(char* dst, const char* src)
{
    unsigned int i = 0;
    while(dst[i]) {
        i++;
    }
    while(src[i]) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

void memcpy(void * _dst, void * _src, unsigned int bytes)
{
    char * dst = _dst, * src = _src;
    while(bytes-- > 0) {
        dst[bytes] = src[bytes];
    }
}

void memset(void * _dst, unsigned char val, unsigned int bytes)
{
    char * dst = _dst;
    while(bytes-- > 0) {
        dst[bytes] = val;
    }
}

int memcmp(void* _m1, void* _m2, unsigned int bytes)
{
    char* m1 = _m1, * m2 = _m2;
    for(unsigned int i = 0; i < bytes; i++)
        if(m1[i] != m2[i]) {
            return (m1[i]-m2[i] < 0) ? -1 : 1;
        }
    return 0;
}

int strcmp(char* str1, char* str2)
{
    unsigned int i;
    for(i = 0; str1[i] == str2[i]; i++)
        if(str1[i] == '\0') {
            return 0;
        }
    return str1[i]-str2[i];
}

static void remove_zeros(char* buffer)
{
    unsigned int i,j;
    for(i = 0; buffer[i]; i++) {
        if(buffer[i] != '0') {
            break;
        }
    }
    if(!buffer[i]) {
        buffer[0] = '0';
        buffer[1] = '\0';
        return;
    }
    for(j = i; buffer[j]; j++) {
        buffer[j-i]=buffer[j];
    }
    buffer[j-i] = '\0';
}

void num_to_str(int n, unsigned int base, char* output)
{
    char buf[33];
    memset(buf,'0',sizeof(buf));
    *(buf+32) = '\0';
    if(n < 0) {
        *output++ = '-';
        n = -n;
    }
    unsigned int ind = 31;
    do {
        char c = n % base;
        if(c > 9) {
            c = c-10+'A';
        } else {
            c = c+'0';
        }
        buf[ind--] = c;
        n = n/base;
    } while(n > 0);
    remove_zeros(buf);
    strcpy(output,buf);
}

void strncpy(char* dst, char* src, unsigned int len)
{
    for(int i = 0; i < len; i++) {
        if(src[i] == '\0') {
            return;
        }
        dst[i] = src[i];
    }
}
