#include "syscall_wrappers.h"
#include "stdio.h"
#include "parser.h"
#include "utils.h"

char buf[1024];
int main(int argc, const char * argv[])
{
	if(argc != 3){
		printf("Numero de argumentos incorrectos\n");
		printf("\tUso: cp <desde> <hasta>\n");
		exit();
	}		

	int fd1 = open(argv[1],FS_RD);
	if(fd1 < 0){
		printf("Abrir el archivo %s fallo\n",argv[1]);
		exit();
	}
	int fd2 = open(argv[2],FS_WR | FS_TRUNC | FS_CREAT);
	if(fd2 < 0){
		printf("Abrir el archivo %s fallo\n",argv[2]);
		exit();
	}	
	int rd;	
	for(rd = read(fd1,1023,buf);;
		rd = read(fd1,1023,buf)){

		if(rd <= 0){
			if(rd < 0)
				printf("Error en cp al leer: %d\n",rd);
			break;
		}

		int wr = write(fd2,rd,buf);
		if(wr < rd){
			printf("Error en cp al escribir: %d\n",wr);
		}
	}

	close(fd1); close(fd2);
	exit();
	return 0;
}
