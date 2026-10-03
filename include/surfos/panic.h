/*
SurfOS Exception Prototypes
---------------------------
File: panic.h   Date: 4/23/04, rebuilt 10/2026 (roadmap K1)
---------------------------
(C)2004 Brandon Burr
*/

#ifndef _SURFOS_PANIC_H
#define _SURFOS_PANIC_H

#include <surfos/types.h>
#include <surfos/trap.h>

void init_exceptions(void);                      /* install the handlers for vectors 0-31 */
u_long *trap_unexpected(struct trapframe *tf);   /* a vector nobody claimed */
void dump_trapframe(struct trapframe *tf);
void panic(const char *msg);
void BUG(void);

#endif
