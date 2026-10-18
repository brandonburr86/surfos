/*
SurfOS autonet: the network and the telnet server at boot
--------------------
File: autonet.c Date: 10/9/26
--------------------
The kernel word autonet, for the development loop (docs/USB-BOOT.md section 13). GRUB's
development entry ("from the data stick", or "from this stick" on one stick) passes it with a
build from the stick, so that the integrator can reach the laptop over the network with nobody
at its keyboard. Nothing else passes it: without the word nothing here runs.

A kernel thread on F1 waits up to 90 seconds for a network interface with a link (a USB
adapter is set up by the usb thread a few seconds into the boot), asks for an address by
DHCP (three tries), then starts /initrd/bin/telnetd on as a process on F1, where its line
"telnetd: listening on ... port 23" shows on the laptop's screen. Every step is a
"+AUTONET:" line in the kernel log (dmesg) and on F1. Without a link or a lease it says
so and stops; dhcp and telnetd on still work by hand. It mounts nothing: the data stick
stays unmounted and the boot drive read-only.

10/17/26 (the network manager, kernel/netman.c, which may join a saved Wi-Fi network at the
same time): only an Ethernet link counts, as it did in practice before (the routes prefer
Ethernet, and tools/devloop.py telnets to the Ethernet address). A Wi-Fi address is the
manager's business, and telnetd for it its setting telnetd = on, which the manager acts on
once this thread is done; so the development entry starts telnetd only for an Ethernet link,
never because of a Wi-Fi network SurfOS joined by itself. telnetd is started through
netman_start_telnetd(), on port 23. While this thread runs, the manager leaves Ethernet's DHCP
to it, and its Wi-Fi keeper joins no open network by itself (autonet_busy(): telnetd is due).
*/

#include <surfos/types.h>
#include <surfos/console.h>
#include <surfos/kernel.h>
#include <surfos/task.h>
#include <surfos/ktimer.h>
#include <surfos/multiboot.h>
#include <surfos/process.h>
#include <net/net.h>
#include <net/dhcp.h>
#include <net/netman.h>
#include <fs/vfs.h>
#include <blibc_common.h>

#define AUTONET_LINK_MS    90000        /* how long a link may take to come */
#define AUTONET_POLL_MS    250
#define AUTONET_DHCP_TRIES 3
#define AUTONET_DHCP_MS    8000         /* each try, as the dhcp command waits */
#define AUTONET_DHCP_PAUSE 2000

static volatile bool running;           /* 10/17/26: autonet_busy() */

bool autonet_busy(void) {
    return running;
}

/* the first Ethernet interface with a link (the list only grows: an unplugged adapter stays). 10/17/26: Ethernet only
   (a wlan0 the network manager joined is not autonet's), looked for in the whole list */
static struct netdev *linked(void) {
    struct netdev *d;
    for(d = netdev_first(); d; d = d->next) if(d->link_up && !strncmp(d->name, "eth", 3)) return d;
    return NULL;
}

static void autonet_main(void *arg) {
    u_long start = uptime_ms(), took;
    struct netdev *dev;
    char a[16];
    int i;

    kprintf("+AUTONET: waiting up to %i s for a network link\n", AUTONET_LINK_MS / 1000);
    while(!(dev = linked()) && uptime_ms() - start < AUTONET_LINK_MS) sleep_ms(AUTONET_POLL_MS);
    if(!dev) {
        kprintf("+AUTONET: no network link within %i s: telnetd not started (plug in the Ethernet adapter and "
                "its cable, then type dhcp and telnetd on)\n", AUTONET_LINK_MS / 1000);
        return;
    }
    took = uptime_ms() - start;
    kprintf("+AUTONET: %s: link up after %lu.%lu s; asking for an address (DHCP)\n", dev->name, took / 1000,
            (took % 1000) / 100);
    for(i = 0; i < AUTONET_DHCP_TRIES; i++) {
        if(i) sleep_ms(AUTONET_DHCP_PAUSE);
        if(dhcp_configure(dev, AUTONET_DHCP_MS) == 0) break;
    }
    if(i == AUTONET_DHCP_TRIES) {
        kprintf("+AUTONET: %s: no DHCP lease after %i tries: telnetd not started (dhcp, then telnetd on)\n",
                dev->name, AUTONET_DHCP_TRIES);
        return;
    }
    kprintf("+AUTONET: %s: %s; starting telnetd on F1\n", dev->name, ipfmt(dev->ip, a));
    i = netman_start_telnetd(0);
    if(i == 0) kprintf("+AUTONET: done: telnet %s\n", ipfmt(dev->ip, a));
    else if(i > 0) kprintf("+AUTONET: telnetd on ended with status %i\n", i);
}

/* 10/17/26: the thread's end, however it ends: the manager takes Ethernet's DHCP over */
static void autonet_thread(void *arg) {
    autonet_main(arg);
    running = false;
}

/* the boot: a thread for the word autonet, which waits on its own while the boot goes on */
void init_autonet(void) {
    if(!bootinfo_has_arg(&bootinfo, "autonet")) return;
    running = true;
    if(!kthread_create_on("autonet", &conArray[0], autonet_thread, NULL, PL_NORMAL, TF_DETACHED)) {
        running = false;
        kprintf("+AUTONET: no memory for its thread: type dhcp and telnetd on\n");
    }
}
