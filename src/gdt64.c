#include <gdt64.h>
#include <memory.h>

// GDT: null, kernel code/data, user code/data, and a 16-byte (two-slot) TSS
// descriptor. In long mode the code/data segment bases and limits are ignored;
// only the access bits (privilege, code/data, present, and the L=long bit on
// code) matter.
static uint64_t gdt[7];
static tss64 tss;
static struct {
    uint16_t limit;
    uint64_t base;
} __attribute__((__packed__)) gdtr;

#define KERNEL_STACK_SZ 0x4000

void tss_set_rsp0(uint64_t rsp0)
{
    tss.rsp0 = rsp0;
}

void gdt_init(void)
{
    gdt[0] = 0;
    gdt[1] = 0x00AF9A000000FFFFull; // kernel code: present, exec, ring 0, L=1
    gdt[2] = 0x00CF92000000FFFFull; // kernel data: present, writable, ring 0
    gdt[3] = 0x00AFFA000000FFFFull; // user code:   present, exec, ring 3, L=1
    gdt[4] = 0x00CFF2000000FFFFull; // user data:   present, writable, ring 3

    // 64-bit TSS system descriptor (occupies two GDT slots).
    uint64_t base = (uint64_t)&tss;
    uint32_t limit = sizeof(tss) - 1;
    uint64_t low = 0;
    low |= (uint64_t)(limit & 0xFFFF);
    low |= (base & 0xFFFFFF) << 16;
    low |= (uint64_t)0x89 << 40; // type: available 64-bit TSS, present
    low |= (uint64_t)((limit >> 16) & 0xF) << 48;
    low |= ((base >> 24) & 0xFF) << 56;
    gdt[5] = low;
    gdt[6] = (base >> 32) & 0xFFFFFFFF;

    // Kernel stack used on a ring-3 -> ring-0 privilege transition.
    tss.rsp0 = (uint64_t)kmalloc(KERNEL_STACK_SZ) + KERNEL_STACK_SZ;
    tss.iomap_base = sizeof(tss);

    gdtr.limit = sizeof(gdt) - 1;
    gdtr.base = (uint64_t)gdt;
    gdt_flush(&gdtr);
    tss_flush(GDT_TSS);
}
