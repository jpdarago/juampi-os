#include <shell.h>
#include <console.h>
#include <luashell.h>

#include <stddef.h>

#define LINE_MAX 256

void shell_run(void)
{
    static char line[LINE_MAX];
    luashell_init();
    console_print("\njuampiOS Lua shell (Lua 5.4). Try: print(1+2) or "
                  "math.sqrt(2)\n");
    for (;;) {
        console_print("lua> ");
        console_read_line(line, sizeof(line));
        luashell_eval(line);
    }
}
