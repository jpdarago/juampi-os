#include "parser.h"
#include "errors.h"
#include "shell.h"
#include "utils.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <setjmp.h>

#define EOS -1

static int offset, 
	error_code, 
	command_length,
	argument_number;

static char * command;
static bool invalid = false;

static jmp_buf buf;

#define set_parse_err(x,code)\
	if(x){ invalid = true; error_code = code; longjmp(buf, 1); }

char * str(){
	return command+offset;
}

char peek_token(){
	if(offset >= command_length) 
		return EOS;
	return command[offset];
}

void shift_token(){
	offset++;
}

char next_token(){
	char c = peek_token();
	shift_token();
	return c;
}

bool space(char c){ return c == ' ' || c == EOS; }
bool alpha(char c){ return c >= 'a' && c <= 'z'; }
bool reserved(char c){ return space(c) || c == '|'; }
bool valid(char c){ return alpha(c) || reserved(c); }

void ignore_spaces(){
	for(;offset < command_length;offset++){
		if(command[offset] != ' ')
			break;
	}
}

void parse_name(){
	int i; char c; 
	program * p = &program_call[programs];
	for(i = 0, c = peek_token(); 
			i < MAX_FILENAME && alpha(c); 
			i++, c = peek_token())
	{
		p->filename[i] = c;
		shift_token();
	}

	set_parse_err(i >= MAX_FILENAME,EPROGLONG);	
	set_parse_err(i == 0, EINVEXPR);
	set_parse_err(!valid(c),EINVCHAR);
	
	p->filename[i] = '\0';	
	p->arguments[0] = malloc(MAX_ARGLEN+1);
	strcpy(p->arguments[0],p->filename);
}

void parse_argument_list(){
	set_parse_err(argument_number >= MAX_ARGS,EMANYARGS);
	ignore_spaces();
		
	if(reserved(peek_token()))
		return;
	
	char * arg = malloc(MAX_ARGLEN+1);
	program_call[programs]
		.arguments[argument_number] = arg;
	
	char c; int i;

	for(i = 0, c = next_token(); 
		i < MAX_ARGLEN && !reserved(c);
		i++, c = next_token() ){
		
		arg[i] = c;
	}
	set_parse_err(i >= MAX_ARGLEN,EARGLONG);
	
	arg[i] = '\0';
	argument_number++;
	ignore_spaces();
	
	switch(c = peek_token()){
		case EOS:
		case '|':
			break;	
		default:
			if(!reserved(c)){
				parse_argument_list();
			}else{
				set_parse_err(true,EINVCHAR);		
			}
			break;
	}
}

void parse_program_call(){
	set_parse_err(programs >= MAX_PROGRAMS,ETOOMANY);
	
	parse_name();
	ignore_spaces();
	
	argument_number = 1;
	parse_argument_list();

	program_call[programs]
		.arguments[argument_number] = NULL;
}

void parse_program_list(){
	ignore_spaces();
	parse_program_call();
	programs++;
	ignore_spaces();
	switch(peek_token()){
		case '|':
			shift_token();
			parse_program_list();
		case EOS:
			break;
		default:
			break;
	}
}

int parse_command(char * _command){
	command = _command;
	command_length = strlen(_command);
	if(command_length == 0) return 0;

	offset = programs = 0;
	invalid = false;

	if(!setjmp(buf)){
		parse_program_list();
	}else{
		if(invalid){	
			free_current_command();
			return -error_code;
		}
	}

	return programs;
}
