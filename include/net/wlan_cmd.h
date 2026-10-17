/*
SurfOS Wi-Fi: what the wifi command needs beyond wlan.h
----------------------
File: wlan_cmd.h  Date: 10/9/26 (Wi-Fi, track S)
----------------------
include/net/wlan.h is the contract between the station layer and the chip drivers, and
its wlan_down() and wlan_leave() return nothing. The wifi command (shell/wifi.c) must say
when they did not happen: another command's request was pending (one at a time per
device), or the command was cut short. net/wlan/wlan.c implements these; wlan_down() and
wlan_leave() are they with the outcome dropped.
*/

#ifndef _NET_WLAN_CMD_H
#define _NET_WLAN_CMD_H

#include <net/wlan.h>

/* 0, -ENODEV (no device), -EBUSY (another command's request is pending), -EINTR (cut short: the
   station layer finishes or cancels it by itself) */
int wlan_down_wait(struct wlan_dev *w);
int wlan_leave_wait(struct wlan_dev *w);

/* 10/17/26 (the network manager, kernel/netman.c): what the station is doing, for the Wi-Fi keeper,
   which must not get in the way of the wifi command, and for wifi save, which keeps the network
   joined now. A snapshot, taken without a request: it never waits and is never busy. */
#define WLAN_STA_DOWN         0     /* wifi down, or never up */
#define WLAN_STA_IDLE         1     /* up, not associated */
#define WLAN_STA_JOINING      2     /* a command's join (wlan_join()) under way */
#define WLAN_STA_UP           3     /* associated, the keys in: wlan0's link is up */
#define WLAN_STA_RECONNECTING 4     /* the link went: joining the same network again by itself */

/* why an idle station is idle */
#define WLAN_IDLE_NEW         0     /* nothing joined since it came up */
#define WLAN_IDLE_LEFT        1     /* wifi leave (or wifi down) */
#define WLAN_IDLE_JOIN_FAILED 2     /* the last wlan_join() failed */
#define WLAN_IDLE_GAVE_UP     3     /* the link went and the reconnects ran out */

struct wlan_station_info {
    int state;                      /* WLAN_STA_* */
    int idle;                       /* WLAN_IDLE_*, while idle or down */
    u8 ssid[WLAN_SSID_MAX];         /* the network joined, being joined, or joined last */
    u8 ssid_len;
    u8 security;                    /* its enum wlan_security */
    bool hidden;                    /* its beacons carry no SSID */
    u_int joins;                    /* links up so far: a new association counts one more */
    u_int join_requests;            /* wlan_join() calls so far: whose join the association is */
    bool has_key;                   /* key holds its PMK (asked for, and the station keeps one: WPA2) */
    u8 key[32];
};

/* 0, or -ENODEV; with want_key the PMK too (the caller wipes it) */
int wlan_station(struct wlan_dev *w, struct wlan_station_info *out, bool want_key);

/* 10/17/26: the simulated chip's access point named ssid off or on (driver/wifi/wlansim.c, the tests' wlansim
   command): 0, -ENOENT, -ENODEV without the kernel word wlansim */
int wlansim_switch(const char *ssid, bool on);

#endif
