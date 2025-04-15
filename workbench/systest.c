#include <stdio.h>

extern void my_memcpy(void *dest, void *src, unsigned int count);
extern void _memsetb(void *dest, unsigned char val, int count);
extern int _strlen(char *str);

int strlen_c(char * src){
	int i = 0;
	while(src[i]) i++;
	return i;
}

void memcpy_c(void *dest, const void *src, unsigned int count){
	unsigned char * d = (unsigned char *) dest;
	unsigned char * s = (unsigned char *) src;

	unsigned int i;
	for(i = 0; i < count; i++){
		d[i] = s[i];
	}
}

char dst[] = "HOLA SOY JUANCITO PEREZ  ";
char src[] = "HOLA SOY PEPERINO POMBA  ";

int main(){
	printf("%d %d\n",strlen_c(src),strlen_c(dst));
	printf("ORIGINAL = %s\tSTRLEN = %d\n", dst, strlen_c(dst));
	my_memcpy(dst,src,strlen_c(dst));
	printf("%d\n",_strlen(dst));
	printf("%d\n",_strlen(dst));
	printf("COPIADO  = %s\tSTRLEN = %d %d\n", dst,strlen_c(dst),_strlen(dst));
/*	_memsetb(dst,'2',24);
	dst[24] = 0;
	printf("%s\n", dst);
*/
	return 0;
}
