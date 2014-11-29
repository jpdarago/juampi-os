#include <ports.h>
#include <bochs_debug.h>
#include <utils.h>

void dbg_putc(char c)
{
    outb(0xe9,c);
}

void dbg_print(const char* s)
{
    while(*s) {
        dbg_putc(*s++);
    }
    dbg_putc('\n');
}

static void dbg_vprintf(const char* fmt, va_list v)
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
                num_to_str(va_arg(v,uint), 16, buffer);
                dbg_print(buffer);
                break;
            case 'd':
                num_to_str(va_arg(v,uint), 10, buffer);
                dbg_print(buffer);
                break;
            case 's':
                dbg_print(va_arg(v,char*));
                break;
            case 'c':
                dbg_putc(va_arg(v,uint));
                break;
            case 'b':
                ;
                dbg_print(va_arg(v,uint) ? "true" : "false");
                break;
            }
            break;
        default:
            dbg_putc(fmt[i]);
            break;
        }
    }
}

void dbg_printf(const char* fmt, ...)
{
    va_list v;
    va_start(v,fmt);
    dbg_vprintf(fmt,v);
    va_end(v);
}
