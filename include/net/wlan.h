/*
SurfOS Wi-Fi: the station layer and its chip drivers
----------------------
File: wlan.h    Date: 10/9/26
----------------------
The contract between the 802.11 station layer (net/wlan/: scanning, joining, the supplicant,
wlan0) and a Wi-Fi chip driver (driver/wifi/: the Intel 9560). The brief is
docs/superpowers/plans/2026-10-09-wifi.md.

Frames cross it as they are on the air: the 802.11 header first, then the body, no FCS. A
protected frame received is handed over decrypted, its CCMP header and MIC removed
(wlan_rx_info.decrypted); a frame to protect is handed over in the clear with protect set,
and the driver has it encrypted (or encrypts it).

Context:
* A driver delivers received frames with wlan_rx() from task context (its own thread),
  never from an interrupt handler; wlan_rx() copies the frame and returns at once.
* tx_data() and tx_mgmt() never wait: they queue the frame or drop it (non-zero), and may
  be called with net_lock held. 10/9/26 (the review of track S): tx_data() may be called by
  two threads at once, wlan0's sends from the IP stack (always under net_lock) and the
  station layer's own thread (EAPOL); tx_mgmt() by the station layer's thread. Neither is
  called during or after leave() or stop(): the station layer stops wlan0's sends, waits
  out one already under way (by taking net_lock) and only then calls them; the next comes
  after join() or start().
* Every other operation may wait for the hardware and is called by the station layer only
  from its own thread or a command's task, never with net_lock held, never two at a time
  for one device. They return 0 or a negative errno (include/fs/vfs.h).
*/

#ifndef _SURFOS_WLAN_H
#define _SURFOS_WLAN_H

#include <surfos/types.h>
#include <net/net.h>

#define WLAN_ADDR_LEN    6
#define WLAN_SSID_MAX    32
#define WLAN_MAX_CHANNELS 64

enum wlan_band { WLAN_BAND_2GHZ = 0, WLAN_BAND_5GHZ = 1 };

/* what a network asks of a station: its RSN element and the privacy bit */
enum wlan_security {
    WLAN_SEC_OPEN = 0,      /* no privacy */
    WLAN_SEC_WEP,           /* the privacy bit, no RSN element: not supported */
    WLAN_SEC_WPA2_PSK,      /* RSN, AKM 2 (PSK), CCMP pairwise */
    WLAN_SEC_WPA2_WPA3,     /* RSN offering AKM 2 and 8 (SAE): joined as WPA2-PSK */
    WLAN_SEC_WPA3_SAE,      /* AKM 8 only: not supported yet */
    WLAN_SEC_ENTERPRISE,    /* AKM 1, 3, 5, ...: not supported */
    WLAN_SEC_OTHER          /* WPA1, TKIP only, OWE, ...: not supported */
};

/* a network seen in a scan (the station layer fills it from beacons and probe responses) */
struct wlan_bss {
    u8 bssid[WLAN_ADDR_LEN];
    u8 ssid[WLAN_SSID_MAX];
    u8 ssid_len;            /* 0: hidden */
    u8 channel;             /* IEEE channel number */
    u8 band;                /* enum wlan_band */
    i8 rssi;                /* dBm, of the last frame heard */
    u16 beacon_interval;    /* TU (1024 us) */
    u16 capability;         /* the capability information field */
    u8 security;            /* enum wlan_security */
    u8 rsn_ie[64];          /* the RSN element as it came (id, length, body), for the 4-way handshake */
    u8 rsn_ie_len;
    u8 rates[16];           /* supported and extended supported rates, as the elements list them */
    u8 nrates;
    bool ht, vht;           /* HT and VHT capabilities elements present */
    u8 ht_cap[26];          /* the HT capabilities element's body when ht */
    u_long seen;            /* ticks, when last heard */
};

/* with a received frame */
struct wlan_rx_info {
    u8 channel;
    u8 band;                /* enum wlan_band */
    i8 rssi;                /* dBm */
    bool decrypted;         /* it was protected and the hardware decrypted and checked it */
};

/* what to scan for */
struct wlan_scan_req {
    u8 ssid[WLAN_SSID_MAX]; /* a directed probe for this SSID too when ssid_len > 0 */
    u8 ssid_len;
    u8 nchannels;           /* 0: every channel the driver may use */
    u8 channels[WLAN_MAX_CHANNELS];
    u8 bands[WLAN_MAX_CHANNELS];
};

#define WLAN_CIPHER_CCMP 1

struct wlan_key {
    u8 cipher;              /* WLAN_CIPHER_CCMP */
    bool pairwise;          /* the PTK's temporal key; else a group key */
    u8 index;               /* 0 for the pairwise key, 1 to 3 for a group key */
    u8 key[32];
    u8 len;                 /* 16 for CCMP-128 */
    u8 rsc[8];              /* a group key's receive sequence counter, low byte first */
};

struct wlan_dev;

/* what a chip driver gives the station layer */
struct wlan_hw_ops {
    int (*start)(struct wlan_dev *w);       /* power up and load the firmware; fill w->mac */
    void (*stop)(struct wlan_dev *w);       /* everything off; start() may follow */
    int (*scan)(struct wlan_dev *w, const struct wlan_scan_req *req);   /* begins; frames come with
                                                wlan_rx(), the end with wlan_scan_done() */
    void (*scan_abort)(struct wlan_dev *w);
    int (*join)(struct wlan_dev *w, const struct wlan_bss *bss);        /* tune to its channel, make the
                                                AP a station, stay on the channel for authentication
                                                and association */
    int (*tx_mgmt)(struct wlan_dev *w, const u8 *frame, u_int len);     /* authentication, association,
                                                deauthentication, probe: never waits */
    int (*associated)(struct wlan_dev *w, u16 aid, const u8 *ies, u_int ies_len);   /* the association
                                                response's elements: rates, HT; the link is up */
    int (*set_key)(struct wlan_dev *w, const struct wlan_key *key);
    int (*tx_data)(struct wlan_dev *w, struct netbuf *nb, bool protect);   /* an 802.11 data frame,
                                                header first; never waits; nb stays the caller's */
    void (*leave)(struct wlan_dev *w);      /* the AP's station, its keys and the binding go */
    void (*status)(struct wlan_dev *w, char *buf, size_t size);         /* a line for wifi status */
};

struct wlan_state;                          /* the station layer's own (net/wlan/) */

struct wlan_dev {
    char name[8];                           /* "wlan0" */
    const struct wlan_hw_ops *ops;
    void *priv;                             /* the driver's */
    u8 mac[WLAN_ADDR_LEN];                  /* known after start() */
    bool up;                                /* start() succeeded */
    struct netdev *netdev;                  /* wlan0 for the IP stack */
    struct wlan_state *st;
};

/* the driver calls these */
struct wlan_dev *wlan_register(const struct wlan_hw_ops *ops, void *priv);   /* once it has found its
                                                device (no firmware yet): wlan0, wlan1, ... */
void wlan_rx(struct wlan_dev *w, const u8 *frame, u_int len, const struct wlan_rx_info *info);
void wlan_scan_done(struct wlan_dev *w, int status);   /* 0, or why the scan ended early */
void wlan_link_lost(struct wlan_dev *w);    /* the firmware lost the AP (beacons missed) */

/* the wifi command and the rest of SurfOS */
struct wlan_dev *wlan_find(const char *name);           /* NULL: the first */
int wlan_up(struct wlan_dev *w);
void wlan_down(struct wlan_dev *w);
int wlan_scan(struct wlan_dev *w, u_int timeout_ms);    /* waits for the end; the list grows */
u_int wlan_bss_list(struct wlan_dev *w, struct wlan_bss *out, u_int max);   /* sorted by signal */
int wlan_join(struct wlan_dev *w, const u8 *ssid, u_int ssid_len, const u8 *bssid, const u8 *pmk);
                                            /* bssid NULL: the strongest with that SSID; pmk NULL for an
                                               open network; waits until associated (and the keys are
                                               in) or failed */
void wlan_leave(struct wlan_dev *w);
void wlan_describe(struct wlan_dev *w, char *buf, size_t size);   /* state, SSID, BSSID, channel, signal */

#endif
