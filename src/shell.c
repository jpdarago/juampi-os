#include <shell.h>
#include <console.h>
#include <luashell.h>
#include <fault.h>
#include <ksym.h>
#include <gfx.h>
#include <qoi.h>
#include <kmodule.h>
#include <memory.h>

#include <stddef.h>
#include <stdint.h>

#define LINE_MAX 256

// Boot logo, decoded once from the logo.qoi Limine module and kept resident so
// it can be re-blitted cheaply. The text console (flanterm) keeps its own
// canvas and rewrites the whole framebuffer from it whenever it scrolls, which
// erases anything we drew directly — so the logo has to be redrawn after output
// that may have scrolled, not just once at startup.
static uint32_t* logo_pixels;
static qoi_image logo_img;

static void logo_load(void)
{
    size_t size = 0;
    const void* data = kmodule_find("logo.qoi", &size);
    if (data != NULL) {
        logo_pixels = qoi_decode(&heap_default()->base, data, size, &logo_img);
    }
}

// Blit the logo into the top-right corner (clamped on-screen).
static void logo_draw(void)
{
    if (!gfx_available() || logo_pixels == NULL) {
        return;
    }
    int64_t x = (int64_t)gfx_width() - (int64_t)logo_img.width - 16;
    if (x < 0) {
        x = 0;
    }
    gfx_blit(x, 16, logo_img.width, logo_img.height, logo_pixels);
}

void shell_run(void)
{
    static char line[LINE_MAX];
    luashell_init();
    logo_load();
    console_print(
            "\njuampiOS Lua shell (Lua 5.4).\n"
            "  math/string/table/coroutine, and 'k' for kernel introspection:\n"
            "  k.cpubrand()  k.freemem()  k.uptime()  k.ncores()  "
            "k.hexdump(addr)\n"
            "  run(name[,arg]) runs a .lua script or a native .elf binary.\n"
            "  bench(fn|name[,arg[,iters]]) -> total,per_call (Lua or "
            "native).\n");
    // Draw the logo after the banner (which may have scrolled the console).
    logo_draw();

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
        // A completed evaluation may have printed output and scrolled the
        // console, wiping the logo; redraw it so it stays in the corner.
        if (!cont) {
            logo_draw();
        }
    }
}
