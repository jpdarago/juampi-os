#include "utils.h"
#include "shell.h"
#include <stdlib.h>

void free_current_command(){
	for(int i = 0; i < MAX_PROGRAMS; i++)
		for(int j = 0; j < MAX_ARGS; j++){
			free(program_call[i].arguments[j]);	
			program_call[i].arguments[j] = NULL;
		}
}
