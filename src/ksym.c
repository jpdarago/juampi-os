#include <ksym.h>
#include <console.h>

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

// Minimal ELF64 section-header and symbol structures (enough to reach .symtab).
typedef struct {
    uint8_t e_ident[16];
    uint16_t e_type, e_machine;
    uint32_t e_version;
    uint64_t e_entry, e_phoff, e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize, e_phentsize, e_phnum;
    uint16_t e_shentsize, e_shnum, e_shstrndx;
} __attribute__((packed)) Elf64_Ehdr;

typedef struct {
    uint32_t sh_name, sh_type;
    uint64_t sh_flags, sh_addr, sh_offset, sh_size;
    uint32_t sh_link, sh_info;
    uint64_t sh_addralign, sh_entsize;
} __attribute__((packed)) Elf64_Shdr;

typedef struct {
    uint32_t st_name;
    uint8_t st_info, st_other;
    uint16_t st_shndx;
    uint64_t st_value, st_size;
} __attribute__((packed)) Elf64_Sym;

#define SHT_SYMTAB 2
#define STT_FUNC 2

static const Elf64_Sym* g_syms;
static uint64_t g_nsyms;
static const char* g_strs;

void ksym_init(void* elf)
{
    if (elf == NULL) {
        return;
    }
    const uint8_t* base = elf;
    const Elf64_Ehdr* eh = elf;
    if (base[0] != 0x7F || base[1] != 'E' || base[2] != 'L' || base[3] != 'F') {
        return;
    }
    const Elf64_Shdr* sh = (const Elf64_Shdr*)(base + eh->e_shoff);
    for (uint16_t i = 0; i < eh->e_shnum; i++) {
        if (sh[i].sh_type == SHT_SYMTAB) {
            g_syms = (const Elf64_Sym*)(base + sh[i].sh_offset);
            g_nsyms = sh[i].sh_size / sizeof(Elf64_Sym);
            g_strs = (const char*)(base + sh[sh[i].sh_link].sh_offset);
            return;
        }
    }
}

const char* ksym_lookup(uint64_t addr, uint64_t* offset)
{
    // The function symbol with the greatest value <= addr (that contains addr,
    // when a size is present).
    const Elf64_Sym* best = NULL;
    for (uint64_t i = 0; i < g_nsyms; i++) {
        const Elf64_Sym* s = &g_syms[i];
        if ((s->st_info & 0xF) != STT_FUNC || s->st_value == 0) {
            continue;
        }
        if (s->st_value <= addr &&
            (best == NULL || s->st_value > best->st_value)) {
            best = s;
        }
    }
    if (best == NULL) {
        return NULL;
    }
    if (best->st_size != 0 && addr >= best->st_value + best->st_size) {
        return NULL; // past the end of that function
    }
    if (offset) {
        *offset = addr - best->st_value;
    }
    return g_strs + best->st_name;
}

// Only frames whose return address falls in the higher-half kernel image are
// plausible; stop the walk otherwise.
static bool plausible_code(uint64_t addr)
{
    return addr >= 0xffffffff80000000ull;
}

void backtrace_from(uint64_t rip, uint64_t rbp)
{
    console_print("backtrace:\n");
    for (int depth = 0; depth < 32; depth++) {
        uint64_t off = 0;
        const char* name = ksym_lookup(rip, &off);
        console_print("  ");
        console_hex(rip);
        if (name) {
            console_print(" ");
            console_print(name);
            console_print("+");
            console_hex(off);
        }
        console_print("\n");

        if (rbp < 0xffffff0000000000ull) {
            break; // rbp no longer looks like a kernel stack address
        }
        uint64_t next_rbp = ((uint64_t*)rbp)[0];
        uint64_t ret = ((uint64_t*)rbp)[1];
        if (next_rbp <= rbp || !plausible_code(ret)) {
            break;
        }
        rip = ret;
        rbp = next_rbp;
    }
}

void backtrace(void)
{
    uint64_t rbp;
    __asm__ __volatile__("mov %%rbp, %0" : "=r"(rbp));
    // Start one frame up: report our caller, using this frame's saved return.
    uint64_t rip = ((uint64_t*)rbp)[1];
    backtrace_from(rip, ((uint64_t*)rbp)[0]);
}
