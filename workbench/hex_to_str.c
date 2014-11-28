#include <string.h>
typedef unsigned int uint;

static void remove_zeros(char * buffer){
        uint i,j; 
        for(i = 0;buffer[i];i++){ 
                if(buffer[i] != '0') break; 
        } 
 
        if(!buffer[i]){
				buffer[0] = '0'; buffer[1] = '\0';
				return; 
		}
        for(j = i;buffer[j]; j++){ 
                buffer[j-i]=buffer[j]; 
        }
		buffer[j-i] = '\0';
}

static void num_str(uint n, uint base, char * output){ 
		char buf[32];
		memset(buf,'0',sizeof(buf));
        *(buf+32) = '\0'; //Voy a imprimir con todos los ceros delante, asi que necesito 8 lugares fijos. 
        uint ind = 31, count = 8;         
        do{ 
                char c = n % base; 
                if(c > 9){ 
                        c = c-10+'A';    
                }else{ 
                        c = c+'0';               
                } 
 
                buf[ind--] = c; 
                n = n/base; 
        }while(n > 0);
 		remove_zeros(buf);
		strcpy(output,buf);
}

#include <stdio.h>

int main(){
	char buf[16];
	int tests[] = {0, 0xBADBEEF, 0xDEAD, 0xFEDE, 0xAAAA, 0x1234, 0xFFFFFFFF};
	int i;

	for(i = 0; i < 7; i++)
	{
		num2str(tests[i],16,buf);
		printf("%x: %s\n",tests[i],buf);
	}

	for(i = 0; i < 30; i++){
		num2str(i,10,buf);
		printf("%d: %s\n",i,buf);
	}

	return 0;	
}
