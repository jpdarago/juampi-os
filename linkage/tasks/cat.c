#include "syscall_wrappers.h"
#include "stdio.h"
#include "utils.h"

#define BUFFER_SIZE 1024
char buffer[BUFFER_SIZE];
int main(int argc, const char * argv[])
{
	if(argc < 2){
		printf("Argumentos incorrectos\n");
		printf("\tUso: cat <nombre archivo>\n");
		exit();
	}

	int fd = open(argv[1],FS_RD), rd;
	if(fd < 0){
		printf("Error en cat al abrir archivo: codigo %d\n", fd);
		exit();
	}
	
	for(rd = read(fd,BUFFER_SIZE-1,buffer);;
		rd = read(fd,BUFFER_SIZE-1,buffer)){
		
		if(rd <= 0){
			if(rd < 0)
				printf("Error en cat al leer: codigo%d\n",read);
			break;
		}

		buffer[rd] = '\0';
		printf("%s",buffer);
	}
	
	close(fd);
	exit();
	return 0;
}
