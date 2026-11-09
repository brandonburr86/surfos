/*
SurfOS network settings for programs
----------------------
File: netctl.h  Date: 11/05/26 (the desktop applications, track settings)
----------------------
SYS_netctl (int op, void *arg, unsigned size): what the wifi and settings commands do (shell/wifi.c,
shell/settings.c), for a program with a window (Settings, user/wsettings/), as a small structured
interface (kernel/netctl.c). A command cannot serve such a program: it asks a WPA2 password on a
terminal the program does not have, it says what it found in sentences made for people, and it waits
for its scan, join or address while the program's window could not answer. Here every answer is a
structure, and nothing a program asks waits for the radio or the stick.

Reads answer at once and are never busy: NETCTL_WIFI (the card, the station and wlan0's address),
NETCTL_NETWORKS (what the scans heard in the last minute, every access point), NETCTL_SAVED (the
saved networks in the order they are tried, never their keys), NETCTL_SETTINGS (every key of the
network manager's table with its value, net/netcfg.c's, and where they are kept), NETCTL_IFACES
(every interface's addresses and DHCP lease, and the host name the requests carry) and NETCTL_JOB
(the last job).

NETCTL_APPLY puts a step of the desktop's theme or pointer speed in force at once, writing nothing
(the desktop reads both each second): a person stepping through them writes the stick once, by a
NETCTL_SET job once the steps stop, never at each step.

Jobs are what waits: the card on or off, a scan, a join, a leave, the joined network saved, a saved
network forgotten, a setting changed. The call checks what it was given, starts the job and returns
its number (1 and up) at once; a kernel thread does it, and NETCTL_JOB says how it goes (its stage)
and, once it is done, how it went (a result, flags, and why in words: never a secret). One job runs
at a time in all, whichever program started it: another start meanwhile is -EBUSY. A job goes on to
its end when its program ends.

A join's password comes in its request (struct netctl_join's passphrase, 8 to 63 printable ASCII
characters and a NUL, or "" for an open network). The call wipes that field in the caller's memory
whatever it answers, once it has seen the structure is the caller's to write, makes the PMK from a
copy at once (PBKDF2 with the SSID, wpa_passphrase_to_pmk()) and wipes the copy: the password is
never echoed, logged, kept or written anywhere. The PMK stays in the kernel: the job hands it to the
station, saves it in networks.conf when the request asks (the key, as wifi save keeps it: never the
password), and wipes it. No answer ever holds a key.

The security a join uses is the one wifi join would: the networks of that name in the last scans,
WPA2 first, then open. One not heard in the last minute is looked for first, by the job's own scan
(as wifi join scans before it calls a network hidden), and decided on what that heard; one still
not heard is joined as a hidden network, with WPA2 when a password came and open otherwise.

An open network is joined while a server without a password runs (telnetd or ftpd: anyone on that
network reaches it) only when the request says the person was told (NETCTL_JOIN_EXPOSED): otherwise
the call answers -EPERM, and so does the job (NETCTL_DONE_EXPOSED in its flags, and why) when such a
server started while it looked for the network. A join made with the flag while one runs has
NETCTL_DONE_EXPOSED too. A program sets the flag only from the person's answer to that warning.

CLAUDE.md's rules hold as for the commands: the settings are the network manager's
(kernel/netman.c), written only to the stick SurfOS was started from, and a saved network is joined
by itself only as the network manager decides.

Errors are -errno: -EINVAL for an op, size or field that is none (sizes must be exactly the
structure's: they say its version), -EFAULT for memory that is not the caller's, -ENODEV without a
Wi-Fi device or with its card off (the station layer's answer too), -EBUSY while a job runs. A
join's own: -EACCES (the network takes a password and none came), -ENOSYS (a security SurfOS does
not join: WEP, WPA3 only, enterprise, other; or no WPA2 supplicant in this kernel), -EPERM (an open
network while a server without a password runs, and the person was not told).

This header uses no SurfOS types, and its structures no pointers, so that they have the same layout
for the kernel, for programs and for host tests (kernel/netctl.c checks the sizes). User programs
reach it through user/include/surfos/netctl.h.
*/

#ifndef _SURFOS_NETCTL_H
#define _SURFOS_NETCTL_H

/* SYS_netctl's number (include/surfos/syscall.h), for programs, which see no syscall.h and have no libsurf wrapper
   for it: kernel/netctl.c checks the two are the same */
#define NETCTL_SYSCALL      55

#define NETCTL_SSID_MAX     32
#define NETCTL_PASS_MAX     64      /* a WPA2 password's bytes with its NUL */
#define NETCTL_NAME_MAX     8       /* an interface's name with its NUL: wlan0 */
#define NETCTL_TEXT_MAX     160
#define NETCTL_WHY_MAX      200
#define NETCTL_BSS_MAX      48      /* the station layer's scan list holds as many */
#define NETCTL_SAVED_MAX    16      /* NETCFG_NETS_MAX */
#define NETCTL_KEYS_MAX     16      /* settings in the network manager's table, at most */
#define NETCTL_KEY_MAX      24      /* a setting's name with its NUL */
#define NETCTL_VALUE_MAX    48      /* a setting's value with its NUL (a host name has 32 characters) */
#define NETCTL_VALUES_MAX   40      /* what it takes, as the settings command says it: "on | off" */
#define NETCTL_IF_MAX       8
#define NETCTL_HOST_MAX     36      /* the host name DHCP requests carry, with its NUL */

/* reads */
#define NETCTL_WIFI         1       /* struct netctl_wifi */
#define NETCTL_NETWORKS     2       /* struct netctl_bss_list */
#define NETCTL_SAVED        3       /* struct netctl_saved */
#define NETCTL_SETTINGS     4       /* struct netctl_settings */
#define NETCTL_IFACES       5       /* struct netctl_ifaces */
#define NETCTL_JOB          6       /* struct netctl_job */
/* at once, writing nothing */
#define NETCTL_APPLY        8       /* struct netctl_req: key, value: desktop.theme or pointer.speed in force now. 0, 1 not
                                       in force (settings=off), -EINVAL another key or a value it does not take */
/* jobs: the call returns the job's number */
#define NETCTL_CARD         16      /* struct netctl_req: on 1 starts the card (and has the network manager join a
                                       saved network, as wifi connect does, when the setting wifi is on), 0 stops it */
#define NETCTL_SCAN         17      /* no argument (NULL, size 0) */
#define NETCTL_JOIN         18      /* struct netctl_join */
#define NETCTL_LEAVE        19      /* no argument */
#define NETCTL_SAVE         20      /* no argument: the network wlan0 is joined to, with the key it joined with */
#define NETCTL_FORGET       21      /* struct netctl_req: ssid, ssid_len */
#define NETCTL_SET          22      /* struct netctl_req: key, value (as `settings key value` takes them) */

/* the station (include/net/wlan_cmd.h's WLAN_STA_* and WLAN_IDLE_*) */
#define NETCTL_STA_DOWN         0   /* the card is off */
#define NETCTL_STA_IDLE         1   /* on, not joined */
#define NETCTL_STA_JOINING      2
#define NETCTL_STA_UP           3   /* joined, the keys in: wlan0's link is up */
#define NETCTL_STA_RECONNECTING 4   /* the link went: joining the same network again by itself */
#define NETCTL_IDLE_NEW         0   /* nothing joined since the card started */
#define NETCTL_IDLE_LEFT        1   /* a leave (or the card stopped) */
#define NETCTL_IDLE_JOIN_FAILED 2
#define NETCTL_IDLE_GAVE_UP     3   /* the link went and the station's tries to join again ran out */

/* a network's security (include/net/wlan.h's enum wlan_security) */
#define NETCTL_SEC_OPEN         0
#define NETCTL_SEC_WEP          1   /* not joined: broken */
#define NETCTL_SEC_WPA2         2   /* WPA2-Personal */
#define NETCTL_SEC_WPA2_WPA3    3   /* offers WPA2 and WPA3: joined as WPA2 */
#define NETCTL_SEC_WPA3         4   /* WPA3-Personal only: not joined yet */
#define NETCTL_SEC_ENTERPRISE   5   /* 802.1X with a login: not joined */
#define NETCTL_SEC_OTHER        6   /* WPA, TKIP, OWE, ...: not joined */
/* a saved network's (include/net/netcfg.h's NETCFG_SEC_*) */
#define NETCTL_SAVED_OPEN       0
#define NETCTL_SAVED_WPA2       1

/* exposed: a server without a password runs (or telnetd is to start by itself): meanwhile SurfOS joins no open
   network by itself, and a person joining one should know (the wifi command says so too) */
#define NETCTL_EXPOSED_TELNET   1
#define NETCTL_EXPOSED_FTP      2

/* struct netctl_join's flags */
#define NETCTL_JOIN_SAVE        1   /* once joined, save it on the stick (first in the list), as wifi save does */
#define NETCTL_JOIN_EXPOSED     2   /* 11/05/26 (the review, M2): an open network, although a server without a password
                                       runs: the person was told (the program's question) and joins all the same */

/* a job */
#define NETCTL_JOB_NONE         0   /* none since SurfOS started */
#define NETCTL_JOB_RUNNING      1
#define NETCTL_JOB_DONE         2
/* what a running job does now */
#define NETCTL_STAGE_STARTING   1   /* the card starts */
#define NETCTL_STAGE_STOPPING   2   /* the card stops */
#define NETCTL_STAGE_SCANNING   3
#define NETCTL_STAGE_JOINING    4
#define NETCTL_STAGE_SAVING     5   /* the network written in networks.conf on the stick */
#define NETCTL_STAGE_ADDRESS    6   /* DHCP for wlan0 */
#define NETCTL_STAGE_CONNECTING 7   /* the network manager looks for a saved network in range */
#define NETCTL_STAGE_LEAVING    8
#define NETCTL_STAGE_WRITING    9   /* a setting or the saved networks, on the stick */
/* what came of a job besides its result */
#define NETCTL_DONE_ADDRESS     0x1 /* wlan0 has an address (a join, the card started) */
#define NETCTL_DONE_SAVED       0x2 /* a join's network was saved */
#define NETCTL_DONE_NOT_SAVED   0x4 /* not saved (a join's save, a setting kept in force only): why says why */
#define NETCTL_DONE_JOINED      0x8 /* the card started and a saved network was joined */
#define NETCTL_DONE_NONE_NEAR   0x10 /* the card started; no saved network was in range */
#define NETCTL_DONE_PARTIAL     0x20 /* a scan that did not end in time: what it heard until then */
#define NETCTL_DONE_EXPOSED     0x40 /* an open network while a server without a password runs: joined (the person was
                                        told), or refused (-EPERM: they were not) */

struct netctl_wifi {                        /* 388 bytes */
    unsigned char present;                  /* a Wi-Fi device: nothing below means anything without one */
    unsigned char up;                       /* its card is on */
    unsigned char state;                    /* NETCTL_STA_* */
    unsigned char idle;                     /* NETCTL_IDLE_*: why an idle (or stopped) station is so */
    char name[NETCTL_NAME_MAX];             /* wlan0 */
    unsigned char ssid[NETCTL_SSID_MAX];    /* the network joined, being joined, or joined last */
    unsigned char ssid_len;
    unsigned char security;                 /* NETCTL_SEC_* */
    unsigned char hidden;                   /* its beacons carry no name */
    unsigned char link_up;                  /* wlan0's link */
    unsigned char exposed;                  /* NETCTL_EXPOSED_* */
    unsigned char pad[3];
    unsigned ip, netmask, gateway, dns;     /* wlan0's, in network byte order; 0: none */
    char state_text[NETCTL_TEXT_MAX];       /* the station as wifi status says it: network, channel, signal */
    char last[NETCTL_TEXT_MAX];             /* why the last join failed or the link went, or "" */
};

struct netctl_bss {                         /* 48 bytes: an access point heard */
    unsigned char ssid[NETCTL_SSID_MAX];
    unsigned char bssid[6];
    unsigned char ssid_len;                 /* 0: its beacons carry no name */
    unsigned char security;                 /* NETCTL_SEC_* */
    unsigned char channel;
    unsigned char band;                     /* 0: 2.4 GHz, 1: 5 GHz */
    signed char rssi;                       /* dBm, of the last frame heard */
    unsigned char hidden;                   /* a beacon of it carried no name */
    unsigned short age_s;                   /* seconds since it was last heard */
    unsigned short pad;
};

struct netctl_bss_list {                    /* 2308 bytes: strongest first */
    unsigned count;
    struct netctl_bss bss[NETCTL_BSS_MAX];
};

struct netctl_saved_net {                   /* 36 bytes */
    unsigned char ssid[NETCTL_SSID_MAX];
    unsigned char ssid_len;
    unsigned char security;                 /* NETCTL_SAVED_* */
    unsigned char hidden;
    unsigned char refused;                  /* its key was refused three times in a row: set aside for a while */
};

struct netctl_saved {                       /* 584 bytes: in the order they are tried */
    unsigned count;
    unsigned kept;                          /* parts of networks.conf SurfOS did not understand, kept there */
    struct netctl_saved_net net[NETCTL_SAVED_MAX];
};

struct netctl_setting {                     /* 116 bytes */
    char name[NETCTL_KEY_MAX];              /* as the settings command names it: wifi, telnetd.port, ... */
    char value[NETCTL_VALUE_MAX];           /* as the file writes it */
    char values[NETCTL_VALUES_MAX];         /* what it takes: "on | off", "1-65535", ... */
    unsigned set;                           /* set (in the file, or since); 0: its default */
};

struct netctl_settings {                    /* 2064 bytes */
    unsigned count;
    unsigned char off;                      /* settings=off: not read at this start, a change is for the next */
    unsigned char can_save;                 /* the stick SurfOS was started from is there and may be written */
    unsigned char exposed;                  /* NETCTL_EXPOSED_* */
    unsigned char pad;
    char where[NETCTL_WHY_MAX];             /* where they are kept, as the settings command says it */
    struct netctl_setting key[NETCTL_KEYS_MAX];
};

struct netctl_if {                          /* 44 bytes */
    char name[NETCTL_NAME_MAX];             /* eth0, wlan0 */
    unsigned char mac[6];
    unsigned char link_up;
    unsigned char wifi;                     /* a Wi-Fi station's */
    unsigned ip, netmask, gateway, dns;     /* network byte order; 0: none */
    unsigned dhcp_server;                   /* the server that granted the lease */
    unsigned lease_s;                       /* its length in seconds; 0: no lease (none, or set by hand) */
    unsigned lease_left;                    /* seconds left */
};

struct netctl_ifaces {                      /* 392 bytes */
    unsigned count;
    char hostname[NETCTL_HOST_MAX];         /* the name DHCP requests carry now ("" for none) */
    struct netctl_if iface[NETCTL_IF_MAX];
};

struct netctl_job {                         /* 260 bytes */
    unsigned seq;                           /* its number; 0: no job yet */
    int op;                                 /* NETCTL_CARD .. NETCTL_SET */
    int state;                              /* NETCTL_JOB_* */
    int stage;                              /* NETCTL_STAGE_*, while it runs */
    int result;                             /* once done: 0, or -errno */
    unsigned flags;                         /* once done: NETCTL_DONE_* */
    unsigned char ssid[NETCTL_SSID_MAX];    /* the network it is about (join, forget), or none */
    unsigned ssid_len;
    char why[NETCTL_WHY_MAX];               /* why it failed, or what else to know: never a secret */
};

struct netctl_join {                        /* 104 bytes */
    unsigned char ssid[NETCTL_SSID_MAX];
    unsigned ssid_len;                      /* 1 to 32 */
    unsigned flags;                         /* NETCTL_JOIN_* */
    char passphrase[NETCTL_PASS_MAX];       /* "" for an open network; wiped by the call */
};

struct netctl_req {                         /* 112 bytes */
    unsigned on;                            /* NETCTL_CARD */
    unsigned char ssid[NETCTL_SSID_MAX];    /* NETCTL_FORGET */
    unsigned ssid_len;
    char key[NETCTL_KEY_MAX];               /* NETCTL_SET, NUL-terminated */
    char value[NETCTL_VALUE_MAX];
};

#endif
