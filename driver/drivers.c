/*
SurfOS Driver Initialization
--------------------
File: drivers.c Date: 4/23/04
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

extern void *net_3c905b_attach(void *p);

void init_drivers() {
    init_serial_irq(); //serial input (needs the task system, hence not in init_serial)
    init_parport();

    init_dma(); //the floppy driver needs DMA, so do it first
    init_floppy();

    if(init_pci()) { //no point looking for a network card without PCI devices
        net_3c905b_attach(0);
    }
}
