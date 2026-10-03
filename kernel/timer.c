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

/* PIT ticks (1.193 MHz) spent in delay_loop(n), timed on channel 2 (the speaker timer) in
   one-shot mode so no interrupt is needed; 0 if the loop outran the 54 ms the counter holds. */
#define PIT_HZ 1193182
static u_long pit2_time(u_long n) {
    u_long count;
    outb(0x61, (inb(0x61) & ~0x02) | 0x01);  /* gate 2 on, speaker off */
    outb(0x43, 0xB0);                        /* channel 2, lo/hi byte, mode 0: count down once */
    outb(0x42, 0xFF);
    outb(0x42, 0xFF);
    delay_loop(n);
    outb(0x43, 0x80);                        /* latch channel 2 */
    count = inb(0x42);
    count |= inb(0x42) << 8;
    if(inb(0x61) & 0x20) count = 0;          /* OUT2 went high: the count ran out */
    outb(0x61, inb(0x61) & ~0x03);
    return count ? 0xFFFF - count : 0;
}

/* Measure how many delay_loop() iterations make a millisecond. Runs before the tick is
   enabled, so udelay() works for every driver and nothing preempts the measurement. */
static void calibrate_delay(void) {
    u_long n = 1 << 10, ticks;
    for(;;) {
        ticks = pit2_time(n);
        if(!ticks) { ticks = 0xFFFF; break; }   /* cannot happen after the doubling below */
        if(ticks >= PIT_HZ / 100) break;        /* at least 10 ms measured: good enough */
        n <<= 1;
    }
    /* n * 1193.182 / ticks without 64-bit arithmetic */
    loops_per_ms = (n / ticks) * (PIT_HZ / 1000) + ((n % ticks) * (PIT_HZ / 1000)) / ticks;
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

    calibrate_delay();
    kprintf("*%lu delay loops per ms\n", loops_per_ms);
    kprintf("*Enable IRQ0\n*DONE\n");
    _enable_irq(IRQ_TIMER);                  /* from here on the scheduler runs */
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
