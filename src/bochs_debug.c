#include <ports.h>
#include <bochs_debug.h>
#include <utils.h>

void dbg_putc(char c)
{
    outb(0xe9,c);
}

void dbg_print(char* s)
{
    while(*s) {
        dbg_putc(*s++);
    }
    dbg_putc('\n');
}

void dbg_vprintf(char* fmt, varg_list v)
{
    uint i;
    char buffer[32];
    for(i = 0; fmt[i]; i++) {
        switch(fmt[i]) {
        case '%':
            i++;
            switch(fmt[i]) {
            case '%':
                dbg_putc(fmt[i]);
                break;
            case 'u':
                num_to_str(varg_yield(v,uint), 16, buffer);
                dbg_print(buffer);
                break;
            case 'd':
                num_to_str(varg_yield(v,uint), 10, buffer);
                dbg_print(buffer);
                break;
            case 's':
                dbg_print(varg_yield(v,char*));
                break;
            case 'c':
                dbg_putc(varg_yield(v,char));
                break;
            case 'b':
                ;
                dbg_print(varg_yield(v,uint) ? "true" : "false");
                break;
            }
            break;
        default:
            dbg_putc(fmt[i]);
            break;
        }
    }
}

void dbg_printf(char* fmt, ...)
{
    varg_list v;
    varg_set(v,fmt);
    dbg_vprintf(fmt,v);
    varg_end(v);
}
