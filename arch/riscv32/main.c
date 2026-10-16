/*
SurfOS riscv32 Main C Entry
--------------------
File: main.c    Date: 10/15/26 (SurfCPU), after kernel/main.c (C)2004 Brandon Burr
--------------------
kmain() for a machine-mode RISC-V hart, in the order of the x86 kmain(): the console
UART first (it needs no memory), then the boot information (copied before anything
could overwrite it), the heaps, the consoles and ttys of kernel/console.c and
kernel/tty.c, interrupts, the scheduler of kernel/task.c, the drivers, the file
systems of fs/ (every FAT volume found on a block device is mounted), and the timer.
From then on this code is the idle task: init (pid 1) starts the kernel shell on every
console, and this only waits for interrupts.

What the x86 kmain() does that has no counterpart here: the GDT and TSS, Multiboot,
CPUID and paging, the PIC, the PIT's calibration, PCI, the network, system calls and
user processes (surfcpu/docs/PORTING-RISCV.md says what replaces what).
*/

#include <surfos/types.h>
#include <surfos/console.h>
#include <surfos/kernel.h>
#include <surfos/panic.h>
#include <surfos/timer.h>
#include <surfos/interrupt.h>
#include <surfos/task.h>
#include <surfos/tty.h>
#include <surfos/multiboot.h>
#include <surfos/system.h>
#include <surfos/cpu.h>
#include <sys/driver.h>
#include <sys/serial.h>
#include <sys/framebuffer.h>
#include <fs/vfs.h>
#include <mm/memory.h>
#include <riscv/csr.h>
#include <riscv/board.h>
#include <riscv/arch.h>
#include <blibc_common.h>

volatile u_long heartbeat_count;

/* a kernel task that sleeps and wakes forever: on the Arty it blinks LD4 (the board's
   heartbeat hook); everywhere its count shows that the scheduler and the sleep list work */
void board_heartbeat(u_long beat);

static void heartbeat(void *arg) {
    (void)arg;
    for(;;) {
        heartbeat_count++;
        board_heartbeat(heartbeat_count);
        sleep_ms(500);
    }
}

static void check_boot(void) {
    char buf[160];
    if(bootinfo_problem() && !strcmp(board_name(), "arty_a7"))
        kprintf("\nBOOT PROBLEM: %s; running with the board's defaults\n", bootinfo_problem());
    if(!framebuffer_active()) kprintf("screen: %s\n", console_describe(buf, sizeof(buf)));
}

void kmain(u_long hartid, u_long bootinfo_addr, u_long magic) {
    board_early_init();                     /* the console UART */
    init_serial();
    kprintf(serial_present() ? "SurfOS: serial console on the %s UART (115200 8N1)\n" : "SurfOS: no serial port%s\n",
            serial_present() ? board_name() : "");
    bootinfo_init(hartid, bootinfo_addr, magic);   /* SurfBoot's block, copied before scratch RAM is touched */
    init_cpu();
    init_mem();
    init_console(); init_tty();
    console_replay_log();                   /* the lines above, which only the serial port and the kernel log saw */
    kprintf("Booting SurfOS Kernel.....\n\n");
    check_boot();
    run_memcheck();

    init_interrupt();                       /* mtvec, the board's interrupt controller, MIE */
    init_console_grid();                    /* the display's whole grid (80x30 on the Arty) */
    init_delay();
    init_task();                            /* the idle task (this code) and init */
    kthread_create("heartbeat", heartbeat, NULL, PL_LOW, TF_DETACHED);

    init_drivers();                         /* serial input, then the board's: RAM disk, LEDs, keyboard, card */
    init_fs();                              /* every FAT volume on a block device is mounted */

    init_timer();                           /* start the tick, and go */
    timer_confirm();

    for(;;) rv_wfi();                       /* the idle task */
}
