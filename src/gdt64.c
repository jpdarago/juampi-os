#include <gdt64.h>

// GDT: null, kernel code/data, user code/data, and a 16-byte (two-slot) TSS
// descriptor. In long mode the code/data segment bases and limits are ignored;
// only the access bits (privilege, code/data, present, and the L=long bit on
// code) matter.
static uint64_t gdt[7];
static tss64 tss;

#define KERNEL_STACK_SZ 0x4000

void tss_set_rsp0(uint64_t rsp0)
{
    tss.rsp0 = rsp0;
}

// Fill a 7-entry GDT `g` with the shared kernel/user code+data descriptors and
// a 64-bit TSS system descriptor (two slots) pointing at `t`. Shared by the BSP
// (gdt_init) and each AP (gdt_ap_load) so every core's GDT is identical bar its
// own TSS.
static void build_gdt(uint64_t* g, tss64* t)
{
    g[0] = 0;
    g[1] = 0x00AF9A000000FFFFull; // kernel code: present, exec, ring 0, L=1
    g[2] = 0x00CF92000000FFFFull; // kernel data: present, writable, ring 0
    g[3] = 0x00AFFA000000FFFFull; // user code:   present, exec, ring 3, L=1
    g[4] = 0x00CFF2000000FFFFull; // user data:   present, writable, ring 3

    // 64-bit TSS system descriptor (occupies two GDT slots).
    uint64_t base = (uint64_t)t;
    uint32_t limit = sizeof(*t) - 1;
    uint64_t low = 0;
    low |= (uint64_t)(limit & 0xFFFF);
    low |= (base & 0xFFFFFF) << 16;
    low |= (uint64_t)0x89 << 40; // type: available 64-bit TSS, present
    low |= (uint64_t)((limit >> 16) & 0xF) << 48;
    low |= ((base >> 24) & 0xFF) << 56;
    g[5] = low;
    g[6] = (base >> 32) & 0xFFFFFFFF;
}

// Load a GDT `g` (with its TSS `t` set to interrupt-stack `rsp0`) and the TSS
// on the current core, reloading the segment registers. Used to give each AP
// its own GDT/TSS; the local descriptor register is only read by lgdt/ltr here.
static void load_gdt(uint64_t* g, tss64* t, uint64_t rsp0)
{
    t->rsp0 = rsp0;
    t->iomap_base = sizeof(*t);
    struct {
        uint16_t limit;
        uint64_t base;
    } __attribute__((__packed__)) r = {.limit = 7 * sizeof(uint64_t) - 1,
                                       .base = (uint64_t)g};
    gdt_flush(&r);
    tss_flush(GDT_TSS);
}

void gdt_init(allocator* mem)
{
    build_gdt(gdt, &tss);
    // Kernel stack used on a ring-3 -> ring-0 privilege transition.
    uint64_t rsp0 =
            (uint64_t)new (mem, char, KERNEL_STACK_SZ) + KERNEL_STACK_SZ;
    load_gdt(gdt, &tss, rsp0);
}

void gdt_ap_load(uint64_t g[7], tss64* t, uint64_t rsp0)
{
    build_gdt(g, t);
    load_gdt(g, t, rsp0);
}
