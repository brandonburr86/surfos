/*
    SurfOS System Calls -- (C)2004 Brandon Burr
*/

#include <asm/io.h>
#include <blibc_common.h>
#include <surfos/types.h>
#include <surfos/info.h>
#include <surfos/timer.h>
#include <surfos/keyboard.h>
#include <surfos/irq.h>
#include <surfos/console.h>
#include <surfos/task.h>

/* pulse the reset line through the keyboard controller */
void reset() {
    irq_disable();
    while(inb(0x64) & 0x02);
    outb(0x64, 0xFE);
    for(;;) asm volatile("hlt");
}

void do_banner(void) {
    cputs(LRED_TXT,"\n\nPhrite's SurfOS\n");
    cputs(LRED_TXT,"Kernel version ");
    cputs(ORANGE_TXT,"0.007");
    cputs(LRED_TXT," on an x86");
    printf("\n");
}

/* milliseconds, despite the 2004 parameter name; a task sleeps, boot code and ISRs spin */
void sleep(u_long usec) {
  sleep_ms(usec);
}

void halt() {
    irq_disable();
    for(;;) asm volatile("hlt");
}

void sysbeep(u_long frequency, u_long duration) {
  u_long tmp;
  if(frequency>5000) return;
  outb(0x43,0xb6); //init speaker
  tmp = 0x120000L / frequency;
  outb(0x42,tmp);
  outb(0x42,(tmp>>8));
  tmp=inb(0x61);
  tmp |= 3;
  outb(0x61,tmp);
  sleep(duration);
  tmp=inb(0x61);
  tmp &= 0xfc;
  outb(0x61,tmp);
}


void reboot() {
  kprintf("\n\nSurfOS System Reboot Sequence");
  setLED(NUM_LED,1);
  setLED(CAPS_LED,0);
  setLED(SCROLL_LED,0);
  kprintf("."); sleep(333);
  setLED(NUM_LED,0);
  setLED(CAPS_LED,1);
  setLED(SCROLL_LED,0);
  kprintf("."); sleep(333);
  setLED(NUM_LED,0);
  setLED(CAPS_LED,0);
  setLED(SCROLL_LED,1);
  kprintf("."); sleep(333);
  reset(); //pulse reset line
  halt();
}
