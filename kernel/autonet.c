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
#include <fs/vfs.h>
#include <blibc_common.h>

#define AUTONET_LINK_MS    90000        /* how long a link may take to come */
#define AUTONET_POLL_MS    250
#define AUTONET_DHCP_TRIES 3
#define AUTONET_DHCP_MS    8000         /* each try, as the dhcp command waits */
#define AUTONET_DHCP_PAUSE 2000
#define AUTONET_TELNETD_MS 30000        /* telnetd on returns once its server runs (3 s at most) */
#define AUTONET_TELNETD    "/initrd/bin/telnetd"

/* the first interface with a link (the list only grows: an unplugged adapter stays) */
static struct netdev *linked(void) {
    struct netdev *d;
    for(d = netdev_default(); d; d = d->next) if(d->link_up) return d;
    return NULL;
}

/* telnetd on, as a process on F1: its status, or -1 when it could not be started or waited for */
static int start_telnetd(void) {
    char *argv[] = { "telnetd", "on" };
    int pid = process_spawn(AUTONET_TELNETD, 2, argv, &conArray[0], 0), status = -1, waited, r;
    if(pid < 0) {
        kprintf("+AUTONET: %s: %s\n", AUTONET_TELNETD, strerror(pid));
        return -1;
    }
    for(waited = 0; waited < AUTONET_TELNETD_MS; waited += 100) {
        r = task_wait_opts((u_long)pid, &status, WNOHANG);
        if(r == pid) return status;
        if(r < 0) break;
        sleep_ms(100);
    }
    kprintf("+AUTONET: telnetd on (pid %i) has not finished after %i s; not waiting for it\n", pid,
            AUTONET_TELNETD_MS / 1000);
    return -1;                          /* it is init's when this thread ends */
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
    i = start_telnetd();
    if(i == 0) kprintf("+AUTONET: done: telnet %s\n", ipfmt(dev->ip, a));
    else if(i > 0) kprintf("+AUTONET: telnetd on ended with status %i\n", i);
}

/* the boot: a thread for the word autonet, which waits on its own while the boot goes on */
void init_autonet(void) {
    if(!bootinfo_has_arg(&bootinfo, "autonet")) return;
    if(!kthread_create_on("autonet", &conArray[0], autonet_main, NULL, PL_NORMAL, TF_DETACHED))
        kprintf("+AUTONET: no memory for its thread: type dhcp and telnetd on\n");
}
