/*
SurfOS Driver Initialization
--------------------
File: drivers.c Date: 4/23/04, driver table 10/2026 (roadmap D1)
--------------------
(C)2004 Brandon Burr
*/
//Rebuilt 9/28/2026 - not in the printout, see REBUILD-NOTES.md

#include <surfos/types.h>
#include <surfos/console.h>
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

static int drv_serial(void) { init_serial_irq(); return 0; }
static int drv_parport(void) { init_parport(); return PAR0 ? 0 : 1; }
static int drv_dma(void) { init_dma(); return 0; }
static int drv_floppy(void) { init_floppy(); return 0; }
static int drv_pci(void) { return init_pci(); }

static struct driver drivers[] = {
    { "serial",  drv_serial, 0 },     /* serial input; needs the task system, hence not in init_serial() */
    { "parport", drv_parport, 0 },
    { "dma",     drv_dma, 0 },        /* the floppy driver needs DMA, so do it first */
    { "floppy",  drv_floppy, 0 },
    { "pci",     drv_pci, 0 },
    { "3c905b",  net_3c905b_init, 0 }, /* no point looking for a network card without PCI devices: it checks */
    { "ramdisk", init_ramdisk, 0 },    /* block devices: boot modules first, so they are rd0.. in module order */
    { "ata",     init_ata, 0 },
    { "e1000",   init_e1000, 0 },      /* QEMU's default NIC */
};
#define NDRIVERS (sizeof(drivers) / sizeof(drivers[0]))

void init_drivers() {
    u_int i;
    for(i = 0; i < NDRIVERS; i++) {
        drivers[i].status = drivers[i].init();
    }
    kprintf("\nDrivers:");
    for(i = 0; i < NDRIVERS; i++) {
        kprintf(" %s%s", drivers[i].name, drivers[i].status == 0 ? "" : (drivers[i].status > 0 ? "(absent)" : "(FAILED)"));
    }
    kprintf("\n");
}

void print_drivers() {
    u_int i;
    printf("    driver    status\n");
    for(i = 0; i < NDRIVERS; i++) {
        printf("    %-9s %s\n", drivers[i].name, drivers[i].status == 0 ? "ok" : (drivers[i].status > 0 ? "not present" : "failed"));
    }
}
