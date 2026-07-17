#include "syscall_wrappers.h"

int dst[5];
int src[5] = {0,1,2,3,4};

char * mensajes_padre[] = {
    "Hello I am a task   ",
    "Hello I am a task.  ",
    "Hello I am a task.. ",
    "Hello I am a task..."
};

char * mensajes_hija[] = {
    "Hello I am a child task   ",
    "Hello I am a child task.  ",
    "Hello I am a child task.. ",
    "Hello I am a child task..."
};

void esperar(int tiempo){
    while(tiempo-- > 0) ;
}

void imprimir(char * buf[],int row){
    int i;
    for(i = 0; i < 4; ) {
        scrn_print(row,0,buf[i]);
        i = (i+1)%4;
    }
}

void padre(int hija){
    esperar(100000);
    kill(hija,SIGSTOP);
    esperar(600000*20);
    kill(hija,SIGCONT);
    imprimir(mensajes_padre,18);
}

void hija(){
    imprimir(mensajes_hija,19);
}

int main(){
    int forked = 0;
    if((forked = fork())) {
        padre(forked);
    }else{
        hija();
    }

    exit();
    return 0;
}
