/*
SurfOS Timer functions (INT 0)
Copyright (C)2004 Brandon Burr
*/

#include <asm/io.h>
#include <surfos/timer.h>
#include <blibc_common.h>
#include <surfos/interrupt.h>
#include <surfos/console.h>
#include <surfos/task.h>

volatile u_long sysTick;

void init_timer() {
    u_long divisor = CPU_FREQ/HZ;
    kprintf("\nTimer Initialization\n");
    kprintf("*PIT channel 0: %i Hz\n", HZ);

    /* The 2004 code wrote the command byte to port 0x36 (arguments swapped), so the PIT
       stayed in whatever mode the BIOS left and only the divisor took effect. */
    outb(0x43, 0x36);               /* channel 0, lobyte/hibyte, mode 3 (square wave) */
    outb(0x40, divisor & 0xff);
    outb(0x40, divisor >> 8);

    sysTick=0;

    kprintf("*Enable IRQ0\n*DONE\n"); /* print first: the first tick hands the CPU to the shell */
    _enable_irq(IRQ_TIMER);
}

u_long getticks() {
    return sysTick;
}

/* Every tick: account time, then let the scheduler choose the frame to resume. */
u_long *timer_tick(struct trapframe *tf) {
    sysTick++;
    return schedule(tf);
}
