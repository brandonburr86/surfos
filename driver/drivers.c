/*
SurfOS Driver Initialization
--------------------
File: drivers.c Date: 4/23/04, driver table 10/2026 (roadmap D1)
--------------------
(C)2004 Brandon Burr

10/5/26 (USB boot): with profile=laptop the ISA devices a modern laptop does not have
(parallel port, 8237 DMA, floppy, legacy IDE ports) are skipped rather than probed, and
`drivers` also says what the console, the timer and the keyboard ended up being.
*/
//Rebuilt 9/28/2026 - not in the printout, see REBUILD-NOTES.md

#include <surfos/types.h>
#include <surfos/console.h>
#include <surfos/multiboot.h>
#include <surfos/timer.h>
#include <surfos/keyboard.h>
#include <surfos/apic.h>
#include <sys/driver.h>
#include <sys/dma.h>
#include <sys/floppy.h>
#include <sys/parport.h>
#include <sys/pci.h>
#include <sys/serial.h>
#include <sys/bdev.h>
#include <blibc_common.h>

extern int net_3c905b_init(void);
int init_e1000(void);

static int drv_serial(void) { return init_serial_irq(); }
static int drv_parport(void) { init_parport(); return PAR0 ? 0 : 1; }
static int drv_dma(void) { init_dma(); return 0; }
static int drv_floppy(void) { init_floppy(); return 0; }
static int drv_pci(void) { return init_pci(); }

/* legacy: skipped with profile=laptop (there is nothing at those ports on such a machine,
   and the BIOS data area the parallel port reads is not filled in by UEFI firmware) */
#define LEGACY 1

static struct driver drivers[] = {
    { "serial",  drv_serial, 0, 0 },          /* serial input; needs the task system, hence not in init_serial() */
    { "parport", drv_parport, 0, LEGACY },
    { "dma",     drv_dma, 0, LEGACY },        /* the floppy driver needs DMA, so do it first */
    { "floppy",  drv_floppy, 0, LEGACY },
    { "pci",     drv_pci, 0, 0 },
    { "3c905b",  net_3c905b_init, 0, 0 },     /* no point looking for a network card without PCI devices: it checks */
    { "ramdisk", init_ramdisk, 0, 0 },        /* block devices: boot modules first, so they are rd0.. in module order */
    { "ata",     init_ata, 0, LEGACY },
    { "e1000",   init_e1000, 0, 0 },          /* QEMU's default NIC */
};
#define NDRIVERS (sizeof(drivers) / sizeof(drivers[0]))

static const char *status_name(int status) {
    if(status == DRV_SKIPPED) return "skipped";
    return status == 0 ? "ok" : (status > 0 ? "not present" : "failed");
}

void init_drivers() {
    u_int i;
    for(i = 0; i < NDRIVERS; i++) {
        if(bootinfo.laptop && (drivers[i].flags & LEGACY)) drivers[i].status = DRV_SKIPPED;
        else drivers[i].status = drivers[i].init();
    }
    kprintf("\nDrivers:");
    for(i = 0; i < NDRIVERS; i++) {
        int s = drivers[i].status;
        kprintf(" %s%s", drivers[i].name, s == 0 ? "" : (s == DRV_SKIPPED ? "(skipped)" : (s > 0 ? "(absent)" : "(FAILED)")));
    }
    kprintf("\n");
}

void print_drivers() {
    char buf[160];
    u_int i;
    printf("    driver    status\n");
    for(i = 0; i < NDRIVERS; i++) {
        printf("    %-9s %s\n", drivers[i].name, status_name(drivers[i].status));
    }
    printf("    profile   %s\n", bootinfo.laptop ? "laptop (native hardware: legacy ISA devices skipped)" : "emulator");
    printf("    console   %s\n", console_describe(buf, sizeof(buf)));
    printf("    serial    %s\n", serial_present() ? "COM1 at 0x3f8" : "none");
    printf("    timer     %s\n", timer_describe(buf, sizeof(buf)));
    printf("    keyboard  %s\n", keyboard_describe(buf, sizeof(buf)));
    printf("    apic      %s\n", apic_describe(buf, sizeof(buf)));
}
