/*
SurfOS autonet: the network and the telnet server at boot
--------------------
File: autonet.c Date: 10/9/26
--------------------
The kernel word autonet, for the development loop (docs/USB-BOOT.md section 13). GRUB's
development entry passes it with a build from the data stick, so that the integrator can
reach the laptop over the network with nobody at its keyboard. Nothing else passes it:
without the word nothing here runs, and telnetd never starts by itself.

A kernel thread on F1 waits up to 90 seconds for a network interface with a link (a USB
adapter is set up by the usb thread a few seconds into the boot), asks for an address by
DHCP (three tries), then starts /initrd/bin/telnetd on as a process on F1, where its line
"telnetd: listening on ... port 23" shows on the laptop's screen. Every step is a
"+AUTONET:" line in the kernel log (dmesg) and on F1. Without a link or a lease it says
so and stops; dhcp and telnetd on still work by hand. It mounts nothing: the data stick
stays unmounted and the boot drive read-only.

10/17/26 (the network manager, kernel/netman.c, which may join a saved Wi-Fi network at the
same time): the Ethernet links that come up in the 90 seconds get their address here, one
after the other until one has it (the routes prefer Ethernet, and tools/devloop.py telnets to
the Ethernet address); a Wi-Fi address is the network manager's business, not autonet's. telnetd starts once there is an
address on a network it may start for by itself (netman_trusted(): Ethernet, or Wi-Fi joined
to a WPA2 network; never an open one, which anyone nearby may join), on port 23 whatever the
settings say. Started for Wi-Fi first, autonet still waits out the 90 seconds for Ethernet.
While this thread runs, the manager leaves Ethernet to it (autonet_busy()), and its Wi-Fi
keeper joins no open network by itself, since telnetd is to start.
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
#define AUTONET_IFS        8            /* 10/17/26: Ethernet interfaces it tried */

static volatile bool running;           /* 10/17/26: autonet_busy() */

bool autonet_busy(void) {
    return running;
}

/* 10/17/26: an Ethernet interface whose link is up and which autonet has not tried yet (the list only grows: an
   unplugged adapter stays); tried[] notes it */
static struct netdev *new_link(struct netdev **tried, int *ntried) {
    struct netdev *d;
    int i;
    for(d = netdev_first(); d; d = d->next) {
        if(strncmp(d->name, "eth", 3) || !d->link_up) continue;
        for(i = 0; i < *ntried && tried[i] != d; i++);
        if(i < *ntried || *ntried >= AUTONET_IFS) continue;
        tried[(*ntried)++] = d;
        return d;
    }
    return NULL;
}

/* 10/17/26: an address telnetd may start for: Ethernet first, then Wi-Fi on a WPA2 network */
static struct netdev *trusted(void) {
    struct netdev *d;
    for(d = netdev_first(); d; d = d->next) if(!strncmp(d->name, "eth", 3) && netman_trusted(d)) return d;
    for(d = netdev_first(); d; d = d->next) if(netman_trusted(d)) return d;
    return NULL;
}

/* telnetd on, for dev's address */
static void start_telnetd(struct netdev *dev) {
    char a[16];
    int r;
    kprintf("+AUTONET: %s: %s; starting telnetd on F1\n", dev->name, ipfmt(dev->ip, a));
    r = netman_start_telnetd(0);
    if(r == 0) kprintf("+AUTONET: done: telnet %s\n", ipfmt(dev->ip, a));
    else if(r > 0) kprintf("+AUTONET: telnetd on ended with status %i\n", r);
}

static void autonet_main(void *arg) {
    struct netdev *tried[AUTONET_IFS], *dev, *tel = NULL, *leased = NULL;
    u_long start = uptime_ms(), took;
    char a[16];
    int ntried = 0, i;

    kprintf("+AUTONET: waiting up to %i s for a network link\n", AUTONET_LINK_MS / 1000);
    while(uptime_ms() - start < AUTONET_LINK_MS && !(leased && tel)) {
        while(!leased && (dev = new_link(tried, &ntried)) != NULL) {
            took = uptime_ms() - start;
            kprintf("+AUTONET: %s: link up after %lu.%lu s; asking for an address (DHCP)\n", dev->name, took / 1000,
                    (took % 1000) / 100);
            for(i = 0; i < AUTONET_DHCP_TRIES && !dev->ip; i++) {
                if(i) sleep_ms(AUTONET_DHCP_PAUSE);
                if(dhcp_configure(dev, AUTONET_DHCP_MS) == 0) break;
            }
            if(dev->ip) leased = dev;
            else kprintf("+AUTONET: %s: no DHCP lease after %i tries (dhcp, then telnetd on)\n", dev->name,
                         AUTONET_DHCP_TRIES);
        }
        if(!tel && (tel = trusted()) != NULL) start_telnetd(tel);
        else if(tel && leased && leased != tel)         /* telnetd runs for Wi-Fi already: it answers here too */
            kprintf("+AUTONET: %s: %s as well: telnet %s\n", leased->name, ipfmt(leased->ip, a), ipfmt(leased->ip, a));
        if(!(leased && tel)) sleep_ms(AUTONET_POLL_MS);
    }
    if(tel) return;
    if(!ntried)
        kprintf("+AUTONET: no network link within %i s: telnetd not started (plug in the Ethernet adapter and "
                "its cable, then type dhcp and telnetd on)\n", AUTONET_LINK_MS / 1000);
    else
        kprintf("+AUTONET: no address on Ethernet or on a WPA2 Wi-Fi network within %i s: telnetd not started "
                "(dhcp, then telnetd on)\n", AUTONET_LINK_MS / 1000);
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
