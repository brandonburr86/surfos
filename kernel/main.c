/*
SurfOS Main C Entry
--------------------
File: main.c    Date: Prior to 4/23/04
--------------------
(C)2004 Brandon Burr
*/

#include <surfos/console.h>
#include <blibc_common.h>
#include <surfos/kernel.h>
#include <surfos/panic.h>
#include <surfos/timer.h>
#include <surfos/keyboard.h>
#include <surfos/interrupt.h>
#include <surfos/gdt.h>
#include <mm/memory.h>
#include <surfos/task.h>

#include <sys/driver.h>
#include <sys/serial.h>
#include <surfos/multiboot.h>
#include <surfos/tty.h>
#include <fs/vfs.h>
#include <mm/paging.h>

void kmain(u_long magic, u_long addr);

void kmain(u_long magic, u_long addr) {

    /* Begin C Kernel */
    init_serial(); //COM1 needs no memory, so the whole boot log reaches a terminal
    kprintf("SurfOS: serial console on COM1 (115200 8N1)\n");
    init_gdt(); //setup GDT
    mb_init(magic, addr); //copy the boot loader's memory map and modules while they are addressable
    init_mem();
    init_console(); init_tty(); kprintf("Booting SurfOS Kernel.....\n\n");
    mb_print();
    run_memcheck(); //check for enough ram..

    init_interrupt(); //interrupt subsystem
    init_delay(); //udelay() for the drivers; the PIT needs no interrupts for this
    init_task(); //setup multitasking

    init_keyboard(); //get keyboard ready

    init_drivers(); //some driver stuff :P

    init_fs(); //mount the initrd and whatever the disks hold

    vmm_unmap(0); //the BIOS data area has been read; from here on a NULL dereference faults

    init_timer(); //start timer and go!!

    for(;;) { //the idle task: init (pid 1) reaps and restarts, this only waits for interrupts
        asm volatile("hlt");
    }
    return; /* End C Kernel */
}
