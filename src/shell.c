#include <shell.h>
#include <serial.h>
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

// Read one line from serial into buf (NUL-terminated), with basic editing:
// echo, backspace, and CR/LF to submit. Returns the line length.
static size_t read_line(char* buf, size_t max)
{
    size_t n = 0;
    for (;;) {
        char c = serial_getc();
        if (c == '\r' || c == '\n') {
            serial_print("\r\n");
            buf[n] = '\0';
            return n;
        }
        if (c == 0x7F || c == 0x08) { // DEL / backspace
            if (n > 0) {
                n--;
                serial_print("\b \b"); // erase the character on the terminal
            }
            continue;
        }
        if (c >= 0x20 && c < 0x7F && n < max - 1) {
            buf[n++] = c;
            serial_putc(c); // echo
        }
    }
}

static void cmd_help(void)
{
    serial_print("commands:\n"
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
        serial_print("free frames: ");
        serial_dec(frames_available());
        serial_print("\n");
    } else if (str_eq(line, "ticks")) {
        serial_dec(timer_ticks());
        serial_print("\n");
    } else if (line[0] == 'e' && line[1] == 'c' && line[2] == 'h' &&
               line[3] == 'o' && (line[4] == ' ' || line[4] == '\0')) {
        serial_print(line[4] == ' ' ? line + 5 : "");
        serial_print("\n");
    } else {
        serial_print("unknown command: ");
        serial_print(line);
        serial_print(" (try 'help')\n");
    }
}

void shell_run(void)
{
    static char line[LINE_MAX];
    serial_print("\njuampiOS shell. Type 'help'.\n");
    for (;;) {
        serial_print("jp> ");
        read_line(line, sizeof(line));
        shell_eval(line);
    }
}
