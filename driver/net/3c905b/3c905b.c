/*
SurfOS 3c905B NIC Driver
--------------------
File: 3c905b.c  Date: 7/2/04
--------------------
(C)2004 Martin McCormick
*/

#include <surfos/types.h>
#include <surfos/console.h>

#include <net/ethernet.h>
#include <net/wnet.h>

#include <sys/pci.h>

#define DEVICE_3C905B 0x9055
#define VENDOR_3C905B 0x10b7

struct pci_dev ** pci_array;
struct iface * iface_array[32];
u_int iface_count=0;

int init_3c905b() {
    int j,i=0;

   pci_array = pci;

    for(i=0;i<32;i++)  {
        if(pci_array[i]->vendor == VENDOR_3C905B && pci_array[i]->device == DEVICE_3C905B) {
            iface_array[iface_count++] = alphaSetup(pci_array[i]);
            if(!iface_array[iface_count-1]) {
                    kprintf("Setup failed.\n");
                    return;
         }
            kprintf("+NET: 3Com 3C905B detected: MAC =");
         for(j=0;j<6;j++) kprintf("%c%x", (j ? ':' : ' '), ((struct sAlphaCard*)(iface_array[iface_count-1]->driver_struct))->MacAddress[j]);
           kprintf("\n");
           iface_array[iface_count-1]->Setting(iface_array[iface_count-1], 1, 0); /*enable */
      }
    }
}

int net_3c905b_probe() {
}

void *net_3c905b_attach(void *p) {
    init_3c905b();
}

void *net_3c905b_detach(void *p) {
}

u_long net_3c905b_entropy() {
    return 0;
}
