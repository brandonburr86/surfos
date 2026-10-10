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

#endif
