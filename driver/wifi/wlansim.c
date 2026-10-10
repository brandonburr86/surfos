/*
SurfOS Wi-Fi: a simulated chip for the tests (wlansim)
--------------------
File: wlansim.c Date: 10/9/26 (Wi-Fi, track S)
--------------------
With the word `wlansim` on the kernel command line (and only then: without it nothing here
runs, registers or prints) a fake chip registers with the station layer as wlan0: a struct
wlan_hw_ops whose "firmware" is a handful of access points on the air, so the station
layer, the wifi command, DHCP and the IP stack over wlan0 can be tested under QEMU, which
has no Wi-Fi:

  SurfSim        open, 2.4 GHz channel 6, HT, WMM (QoS data, A-MSDU now and then)
  SurfSimWPA     WPA2-Personal (CCMP, PSK), 5 GHz channel 36, HT and VHT, WMM; its test-only
                 passphrase is WLANSIM_PASSPHRASE below (the smoke test types it)
  (hidden)       SurfSimHidden: open, 2.4 GHz channel 11, an empty SSID in its beacons; it
                 answers a probe for its name only; no HT, no WMM (plain data frames)
  SurfSimWEP     WEP (the privacy bit, no RSN element), channel 1
  SurfSimSAE     WPA3-Personal only (SAE, protected management frames required), channel 149
  SurfSimFlaky   open, channel 1, WMM: 3 s after the first association it deauthenticates the
                 station; 1 s after the second its beacons stop (wlan_link_lost()) for 4 s

The simulated firmware runs in a kernel thread of its own (`wlansim`), as a real driver's
would: a scan takes 600 ms and delivers a beacon from every access point on the channels
asked for, and a probe response from those that answer the probe (an aborted scan's end is
reported 300 ms after the abort, as iwx's firmware reports it); authentication and
association are answered as an access point does (an association to the WPA2 network must
carry an RSN element it accepts); a station that sends data frames while not associated is
deauthenticated (reason 7). The associated network beacons once a second.

Bridging: the access points' network is the first Ethernet interface (QEMU's e1000, eth0).
A data frame from the station becomes an Ethernet frame on it from wlan0's own MAC address
(eth0's with the last byte one higher), and frames on eth0 to that address, broadcasts and
multicasts become data frames from the access point (eth0 is given the address with
NETDEV_ADD_RX_ADDR, and its bridge_rx hook brings them here). QEMU's DHCP server so gives
wlan0 an address of its own. Every seventh frame to the station comes twice, the second as
a retransmission, and with WMM every third unicast one as an A-MSDU.

WPA2: the access point runs the authenticator side of the 4-way handshake (802.11-2016
12.7.6) and, 3 seconds after it, one group key handshake (12.7.7), with track C's crypto
(include/crypto/crypto.h: PBKDF2, the PRF, HMAC-SHA1, AES key wrap, random nonces). Those
functions are weak references: until lib/crypto/ is linked in, the WPA2 network refuses
associations (and the station layer refuses to join it without wpa.c anyway). Encryption
itself is simulated as a chip does it: a frame the station hands over with protect set is
"encrypted" with the pairwise key it installed, and the access point reads it only when that
key is the TK it derived; a frame to the station arrives decrypted only when the station
installed the matching pairwise or group key, else as a protected frame the "hardware" could
not decrypt. A wrong password shows as an access point that never accepts message 2 and
gives up after three tries (deauthentication, reason 15). Nothing here logs a key.
*/

#include <surfos/types.h>
#include <surfos/console.h>
#include <surfos/task.h>
#include <surfos/sync.h>
#include <surfos/irq.h>
#include <surfos/timer.h>
#include <surfos/multiboot.h>
#include <mm/kalloc.h>
#include <fs/vfs.h>
#include <net/net.h>
#include <net/ip.h>
#include <net/wlan.h>
#include <net/ieee80211.h>
#include <crypto/crypto.h>
#include <blibc_common.h>

/* track C's crypto: absent (NULL) until lib/crypto/ is linked in */
#pragma weak pbkdf2_sha1
#pragma weak hmac_sha1
#pragma weak ieee80211_prf_sha1
#pragma weak aes_wrap
#pragma weak crypto_random

/* the test network's passphrase: for SurfSimWPA in the tests, never a real network's */
#define WLANSIM_PASSPHRASE "surfsim-wpa2-test-passphrase"

enum { AP_OPEN, AP_WPA2, AP_WEP, AP_SAE };

struct sim_ap {
    const char *ssid;                   /* as the AP knows itself */
    bool hidden;                        /* an empty SSID in beacons, answering only a probe for it */
    u8 last;                            /* its BSSID's last byte: 02:53:49:4d:00:last */
    u8 channel;
    i8 rssi;
    u8 kind;                            /* AP_* */
    bool ht, vht, wmm, flaky;
};

static const struct sim_ap aps[] = {
    { "SurfSim",       false, 1,   6, -45, AP_OPEN, true,  false, true,  false },
    { "SurfSimWPA",    false, 2,  36, -52, AP_WPA2, true,  true,  true,  false },
    { "SurfSimHidden", true,  3,  11, -67, AP_OPEN, false, false, false, false },
    { "SurfSimWEP",    false, 4,   1, -71, AP_WEP,  false, false, false, false },
    { "SurfSimSAE",    false, 5, 149, -60, AP_SAE,  true,  false, true,  false },
    { "SurfSimFlaky",  false, 6,   1, -58, AP_OPEN, false, false, true,  true  },
};
#define NAPS ((int)(sizeof(aps) / sizeof(aps[0])))

enum { IT_MGMT, IT_DATA, IT_UPLINK };

struct sim_item {
    struct sim_item *next;
    int kind;                           /* IT_*: from the station (management, data), or from eth0 */
    bool protect;                       /* data: the station asked for protection */
    bool key_valid;                     /* ...and had this pairwise key installed then */
    u8 key[16];
    u_int len;
    u8 data[];
};

/* the authenticator of the WPA2 network */
enum { A_IDLE, A_MSG1, A_MSG3, A_OPEN, A_GROUP1 };
struct auth4 {
    int stage;                          /* A_*: what was sent last and is waited for */
    bool pmk_ready;
    u8 pmk[32];
    u8 anonce[32], snonce[32];
    u8 ptk[48];                         /* KCK, KEK, TK */
    u8 gtk[16];
    u8 gtk_index;
    u8 tx_gtk[16];                      /* the group key broadcasts go out with: the new one once acknowledged */
    u8 tx_gtk_index;
    u64 replay;
    u_long resend_at;
    u_int resends;
    u_long rekey_at;                    /* the group key renewal, once per association; 0: done */
};

struct sim {
    struct wlan_dev *w;
    struct netdev *uplink;              /* eth0 */
    event_t kick;
    struct sim_item *q_head, *q_tail;   /* irq_save */
    u_int q_count;
    bool started;
    /* the scan the station asked for */
    volatile bool scan_pending;
    struct wlan_scan_req scan_req;
    u_long scan_at;
    u_long stale_done_at;               /* 10/9/26 (review L2): when to report the end of an aborted scan, 0: none */
    /* the air */
    int tuned;                          /* join(): the AP whose channel the chip is on, -1 */
    int authed, assoc;                  /* the AP that authenticated, associated the station; -1 */
    bool sta_wmm;                       /* its association request had a WMM element */
    u8 sta_rsn[64];                     /* and its RSN element */
    u_int sta_rsn_len;
    u16 seq;
    u_long next_beacon;
    u_int count;                        /* frames to the station, for the retransmissions and A-MSDUs */
    /* the station's keys (set_key) */
    bool ptk_set, gtk_set;
    u8 ptk_key[16], gtk_key[16];
    u8 gtk_key_index;
    struct auth4 a;
    /* SurfSimFlaky */
    u_int flaky_assocs;
    u_long flaky_deauth_at, flaky_lost_at, flaky_silent_until;
    /* counters */
    u_long out, in, dropped, undecryptable;
    /* 10/9/26 (review M3): leave() or stop() came and no join() or start() since: wlan.h says no frame is handed
       over then, and the chip counts (and says once) any that is */
    bool left;
    u_long late;
};

static struct sim *the_sim;             /* one simulated chip: the bridge hook finds it here */

static void wipe(void *p, size_t n) {
    volatile u8 *q = (volatile u8 *)p;
    while(n--) *q++ = 0;
}

static void bssid_of(int ap, u8 *b) {
    b[0] = 0x02; b[1] = 0x53; b[2] = 0x49; b[3] = 0x4d; b[4] = 0x00; b[5] = aps[ap].last;
}

static int ap_by_bssid(const u8 *b) {
    int i;
    u8 x[6];
    for(i = 0; i < NAPS; i++) {
        bssid_of(i, x);
        if(!memcmp(x, b, 6)) return i;
    }
    return -1;
}

static u8 band_of_ap(int ap) {
    return ieee80211_band_of(aps[ap].channel);
}

static bool have_crypto(void) {
    return pbkdf2_sha1 && hmac_sha1 && ieee80211_prf_sha1 && aes_wrap && crypto_random;
}

static bool silent(struct sim *s, int ap) {
    return aps[ap].flaky && (long)(getticks() - s->flaky_silent_until) < 0;
}

/**** frames to the station ****/

static void deliver(struct sim *s, int ap, const u8 *frame, u_int len, bool decrypted) {
    struct wlan_rx_info info;
    if(!len) return;
    info.channel = aps[ap].channel;
    info.band = band_of_ap(ap);
    info.rssi = aps[ap].rssi;
    info.decrypted = decrypted;
    wlan_rx(s->w, frame, len, &info);
}

static const u8 rsn_wpa2[22] = { WLAN_EID_RSN, 20, 1, 0, 0x00, 0x0f, 0xac, 4, 1, 0, 0x00, 0x0f, 0xac, 4,
                                 1, 0, 0x00, 0x0f, 0xac, 2, 0, 0 };
static const u8 rsn_sae[22] = { WLAN_EID_RSN, 20, 1, 0, 0x00, 0x0f, 0xac, 4, 1, 0, 0x00, 0x0f, 0xac, 4,
                                1, 0, 0x00, 0x0f, 0xac, 8, 0xc0, 0 };
static const u8 wmm_param[24] = { 0x00, 0x50, 0xf2, 0x02, 0x01, 0x01, 0x80, 0x00, 0x03, 0xa4, 0x00, 0x00, 0x27, 0xa4,
                                  0x00, 0x00, 0x42, 0x43, 0x5e, 0x00, 0x62, 0x32, 0x2f, 0x00 };

static void put_ht(struct wbuf *b, int ap) {
    u8 ht[2 + WLAN_HT_CAP_LEN], op[22];
    wbuf_put(b, ht, ieee80211_ht_cap(ht, 2, band_of_ap(ap) == WLAN_BAND_5GHZ));
    memset(op, 0, sizeof(op));
    op[0] = aps[ap].channel;
    wbuf_elem(b, WLAN_EID_HT_OPERATION, op, sizeof(op));
}

/* a beacon, or a probe response to the station */
static u_int ap_frame(struct sim *s, int ap, u8 subtype, u8 *buf, u_int size) {
    static const u8 tsf[8] = { 0 };
    const struct sim_ap *p = &aps[ap];
    struct wbuf b;
    u8 bssid[6], r[12], vht[12], vhtop[5], ch = p->channel;
    u16 cap = WLAN_CAP_ESS;
    u_int n;
    bool beacon = subtype == WLAN_ST_BEACON;
    bssid_of(ap, bssid);
    if(p->kind != AP_OPEN) cap |= WLAN_CAP_PRIVACY;
    if(band_of_ap(ap) == WLAN_BAND_2GHZ) cap |= WLAN_CAP_SHORT_SLOT | WLAN_CAP_SHORT_PREAMBLE;
    wbuf_init(&b, buf, size);
    wbuf_hdr(&b, WLAN_FC(WLAN_FC_TYPE_MGMT, subtype), beacon ? (const u8 *)"\xff\xff\xff\xff\xff\xff" : s->w->mac,
             bssid, bssid, s->seq++);
    wbuf_put(&b, tsf, 8);
    wbuf_le16(&b, 100);
    wbuf_le16(&b, cap);
    if(p->hidden && beacon) wbuf_elem(&b, WLAN_EID_SSID, NULL, 0);
    else wbuf_elem(&b, WLAN_EID_SSID, p->ssid, strlen(p->ssid));
    n = ieee80211_rates(band_of_ap(ap), r);
    wbuf_elem(&b, WLAN_EID_RATES, r, n < 8 ? n : 8);
    if(band_of_ap(ap) == WLAN_BAND_2GHZ) wbuf_elem(&b, WLAN_EID_DS_PARAMS, &ch, 1);
    if(beacon) wbuf_elem(&b, WLAN_EID_TIM, "\x00\x01\x00\x00", 4);
    if(n > 8) wbuf_elem(&b, WLAN_EID_EXT_RATES, r + 8, n - 8);
    if(p->kind == AP_WPA2) wbuf_put(&b, rsn_wpa2, sizeof(rsn_wpa2));
    if(p->kind == AP_SAE) wbuf_put(&b, rsn_sae, sizeof(rsn_sae));
    if(p->ht) put_ht(&b, ap);
    if(p->vht) {
        memset(vht, 0, sizeof(vht));
        vht[0] = 0x92; vht[1] = 0x01; vht[2] = 0x80; vht[3] = 0x33;
        memset(vhtop, 0, sizeof(vhtop));
        vhtop[0] = 1;
        vhtop[1] = (u8)(ch + 6);
        wbuf_elem(&b, WLAN_EID_VHT_CAP, vht, sizeof(vht));
        wbuf_elem(&b, WLAN_EID_VHT_OPERATION, vhtop, sizeof(vhtop));
    }
    if(p->wmm) wbuf_elem(&b, WLAN_EID_VENDOR, wmm_param, sizeof(wmm_param));
    return wbuf_done(&b);
}

static void ap_mgmt(struct sim *s, int ap, u8 subtype, const u8 *body, u_int n) {
    struct wbuf b;
    u8 f[256], bssid[6];                                /* an association response with HT and WMM: 124 */
    bssid_of(ap, bssid);
    wbuf_init(&b, f, sizeof(f));
    wbuf_hdr(&b, WLAN_FC(WLAN_FC_TYPE_MGMT, subtype), s->w->mac, bssid, bssid, s->seq++);
    wbuf_put(&b, body, n);
    deliver(s, ap, f, wbuf_done(&b), false);
}

/* the AP sends the station away: deauthentication with a reason, its state for it gone */
static void ap_deauth(struct sim *s, int ap, u16 reason) {
    u8 body[2];
    body[0] = (u8)reason;
    body[1] = (u8)(reason >> 8);
    ap_mgmt(s, ap, WLAN_ST_DEAUTH, body, 2);
    kprintf("wlansim: %s deauthenticates the station (reason %u)\n", aps[ap].ssid, reason);
    if(s->assoc == ap || s->authed == ap) {
        s->assoc = s->authed = -1;
        s->a.stage = A_IDLE;
    }
}

/* data from the AP's network to the station: From DS, QoS when it has WMM, protected (and decrypted
   by the "chip" when its key matches) on the WPA2 network */
static void to_station(struct sim *s, const u8 *eth, u_int len) {
    const struct sim_ap *p;
    u8 f[NETBUF_SIZE + 64], bssid[6];
    bool group = eth[0] & 1, decrypted = false, amsdu;
    u_int n;
    int ap = s->assoc;
    if(ap < 0 || len < ETH_HLEN || silent(s, ap)) return;
    p = &aps[ap];
    if(p->kind == AP_WPA2 && s->a.stage != A_OPEN && s->a.stage != A_GROUP1) return;   /* the port is closed */
    bssid_of(ap, bssid);
    s->count++;
    amsdu = s->sta_wmm && !group && s->count % 3 == 0;
    if(amsdu) {                                         /* one subframe: DA, SA, length, LLC/SNAP, payload */
        struct wbuf b;
        wbuf_init(&b, f, sizeof(f));
        wbuf_hdr(&b, WLAN_FC(WLAN_FC_TYPE_DATA, WLAN_ST_QOS_DATA) | WLAN_FC_FROMDS, eth, bssid, bssid, s->seq++);
        wbuf_le16(&b, WLAN_QOS_AMSDU);
        wbuf_put(&b, eth, 12);
        wbuf_be16(&b, (u16)(8 + len - ETH_HLEN));
        wbuf_put(&b, "\xaa\xaa\x03\x00\x00\x00", 6);
        wbuf_put(&b, eth + 12, 2);
        wbuf_put(&b, eth + ETH_HLEN, len - ETH_HLEN);
        n = wbuf_done(&b);
    } else {
        n = ieee80211_build_data(f, sizeof(f), false, eth, bssid, eth + 6, s->sta_wmm, s->seq++, wlan_be16(eth + 12),
                                 eth + ETH_HLEN, len - ETH_HLEN);
    }
    if(!n) return;
    if(p->kind == AP_WPA2) {
        if(group) decrypted = s->gtk_set && s->gtk_key_index == s->a.tx_gtk_index && !memcmp(s->gtk_key, s->a.tx_gtk, 16);
        else decrypted = s->ptk_set && !memcmp(s->ptk_key, s->a.ptk + 32, 16);
        if(!decrypted) {
            s->undecryptable++;
            f[1] |= WLAN_FC_PROTECTED >> 8;             /* what the hardware could not decrypt comes as it was */
        }
    }
    deliver(s, ap, f, n, decrypted);
    s->in++;
    if(s->count % 7 == 0) {                             /* a retransmission of it: the station must drop it */
        f[1] |= WLAN_FC_RETRY >> 8;
        deliver(s, ap, f, n, decrypted);
    }
}

/**** 10/9/26 (review L1): 10.0.2.77, a host on the access point's own network ****/

/* It answers ARP with two A-MSDUs of 64 copies of its reply each, one burst bigger than one device's share
   of the receive queue (net/netbuf.c), and ping. */
#define VHOST_COPIES 64
static const u8 vhost_mac[6] = { 0x02, 0x53, 0x49, 0x4d, 0x00, 0x77 };

/* n copies of one Ethernet frame to the station in one A-MSDU from the access point */
static void burst(struct sim *s, const u8 *eth, u_int len, u_int n) {
    static u8 f[4096];                                  /* the sim thread's alone */
    struct wbuf b;
    u8 bssid[6];
    u_int i;
    int ap = s->assoc;
    if(ap < 0 || len < ETH_HLEN || !s->sta_wmm) return;
    bssid_of(ap, bssid);
    wbuf_init(&b, f, sizeof(f));
    wbuf_hdr(&b, WLAN_FC(WLAN_FC_TYPE_DATA, WLAN_ST_QOS_DATA) | WLAN_FC_FROMDS, eth, bssid, bssid, s->seq++);
    wbuf_le16(&b, WLAN_QOS_AMSDU);
    for(i = 0; i < n; i++) {
        u_int sub = 14 + 8 + len - ETH_HLEN;
        wbuf_put(&b, eth, 12);
        wbuf_be16(&b, (u16)(8 + len - ETH_HLEN));
        wbuf_put(&b, "\xaa\xaa\x03\x00\x00\x00", 6);
        wbuf_put(&b, eth + 12, 2);
        wbuf_put(&b, eth + ETH_HLEN, len - ETH_HLEN);
        if(i + 1 < n) while(sub++ % 4) wbuf_u8(&b, 0);   /* padding, but after the last */
    }
    if(aps[ap].kind == AP_OPEN) deliver(s, ap, f, wbuf_done(&b), false);
    s->in += n;
}

/* an MSDU from the station for the virtual host: answered here, and true; false for anything else */
static bool vhost(struct sim *s, const struct ieee80211_msdu *m) {
    ipaddr_t me = IP4(10, 0, 2, 77);
    u8 r[ETH_HLEN + 1500];
    if(m->type == ETH_P_ARP && m->len >= 28 && wlan_be16(m->payload + 6) == 1 && !memcmp(m->payload + 24, &me, 4)) {
        memcpy(r, m->sa, 6);                            /* who asked */
        memcpy(r + 6, vhost_mac, 6);
        r[12] = 0x08; r[13] = 0x06;
        memcpy(r + 14, m->payload, 6);                  /* hardware and protocol types and lengths */
        r[20] = 0; r[21] = 2;                           /* a reply */
        memcpy(r + 22, vhost_mac, 6);
        memcpy(r + 28, &me, 4);
        memcpy(r + 32, m->payload + 8, 10);             /* the asker's MAC and address */
        burst(s, r, 42, VHOST_COPIES);
        burst(s, r, 42, VHOST_COPIES);
        return true;
    }
    if(memcmp(m->da, vhost_mac, 6)) return false;
    if(m->type == ETH_P_IP && m->len >= 28 && m->len <= 1500 && m->payload[9] == IPPROTO_ICMP) {
        u_int ihl = (m->payload[0] & 15) * 4, total = wlan_be16(m->payload + 2);
        u8 *ip = r + ETH_HLEN;
        if(ihl < 20 || total > m->len || total < ihl + 8 || m->payload[ihl] != ICMP_ECHO) return true;
        memcpy(r, m->sa, 6);
        memcpy(r + 6, vhost_mac, 6);
        r[12] = 0x08; r[13] = 0x00;
        memcpy(ip, m->payload, total);
        memcpy(ip + 12, m->payload + 16, 4);            /* the addresses the other way round */
        memcpy(ip + 16, m->payload + 12, 4);
        ip[8] = 64;
        ip[10] = ip[11] = 0;
        *(u16 *)(ip + 10) = ip_checksum(ip, ihl);
        ip[ihl] = ICMP_ECHO_REPLY;
        ip[ihl + 2] = ip[ihl + 3] = 0;
        *(u16 *)(ip + ihl + 2) = ip_checksum(ip + ihl, total - ihl);
        to_station(s, r, ETH_HLEN + total);
    }
    return true;                                        /* nothing for it goes to eth0 */
}

/**** WPA2: the authenticator (with track C's crypto) ****/

#define KI_VERSION2  0x0002
#define KI_PAIRWISE  0x0008
#define KI_INSTALL   0x0040
#define KI_ACK       0x0080
#define KI_MIC       0x0100
#define KI_SECURE    0x0200
#define KI_ENCRYPTED 0x1000
#define OFF_INFO     5                  /* in an EAPOL-Key frame, from its EAPOL header on */
#define OFF_KEYLEN   7
#define OFF_REPLAY   9
#define OFF_NONCE    17
#define OFF_MIC      81
#define OFF_DLEN     97
#define OFF_DATA     99

static void put_be16(u8 *p, u16 v) { p[0] = (u8)(v >> 8); p[1] = (u8)v; }

static u64 get_be64(const u8 *p) {
    u64 v = 0;
    int i;
    for(i = 0; i < 8; i++) v = (v << 8) | p[i];
    return v;
}

static void put_be64(u8 *p, u64 v) {
    int i;
    for(i = 7; i >= 0; i--) { p[i] = (u8)v; v >>= 8; }
}

/* an EAPOL-Key frame (802.1X-2004 version 2, descriptor type 2), with its MIC when info asks */
static u_int eapol_key(struct sim *s, u8 *buf, u_int size, u16 info, u16 keylen, const u8 *nonce, const u8 *data, u_int dlen) {
    u8 h[SHA1_LEN];
    if(OFF_DATA + dlen > size) return 0;
    memset(buf, 0, OFF_DATA);
    buf[0] = 2;
    buf[1] = 3;
    put_be16(buf + 2, (u16)(OFF_DATA - 4 + dlen));
    buf[4] = 2;
    put_be16(buf + OFF_INFO, info);
    put_be16(buf + OFF_KEYLEN, keylen);
    put_be64(buf + OFF_REPLAY, ++s->a.replay);
    if(nonce) memcpy(buf + OFF_NONCE, nonce, 32);
    put_be16(buf + OFF_DLEN, (u16)dlen);
    if(dlen) memcpy(buf + OFF_DATA, data, dlen);
    if(info & KI_MIC) {
        hmac_sha1(s->a.ptk, 16, buf, OFF_DATA + dlen, h);
        memcpy(buf + OFF_MIC, h, 16);
    }
    return OFF_DATA + dlen;
}

static bool mic_ok(struct sim *s, const u8 *frame, u_int len) {
    u8 copy[512], h[SHA1_LEN];
    if(len > sizeof(copy) || len < OFF_DATA) return false;
    memcpy(copy, frame, len);
    memset(copy + OFF_MIC, 0, 16);
    hmac_sha1(s->a.ptk, 16, copy, len, h);
    return !memcmp(h, frame + OFF_MIC, 16);
}

static void send_eapol(struct sim *s, const u8 *frame, u_int len) {
    u8 f[600], bssid[6];
    bssid_of(s->assoc, bssid);
    deliver(s, s->assoc, f, ieee80211_build_data(f, sizeof(f), false, s->w->mac, bssid, bssid, s->sta_wmm, s->seq++,
                                                 ETH_P_EAPOL, frame, len), false);
}

/* the GTK KDE, padded and wrapped with the KEK (after the AP's RSN element in message 3) */
static u_int key_data(struct sim *s, bool with_rsn, u8 *out) {
    u8 plain[96];
    u_int n = 0;
    if(with_rsn) {
        memcpy(plain, rsn_wpa2, sizeof(rsn_wpa2));
        n = sizeof(rsn_wpa2);
    }
    plain[n++] = 0xdd;
    plain[n++] = 22;
    plain[n++] = 0x00; plain[n++] = 0x0f; plain[n++] = 0xac; plain[n++] = 0x01;
    plain[n++] = s->a.gtk_index & 3;
    plain[n++] = 0;
    memcpy(plain + n, s->a.gtk, 16);
    n += 16;
    if(n % 8 || n < 16) {
        plain[n++] = 0xdd;
        while(n % 8 || n < 16) plain[n++] = 0;
    }
    aes_wrap(s->a.ptk + 16, plain, n, out);
    wipe(plain, sizeof(plain));
    return n + 8;
}

static void send_msg1(struct sim *s) {
    u8 f[160];
    u_int n = eapol_key(s, f, sizeof(f), KI_VERSION2 | KI_PAIRWISE | KI_ACK, 16, s->a.anonce, NULL, 0);
    send_eapol(s, f, n);
    s->a.stage = A_MSG1;
    s->a.resend_at = getticks() + HZ;
}

static void send_msg3(struct sim *s) {
    u8 f[300], kd[136];
    u_int k = key_data(s, true, kd);
    u_int n = eapol_key(s, f, sizeof(f), KI_VERSION2 | KI_PAIRWISE | KI_INSTALL | KI_ACK | KI_MIC | KI_SECURE | KI_ENCRYPTED,
                        16, s->a.anonce, kd, k);
    send_eapol(s, f, n);
    s->a.stage = A_MSG3;
    s->a.resend_at = getticks() + HZ;
}

static void send_group1(struct sim *s) {
    u8 f[300], kd[136];
    u_int k = key_data(s, false, kd);
    u_int n = eapol_key(s, f, sizeof(f), KI_VERSION2 | KI_ACK | KI_MIC | KI_SECURE | KI_ENCRYPTED, 0, NULL, kd, k);
    send_eapol(s, f, n);
    s->a.stage = A_GROUP1;
    s->a.resend_at = getticks() + HZ;
}

static void derive_ptk(struct sim *s) {
    u8 data[76], aa[6];
    const u8 *spa = s->w->mac;
    bssid_of(s->assoc, aa);
    memcpy(data, memcmp(aa, spa, 6) < 0 ? aa : spa, 6);
    memcpy(data + 6, memcmp(aa, spa, 6) < 0 ? spa : aa, 6);
    memcpy(data + 12, memcmp(s->a.anonce, s->a.snonce, 32) < 0 ? s->a.anonce : s->a.snonce, 32);
    memcpy(data + 44, memcmp(s->a.anonce, s->a.snonce, 32) < 0 ? s->a.snonce : s->a.anonce, 32);
    ieee80211_prf_sha1(s->a.pmk, 32, "Pairwise key expansion", data, sizeof(data), s->a.ptk, sizeof(s->a.ptk));
}

/* after the association with SurfSimWPA: message 1 */
static void auth_begin(struct sim *s) {
    const char *pass = WLANSIM_PASSPHRASE;
    if(!s->a.pmk_ready) {
        pbkdf2_sha1((const u8 *)pass, strlen(pass), (const u8 *)aps[s->assoc].ssid, strlen(aps[s->assoc].ssid), 4096,
                    s->a.pmk, 32);
        s->a.pmk_ready = true;
    }
    crypto_random(s->a.anonce, 32);
    if(!s->a.gtk_index) {
        crypto_random(s->a.gtk, 16);
        s->a.gtk_index = 1;
        memcpy(s->a.tx_gtk, s->a.gtk, 16);
        s->a.tx_gtk_index = 1;
    }
    s->a.replay = 0;
    s->a.resends = 0;
    s->a.rekey_at = 0;
    s->ptk_set = s->gtk_set = false;
    send_msg1(s);
}

/* an EAPOL frame from the station (from its EAPOL header on); protected: it came encrypted with the TK
   (review L4: the station protects EAPOL once its pairwise key is in; both are taken) */
static void auth_rx(struct sim *s, const u8 *e, u_int len, bool protected) {
    u16 info;
    if(len < OFF_DATA || e[1] != 3 || e[4] != 2) return;
    info = wlan_be16(e + OFF_INFO);
    if((info & KI_ACK) || !(info & KI_MIC) || get_be64(e + OFF_REPLAY) != s->a.replay) return;
    if(s->a.stage == A_MSG1 && (info & KI_PAIRWISE)) {
        u_int dlen = wlan_be16(e + OFF_DLEN);
        memcpy(s->a.snonce, e + OFF_NONCE, 32);
        derive_ptk(s);
        if(!mic_ok(s, e, len)) return;                  /* a wrong password: message 1 goes again, then reason 15 */
        if(dlen > len - OFF_DATA || dlen != s->sta_rsn_len || memcmp(e + OFF_DATA, s->sta_rsn, dlen)) {
            ap_deauth(s, s->assoc, 17);                 /* not the RSN element of its association request */
            return;
        }
        s->a.resends = 0;
        send_msg3(s);
        return;
    }
    if(s->a.stage == A_MSG3 && (info & KI_PAIRWISE) && mic_ok(s, e, len)) {
        s->a.stage = A_OPEN;
        s->a.rekey_at = getticks() + 3 * HZ;
        kprintf("wlansim: %s: 4-way handshake done, the port is open\n", aps[s->assoc].ssid);
        return;
    }
    if(s->a.stage == A_GROUP1 && !(info & KI_PAIRWISE) && mic_ok(s, e, len)) {
        s->a.stage = A_OPEN;
        memcpy(s->a.tx_gtk, s->a.gtk, 16);
        s->a.tx_gtk_index = s->a.gtk_index;
        kprintf("wlansim: %s: group key handshake done (key %u, its message 2 %s)\n", aps[s->assoc].ssid, s->a.gtk_index,
                protected ? "protected" : "in the clear");
    }
}

static void auth_tick(struct sim *s) {
    u_long now = getticks();
    if(s->assoc < 0 || aps[s->assoc].kind != AP_WPA2 || !have_crypto()) return;
    if(s->a.stage == A_OPEN && s->a.rekey_at && (long)(now - s->a.rekey_at) >= 0) {
        s->a.rekey_at = 0;
        s->a.gtk_index = s->a.gtk_index == 1 ? 2 : 1;
        crypto_random(s->a.gtk, 16);
        s->a.resends = 0;
        send_group1(s);
        return;
    }
    if((s->a.stage == A_MSG1 || s->a.stage == A_MSG3 || s->a.stage == A_GROUP1) && (long)(now - s->a.resend_at) >= 0) {
        if(++s->a.resends > 3) {
            ap_deauth(s, s->assoc, WLAN_REASON_4WAY_TIMEOUT);
            return;
        }
        if(s->a.stage == A_MSG1) send_msg1(s);
        else if(s->a.stage == A_MSG3) send_msg3(s);
        else send_group1(s);
    }
}

/**** what the station sends ****/

static void rx_station_mgmt(struct sim *s, const u8 *f, u_int len) {
    struct ieee80211_hdr h;
    struct ieee80211_rsn r;
    const u8 *ies, *e;
    u8 body[200], rates[12];
    u_int n, ies_len;
    int ap;
    if(ieee80211_parse_hdr(f, len, &h) || h.type != WLAN_FC_TYPE_MGMT) return;
    ap = ap_by_bssid(h.a1);
    if(ap < 0 || memcmp(h.a3, h.a1, 6) || memcmp(h.a2, s->w->mac, 6)) return;
    if(silent(s, ap)) return;
    switch(h.subtype) {
    case WLAN_ST_AUTH:
        if(len - h.hdr_len < 6 || ap != s->tuned) return;
        if(wlan_le16(f + h.hdr_len) != WLAN_AUTH_OPEN || wlan_le16(f + h.hdr_len + 2) != 1) return;
        s->authed = ap;
        s->assoc = -1;
        body[0] = 0; body[1] = 0; body[2] = 2; body[3] = 0; body[4] = 0; body[5] = 0;   /* open, 2, success */
        ap_mgmt(s, ap, WLAN_ST_AUTH, body, 6);
        return;
    case WLAN_ST_ASSOC_REQ: {
        u16 status = 0;
        struct wbuf b;
        if(s->authed != ap || len - h.hdr_len < 4) return;
        ies = f + h.hdr_len + 4;
        ies_len = len - h.hdr_len - 4;
        if(!ieee80211_elems_ok(ies, ies_len)) status = 40;
        e = ieee80211_find_elem(ies, ies_len, WLAN_EID_SSID);
        if(!status && (!e || e[1] != strlen(aps[ap].ssid) || memcmp(e + 2, aps[ap].ssid, e[1]))) status = 1;
        if(!status && !ieee80211_find_elem(ies, ies_len, WLAN_EID_RATES)) status = 18;
        if(!status && (aps[ap].kind == AP_WEP || aps[ap].kind == AP_SAE)) status = 43;
        if(!status && aps[ap].kind == AP_WPA2) {
            e = ieee80211_find_elem(ies, ies_len, WLAN_EID_RSN);
            if(!e || ieee80211_parse_rsn(e, &r)) status = 72;
            else if(r.group != RSN_CIPHER_CCMP) status = 41;
            else if(r.pairwise != RSN_P_CCMP) status = 42;
            else if(r.akm != RSN_A_PSK) status = 43;
            else if(!have_crypto()) status = 1;
            if(!status) {
                s->sta_rsn_len = (u_int)e[1] + 2 <= sizeof(s->sta_rsn) ? (u_int)e[1] + 2 : 0;
                memcpy(s->sta_rsn, e, s->sta_rsn_len);
            }
        }
        s->sta_wmm = aps[ap].wmm && ieee80211_find_vendor(ies, ies_len, WLAN_OUI_MICROSOFT, 2) != NULL;
        wbuf_init(&b, body, sizeof(body));
        wbuf_le16(&b, WLAN_CAP_ESS | (aps[ap].kind != AP_OPEN ? WLAN_CAP_PRIVACY : 0));
        wbuf_le16(&b, status);
        wbuf_le16(&b, status ? 0 : 0xc001);
        n = ieee80211_rates(band_of_ap(ap), rates);
        wbuf_elem(&b, WLAN_EID_RATES, rates, n < 8 ? n : 8);
        if(n > 8) wbuf_elem(&b, WLAN_EID_EXT_RATES, rates + 8, n - 8);
        if(aps[ap].ht && !status) put_ht(&b, ap);
        if(s->sta_wmm && !status) wbuf_elem(&b, WLAN_EID_VENDOR, wmm_param, sizeof(wmm_param));
        ap_mgmt(s, ap, WLAN_ST_ASSOC_RESP, body, wbuf_done(&b));
        if(status) {
            kprintf("wlansim: %s refuses the association (status %u)\n", aps[ap].ssid, status);
            return;
        }
        s->assoc = ap;
        s->next_beacon = getticks() + HZ;
        kprintf("wlansim: %s: the station is associated%s\n", aps[ap].ssid, s->sta_wmm ? " (QoS)" : "");
        if(aps[ap].flaky) {
            s->flaky_assocs++;
            if(s->flaky_assocs == 1) s->flaky_deauth_at = getticks() + 3 * HZ;   /* time for a lease first */
            if(s->flaky_assocs == 2) s->flaky_lost_at = getticks() + HZ;
        }
        if(aps[ap].kind == AP_WPA2) auth_begin(s);
        return;
    }
    case WLAN_ST_DEAUTH:
    case WLAN_ST_DISASSOC:
        if(s->authed == ap || s->assoc == ap) {
            s->authed = s->assoc = -1;
            s->a.stage = A_IDLE;
            if(aps[ap].flaky && len - h.hdr_len >= 2 && wlan_le16(f + h.hdr_len) == WLAN_REASON_LEAVING)
                s->flaky_assocs = 0;                    /* wifi leave */
        }
        return;
    default:
        return;
    }
}

static void rx_station_data(struct sim *s, struct sim_item *it) {
    struct ieee80211_hdr h;
    struct ieee80211_msdu m[4];
    struct netbuf *nb;
    int n, i, ap;
    if(ieee80211_parse_hdr(it->data, it->len, &h) || h.type != WLAN_FC_TYPE_DATA) return;
    if((h.fc & (WLAN_FC_TODS | WLAN_FC_FROMDS)) != WLAN_FC_TODS || memcmp(h.a2, s->w->mac, 6)) return;
    ap = ap_by_bssid(h.a1);
    if(ap < 0 || silent(s, ap)) return;
    if(s->assoc != ap) {
        ap_deauth(s, ap, WLAN_REASON_CLASS3);           /* data from a station that is not associated */
        return;
    }
    n = ieee80211_msdus(it->data, it->len, &h, m, 4);
    for(i = 0; i < n; i++) {
        if(aps[ap].kind == AP_OPEN && vhost(s, &m[i])) continue;   /* 10.0.2.77, on the AP's own network */
        if(m[i].type == ETH_P_EAPOL) {
            if(aps[ap].kind == AP_WPA2 && have_crypto()) {
                if(it->protect && (!it->key_valid || memcmp(it->key, s->a.ptk + 32, 16))) {
                    s->undecryptable++;                 /* "encrypted" with a key that is not the TK */
                    continue;
                }
                auth_rx(s, m[i].payload, m[i].len, it->protect);
            }
            continue;
        }
        if(aps[ap].kind == AP_WPA2) {
            if(s->a.stage != A_OPEN && s->a.stage != A_GROUP1) { s->dropped++; continue; }   /* the port is closed */
            if(!it->protect || !it->key_valid || memcmp(it->key, s->a.ptk + 32, 16)) { s->undecryptable++; continue; }
        }
        if(!s->uplink || m[i].len + ETH_HLEN > NETBUF_SIZE - NETBUF_HEADROOM) { s->dropped++; continue; }
        mutex_lock(&net_lock);                          /* eth0's driver is the stack's, under its lock */
        nb = netbuf_alloc();
        if(nb) {
            u8 *p = (u8 *)netbuf_put(nb, ETH_HLEN + m[i].len);
            memcpy(p, m[i].da, 6);
            memcpy(p + 6, m[i].sa, 6);
            p[12] = (u8)(m[i].type >> 8);
            p[13] = (u8)m[i].type;
            memcpy(p + ETH_HLEN, m[i].payload, m[i].len);
            while(nb->len < 60) *(u8 *)netbuf_put(nb, 1) = 0;
            if(netdev_send(s->uplink, nb) == 0) s->out++;
            netbuf_free(nb);
        }
        mutex_unlock(&net_lock);
    }
}

/**** the simulated firmware ****/

static void enqueue(struct sim *s, int kind, const u8 *data, u_int len, bool protect) {
    struct sim_item *it;
    u_long flags;
    if(s->q_count >= 256 || len > 4096) { s->dropped++; return; }
    it = (struct sim_item *)kalloc(sizeof(*it) + len);
    if(!it) { s->dropped++; return; }
    it->next = NULL;
    it->kind = kind;
    it->protect = protect;
    it->len = len;
    memcpy(it->data, data, len);
    flags = irq_save();
    it->key_valid = s->ptk_set;                         /* the key the "hardware" encrypts with, now */
    memcpy(it->key, s->ptk_key, 16);
    if(s->q_tail) s->q_tail->next = it;
    else s->q_head = it;
    s->q_tail = it;
    s->q_count++;
    irq_restore(flags);
    event_set(&s->kick);
}

static bool scan_hears(const struct wlan_scan_req *req, int ap) {
    u_int i;
    if(!req->nchannels) return true;
    for(i = 0; i < req->nchannels && i < WLAN_MAX_CHANNELS; i++) if(req->channels[i] == aps[ap].channel) return true;
    return false;
}

static void scan_run(struct sim *s) {
    struct wlan_scan_req req = s->scan_req;
    u8 f[512];
    int ap;
    for(ap = 0; ap < NAPS && s->scan_pending; ap++) {
        const struct sim_ap *p = &aps[ap];
        if(!scan_hears(&req, ap) || silent(s, ap)) continue;
        deliver(s, ap, f, ap_frame(s, ap, WLAN_ST_BEACON, f, sizeof(f)), false);
        /* a wildcard probe is answered by all but the hidden one; a probe for a name by its owner */
        if(p->hidden ? (req.ssid_len == strlen(p->ssid) && !memcmp(req.ssid, p->ssid, req.ssid_len)) : true)
            deliver(s, ap, f, ap_frame(s, ap, WLAN_ST_PROBE_RESP, f, sizeof(f)), false);
    }
    if(s->scan_pending) {
        s->scan_pending = false;
        wlan_scan_done(s->w, 0);
    }
}

static void sim_thread(void *arg) {
    struct sim *s = (struct sim *)arg;
    u8 f[512];
    for(;;) {
        struct sim_item *it;
        u_long now, flags;
        event_clear(&s->kick);
        if(!s->q_head) event_wait_timeout(&s->kick, 20);
        for(;;) {
            flags = irq_save();
            it = s->q_head;
            if(it) {
                s->q_head = it->next;
                if(!s->q_head) s->q_tail = NULL;
                s->q_count--;
            }
            irq_restore(flags);
            if(!it) break;
            if(s->started) {
                if(it->kind == IT_MGMT) rx_station_mgmt(s, it->data, it->len);
                else if(it->kind == IT_DATA) rx_station_data(s, it);
                else to_station(s, it->data, it->len);
            }
            kfree(it);
        }
        if(!s->started) continue;
        now = getticks();
        if(s->scan_pending && (long)(now - s->scan_at) >= 0) scan_run(s);
        if(s->stale_done_at && (long)(now - s->stale_done_at) >= 0) {
            s->stale_done_at = 0;
            wlan_scan_done(s->w, -EINTR);               /* the aborted scan's end, as iwx's firmware reports it */
        }
        if(s->assoc >= 0 && !silent(s, s->assoc) && (long)(now - s->next_beacon) >= 0) {
            s->next_beacon = now + HZ;
            deliver(s, s->assoc, f, ap_frame(s, s->assoc, WLAN_ST_BEACON, f, sizeof(f)), false);
        }
        if(s->flaky_deauth_at && (long)(now - s->flaky_deauth_at) >= 0) {
            s->flaky_deauth_at = 0;
            if(s->assoc >= 0 && aps[s->assoc].flaky) ap_deauth(s, s->assoc, WLAN_REASON_INACTIVITY);
        }
        if(s->flaky_lost_at && (long)(now - s->flaky_lost_at) >= 0) {
            s->flaky_lost_at = 0;
            if(s->assoc >= 0 && aps[s->assoc].flaky) {
                kprintf("wlansim: %s goes silent for 4 s\n", aps[s->assoc].ssid);
                s->flaky_silent_until = now + 4 * HZ;
                s->assoc = s->authed = -1;
                wlan_link_lost(s->w);                   /* as a chip that missed its beacons says */
            }
        }
        auth_tick(s);
    }
}

/* eth0's frames that are not only eth0's: under net_lock in the net thread, so only copied */
static void sim_bridge_rx(struct netdev *dev, const u8 *frame, u_int len) {
    struct sim *s = the_sim;
    (void)dev;
    if(!s || !s->started || s->assoc < 0 || len < ETH_HLEN) return;
    if(!(frame[0] & 1) && memcmp(frame, s->w->mac, 6)) return;
    enqueue(s, IT_UPLINK, frame, len, false);
}

/**** wlan_hw_ops ****/

static int sim_start(struct wlan_dev *w) {
    struct sim *s = (struct sim *)w->priv;
    struct netdev *d;
    char m[18];
    for(d = netdev_first(); d && (d == w->netdev || strncmp(d->name, "eth", 3)); d = d->next);
    s->uplink = d;
    if(d) memcpy(w->mac, d->mac, 6);
    else memcpy(w->mac, "\x02\x53\x49\x4d\xff\x00", 6);
    w->mac[5]++;                                        /* eth0's with the last byte one higher */
    if(d) {
        if(!d->ops->ioctl || d->ops->ioctl(d, NETDEV_ADD_RX_ADDR, w->mac))
            kprintf("wlansim: %s takes no second address: frames to %s reach it only if its device passes them all "
                    "(QEMU's usb-net does)\n", d->name, macfmt(w->mac, m));
        d->bridge_rx = sim_bridge_rx;
    }
    s->tuned = s->authed = s->assoc = -1;
    s->left = false;
    s->started = true;
    kprintf("wlansim: the simulated chip is on: %d access points, bridged to %s%s\n", NAPS, d ? d->name : "nothing",
            have_crypto() ? "" : " (no crypto in this kernel: SurfSimWPA refuses stations)");
    return 0;
}

static void sim_stop(struct wlan_dev *w) {
    struct sim *s = (struct sim *)w->priv;
    s->left = true;
    s->started = false;
    s->scan_pending = false;
    s->tuned = s->authed = s->assoc = -1;
    s->ptk_set = s->gtk_set = false;
    s->a.stage = A_IDLE;
}

/* 10/9/26 (review M3): a frame handed over between leave() or stop() and the next join() or start() */
static bool too_late(struct sim *s) {
    if(!s->left) return false;
    if(!s->late++) kprintf("wlansim: a frame handed over after leave() or stop()\n");
    return true;
}

static int sim_scan(struct wlan_dev *w, const struct wlan_scan_req *req) {
    struct sim *s = (struct sim *)w->priv;
    if(!s->started) return -EIO;
    if(s->scan_pending) return -EBUSY;
    s->scan_req = *req;
    s->scan_at = getticks() + ms_to_ticks(600);         /* 10/9/26: as long as a real one, about */
    s->scan_pending = true;
    event_set(&s->kick);
    return 0;
}

static void sim_scan_abort(struct wlan_dev *w) {
    struct sim *s = (struct sim *)w->priv;
    if(!s->scan_pending) return;
    s->scan_pending = false;
    /* 10/9/26 (review L2): iwx's firmware reports the end of an aborted scan too, some time later: here 300 ms,
       inside the next scan when one follows at once, which that end must not end */
    s->stale_done_at = getticks() + ms_to_ticks(300);
}

static int sim_join(struct wlan_dev *w, const struct wlan_bss *bss) {
    struct sim *s = (struct sim *)w->priv;
    s->tuned = ap_by_bssid(bss->bssid);
    s->left = false;
    return 0;
}

static int sim_tx_mgmt(struct wlan_dev *w, const u8 *frame, u_int len) {
    struct sim *s = (struct sim *)w->priv;
    if(too_late(s)) return -1;
    enqueue(s, IT_MGMT, frame, len, false);
    return 0;
}

static int sim_associated(struct wlan_dev *w, u16 aid, const u8 *ies, u_int ies_len) {
    (void)w;
    (void)aid;
    return ieee80211_elems_ok(ies, ies_len) && ieee80211_find_elem(ies, ies_len, WLAN_EID_RATES) ? 0 : -EINVAL;
}

static int sim_set_key(struct wlan_dev *w, const struct wlan_key *key) {
    struct sim *s = (struct sim *)w->priv;
    u_long flags;
    if(key->cipher != WLAN_CIPHER_CCMP || key->len != 16 || (key->pairwise ? key->index != 0 : key->index < 1 || key->index > 3))
        return -EINVAL;
    flags = irq_save();
    if(key->pairwise) {
        memcpy(s->ptk_key, key->key, 16);
        s->ptk_set = true;
    } else {
        memcpy(s->gtk_key, key->key, 16);
        s->gtk_key_index = key->index;
        s->gtk_set = true;
    }
    irq_restore(flags);
    return 0;
}

static int sim_tx_data(struct wlan_dev *w, struct netbuf *nb, bool protect) {
    struct sim *s = (struct sim *)w->priv;
    if(too_late(s)) return -1;
    enqueue(s, IT_DATA, netbuf_data(nb), nb->len, protect);
    return 0;
}

static void sim_leave(struct wlan_dev *w) {
    struct sim *s = (struct sim *)w->priv;
    u_long flags = irq_save();
    s->tuned = -1;
    s->left = true;
    s->ptk_set = s->gtk_set = false;
    wipe(s->ptk_key, sizeof(s->ptk_key));
    wipe(s->gtk_key, sizeof(s->gtk_key));
    irq_restore(flags);
}

static void sim_status(struct wlan_dev *w, char *buf, size_t size) {
    struct sim *s = (struct sim *)w->priv;
    snprintf(buf, size, "wlansim, the simulated chip: %d access points, bridged to %s (%lu frames out, %lu in, %lu dropped, "
             "%lu undecryptable)%s", NAPS, s->uplink ? s->uplink->name : "nothing", s->out, s->in, s->dropped,
             s->undecryptable, have_crypto() ? "" : ", no crypto");
}

static const struct wlan_hw_ops sim_ops = {
    sim_start, sim_stop, sim_scan, sim_scan_abort, sim_join, sim_tx_mgmt, sim_associated, sim_set_key, sim_tx_data,
    sim_leave, sim_status
};

/* from init_drivers() with `wlansim` on the kernel command line only */
int init_wlansim(void) {
    struct sim *s;
    if(!bootinfo_has_arg(&bootinfo, "wlansim")) return 1;
    s = (struct sim *)kcalloc(1, sizeof(*s));
    if(!s) return -1;
    event_init(&s->kick);
    s->tuned = s->authed = s->assoc = -1;
    if(!kthread_create("wlansim", sim_thread, s, PL_HIGH, TF_KTHREAD | TF_DETACHED)) {
        kfree(s);
        return -1;
    }
    the_sim = s;
    s->w = wlan_register(&sim_ops, s);
    if(!s->w) {
        kprintf("wlansim: the station layer refused the simulated chip\n");
        return -1;
    }
    kprintf("wlansim: simulated Wi-Fi chip registered as %s (wifi up starts it)\n", s->w->name);
    return 0;
}
