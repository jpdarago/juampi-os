#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>
#include <sys/wait.h>

#include "parser.h"
#include "errors.h"
#include "shell.h"
#include "utils.h"

char command[MAX_LEN+1];

program program_call[1+MAX_PROGRAMS];
int programs;

int pipe_fds[2*(1+MAX_PROGRAMS)],
	command_length;

int print_and_fetch(){
	printf("> ");
	char * str = fgets(command,MAX_LEN-1,stdin);
	if(str == NULL) return -1;
	int read = strlen(str);
	if(read == MAX_LEN-1) return -ETOOLONG;
	command[read-1] = '\0';
	command_length = read-1;
	return read;
}

#define syscall_wrap(x) \
	if((x) < 0){ perror("Haciendo " #x); exit(EXIT_FAILURE); }

int main()
{
	int read = 0;
	for(read = print_and_fetch() ; 
		read > 0 ; 
		read = print_and_fetch() )
	{		
		int count = parse_command(command);
		if(count < 0){
			handle_error(count);
			continue;
		}
	
		for(int i = 0; i < count-1; i++){
			syscall_wrap(pipe(pipe_fds+2*i));
		}
		
		for(int i = 0; i < count; i++){
			program * p = &program_call[i];
			if(fork() == 0){
				int prev_in = 2*(i-1), prev_out= 2*i+1;
				
				if(i > 0) 
					syscall_wrap(
						dup2(pipe_fds[prev_in],STDIN_FILENO));
				if(i+1 < count) 
					syscall_wrap(
						dup2(pipe_fds[prev_out],STDOUT_FILENO));

				for(int j = 0; j < 2*count-2; j++)
					syscall_wrap(close(pipe_fds[j]));
				
				syscall_wrap(execvp(p->filename,p->arguments));
			}		
		}

		for(int i = 0; i < 2*count-2; i++){
			syscall_wrap(close(pipe_fds[i]));
		}
		
		for(int i = 0; i < count; i++)
			wait(NULL);
		free_current_command();
	}
	return 0;
}
