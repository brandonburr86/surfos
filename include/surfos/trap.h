/*
SurfOS Trap Frames and Dispatch
----------------------
File: trap.h    Date: 10/3/26 (roadmap K1)
----------------------
*/

#ifndef _SURFOS_TRAP_H
#define _SURFOS_TRAP_H

#include <surfos/types.h>

/*
    The frame every interrupt, exception, IRQ and software trap builds on the
    stack (kernel/traps.asm). It is also the saved context of a task that is
    not running: the scheduler switches tasks by returning another frame's
    address from trap_dispatch(). Lowest address first.
*/
struct trapframe {
    u_long edi, esi, ebp, esp_at_push, ebx, edx, ecx, eax; /* pusha */
    u_long ds, es, fs, gs;                                 /* pushed by trap_common */
    u_long vector;                                         /* pushed by the vector stub */
    u_long errcode;                                        /* CPU error code, or 0 */
    u_long eip, cs, eflags;                                /* pushed by the CPU */
    u_long useresp, ss;                                    /* only when coming from ring 3 */
} __attribute__((packed));

/* size of the frame the CPU pops on iret back to ring 0 (no useresp/ss) */
#define TRAPFRAME_KERNEL_SIZE offsetof(struct trapframe, useresp)

#define T_DIVIDE      0
#define T_DEBUG       1
#define T_NMI         2
#define T_BREAKPOINT  3
#define T_OVERFLOW    4
#define T_BOUND       5
#define T_ILLOP       6
#define T_DEVICE      7
#define T_DBLFLT      8
#define T_TSS        10
#define T_SEGNP      11
#define T_STACK      12
#define T_GPFLT      13
#define T_PGFLT      14
#define T_FPERR      16
#define T_ALIGN      17
#define T_MCHK       18
#define T_SIMDERR    19

#define T_IRQ0       0x20   /* IRQ 0-15 live at 0x20-0x2F */
#define T_YIELD      0x40   /* yield(): int $0x40 */
#define T_SYSCALL    0x80   /* system calls (roadmap P1) */

#define NR_VECTORS   256

/* a handler receives the frame and returns the frame to resume (normally the same) */
typedef u_long *(*trap_handler_t)(struct trapframe *tf);

void trap_set_handler(u_int vector, trap_handler_t handler);
u_long *trap_dispatch(struct trapframe *tf);     /* called from traps.asm */
const char *trap_name(u_int vector);

extern const u_long isr_stub_table[NR_VECTORS];  /* kernel/traps.asm */

#endif
