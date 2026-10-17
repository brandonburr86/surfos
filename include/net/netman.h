/*
SurfOS network manager
----------------------
File: netman.h  Date: 10/17/26
----------------------
kernel/netman.c: the settings and the saved Wi-Fi networks, read at boot from
surfos-config/ on the stick SurfOS was started from (the files: include/net/netcfg.h) and
written there when they change; at boot and afterwards the network they ask for: a saved
Wi-Fi network joined with its address (the Wi-Fi keeper, `wifiman`), Ethernet's DHCP when its
link comes up, the DHCP leases renewed, telnetd at startup. The settings and wifi commands
(shell/settings.c, shell/wifi.c) and autonet (kernel/autonet.c) use what is here.
The brief is docs/superpowers/plans/2026-10-17-network-manager.md.
*/

#ifndef _NET_NETMAN_H
#define _NET_NETMAN_H

#include <surfos/types.h>
#include <net/net.h>
#include <net/netcfg.h>
#include <net/wlan.h>

void init_netman(void);                     /* the boot (kernel/main.c): the netman thread */

/**** the settings ****/

void netman_settings(struct netcfg_settings *out);     /* a copy of those in force */
/* where they are, a line for the settings command: the volume and what was read at boot */
void netman_where(char *buf, size_t size);
/* key k = value, now and in settings.conf on the stick: 0 saved; 1 in force but not saved (why says why);
   -1 refused (why) */
int netman_set(int k, const char *value, char *why, size_t size);

/**** the saved Wi-Fi networks ****/

struct netman_net {                         /* as the commands see one: never its key */
    u8 ssid[NETCFG_SSID_MAX];
    u8 ssid_len;
    u8 security;                            /* NETCFG_SEC_* */
    bool hidden;
    bool refused;                           /* its key was refused at the last try (the password changed?) */
};

int netman_networks(struct netman_net *out, int max);  /* how many, in the order they are tried */
/* net (with its key) saved first in networks.conf on the stick, replacing one of the same name: 0; *where the
   position it had before (-1: new); -errno and why */
int netman_save_network(const struct netcfg_net *net, int *had, char *why, size_t size);
/* 0, -ENOENT (no such saved network), -errno and why */
int netman_forget_network(const u8 *ssid, u_int len, char *why, size_t size);

/* wifi connect: the card up, a scan, the first saved network in range joined, its address (DHCP); what happens is
   said on the caller's console. 0 joined (an address or not), -ENOENT none in range, -EBUSY the keeper is at it,
   other errors */
int netman_wifi_connect(struct wlan_dev *w);

/**** for autonet (kernel/autonet.c) ****/

/* does the Wi-Fi keeper get dev its address (it joined dev's network itself)? autonet then waits for it */
bool netman_wifi_owns(struct netdev *dev);
/* telnetd on [port] as a process on F1 (port 0: its own, 23): its status, -1 when it could not be started or waited for */
int netman_start_telnetd(u16 port);
bool autonet_busy(void);                    /* kernel/autonet.c: its thread still runs */

#endif
