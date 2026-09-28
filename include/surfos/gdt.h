/*
SurfOS GDT Handler
----------------------
File: gdt.h Date: Prior to 4/23/04
----------------------
(C)2004 Brandon Burr
*/

#ifndef _SURFOS_GDT_H
#define _SURFOS_GDT_H

#include <surfos/types.h>

#define GDT_BASE 0x6000
#define GDT_BASE_LOW 0x6000
#define GDT_BASE_HIGH 0x0000

#define GDO 0xC

typedef enum present_tag { GDT_ABSENT=0, GDT_PRESENT } present_t;
typedef enum dpl_tag { GDT_RING0, GDT_RING1, GDT_RING2, GDT_RING3 } dpl_t;
typedef enum seg_desc_tag { SYSTEM, CODE_DATA } seg_desc_t;

typedef enum seg_tag {
    RES_1,
    TSS_16A,
    LDT,
    TSS_16B,
    CALL_GATE_16,
    TASK_GATE,
    INT_GATE_16,
    TRAP_GATE_16,
    RES_2,
    TSS_32A,
    RES_3,
    TSS_32B,
    CALL_GATE_32,
    RES_4,
    INT_GATE_32,
    TRAP_GATE_32
} seg_t;

typedef struct gdt_str_tag {
    u_short seg_limit_low;
    u_short base_low;
    u_char base_mid;
    seg_t seg_type:4;
    seg_desc_t seg_desc_type:1;
    dpl_t dpl:2;
    present_t present:1;
    u_char seg_limit_high:4;
    u_char gran_def_op:4;
    u_char base_high;
} gdt_st_t;

typedef struct gdtr_str_tag {
    u_short gdt_length;
    u_short gdt_base_low;
    u_short gdt_base_high;
} gdtr_st_t;

void init_gdt();
void make_gdt_entry(gdt_st_t*, int);
void load_gdtr();

#endif
