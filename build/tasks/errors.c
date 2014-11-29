#include "errors.h"
#include "utils.h"

static char * text_errors[] = {
    NULL,
    "Comando demasiado largo",
    "Comando invalido",
    "Argumento demasiado largo",
    "Demasiados argumentos",
    "Caracter invalido",
    "Expresion invalida"
};

static unsigned int errors =
    sizeof(text_errors)/sizeof(text_errors[0]);

char * error_message(int error){
    if(error < 0 || error >= errors)
        return NULL;
    return text_errors[error];
}
