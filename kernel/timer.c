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

void timerISR();

void init_timer() {
  u_long hertz = CPU_FREQ/HZ;
    kprintf("\nTimer Initialization\n");
    kprintf("*Enable Timer\n");

    outb(0x36, 0x43);
  outb(0x40,hertz & 0xff);
  outb(0x40,hertz >> 8);

    sysTick=0;

    kprintf("*Load ISR for IRQ0\n");
    _set_idt_trap(32,(u_int*)timerISR);
    kprintf("*Enable IRQ0\n");
    _enable_irq(IRQ_TIMER);
}

u_long getticks() {
    u_long ticks=0;
    ticks=sysTick;
    return ticks;
}


inline u_long *timer_handler(u_long *esp) {
    sysTick++;
    return schedule(esp);
}
