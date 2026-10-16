/*
SurfOS riscv32 System Calls -- the banner, reboot, halt
--------------------
File: sys.c     Date: 10/15/26 (SurfCPU), after kernel/sys.c (C)2004 Brandon Burr
--------------------
The banner is the x86 kernel's, in its old colours, with the machine named. A reboot
is the board's (QEMU's reset register; on the Arty a jump back to SurfBoot at the reset
vector), halt() stops the hart with interrupts off, and there is no speaker to beep.
*/

#include <surfos/types.h>
#include <surfos/system.h>
#include <surfos/kernel.h>
#include <surfos/console.h>
#include <surfos/task.h>
#include <surfos/irq.h>
#include <surfos/version.h>
#include <riscv/csr.h>
#include <riscv/board.h>
#include <blibc_common.h>

void do_banner(void) {
    cputs(LRED_TXT, "\n\n" SURFOS_AUTHOR "'s " SURFOS_NAME "\n");
    cputs(LRED_TXT, "Version ");
    cputs(ORANGE_TXT, SURFOS_VERSION);
    cputs(LRED_TXT, " on a RISC-V (");
    cputs(ORANGE_TXT, (char *)board_machine());
    cputs(LRED_TXT, "), ");
    cputs(ORANGE_TXT, SURFOS_YEARS);
    printf("\n");
}

/* milliseconds, despite the 2004 parameter name; a task sleeps, boot code and ISRs spin */
void sleep(u_long usec) {
    sleep_ms(usec);
}

void halt(void) {
    irq_disable();
    for(;;) rv_wfi();
}

/* no PC speaker on either board */
void sysbeep(u_long frequency, u_long duration) {
    (void)frequency;
    (void)duration;
}

void reboot(void) {
    kprintf("\n\nSurfOS System Reboot Sequence");
    kprintf("."); sleep(333);
    kprintf("."); sleep(333);
    kprintf("."); sleep(333);
    kprintf("\n");
    board_reset();
}
