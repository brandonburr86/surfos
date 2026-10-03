/*
SurfOS 3c905B NIC Driver
--------------------
File: 3c905b.c  Date: 7/2/04
--------------------
(C)2004 Martin McCormick
*/

#include <surfos/types.h>
#include <blibc_common.h>
#include <surfos/console.h>

#include <net/ethernet.h>
#include <net/wnet.h>

#include <sys/pci.h>

#define DEVICE_3C905B 0x9055
#define VENDOR_3C905B 0x10b7

struct iface * iface_array[32];
u_int iface_count=0;

struct iface *alphaSetup(struct pci_dev *pci);

int init_3c905b() {
    struct pci_dev *d = NULL;
    int j;

    while((d = pci_find_device(VENDOR_3C905B, DEVICE_3C905B, d)) != NULL && iface_count < 32) {
        iface_array[iface_count++] = alphaSetup(d);
        if(!iface_array[iface_count-1]) {
            kprintf("Setup failed.\n");
            iface_count--;
            return -1;
        }
        kprintf("+NET: 3Com 3C905B detected: MAC =");
        for(j=0;j<6;j++) kprintf("%c%02x", (j ? ':' : ' '), ((struct sAlphaCard*)(iface_array[iface_count-1]->driver_struct))->MacAddress[j]);
        kprintf("\n");
        iface_array[iface_count-1]->Setting(iface_array[iface_count-1], 1, 0); /*enable */
    }
    return iface_count ? 0 : 1;
}

/* driver table entry */
int net_3c905b_init(void) {
    return init_3c905b();
}

u_long net_3c905b_entropy() {
    return 0;
}
