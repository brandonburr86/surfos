/*
SurfOS Floating Point for Programs
----------------------
File: fpu.h     Date: 10/25/26 (robustness, track B: L10-L13)
----------------------
Each process has floating-point registers of its own, switched lazily (kernel/fpu.c).
The kernel itself never uses the FPU (-mno-80387 -mno-sse: a float in kernel code
fails to link), and kernel threads have no registers for it.
*/

#ifndef _SURFOS_FPU_H
#define _SURFOS_FPU_H

#include <surfos/types.h>

struct surf_task;

#define FPU_AREA 512            /* fxsave's area (fnsave uses 108 bytes of it); kalloc() aligns it to 16 */

void init_fpu(void);                            /* the device-not-available trap (#NM); from init_task() */
int fpu_task_new(struct surf_task *t);          /* a process's area, holding what a program starts with: 0, -ENOMEM */
void fpu_task_gone(struct surf_task *t);        /* become_zombie(): the FPU no longer holds its registers (irqs off) */
void fpu_task_free(struct surf_task *t);        /* reap(): its area */
void fpu_switch(struct surf_task *next);        /* schedule(): CR0.TS set unless next's registers are in the FPU */
u_long fpu_saves(void);                         /* how many times a task's registers were saved for another's */

#endif
