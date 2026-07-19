#include <shell.h>
#include <console.h>
#include <luashell.h>
#include <fault.h>
#include <ksym.h>

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
            "k.hexdump(addr)\n"
            "  run(\"name.lua\") runs a shipped script.\n");

    int cont = 0;

    // Recovery point: a CPU fault while evaluating Lua (e.g. a bad k.poke)
    // longjmps here instead of halting. The fault handler entered with
    // interrupts masked and left the interpreter mid-call, so re-enable
    // interrupts and start a fresh interpreter (the old, inconsistent state is
    // abandoned).
    if (setjmp(fault_env) != 0) {
        __asm__ __volatile__("sti");
        console_print("\n[recovered from fault: vector ");
        console_dec(fault_vector);
        console_print(", addr ");
        console_hex(fault_addr);
        console_print(", rip ");
        console_hex(fault_rip);
        uint64_t off = 0;
        const char* fn = ksym_lookup(fault_rip, &off);
        if (fn) {
            console_print(" (");
            console_print(fn);
            console_print(")");
        }
        console_print(" — interpreter reset]\n");
        luashell_init();
        cont = 0;
    }

    for (;;) {
        console_print(cont ? "  >> " : "lua> ");
        console_read_line(line, sizeof(line));
        fault_armed = true;
        cont = luashell_eval(line);
        fault_armed = false;
    }
}
