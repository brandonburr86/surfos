/*
SurfOS Main C Entry
--------------------
File: main.c    Date: Prior to 4/23/04
--------------------
(C)2004 Brandon Burr

10/5/26 (USB boot): two profiles. Under QEMU's loader or the GRUB ISO (the default,
"emulator") the boot is what it was. With profile=laptop on the command line (the USB
image's GRUB menu passes it) the kernel expects real UEFI hardware: it reads the ACPI
tables, routes the 8259s through the local APIC, sets up the 8042 and polls it, skips
the ISA devices such a machine does not have, mounts only the initrd, and makes sure
the timer really ticks. The screen is the boot loader's framebuffer when there is one.

10/9/26 (the development loop): the word autonet, which GRUB's development entry passes,
starts a thread that waits for a network link, gets an address and starts telnetd
(kernel/autonet.c).
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
#include <sys/framebuffer.h>
#include <surfos/multiboot.h>
#include <surfos/tty.h>
#include <fs/vfs.h>
#include <surfos/process.h>
#include <net/net.h>
#include <mm/paging.h>
#include <mm/pmm.h>
#include <surfos/cpu.h>
#include <surfos/acpi.h>
#include <surfos/apic.h>
#include <surfos/system.h>

void kmain(u_long magic, u_long addr);

/* Multiboot 2 is the UEFI path: the block is all the kernel knows about the firmware's
   memory. Without a valid block and its memory map no RAM is known to be free, so stop
   before pmm_init() writes anything (only a serial port can show this). */
static void require_memory_map(void) {
    if(bootinfo.protocol != 2 || (bootinfo.valid && bootinfo.nregions)) return;
    kprintf("\nBOOT PROBLEM: %s, so no memory is known to be free. SYSTEM HALTED\n",
            bootinfo.valid ? "the Multiboot 2 information has no memory map" : "the Multiboot 2 information was rejected");
    halt();
}

/* What the boot loader passed, checked now that there is a screen to say it on. */
static void check_boot(void) {
    char buf[160];
    if(bootinfo.error) kprintf("\nBOOT PROBLEM: the Multiboot 2 information was rejected (%s)\n", mb2_strerror(bootinfo.error));
    if(pmm_problem()) kprintf("\nBOOT PROBLEM: %s\n", pmm_problem());
    if(!bootinfo.laptop) return;
    if(!bootinfo.efi) kprintf("\nprofile=laptop without UEFI: fine for testing under QEMU, but this is not the USB boot\n");
    if(bootinfo.efi && bootinfo.efi_bs) {
        kprintf("\nBOOT PROBLEM: the boot loader left the firmware's boot services running\n");
    }
    if(bootinfo.efi && !framebuffer_active())
        kprintf("\nBOOT PROBLEM: no usable screen: %s (only a serial port can show this)\n", console_describe(buf, sizeof(buf)));
}

void kmain(u_long magic, u_long addr) {

    /* Begin C Kernel */
    init_serial(); //COM1 needs no memory, so the whole boot log reaches a terminal (if there is a COM1)
    kprintf(serial_present() ? "SurfOS: serial console on COM1 (115200 8N1)\n" : "SurfOS: no serial port\n");
    init_gdt(); //setup GDT
    mb_init(magic, addr); //copy the boot loader's memory map and modules while they are addressable
    require_memory_map(); //Multiboot 2 without a usable map: stop before any memory is written
    init_cpu(); //CPUID; 4 MB pages and write-combining for a framebuffer
    init_mem();
    init_console(); init_tty();
    console_replay_log(); //the lines above, which only the serial port and the kernel log saw
    kprintf("Booting SurfOS Kernel.....\n\n");
    mb_print();
    check_boot();
    run_memcheck(); //check for enough ram..
    init_himem(); //RAM above 4 GB, if any: the way a framebuffer up there is reached

    if(bootinfo.laptop) init_acpi(); //firmware tables: legacy devices, timers, interrupt controllers
    init_interrupt(); //interrupt subsystem
    init_console_grid(); //a framebuffer's whole text grid: its buffers come from the kernel heap, usable from here
    if(bootinfo.laptop) init_apic(); //8259s through the local APIC's LINT0, I/O APICs masked
    init_delay(); //udelay() for the drivers, calibrated on whatever clock counts
    init_task(); //setup multitasking

    init_keyboard(); //get keyboard ready

    init_drivers(); //some driver stuff :P

    init_net(); //the net thread (frames queue up until it runs)
    init_fs(); //mount the initrd and whatever the disks hold
    init_syscalls(); //int 0x80 for user programs
    init_autonet(); //10/9/26: with the word autonet, a thread brings up the network and telnetd (the development loop)

    vmm_unmap(0); //the BIOS data area has been read; from here on a NULL dereference faults

    init_timer(); //start timer and go!!
    timer_confirm(); //no tick means nothing would ever run: try the local APIC timer, or say so

    for(;;) { //the idle task: init (pid 1) reaps and restarts, this only waits for interrupts
        asm volatile("hlt");
    }
    return; /* End C Kernel */
}
