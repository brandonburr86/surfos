/*
SurfOS Exception Prototypes
---------------------------
File: panic.h   Date: 4/23/04, rebuilt 10/2026 (roadmap K1, K3)
---------------------------
(C)2004 Brandon Burr
*/

#ifndef _SURFOS_PANIC_H
#define _SURFOS_PANIC_H

#include <surfos/types.h>
#include <surfos/trap.h>

void init_exceptions(void);                      /* install the handlers for vectors 0-31 */
u_long *trap_unexpected(struct trapframe *tf);   /* a vector nobody claimed */
u_long *trap_fatal(struct trapframe *tf, const char *fmt, ...); /* kill the task, or wipe out */
void dump_trapframe(struct trapframe *tf);
void panic(const char *fmt, ...);                /* print, backtrace, halt */
void double_fault_task(void);                    /* 10/25/26 (L16): the double fault's task: report, halt */
extern struct trapframe *stack_low_frame;        /* 10/25/26 (L18): set by trap_dispatch() before it sends a stack
                                                    that is nearly full to the double fault's task */
void panic_at(const char *file, int line, const char *what);
void BUG(void);

#define BUG_ON(cond) do { if(cond) panic_at(__FILE__, __LINE__, "BUG_ON(" #cond ")"); } while(0)
#define ASSERT(cond) do { if(!(cond)) panic_at(__FILE__, __LINE__, "ASSERT(" #cond ")"); } while(0)

#endif
