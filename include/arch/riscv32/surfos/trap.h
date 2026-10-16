/*
SurfOS Trap Frames and Dispatch (riscv32)
----------------------
File: trap.h    Date: 10/15/26 (SurfCPU), the riscv32 face of include/surfos/trap.h
----------------------
Every trap (an exception, an interrupt, or ECALL, which is yield()) enters
arch/riscv32/traps.S in machine mode on the stack of whatever was running. The entry
code builds this frame on that stack, 16-byte aligned, and calls trap_dispatch(),
which returns the frame to resume: the same one, or another task's (the scheduler
switches tasks by returning a different frame, as on x86). The exit code reloads
mepc and mstatus, then every register, sp last, and executes MRET. A task that is not
running is exactly such a frame on its own stack.

Register xN is at offset 4*N (x0's slot holds nothing), then the CSRs.
*/

#ifndef _SURFOS_TRAP_H
#define _SURFOS_TRAP_H

#define TF_RA       4
#define TF_SP       8
#define TF_GP       12
#define TF_TP       16
#define TF_T0       20
#define TF_S0       32
#define TF_A0       40
#define TF_MEPC     128
#define TF_MSTATUS  132
#define TF_MCAUSE   136
#define TF_MTVAL    140
#define TF_SIZE     144         /* a multiple of 16: the psABI's stack alignment */

#ifndef __ASSEMBLER__

#include <surfos/types.h>
#include <riscv/csr.h>

struct trapframe {
    u_long zero;                            /* x0: nothing is saved here */
    u_long ra, sp, gp, tp;                  /* x1-x4; sp is the interrupted code's, above this frame */
    u_long t0, t1, t2;                      /* x5-x7 */
    u_long s0, s1;                          /* x8-x9 (s0 is the frame pointer) */
    u_long a0, a1, a2, a3, a4, a5, a6, a7;  /* x10-x17 */
    u_long s2, s3, s4, s5, s6, s7, s8, s9, s10, s11;  /* x18-x27 */
    u_long t3, t4, t5, t6;                  /* x28-x31 */
    u_long mepc;                            /* where it resumes */
    u_long mstatus;                         /* MPIE: the interrupted code's MIE; MPP: always M */
    u_long mcause;
    u_long mtval;
};

/* register N of a frame, for dumps and tests */
#define TF_REG(tf, n) (((u_long *)(tf))[n])

/*
    The dispatch table is indexed by exception cause (mcause with the interrupt bit
    clear, 0-15). Interrupts are not in it: trap_dispatch() sends the machine timer to
    timer_tick() and the external interrupt to the board's interrupt controller.
    yield() is ECALL from M mode; trap_dispatch() steps mepc past the ECALL before its
    handler runs, so the task resumes after it, as after int $0x40 on x86.
*/
#define T_MISALIGNED_FETCH  CAUSE_MISALIGNED_FETCH
#define T_FETCH_ACCESS      CAUSE_FETCH_ACCESS
#define T_ILLOP             CAUSE_ILLEGAL_INSN
#define T_BREAKPOINT        CAUSE_BREAKPOINT
#define T_MISALIGNED_LOAD   CAUSE_MISALIGNED_LOAD
#define T_LOAD_ACCESS       CAUSE_LOAD_ACCESS
#define T_MISALIGNED_STORE  CAUSE_MISALIGNED_STORE
#define T_STORE_ACCESS      CAUSE_STORE_ACCESS
#define T_YIELD             CAUSE_ECALL_M       /* yield(): ecall */

#define NR_VECTORS   16

/* a handler receives the frame and returns the frame to resume (normally the same) */
typedef u_long *(*trap_handler_t)(struct trapframe *tf);

void trap_set_handler(u_int vector, trap_handler_t handler);
u_long *trap_dispatch(struct trapframe *tf);     /* called from traps.S */
const char *trap_name(u_long mcause);
void trap_init(void);                            /* mtvec, the exception handlers */
extern u_long trap_counts[NR_VECTORS];           /* exceptions taken, by cause */

/* the first frame of a new kernel task, at the top of its stack (kernel/task.c); ring must be
   KERNEL: machine mode has no user mode to start a task in */
struct trapframe *arch_task_frame(u_char *stack_top, u_int ring, u_long pc);

extern char trap_entry[];                        /* arch/riscv32/traps.S */
extern volatile u_long trap_depth;               /* traps being handled now (2: a fault inside a trap) */

#endif /* !__ASSEMBLER__ */
#endif
