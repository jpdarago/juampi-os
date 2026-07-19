#include <shell.h>
#include <console.h>
#include <luashell.h>

#include <stddef.h>

#define LINE_MAX 256

void shell_run(void)
{
    static char line[LINE_MAX];
    luashell_init();
    console_print(
            "\njuampiOS Lua shell (Lua 5.4).\n"
            "  math/string/table/coroutine, and 'k' for kernel introspection:\n"
            "  k.cpubrand()  k.freemem()  k.uptime()  k.bench(fn,n)  "
            "k.hexdump(addr)\n");
    for (;;) {
        console_print("lua> ");
        console_read_line(line, sizeof(line));
        luashell_eval(line);
    }
}
