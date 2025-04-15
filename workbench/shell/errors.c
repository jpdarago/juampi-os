#include "errors.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>

char * text_errors[] = {
	"Comando demasiado largo",
	"Demasiados comandos pipeados",
	"Comando invalido",
	"Subcomando demasiado largo",
	"Argumento demasiado largo",
	"Demasiados argumentos",
	"Caracter invalido",
	"Expresion invalida",
	NULL
};

void handle_error(int error){
	bool ok = true;
	error = (error < 0) ? -error : error;

	for(int i = 0; i < error; i++)
		if(text_errors[i] == NULL){
			ok = false;
			break;
		}

	if(ok){
		printf("Error %d: %s\n",error,text_errors[error]);
	}else{ 
		printf("ERROR %d INVALIDO",error);
		exit(EXIT_FAILURE);
	}
}
