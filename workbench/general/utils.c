#include <utils.h>

void memcpy(void * _dst, void* _src, unsigned int count){
	unsigned int i = 0;

	unsigned char * dst = (unsigned char *) _dst;
	unsigned char * src = (unsigned char *) _src;

	for(i = 0; i < count; i++)
		dst[i] = src[i];
}

void memsetb(void * _dst, unsigned char val, unsigned int count){
	unsigned int i = 0;
	
	unsigned char * dst = (unsigned char *) _dst;

	for(i = 0; i < count; i++)
		dst[i] = val;
}

void memsetw(void* _dst, unsigned short val, unsigned int count){
	unsigned int i = 0;

	unsigned short * dst = (unsigned short *) _dst;
	
	for(i = 0; i < count; i++)
		dst[i] = val;
}

unsigned int strlen(const char * str){
	unsigned int i = 0;
	while(str[i]) i++;
	return i;
}

void strcpy(char * dst, const char * src){
	unsigned int i;
	for(i = 0; src[i] != '\0'; i++)
		dst[i] = src[i];
	dst[i] = '\0';
}

void strcat(char * dst, const char * src){
	unsigned int i = 0;
	while(dst[i]) i++;
	while(src[i]){  dst[i] = src[i]; i++; }
	dst[i] = '\0';
}

int memcmp(void * _m1, void * _m2, uint bytes){
	char * m1 = _m1, * m2 = _m2;
	for(uint i = 0; i < bytes; i++)
			if(m1[i] != m2[i]) 
					return (m1[i]-m2[i] < 0) ? -1 : 1;
	return 0;
}

int strcmp(char * str1, char * str2){
	uint i;
	for(i = 0; str1[i] && str2[i]; i++)
			if(str1[i] != str2[i]) 
					return (str1[i]-str2[i] < 0) ? -1 : 1;
	if(!str1[i]) return -1;
	if(!str2[i]) return 1;
	return 0;
}

unsigned int umax(unsigned int a, unsigned int b){
	return (a < b) ? b : a;
}
