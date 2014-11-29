#include <keyboard.h>
#include <memory.h>
#include <ports.h>
#include <keyboard_tables.h>
#include <irq.h>
#include <utils.h>

int * keyboard_queue;
int q_start,q_end,q_size;
bool q_inuse;

#define q_next(x) (((x)+1)%q_size)

// Agrega key al buffer de teclado si hay
// suficiente espacio
void keybuffer_produce(uint key)
{
    if(q_size > 0) {
        keyboard_queue[q_end] = key;
        q_end = q_next(q_end);
        if(q_end == q_start) {
            // Si la cola esta llena perdemos
            // caracteres viejos
            q_start = q_next(q_start);
        }
    }
}

// Saca del buffer de teclado
int keybuffer_consume()
{
    uint eflags = irq_cli();
    int res = -1;
    if(q_start == q_end)
        goto end;
    res = keyboard_queue[q_start];
    q_start = q_next(q_start);

end:
    irq_sti(eflags);
    return res;
}

// Inicializa el buffer de teclado
void keybuffer_init(int buffer_size)
{
    keyboard_queue =
        kmalloc(buffer_size*sizeof(uint));
    q_start = q_end = 0;
    q_size = buffer_size;
    q_inuse = false;
}

uint get_key(uchar scancode){
    return kbdus[scancode];
}

void handle_key_event(uchar scancode)
{
    bool was_released = scancode & 0x80;
    uint code = scancode & ~0x80;

    pressed[code] = !was_released;
    if(was_released) return;

    char key = get_key(code);
    if(key) {
        keybuffer_produce(key);
    }
}

#define KBD_DATAPORT    0x60
#define KBD_STATPORT    0x64
#define KBD_CTRLPORT    0x64

// Punto de entrada para la interrupccion de teclado
void keyboard_irq_handler(uint irq_code,gen_regs g)
{
    uchar scancode = inb(KBD_DATAPORT);
    handle_key_event(scancode);
}
