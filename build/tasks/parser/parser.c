#include "parser.h"
#include "errors.h"
#include <stdio.h>
typedef enum {false,true} bool;

#define EOS -1

static int offset,
           error_code,
           argument_number;

static const char * command;
static argument_list arglist;

static void start_parser(const char * _command)
{
    command = _command;
    offset = error_code = argument_number = 0;
    arglist.length = 0;
}

char peek_token()
{
    if(command[offset] == '\0')
        return EOS;
    return command[offset];
}

void shift_token(){
    offset++;
}

bool space(char c)
{
    return c == ' ' ||
           c == EOS ||
           c == '\t';
}

bool alpha(char c)
{
    return (c >= 'a' && c <= 'z') ||
           (c >= '0' && c <= '9');
}
bool special(char c)
{
    return c == '_' ||
           c == '-' ||
           c == '/' ||
           c == ':' ||
           c == '.';
}

bool valid(char c){
    return alpha(c) || special(c);
}

void ignore_spaces()
{
    char c;
    for(c = peek_token(); c != EOS; c = peek_token()) {
        if(!space(c)) break;
        shift_token();
    }
}

int parse_argument()
{
    int len = 0; char c;
    for(c = peek_token();
        c != EOS && len < MAXARGLEN;
        c = peek_token(), len++)
    {
        if(!valid(c)) break;
        arglist.list[arglist.length].str[len] = c;
        shift_token();
    }
    if(len == MAXARGLEN)
        return -EINVARGLEN;
    if(!space(c))
        return -EINVCHAR;

    arglist.list[arglist.length].str[len] = '\0';
    arglist.length++;
    return 0;
}

int parse_argument_list()
{
    if(arglist.length >= MAXARGS)
        return -ETOOMANY;
    int res = parse_argument();
    if(res < 0) return res;
    ignore_spaces();
    if(peek_token() != EOS) {
        res = parse_argument_list();
        if(res < 0) return res;
    }
    return 0;
}

argument_list * parse_arguments(const char * _command)
{
    start_parser(_command);
    ignore_spaces();
    if(peek_token() == EOS) {
        arglist.length = 0;
        return &arglist;
    }
    error_code = parse_argument_list();
    return (!error_code) ? &arglist : NULL;
}

const char * get_parse_error()
{
    if(error_code == 0)
        return "No hay error";
    printf("El error es %d\n",-error_code);
    char * res = error_message(-error_code);
    if(res == NULL) return "Error invalido";
    return res;
}
