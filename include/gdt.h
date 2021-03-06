#ifndef __GDT_H
#define __GDT_H

#include <types.h>
#include <tss.h>

// Descriptor de un segmento en la gdt
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

// Atributos de un segmento.
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

// Tipos de entrada en la gdt.
enum seg_type {
    // NO SISTEMA
    DATA_R      =  0,
    DATA_RW     =  2,
    DATA_R_SD   =  4,
    DATA_RW_SD  =  6,
    CODE_E_NC   =  8,
    CODE_ER_NC  = 10,
    CODE_E_C    = 12,
    CODE_ER_C   = 14,
    // SISTEMA
    LDT     = 2,
    TASK_GATE   = 5,
    TSS_AVL     = 9,
    TSS_BUSY    =11,
    CALL_GATE   =12,
    INT_GATE    =14,
    TRAP_GATE   =15
};

// Descriptor de GDT. Solo se crea uno, el valor que ponemos en la GDTR
struct gdt_desc {
    uint16 gdt_limit;
    uint32 gdt_base;
} __attribute__((__packed__));
typedef struct gdt_desc gdt_desc;

// CANTIDAD DE ENTRADAS EN LA GDT
#define GDT_COUNT 16

// Declaraciones. Las cosas en si estan en gdt.c
extern seg_desc gdt[];
extern gdt_desc GDT_DESC;

// Cargar una nueva entrada en la gdt, dada su base, limite y datos.
extern void gdt_load_desc(uint, uint, uint, seg_flags);
// Instala la nueva GDT. Esta en assembler, porque si o si tiene que hacerse en assembler. Esta en kernel.c
extern void gdt_flush(void);
// Inicializa la nueva gdt
extern void gdt_init(void);

#define GDT_LOAD_DESC(i,base,limit,...) \
    gdt_load_desc(i,base,limit, \
                  (seg_flags){ .db = 1, .avl = 0, \
                               .p = 1, .g = 1, .s = 1, __VA_ARGS__ })

// Agrega una entrada de gdt para la TSS indicada
// Devuelve el selector de segmento encontrado
short gdt_add_tss(intptr tss_physical);
// Borrar una entrada de tss
void gdt_remove_tss(short index);

#define CODE_SEGMENT_KERNEL 0x08
#define DATA_SEGMENT_KERNEL 0x10
#define CODE_SEGMENT_USER   (0x18+0x3)
#define DATA_SEGMENT_USER   (0x20+0x3)

#endif
