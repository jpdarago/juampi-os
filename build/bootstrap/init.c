#include "syscall_wrappers.h"

char * arguments[] = { "shell.run", "jpdarago", (char *) 0 };

int main()
{
    int res = fork();
    if(res == 0) {
        res = exec(arguments[0],arguments);
        if(res) exit();
    }
    while(1) ;
}
