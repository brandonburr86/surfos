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
#include <surfos/ktimer.h>

volatile u_long sysTick;
static u_long loops_per_ms;

/* a loop the compiler cannot shorten, used for the calibrated delays */
static void delay_loop(u_long n) {
    volatile u_long i;
    for(i = 0; i < n; i++) asm volatile("" : : : "memory");
}

/* Measure how many delay_loop() iterations fit in a few ticks. Needs the tick running. */
static void calibrate_delay(void) {
    u_long n = 1 << 12, t0, t1, dt;
    for(;;) {
        t0 = getticks();
        while(getticks() == t0);     /* start on a tick edge */
        t1 = getticks();
        delay_loop(n);
        dt = getticks() - t1;
        if(dt >= 4) break;
        n <<= 1;
    }
    loops_per_ms = n / (dt * (1000 / HZ));
    if(!loops_per_ms) loops_per_ms = 1;
}

void udelay(u_long us) {
    delay_loop(loops_per_ms * us / 1000);
}

void mdelay(u_long ms) {
    while(ms--) delay_loop(loops_per_ms);
}

u_long delay_loops_per_ms(void) {
    return loops_per_ms;
}

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

    kprintf("*Enable IRQ0\n");
    _enable_irq(IRQ_TIMER);
    calibrate_delay();
    kprintf("*%lu delay loops per ms\n*DONE\n", loops_per_ms);
}

u_long getticks() {
    return sysTick;
}

/* Every tick: time, timers, then let the scheduler choose the frame to resume. */
u_long *timer_tick(struct trapframe *tf) {
    sysTick++;
    ktimers_tick();
    if(sched_tick()) return schedule(tf);
    return (u_long*)tf;
}
