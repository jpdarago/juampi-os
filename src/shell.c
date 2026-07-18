#include <shell.h>
#include <console.h>
#include <frames.h>
#include <idt.h>

#include <stddef.h>
#include <stdbool.h>

#define LINE_MAX 256

static bool str_eq(const char* a, const char* b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

static void cmd_help(void)
{
    console_print("commands:\n"
                  "  help          this message\n"
                  "  echo <text>   print text\n"
                  "  mem           free physical frames\n"
                  "  ticks         timer ticks since boot\n");
}

// Evaluate one line. This is the hook the Lua interpreter replaces.
static void shell_eval(char* line)
{
    if (line[0] == '\0') {
        return;
    }
    if (str_eq(line, "help")) {
        cmd_help();
    } else if (str_eq(line, "mem")) {
        console_print("free frames: ");
        console_dec(frames_available());
        console_print("\n");
    } else if (str_eq(line, "ticks")) {
        console_dec(timer_ticks());
        console_print("\n");
    } else if (line[0] == 'e' && line[1] == 'c' && line[2] == 'h' &&
               line[3] == 'o' && (line[4] == ' ' || line[4] == '\0')) {
        console_print(line[4] == ' ' ? line + 5 : "");
        console_print("\n");
    } else {
        console_print("unknown command: ");
        console_print(line);
        console_print(" (try 'help')\n");
    }
}

void shell_run(void)
{
    static char line[LINE_MAX];
    console_print("\njuampiOS shell. Type 'help'.\n");
    for (;;) {
        console_print("jp> ");
        console_read_line(line, sizeof(line));
        shell_eval(line);
    }
}
