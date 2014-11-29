#include "syscall_wrappers.h"
#include "stdio.h"
#include "parser.h"
#include "utils.h"

char * special_commands[] = {
    "cd", NULL
};

typedef void (*special_handler)(argument_list *);
void cd_handler(argument_list * args)
{
    if(args->length != 2) {
        printf("Error, cantidad incorrecta de argumentos\n");
        return;
    }
    int res = set_cwd(args->list[1].str);
    if(res != 0) printf("No se pudo cdear a %s: error %d\n",
                        args->list[1].str,res);
    return;
}

special_handler special_handlers[] = {
    cd_handler, NULL
};

special_handler get_handler(char * command)
{
    for(int i = 0; special_commands[i]; i++) {
        if(!strcmp(special_commands[i],command))
            return special_handlers[i];
    }
    return NULL;
}

int readline(char * buffer,int len)
{
    int read_bytes = read(STDIN,len,buffer);
    if(read_bytes < 0 || read_bytes >= len)
        return read_bytes;
    buffer[read_bytes] = '\0';
    if(read_bytes >= 1) {
        //Sacamos el \n del final de
        //la nueva linea
        buffer[read_bytes-1] = '\0';
    }
    return read_bytes-1;
}

char * argument_buffer[MAXARGS];
int fork_and_process(argument_list * arg)
{
    int forked = fork();
    if(forked < 0) fail("Fork fallo");
    if(forked == 0) {
        for(int i = 0; i < arg->length; i++)
            argument_buffer[i] = arg->list[i].str;
        argument_buffer[arg->length] = NULL;
        if(exec(arg->list[0].str,argument_buffer) < 0) {
            printf("Exec de %s fallo.\n",arg->list[0].str);
            exit();
        }
    }else{
        wait4(forked);
    }
    return forked;
}

#define MAXLEN 1024
char line_buffer[MAXLEN];
char cwd[FS_MAXLEN];

int main(int argc, char * argv[])
{
    int stdout_term = open("/dev/tty",FS_WR);
    if(stdout_term < 0)
        fail("No se pudo abrir la terminal para escribir");
    int stdin_term = open("/dev/tty",FS_RD);
    if(stdin_term < 0)
        fail("No se pudo abrir la terminal para leer");

    if(argc != 2)
        fail("No se ha podido loguear nadie");

    for(;; ) {
        get_cwd(cwd);
        printf("%s:%s$ ",argv[1],cwd);

        int read_bytes = readline(line_buffer,MAXLEN);
        if(read_bytes < 0 || read_bytes >= MAXLEN)
            fail("Lectura fallo o demasiado leido");

        argument_list * args = parse_arguments(line_buffer);
        if(args == NULL) {
            printf("Error de parseo: %s\n",get_parse_error());
            continue;
        }

        if(args->length == 0) continue;
        special_handler handler = get_handler(args->list[0].str);
        if(handler != NULL) {
            handler(args);
        }else{
            fork_and_process(args);
        }
    }

    exit();
    return 0;
}
