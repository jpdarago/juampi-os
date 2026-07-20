#include <smp.h>
#include <limine.h>
#include <idt.h>
#include <console.h>
#include <ktime.h>

#include <stdint.h>
#include <stdbool.h>

// Limine MP request: Limine brings the application processors up to long mode
// for us and hands back an array of per-CPU info; we start each one by writing
// its goto_address.
__attribute__((
        used,
        section(".limine_requests"))) static volatile struct limine_smp_request
        mp_request = {.id = LIMINE_SMP_REQUEST, .revision = 0};

// The AP entry trampoline (smp_ap.S) — enables SSE, then calls ap_main.
void ap_main(struct limine_smp_info* info);
extern void ap_entry(struct limine_smp_info* info);

#define IA32_GS_BASE 0xC0000101u
#define AP_STACK_SZ 0x4000 // interrupt stack per core (only used on a fault)

static cpu* cpus;
static uint64_t ncpus = 1;
static uint32_t bsp_index;

// Point %gs at this core's per-CPU struct, so smp_this_cpu() can recover it via
// the self pointer at %gs:0.
static void set_gs_base(void* p)
{
    uint64_t v = (uint64_t)p;
    __asm__ __volatile__("wrmsr" ::"c"(IA32_GS_BASE), "a"((uint32_t)v),
                         "d"((uint32_t)(v >> 32)));
}

uint64_t smp_cpu_count(void)
{
    return ncpus;
}

uint32_t smp_bsp_index(void)
{
    return bsp_index;
}

struct cpu* smp_this_cpu(void)
{
    cpu* c;
    __asm__ __volatile__("mov %%gs:0, %0" : "=r"(c));
    return c;
}

// Runs on each application processor after the SSE trampoline. Claims its
// per-CPU struct, installs its own GDT/TSS + the shared IDT, signals ready, and
// then parks polling its work mailbox. Interrupts stay disabled (M8).
void ap_main(struct limine_smp_info* info)
{
    cpu* c = (cpu*)info->extra_argument;
    set_gs_base(c);
    gdt_ap_load(c->gdt, &c->tss, c->kstack_top);
    idt_load();
    __atomic_store_n(&c->ready, 1, __ATOMIC_RELEASE);

    for (;;) {
        if (__atomic_load_n(&c->mbox, __ATOMIC_ACQUIRE) == CPU_MBOX_PENDING) {
            c->fn(c->arg);
            __atomic_store_n(&c->mbox, CPU_MBOX_DONE, __ATOMIC_RELEASE);
        }
        __builtin_ia32_pause();
    }
}

void smp_init(allocator* mem)
{
    struct limine_smp_response* r = mp_request.response;
    uint64_t count = (r != NULL) ? r->cpu_count : 1;
    if (count < 1) {
        count = 1;
    }
    ncpus = count;
    cpus = new (mem, cpu, (ptrdiff_t)ncpus);

    // Initialise every per-CPU struct and find the BSP.
    bsp_index = 0;
    for (uint64_t i = 0; i < ncpus; i++) {
        cpus[i].self = &cpus[i];
        cpus[i].index = (uint32_t)i;
        cpus[i].lapic_id = r ? r->cpus[i]->lapic_id : 0;
        cpus[i].mbox = CPU_MBOX_IDLE;
        cpus[i].ready = 0;
        cpus[i].kstack_top =
                (uint64_t)new (mem, char, AP_STACK_SZ) + AP_STACK_SZ;
        if (r && r->cpus[i]->lapic_id == r->bsp_lapic_id) {
            bsp_index = (uint32_t)i;
        }
    }

    // The BSP is already running: claim its struct (so smp_this_cpu works
    // here).
    set_gs_base(&cpus[bsp_index]);
    cpus[bsp_index].ready = 1;

    if (r == NULL || ncpus <= 1) {
        console_print("juampiOS: SMP: 1 core (uniprocessor)\n");
        return;
    }

    // Start each AP: stash its per-CPU pointer, then publish goto_address (the
    // release fence orders the argument write before Limine sees the entry).
    for (uint64_t i = 0; i < ncpus; i++) {
        if (i == bsp_index) {
            continue;
        }
        r->cpus[i]->extra_argument = (uint64_t)&cpus[i];
        __atomic_thread_fence(__ATOMIC_RELEASE);
        r->cpus[i]->goto_address = ap_entry;
    }

    // Wait (bounded) for the APs to report ready.
    uint64_t deadline = ktime_ms() + 1000;
    for (uint64_t i = 0; i < ncpus; i++) {
        while (!__atomic_load_n(&cpus[i].ready, __ATOMIC_ACQUIRE)) {
            if (ktime_ms() > deadline) {
                break;
            }
            __builtin_ia32_pause();
        }
    }

    uint64_t online = 0;
    for (uint64_t i = 0; i < ncpus; i++) {
        online += cpus[i].ready ? 1 : 0;
    }
    console_print("juampiOS: SMP: ");
    console_dec(online);
    console_print(" of ");
    console_dec(ncpus);
    console_print(" cores online\n");
}

void smp_run_on(uint32_t index, void (*fn)(void* arg), void* arg)
{
    cpu* c = &cpus[index];
    c->arg = arg;
    c->fn = fn;
    __atomic_store_n(&c->mbox, CPU_MBOX_PENDING, __ATOMIC_RELEASE);
}

void smp_join(uint32_t index)
{
    cpu* c = &cpus[index];
    while (__atomic_load_n(&c->mbox, __ATOMIC_ACQUIRE) != CPU_MBOX_DONE) {
        __builtin_ia32_pause();
    }
    __atomic_store_n(&c->mbox, CPU_MBOX_IDLE, __ATOMIC_RELAXED);
}
