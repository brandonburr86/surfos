/*
    SurfOS System Calls -- (C)2004 Brandon Burr

    10/6/26: the banner says SurfOS 1.0, 2004-2026 (surfos/version.h), in its old colours.
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
#include <surfos/acpi.h>
#include <surfos/version.h>
#include <mm/paging.h>

/* Reset the machine. 2004: pulse the reset line through the keyboard controller. Since
   10/5/26 (USB boot) that is the third try: first the reset register the ACPI tables name
   (laptops), then the reset control register at 0xCF9 (Intel chipsets, QEMU), and if the
   machine is still running, a triple fault. Every wait is bounded. */
void reset() {
    u_long i;
    irq_disable();
    if(acpi.reset_ok && acpi.reset_reg.space == 1) {
        outb((u_short)acpi.reset_reg.addr, acpi.reset_value);
        mdelay(100);
    } else if(acpi.reset_ok && acpi.reset_reg.space == 0 && acpi.reset_reg.addr < 0x100000000ULL) {
        volatile u_char *r = (volatile u_char *)ioremap((u_long)acpi.reset_reg.addr, 1, false);
        if(r) *r = acpi.reset_value;
        mdelay(100);
    }
    outb(0xCF9, 0x02);
    udelay(50);
    outb(0xCF9, 0x06);
    mdelay(100);
    for(i = 0; i < 100000 && (inb(0x64) & 0x02); i++);
    outb(0x64, 0xFE);
    mdelay(100);
    {   /* an IDT with no entries: the next interrupt is a triple fault, which resets */
        struct { u_short limit; u_long base; } __attribute__((packed)) none = { 0, 0 };
        asm volatile("lidt %0\n\tint3" : : "m"(none));
    }
    for(;;) asm volatile("hlt");
}

void do_banner(void) {
    cputs(LRED_TXT,"\n\n" SURFOS_AUTHOR "'s " SURFOS_NAME "\n");
    cputs(LRED_TXT,"Version ");
    cputs(ORANGE_TXT,SURFOS_VERSION);
    cputs(LRED_TXT," on an x86, ");
    cputs(ORANGE_TXT,SURFOS_YEARS);
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
