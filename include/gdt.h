#ifndef __GDT_H
#define __GDT_H

#include <types.h>
#include <tss.h>

// Descriptor of a segment in the gdt
struct seg_desc {
    uint16 limit_l;
    uint16 base_l;
    uint8 base_m;
    uint8 type  : 4;
    uint8 s     : 1;
    uint8 dpl   : 2;
    uint8 p     : 1;
    uint8 limit_h   : 4;
    uint8 avl   : 1;
    uint8 l     : 1;
    uint8 db    : 1;
    uint8 g     : 1;
    uint8 base_h;
} __attribute__((__packed__,aligned(8)));
typedef struct seg_desc seg_desc;

// Attributes of a segment.
struct seg_flags {
    uint8 g     : 1;
    uint8 s     : 1;
    uint8 dpl   : 2;
    uint8 type  : 4;
    uint8 p : 1;
    uint8 db : 1;
    uint8 avl : 1;
} __attribute((__packed__));
typedef struct seg_flags seg_flags;

// Types of entry in the gdt.
enum seg_type {
    // NON SYSTEM
    DATA_R      =  0,
    DATA_RW     =  2,
    DATA_R_SD   =  4,
    DATA_RW_SD  =  6,
    CODE_E_NC   =  8,
    CODE_ER_NC  = 10,
    CODE_E_C    = 12,
    CODE_ER_C   = 14,
    // SYSTEM
    LDT     = 2,
    TASK_GATE   = 5,
    TSS_AVL     = 9,
    TSS_BUSY    =11,
    CALL_GATE   =12,
    INT_GATE    =14,
    TRAP_GATE   =15
};

// GDT descriptor. Only one is created, the value that we put in the GDTR
struct gdt_desc {
    uint16 gdt_limit;
    uint32 gdt_base;
} __attribute__((__packed__));
typedef struct gdt_desc gdt_desc;

// NUMBER OF ENTRIES IN THE GDT
#define GDT_COUNT 16

// Declarations. The actual things are in gdt.c
extern seg_desc gdt[];
extern gdt_desc GDT_DESC;

// Load a new entry in the gdt, given its base, limit and data.
extern void gdt_load_desc(uint, uint, uint, seg_flags);
// Installs the new GDT. It is in assembler, because it necessarily has to be done in assembler. It is in kernel.c
extern void gdt_flush(void);
// Initializes the new gdt
extern void gdt_init(void);

#define GDT_LOAD_DESC(i,base,limit,...) \
    gdt_load_desc(i,base,limit, \
                  (seg_flags){ .db = 1, .avl = 0, \
                               .p = 1, .g = 1, .s = 1, __VA_ARGS__ })

// Adds a gdt entry for the indicated TSS
// Returns the segment selector found
short gdt_add_tss(intptr tss_physical);
// Delete a tss entry
void gdt_remove_tss(short index);

#define CODE_SEGMENT_KERNEL 0x08
#define DATA_SEGMENT_KERNEL 0x10
#define CODE_SEGMENT_USER   (0x18+0x3)
#define DATA_SEGMENT_USER   (0x20+0x3)

#endif
