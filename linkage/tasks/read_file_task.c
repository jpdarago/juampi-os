#include "syscall_wrappers.h"
#include "utils.h"
#include "stdio.h"

int start = 18;


char buffer[2049];
char * lecmessage = "Lei lo siguiente: ";

void read_file(char * path)
{
    int fd = open(path,FS_RD);
    if(fd < 0) fail("File descriptor invalido");
    int bytes_read = read(fd,2048,buffer);
    if(bytes_read < 0) fail("Lectura invalida");
    buffer[bytes_read] = '\0';
    scrn_print(start,0,lecmessage);
    scrn_print(start,strlen(lecmessage),buffer);
    start++;
}

int main()
{
    read_file("/docs/docs/prueba3.txt");
    read_file("/docs/docs/prueba2.txt");
    while(1) ;
    exit();
}
