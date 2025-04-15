#include <stdio.h>

void itoa(unsigned int n, char * buf){
	buf[8] = '\0'; //Voy a imprimir con todos los ceros delante, asi que necesito 8 lugares fijos.
	unsigned int ind = 7, count = 8;	
	while(count-- > 0){
		char c = n & 0xF;
		if(c > 9){
			c = c-10+'A';	
		}else{
			c = c+'0';		
		}

		buf[ind] = c;
		ind--;
		n = n >> 4;
	}
}

char buffer[16];

int main(){
	itoa(21,buffer);
	printf("RESULTADO = %s\n",buffer);
	return 0;
}
