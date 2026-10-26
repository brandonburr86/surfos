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
bool netman_settings_off(void);             /* the kernel word settings=off: nothing was read at this start */
bool netman_can_save(void);                 /* the stick SurfOS was started from is there and may be written */
bool netman_stick_idle(u_long ms);          /* 10/25/26: the shutdown's: what is done on the stick ended (ms at most) */
/* key k = value, now and in settings.conf on the stick: 0 saved; 1 in force but not saved (why says why);
   -1 refused (why) */
int netman_set(int k, const char *value, char *why, size_t size);

/**** the saved Wi-Fi networks ****/

struct netman_net {                         /* as the commands see one: never its key */
    u8 ssid[NETCFG_SSID_MAX];
    u8 ssid_len;
    u8 security;                            /* NETCFG_SEC_* */
    bool hidden;
    bool refused;                           /* its key was refused three times in a row: set aside for a while */
};

/* how many, in the order they are tried; *kept (may be NULL): the parts of networks.conf SurfOS did not understand,
   kept there as they were */
int netman_networks(struct netman_net *out, int max, int *kept);
/* net (with its key) saved first in networks.conf on the stick, replacing one of the same name: 0; *had the
   position it had before (-1: new), *count how many are saved now; -errno and why. Under settings=off the file
   changes, for the next start, and nothing else */
int netman_save_network(const struct netcfg_net *net, int *had, int *count, char *why, size_t size);
/* 0 (*count: how many are left), -ENOENT (no such saved network), -errno and why */
int netman_forget_network(const u8 *ssid, u_int len, int *count, char *why, size_t size);

/* wifi connect: the card up, a scan, the first saved network in range joined, its address (DHCP); what happens is
   said on the caller's console, and afterwards the keeper looks for saved networks by itself again. 0 joined (an
   address or not), -ENOENT none in range, -EBUSY the keeper is at it, -ECANCELED a wifi leave or down meanwhile,
   other errors */
int netman_wifi_connect(struct wlan_dev *w);

/**** telnetd ****/

/* an address on a network the setting telnetd = on starts telnetd for: Ethernet, or Wi-Fi joined to a WPA2
   network; never an open one, which anyone nearby may join (autonet starts it for Ethernet only) */
bool netman_trusted(struct netdev *dev);
/* a telnet server runs or is starting (any telnetd process), or is to start by itself (telnetd = on until it has
   started, autonet until it is done): the keeper then joins no open network by itself */
bool netman_telnet_exposed(void);
/* 10/24/26: an FTP server runs or is starting (any ftpd process): no logins, and files written: the keeper then
   joins no open network by itself either */
bool netman_ftp_exposed(void);
/* autonet's telnetd on (port 0: its own, 23) as a process on F1, "+AUTONET:" when it fails: its status, -1 when it
   could not be started or waited for */
int netman_start_telnetd(u16 port);
bool autonet_busy(void);                    /* kernel/autonet.c: its thread still runs (Ethernet's DHCP is its) */

#endif
