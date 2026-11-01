/*
SurfOS riscv32: what the arch files share
----------------------
File: arch.h    Date: 10/15/26 (SurfCPU)
----------------------
*/

#ifndef _RISCV_ARCH_H
#define _RISCV_ARCH_H

#include <surfos/types.h>

#define STACK_CANARY 0x5354434B     /* "STCK": the bottom word of every kernel stack (trap.c) */

struct surf_boot_info;

void kmain(u_long hartid, u_long bootinfo_addr, u_long magic);   /* start.S */
extern u_char stack[];                  /* the boot stack, then the idle task's (start.S) */

/* bootinfo.c: SurfBoot's boot information, checked and copied (BOOT-ABI.md section 4) */
void bootinfo_init(u_long hartid, u_long addr, u_long magic);
const struct surf_boot_info *surfboot_info(void);   /* the kernel's copy, NULL without SurfBoot */
const char *bootinfo_problem(void);                 /* why SurfBoot's block was not taken, or NULL */
void bootinfo_print(void);

/* trap.c, irq.c */
void arch_clear_soft_irq(void);

/* serial.c (10/26/26): bytes received on the console's serial line since boot */
u_long serial_rx_count(void);

/* timer.c */
u64 timer_mtime_at_tick(void);              /* mtime when the current tick began */
u_long timer_period(void);                  /* mtime units per tick */

/* the heartbeat task (main.c): wakes every 500 ms; its count shows the scheduler working */
extern volatile u_long heartbeat_count;

/* the CRC of zlib (BOOT-ABI.md section 3) */
u32 crc32_update(u32 crc, const void *data, u_long len);
u32 crc32(const void *data, u_long len);

/* rvtest.c: the riscv32 self tests (registers across traps, stack alignment, tasks) */
int rv_selftest(void);
int rv_heaptest(void);
/* rvregs.S: a register pattern from seed, spins rounds (taking timer interrupts, or an ECALL
   each round), then every register into out[32] for rvtest.c to compare */
void rv_regs_spin(u_long seed, u_long spins, u_long *out);
void rv_regs_ecall(u_long seed, u_long spins, u_long *out);

#endif
