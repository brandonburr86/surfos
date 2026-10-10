/*
SurfOS Wi-Fi: the station layer
--------------------
File: wlan.c    Date: 10/9/26 (Wi-Fi, track S)
--------------------
The 802.11 station between a chip driver (include/net/wlan.h's struct wlan_hw_ops) and
the IP stack: wlan0 as a netdev, the scan list, joining (open system authentication,
association, the WPA2 supplicant of net/wlan/wpa.c), data frames to and from Ethernet II,
and reconnecting when the link goes. The frames themselves are net/wlan/ieee80211.c's.
The brief is docs/superpowers/plans/2026-10-09-wifi.md.

Threads. wlan_register() starts a kernel thread, `wlan`, per device. wlan_rx() (the
driver's thread) copies each frame into the device's queue and returns; the thread parses
them: beacons and probe responses into the scan list at any time, the answers to
authentication and association, deauthentication, EAPOL for the supplicant, data for the
IP stack (netdev_rx()). A command (shell/wifi.c) never drives the chip itself: wlan_up(),
wlan_scan(), wlan_join() and the others post one request to the thread and wait for its
end; a second command meanwhile is told the device is busy, and Ctrl+C or a kill leaves
the request to the thread, which cancels it. Every hw operation but the two that send is
called with hw_lock held (one at a time per device, as wlan.h asks) and never with
net_lock; the netdev's send runs under net_lock and only calls tx_data(), which never
waits.

Joining: the network comes from the scan list (by BSSID, else the strongest with the SSID
and the security asked for: a PMK means WPA2, none an open network); when it is not
there, or for a reconnect, a scan for its SSID comes first (it finds a hidden network
too). join() tunes the chip; authentication (open system) is sent and answered within
200 ms, three times at most; then the association request (our rates for the band, our
RSN element for WPA2, HT capabilities and a WMM element when the AP has them), the same
way; associated() hands the driver the AP's answer. An open network is up then. WPA2
starts the supplicant (wpa_start() with our RSN element and the beacon's), whose keys
reach the driver through set_key(); the link comes up only once it says the keys are in,
within 10 seconds. Up means netdev link_up and a kernel log line. A deauthentication or
disassociation from the AP, or wlan_link_lost() from the driver, takes the link down
(the log says why) and a new join follows 3 seconds later, then after 6, 9, 12 and 15
seconds, five tries at most; then the station gives up. wlan_leave() sends a
deauthentication, then leave(). The PMK is kept only for those reconnects: it is wiped
when a join fails, on leave and down, and when the tries run out. 10/9/26 (review M2):
wlan0's address, gateway and DNS server, and its ARP entries, go when it leaves its
network (leave, down, the rejoins given up, a join to another SSID); a link lost and
joined again keeps them.

Data: an Ethernet frame from the stack becomes a To DS data frame (QoS data with TID 0
when the AP has WMM), protected once the keys are in; frames from the AP become Ethernet
frames, an A-MSDU several. EAPOL (0x888e) goes to the supplicant, never to the IP stack;
on a WPA2 network nothing else passes before the link is up, nor anything that came
unprotected afterwards. Our own broadcasts that the AP sends back are dropped, and so
are retransmissions already seen.

The supplicant is track C's (net/wlan/wpa.c, include/net/wpa.h): its functions are weak
references here, so this file links and runs before it lands; a WPA2 join is refused
until it does. Nothing here logs a key, a PMK or a password.
*/

#include <surfos/types.h>
#include <surfos/console.h>
#include <surfos/task.h>
#include <surfos/sync.h>
#include <surfos/irq.h>
#include <surfos/timer.h>
#include <mm/kalloc.h>
#include <fs/vfs.h>
#include <net/net.h>
#include <net/eth.h>
#include <net/wlan.h>
#include <net/wlan_cmd.h>
#include <net/wpa.h>
#include <net/ieee80211.h>
#include <blibc_common.h>

/* track C's supplicant: absent (NULL) until net/wlan/wpa.c is linked in */
#pragma weak wpa_rsn_ie
#pragma weak wpa_start
#pragma weak wpa_rx_eapol
#pragma weak wpa_tick
#pragma weak wpa_free

#define WLAN_DEVS        4
#define SCAN_MAX         48             /* networks in the list */
#define SCAN_AGE         (60 * HZ)      /* a network not heard for this long leaves it */
#define SCAN_FRESH       (10 * HZ)      /* a join takes the list's word for a network heard this recently */
#define SCAN_TIMEOUT_MS  10000
#define RXQ_MAX          128            /* frames waiting for the thread */
#define FRAME_MAX        4096           /* the longest frame taken: an A-MSDU of 3839 bytes and its header */
#define AUTH_TIMEOUT     (HZ / 5)       /* 200 ms for each answer */
#define AUTH_TRIES       3
#define ASSOC_TIMEOUT    (HZ / 5)
#define ASSOC_TRIES      3
#define KEYS_TIMEOUT     (10 * HZ)      /* the 4-way handshake, all of it */
#define RECONNECT_DELAY  (3 * HZ)       /* times the tries made so far */
#define RECONNECT_TRIES  5
#define LISTEN_INTERVAL  10
#define HT_STREAMS       2              /* the 9560 is 2x2: the integrator reconciles this with the chip */
#define MSDU_MAX         64             /* a 3839-byte A-MSDU of small segments (a TCP ACK is 62 bytes in it) */

enum { ST_DOWN, ST_IDLE, ST_SCAN, ST_AUTH, ST_ASSOC, ST_KEYS, ST_UP, ST_WAIT };
enum { REQ_NONE, REQ_UP, REQ_DOWN, REQ_SCAN, REQ_JOIN, REQ_LEAVE };

struct scan_entry {
    bool used, wmm;
    struct wlan_bss bss;
};

struct rx_item {
    struct rx_item *next;
    struct wlan_rx_info info;
    u_int len;
    u8 frame[];
};

struct wlan_state {
    struct wlan_dev *w;
    struct netdev *nd;
    mutex_t hw_lock;                    /* around every hw operation but tx_mgmt() and tx_data() */
    mutex_t list_lock;                  /* the scan list */
    event_t kick;                       /* the thread has something to do */
    event_t req_event;                  /* a request is done */

    /* wlan_rx()'s queue and the driver's news, under irq_save */
    struct rx_item *rxq_head, *rxq_tail;
    u_int rxq_count;
    volatile bool scan_done_flag, link_lost_flag;
    volatile int scan_done_status;

    /* a command's request: claimed, filled in, posted (under irq_save); done by the thread */
    volatile bool req_claimed, req_started, req_cancel, req_abandoned;
    volatile int req, req_result;
    volatile u_int req_seq, req_done;
    u8 req_ssid[WLAN_SSID_MAX];
    u_int req_ssid_len;
    u8 req_bssid[WLAN_ADDR_LEN];
    bool req_has_bssid, req_has_pmk;
    u8 req_pmk[WPA_PMK_LEN];
    u_int req_timeout_ms;

    /* the thread's own */
    int state;
    bool scanning;
    u_long scan_deadline;
    struct scan_entry list[SCAN_MAX];

    /* the network being joined, or joined */
    u8 ssid[WLAN_SSID_MAX];
    u_int ssid_len;
    u8 pin[WLAN_ADDR_LEN];              /* the BSSID the user named */
    bool has_pin, has_pmk;
    u8 pmk[WPA_PMK_LEN];
    struct wlan_bss cur;
    bool cur_wmm;
    bool hw_joined;                     /* join() was called: leave() undoes it */
    bool authed;                        /* the AP authenticated us: it is told when we go */
    bool user_join;                     /* a command waits for this join */
    bool join_scanned;
    u_long join_since;                  /* the network must have been heard since (its scan's start) */
    u_int tries, reconnects;
    u_long deadline, reconnect_at;
    u16 aid;
    struct wpa_sm *sm;
    bool wpa_done;
    int wpa_status;
    bool ptk_in, gtk_in;
    u8 our_rsn[64];
    u_int our_rsn_len;
    char last[160];                     /* why the last join failed, or the link went down */

    /* what the netdev's send needs, under irq_save */
    bool tx_up, tx_qos, tx_protect;
    u8 tx_bssid[WLAN_ADDR_LEN];
    u16 seq_mgmt, seq_qos;

    /* receive */
    u16 last_seq[17];                   /* the last sequence control per TID ([16]: no QoS), for retries */
    bool last_seq_valid[17];
    u8 ethbuf[NETBUF_SIZE];
    u_long rx_frames, rx_dropped, rx_bad, rx_clear;
};

static struct wlan_dev *devs[WLAN_DEVS];
static u_int ndevs;

static const u8 group_bcast[WLAN_ADDR_LEN] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };

/* not optimised away: the PMK and the request's copy of it */
static void wipe(void *p, size_t n) {
    volatile u8 *q = (volatile u8 *)p;
    while(n--) *q++ = 0;
}

static bool wpa_available(void) {
    return wpa_rsn_ie && wpa_start && wpa_rx_eapol && wpa_tick && wpa_free;
}

static bool is_wpa2(u8 security) {
    return security == WLAN_SEC_WPA2_PSK || security == WLAN_SEC_WPA2_WPA3;
}

static void set_last(struct wlan_state *st, const char *why) {
    strlcpy(st->last, why, sizeof(st->last));
}

static const char *ssid_text(const u8 *ssid, u_int len, char *buf) {
    u_int i;
    if(!len) return "(hidden)";
    for(i = 0; i < len && i < WLAN_SSID_MAX; i++) buf[i] = ssid[i] >= 32 && ssid[i] < 127 ? (char)ssid[i] : '?';
    buf[i] = 0;
    return buf;
}

static u16 next_seq(struct wlan_state *st, bool qos) {
    u_long flags = irq_save();
    u16 s = qos ? st->seq_qos++ : st->seq_mgmt++;
    irq_restore(flags);
    return (u16)(s & 0x0fff);
}

/**** the chip ****/

static void hw_lock(struct wlan_state *st) { mutex_lock(&st->hw_lock); }
static void hw_unlock(struct wlan_state *st) { mutex_unlock(&st->hw_lock); }

static int tx_mgmt(struct wlan_state *st, const u8 *frame, u_int len) {
    struct wlan_dev *w = st->w;
    if(!len || !w->ops->tx_mgmt) return -EIO;
    return w->ops->tx_mgmt(w, frame, len);
}

/* a data frame with one MSDU to the AP: the supplicant's EAPOL frames */
static int send_data(struct wlan_state *st, u16 type, const u8 *payload, u_int len, bool protect) {
    struct wlan_dev *w = st->w;
    struct netbuf *nb = netbuf_alloc();
    u_int n;
    int r;
    if(!nb) return -ENOMEM;
    nb->off = 0;
    n = ieee80211_build_data(nb->data, NETBUF_SIZE, true, st->cur.bssid, w->mac, st->cur.bssid, st->cur_wmm,
                             next_seq(st, st->cur_wmm), type, payload, len);
    if(!n) {
        netbuf_free(nb);
        return -EINVAL;
    }
    nb->len = n;
    r = w->ops->tx_data(w, nb, protect);
    netbuf_free(nb);
    return r ? -EIO : 0;
}

static void send_deauth(struct wlan_state *st, u16 reason) {
    u8 f[32];
    tx_mgmt(st, f, ieee80211_build_deauth(f, sizeof(f), WLAN_ST_DEAUTH, st->cur.bssid, st->w->mac, st->cur.bssid, reason,
                                          next_seq(st, false)));
}

/* what the netdev's send may use; up false stops it */
static void tx_context(struct wlan_state *st, bool up) {
    u_long flags = irq_save();
    st->tx_up = up;
    if(up) {
        memcpy(st->tx_bssid, st->cur.bssid, WLAN_ADDR_LEN);
        st->tx_qos = st->cur_wmm;
        st->tx_protect = is_wpa2(st->cur.security);
    }
    irq_restore(flags);
}

/**** requests from the commands ****/

/* the thread finishes the pending request with result */
static void req_complete(struct wlan_state *st, int result) {
    u_long flags;
    wipe(st->req_pmk, sizeof(st->req_pmk));
    flags = irq_save();
    st->req_result = result;
    st->req_done = st->req_seq;
    st->req = REQ_NONE;
    if(st->req_abandoned) {                             /* its command went (Ctrl+C, a kill): nobody reads it */
        st->req_abandoned = false;
        st->req_claimed = false;
    }
    irq_restore(flags);
    event_set(&st->req_event);
}

/* a command asks the thread for op and waits for its result: -EBUSY when another command's request
   is pending, -EINTR when the command is cut short (the thread then cancels the request itself) */
static int request(struct wlan_dev *w, int op, const u8 *ssid, u_int ssid_len, const u8 *bssid, const u8 *pmk,
                   u_int timeout_ms) {
    struct wlan_state *st = w->st;
    u_long flags = irq_save();
    u_int seq;
    if(st->req_claimed) {
        irq_restore(flags);
        return -EBUSY;
    }
    st->req_claimed = true;
    irq_restore(flags);
    st->req_ssid_len = ssid_len <= WLAN_SSID_MAX ? ssid_len : WLAN_SSID_MAX;
    if(ssid) memcpy(st->req_ssid, ssid, st->req_ssid_len);
    st->req_has_bssid = bssid != NULL;
    if(bssid) memcpy(st->req_bssid, bssid, WLAN_ADDR_LEN);
    st->req_has_pmk = pmk != NULL;
    if(pmk) memcpy(st->req_pmk, pmk, WPA_PMK_LEN);
    st->req_timeout_ms = timeout_ms;
    flags = irq_save();
    st->req_cancel = false;
    st->req_abandoned = false;
    st->req_started = false;
    seq = ++st->req_seq;
    st->req = op;
    irq_restore(flags);
    event_set(&st->kick);
    for(;;) {
        event_clear(&st->req_event);
        flags = irq_save();
        if(st->req_done == seq) {
            int r = st->req_result;
            st->req_claimed = false;
            irq_restore(flags);
            return r;
        }
        if(task_interrupted()) {
            st->req_cancel = true;
            st->req_abandoned = true;
            irq_restore(flags);
            event_set(&st->kick);
            return -EINTR;
        }
        irq_restore(flags);
        event_wait_timeout(&st->req_event, 100);
    }
}

/**** the scan list ****/

static void list_update(struct wlan_state *st, const struct wlan_bss *b, bool wmm) {
    struct scan_entry *e = NULL, *oldest = NULL;
    int i;
    mutex_lock(&st->list_lock);
    for(i = 0; i < SCAN_MAX; i++) {
        struct scan_entry *x = &st->list[i];
        if(x->used && !memcmp(x->bss.bssid, b->bssid, WLAN_ADDR_LEN)) { e = x; break; }
        if(!x->used) { if(!e) e = x; continue; }
        if(!oldest || (long)(x->bss.seen - oldest->bss.seen) < 0) oldest = x;
    }
    if(i == SCAN_MAX && !e) e = oldest;                 /* full: the one heard longest ago goes */
    if(e) {
        bool keep = e->used && !memcmp(e->bss.bssid, b->bssid, WLAN_ADDR_LEN) && !b->ssid_len && e->bss.ssid_len;
        u8 ssid[WLAN_SSID_MAX];
        u_int ssid_len = e->bss.ssid_len;
        memcpy(ssid, e->bss.ssid, WLAN_SSID_MAX);
        e->bss = *b;
        if(keep) {                                      /* a hidden network's beacon: the SSID its probe response told */
            memcpy(e->bss.ssid, ssid, WLAN_SSID_MAX);
            e->bss.ssid_len = (u8)ssid_len;
        }
        e->wmm = wmm;
        e->used = true;
    }
    mutex_unlock(&st->list_lock);
}

static void list_age(struct wlan_state *st) {
    u_long now = getticks();
    int i;
    mutex_lock(&st->list_lock);
    for(i = 0; i < SCAN_MAX; i++)
        if(st->list[i].used && now - st->list[i].bss.seen > SCAN_AGE) st->list[i].used = false;
    mutex_unlock(&st->list_lock);
}

static void list_clear(struct wlan_state *st) {
    mutex_lock(&st->list_lock);
    memset(st->list, 0, sizeof(st->list));
    mutex_unlock(&st->list_lock);
}

/* the network to join: the pinned BSSID, else the strongest with the SSID and the security a PMK
   (WPA2) or none (open) means, heard since the tick since; false when there is none */
static bool list_pick(struct wlan_state *st, u_long since, struct wlan_bss *out, bool *wmm) {
    struct scan_entry *best = NULL;
    int i;
    mutex_lock(&st->list_lock);
    for(i = 0; i < SCAN_MAX; i++) {
        struct scan_entry *e = &st->list[i];
        if(!e->used || (long)(e->bss.seen - since) < 0) continue;
        if(st->has_pin && memcmp(e->bss.bssid, st->pin, WLAN_ADDR_LEN)) continue;
        if(e->bss.ssid_len != st->ssid_len || memcmp(e->bss.ssid, st->ssid, st->ssid_len)) continue;
        if(st->has_pmk ? !is_wpa2(e->bss.security) : e->bss.security != WLAN_SEC_OPEN) continue;
        if(!best || e->bss.rssi > best->bss.rssi) best = e;
    }
    if(best) {
        *out = best->bss;
        *wmm = best->wmm;
    }
    mutex_unlock(&st->list_lock);
    return best != NULL;
}

/**** scans ****/

static int scan_begin(struct wlan_state *st, const u8 *ssid, u_int ssid_len, u_int timeout_ms) {
    struct wlan_dev *w = st->w;
    struct wlan_scan_req req;
    int r;
    if(st->scanning) return -EBUSY;
    memset(&req, 0, sizeof(req));
    if(ssid_len) {
        memcpy(req.ssid, ssid, ssid_len);
        req.ssid_len = (u8)ssid_len;
    }
    st->scan_done_flag = false;
    hw_lock(st);
    r = w->ops->scan(w, &req);
    hw_unlock(st);
    if(r) return r;
    st->scanning = true;
    st->scan_deadline = getticks() + ms_to_ticks(timeout_ms ? timeout_ms : SCAN_TIMEOUT_MS);
    return 0;
}

static void scan_abort(struct wlan_state *st) {
    struct wlan_dev *w = st->w;
    if(!st->scanning) return;
    if(w->ops->scan_abort) {
        hw_lock(st);
        w->ops->scan_abort(w);
        hw_unlock(st);
    }
    st->scanning = false;
    st->scan_done_flag = false;
}

static void join_continue(struct wlan_state *st);

/* the scan ended (status), or timed out (-ETIMEDOUT, aborted) */
static void scan_finished(struct wlan_state *st, int status) {
    st->scanning = false;
    if(st->req == REQ_SCAN && st->req_started) req_complete(st, status);
    if(st->state == ST_SCAN) join_continue(st);
}

/**** joining ****/

static void link_down(struct wlan_state *st, const char *why);

/* undo a join attempt: the AP told (if it had authenticated us), the supplicant gone, the chip's AP
   station gone */
static void unjoin(struct wlan_state *st, u16 reason) {
    struct wlan_dev *w = st->w;
    tx_context(st, false);
    if(st->authed && reason) send_deauth(st, reason);
    st->authed = false;
    if(st->sm) {
        wpa_free(st->sm);
        st->sm = NULL;
    }
    st->wpa_done = false;
    st->ptk_in = st->gtk_in = false;
    if(st->hw_joined) {
        hw_lock(st);
        if(w->ops->leave) w->ops->leave(w);
        hw_unlock(st);
        st->hw_joined = false;
    }
}

/* 10/9/26 (review M2): wlan0's address belonged to the network it was on. When the station leaves it (wifi
   leave, wifi down, the rejoins given up, a join to another network) the address, netmask, gateway and DNS
   server go, with wlan0's ARP entries, under net_lock: no route keeps pointing into a network wlan0 is no
   longer on. A link lost and joined again (the same network) keeps them: the lease is still good. */
static void forget_address(struct wlan_state *st) {
    struct netdev *nd = st->nd;
    char a[16];
    mutex_lock(&net_lock);
    if(nd->ip) kprintf("%s: address %s forgotten (dhcp %s for one on the next network)\n", nd->name, ipfmt(nd->ip, a),
                       nd->name);
    nd->ip = nd->netmask = nd->gateway = nd->dns = 0;
    arp_flush(nd);
    mutex_unlock(&net_lock);
}

static void forget(struct wlan_state *st) {
    wipe(st->pmk, sizeof(st->pmk));
    st->has_pmk = false;
    st->user_join = false;
    st->reconnects = 0;
}

/* this join attempt failed: err for the command waiting for it, why for the log and status */
static void join_fail(struct wlan_state *st, int err, u16 reason, const char *why) {
    char s[WLAN_SSID_MAX + 1];
    struct wlan_dev *w = st->w;
    unjoin(st, reason);
    set_last(st, why);
    if(st->user_join) {
        kprintf("%s: joining \"%s\" failed: %s\n", w->name, ssid_text(st->ssid, st->ssid_len, s), why);
        st->state = ST_IDLE;
        forget(st);
        if(st->req == REQ_JOIN) req_complete(st, err);
        return;
    }
    if(st->reconnects >= RECONNECT_TRIES || err == -EINTR) {
        kprintf("%s: gave up reconnecting to \"%s\" after %u tries: %s\n", w->name, ssid_text(st->ssid, st->ssid_len, s),
                st->reconnects, why);
        st->state = ST_IDLE;
        forget(st);
        forget_address(st);
        return;
    }
    kprintf("%s: joining \"%s\" again failed (try %u of %u): %s; next try in %u s\n", w->name,
            ssid_text(st->ssid, st->ssid_len, s), st->reconnects, RECONNECT_TRIES, why,
            RECONNECT_DELAY * (st->reconnects + 1) / HZ);
    st->state = ST_WAIT;
    st->reconnect_at = getticks() + RECONNECT_DELAY * (st->reconnects + 1);
}

static void send_auth(struct wlan_state *st) {
    u8 f[64];
    tx_mgmt(st, f, ieee80211_build_auth(f, sizeof(f), st->cur.bssid, st->w->mac, st->cur.bssid, WLAN_AUTH_OPEN, 1, 0,
                                        next_seq(st, false)));
    st->deadline = getticks() + AUTH_TIMEOUT;
}

static void send_assoc(struct wlan_state *st) {
    struct ieee80211_assoc_params p;
    u8 f[256];
    memset(&p, 0, sizeof(p));
    p.capability = WLAN_CAP_ESS | (st->cur.capability & WLAN_CAP_PRIVACY);
    if(st->cur.band == WLAN_BAND_2GHZ) p.capability |= WLAN_CAP_SHORT_SLOT | WLAN_CAP_SHORT_PREAMBLE;
    p.listen_interval = LISTEN_INTERVAL;
    if(is_wpa2(st->cur.security)) {
        p.rsn = st->our_rsn;
        p.rsn_len = st->our_rsn_len;
    }
    p.wmm = st->cur_wmm;
    p.ht = st->cur.ht;
    p.ht_streams = HT_STREAMS;
    p.ht40 = st->cur.band == WLAN_BAND_5GHZ;            /* 20 MHz only on 2.4 GHz: no coexistence duties */
    tx_mgmt(st, f, ieee80211_build_assoc_req(f, sizeof(f), &st->cur, st->w->mac, &p, next_seq(st, false)));
    st->deadline = getticks() + ASSOC_TIMEOUT;
}

/* join the network picked: tune, then authenticate */
static void join_bss(struct wlan_state *st, const struct wlan_bss *bss, bool wmm) {
    struct wlan_dev *w = st->w;
    int r;
    st->cur = *bss;
    memcpy(st->cur.ssid, st->ssid, st->ssid_len);       /* a hidden network's beacon has none */
    st->cur.ssid_len = (u8)st->ssid_len;
    st->cur_wmm = wmm;
    memset(st->last_seq_valid, 0, sizeof(st->last_seq_valid));
    if(is_wpa2(bss->security)) {
        st->our_rsn_len = wpa_rsn_ie(st->our_rsn, sizeof(st->our_rsn));
        if(!st->our_rsn_len) {
            join_fail(st, -EINVAL, 0, "the supplicant gave no RSN element");
            return;
        }
    }
    hw_lock(st);
    r = w->ops->join(w, &st->cur);
    hw_unlock(st);
    if(r) {
        join_fail(st, -EIO, 0, "the driver could not tune to the network");
        return;
    }
    st->hw_joined = true;
    st->state = ST_AUTH;
    st->tries = 1;
    send_auth(st);
}

/* a scan for the network to join (with its SSID: a hidden one answers too); its end continues */
static void join_scan(struct wlan_state *st) {
    int r;
    st->join_scanned = true;
    st->join_since = getticks();
    st->state = ST_SCAN;
    r = scan_begin(st, st->ssid, st->ssid_len, SCAN_TIMEOUT_MS);
    if(r && r != -EBUSY) join_fail(st, -EIO, 0, "the driver could not scan");   /* -EBUSY: one runs, its end will do */
}

/* pick the network from the list, else scan for it first (once) */
static void join_continue(struct wlan_state *st) {
    struct wlan_bss bss;
    bool wmm;
    if(list_pick(st, st->join_since, &bss, &wmm)) {
        join_bss(st, &bss, wmm);
        return;
    }
    if(st->join_scanned) {
        join_fail(st, -ENOENT, 0, st->has_pin ? "that access point was not found" : "no such network in range");
        return;
    }
    join_scan(st);
}

/* a join from the beginning: a command's (user) takes the list's word for a network heard in the
   last 10 seconds; a reconnect scans first, since the access point may have moved */
static void join_start(struct wlan_state *st, bool user) {
    st->user_join = user;
    st->join_scanned = false;
    st->join_since = getticks() - SCAN_FRESH;
    if(user) join_continue(st);
    else join_scan(st);
}

static void link_up(struct wlan_state *st) {
    char s[WLAN_SSID_MAX + 1], m[18];
    struct wlan_dev *w = st->w;
    bool again = !st->user_join;
    st->state = ST_UP;
    tx_context(st, true);
    st->nd->link_up = true;
    set_last(st, "");
    kprintf("%s: link up%s: \"%s\" (%s), channel %u, %s GHz, %d dBm, %s\n", w->name, again ? " again" : "",
            ssid_text(st->ssid, st->ssid_len, s), macfmt(st->cur.bssid, m), st->cur.channel,
            st->cur.band == WLAN_BAND_5GHZ ? "5" : "2.4", st->cur.rssi, ieee80211_security_name(st->cur.security));
    st->reconnects = 0;
    st->user_join = false;
    if(st->req == REQ_JOIN) req_complete(st, 0);
}

/* the link went: down, and a new join later */
static void link_down(struct wlan_state *st, const char *why) {
    char s[WLAN_SSID_MAX + 1];
    struct wlan_dev *w = st->w;
    unjoin(st, 0);
    st->nd->link_up = false;
    set_last(st, why);
    kprintf("%s: link down: \"%s\": %s; joining again in %u s\n", w->name, ssid_text(st->ssid, st->ssid_len, s), why,
            RECONNECT_DELAY / HZ);
    st->state = ST_WAIT;
    st->reconnects = 0;
    st->reconnect_at = getticks() + RECONNECT_DELAY;
}

/* leave whatever network there is (wifi leave, wifi down, a new join): the AP told, the PMK wiped */
static void leave_now(struct wlan_state *st, const char *why) {
    char s[WLAN_SSID_MAX + 1];
    struct wlan_dev *w = st->w;
    bool was_up = st->state == ST_UP;
    if(st->state == ST_SCAN) scan_abort(st);
    unjoin(st, WLAN_REASON_LEAVING);
    if(was_up) {
        st->nd->link_up = false;
        kprintf("%s: left \"%s\" (%s)\n", w->name, ssid_text(st->ssid, st->ssid_len, s), why);
    }
    if(st->state != ST_DOWN) st->state = ST_IDLE;
    forget(st);
}

/**** the supplicant's calls ****/

static int wpa_send(void *ctx, const u8 *frame, u_int len) {
    struct wlan_state *st = (struct wlan_state *)ctx;
    return send_data(st, ETH_P_EAPOL, frame, len, false);   /* unprotected, as wpa.h has it */
}

static int wpa_install(void *ctx, const struct wlan_key *key) {
    struct wlan_state *st = (struct wlan_state *)ctx;
    struct wlan_dev *w = st->w;
    int r;
    hw_lock(st);
    r = w->ops->set_key(w, key);
    hw_unlock(st);
    if(r) return r;
    if(key->pairwise) st->ptk_in = true;
    else {
        if(st->state == ST_UP) kprintf("%s: new group key (index %u)\n", w->name, key->index);
        st->gtk_in = true;
    }
    return 0;
}

static void wpa_finished(void *ctx, int status) {
    struct wlan_state *st = (struct wlan_state *)ctx;
    st->wpa_status = status;
    st->wpa_done = true;
}

static const struct wpa_ops supplicant_ops = { wpa_send, wpa_install, wpa_finished };

/* after each call into the supplicant: what it said */
static void wpa_check(struct wlan_state *st) {
    static const char *const why[] = { "", "4-way handshake: no message in time (the password may be wrong)",
                                       "4-way handshake: a MIC did not check (the password may be wrong)",
                                       "4-way handshake: the access point's RSN element differs from its beacon's",
                                       "4-way handshake: malformed or unexpected frames", "the driver refused a key" };
    if(!st->wpa_done || st->state != ST_KEYS) return;
    st->wpa_done = false;
    if(st->wpa_status == WPA_OK) {
        link_up(st);
        return;
    }
    join_fail(st, st->wpa_status == WPA_E_MIC || st->wpa_status == WPA_E_TIMEOUT ? -EACCES : -ECONNRESET,
              WLAN_REASON_4WAY_TIMEOUT,
              st->wpa_status > 0 && st->wpa_status <= WPA_E_KEY ? why[st->wpa_status] : "the 4-way handshake failed");
}

/**** received frames ****/

static void rx_auth(struct wlan_state *st, const u8 *f, u_int len) {
    struct ieee80211_auth a;
    char why[160];
    if(st->state != ST_AUTH || ieee80211_parse_auth(f, len, &a)) return;
    if(a.algorithm != WLAN_AUTH_OPEN || a.transaction != 2) return;
    if(a.status != WLAN_STATUS_SUCCESS) {
        snprintf(why, sizeof(why), "authentication refused: status %u %s", a.status, ieee80211_status_name(a.status));
        join_fail(st, -ECONNREFUSED, 0, why);
        return;
    }
    st->authed = true;
    st->state = ST_ASSOC;
    st->tries = 1;
    send_assoc(st);
}

static void rx_assoc(struct wlan_state *st, const u8 *f, u_int len) {
    struct wlan_dev *w = st->w;
    struct ieee80211_assoc_resp r;
    char why[160];
    int e;
    if(st->state != ST_ASSOC || ieee80211_parse_assoc_resp(f, len, &r)) return;
    if(r.status != WLAN_STATUS_SUCCESS) {
        snprintf(why, sizeof(why), "association refused: status %u %s", r.status, ieee80211_status_name(r.status));
        join_fail(st, -ECONNREFUSED, WLAN_REASON_LEAVING, why);
        return;
    }
    st->aid = r.aid;
    hw_lock(st);
    e = w->ops->associated(w, r.aid, r.ies, r.ies_len);
    hw_unlock(st);
    if(e) {
        join_fail(st, -EIO, WLAN_REASON_LEAVING, "the driver refused the association");
        return;
    }
    if(!is_wpa2(st->cur.security)) {
        link_up(st);
        return;
    }
    st->state = ST_KEYS;
    st->deadline = getticks() + KEYS_TIMEOUT;
    st->wpa_done = false;
    st->sm = wpa_start(st->pmk, st->cur.bssid, w->mac, st->our_rsn, st->our_rsn_len, st->cur.rsn_ie, st->cur.rsn_ie_len,
                       &supplicant_ops, st);
    if(!st->sm) {
        join_fail(st, -ENOMEM, WLAN_REASON_LEAVING, "no memory for the supplicant");
        return;
    }
    wpa_check(st);
}

static void rx_gone(struct wlan_state *st, const u8 *f, u_int len, bool deauth) {
    char why[160];
    u16 reason = 0;
    ieee80211_parse_reason(f, len, &reason);
    snprintf(why, sizeof(why), "%s by the access point: reason %u %s%s", deauth ? "deauthenticated" : "disassociated", reason,
             ieee80211_reason_name(reason), st->state == ST_KEYS ? " (the password may be wrong)" : "");
    st->authed = false;                                 /* it knows */
    if(st->state == ST_UP) link_down(st, why);
    else if(st->state == ST_KEYS) join_fail(st, -EACCES, 0, why);   /* an AP that never takes message 2: the PMK differs */
    else if(st->state >= ST_AUTH && st->state < ST_KEYS) join_fail(st, -ECONNRESET, 0, why);
}

static void rx_action(struct wlan_state *st, const u8 *f, u_int len, const struct ieee80211_hdr *h) {
    u8 out[64];
    const u8 *body = f + h->hdr_len;
    u_int n = len - h->hdr_len;
    if(n >= 2 && body[0] == 3 && body[1] == 0 && st->state >= ST_ASSOC)   /* an ADDBA request: no aggregation here */
        tx_mgmt(st, out, ieee80211_build_addba_decline(out, sizeof(out), st->cur.bssid, st->w->mac, st->cur.bssid, body, n,
                                                       next_seq(st, false)));
}

static void rx_mgmt(struct wlan_state *st, struct rx_item *it, const struct ieee80211_hdr *h) {
    struct wlan_dev *w = st->w;
    struct wlan_bss bss;
    u_int flags;
    if(h->subtype == WLAN_ST_BEACON || h->subtype == WLAN_ST_PROBE_RESP) {
        if(ieee80211_parse_bss(it->frame, it->len, &it->info, getticks(), &bss, &flags)) {
            st->rx_bad++;
            return;
        }
        list_update(st, &bss, (flags & IEEE80211_BSS_WMM) != 0);
        if(st->state >= ST_AUTH && !memcmp(bss.bssid, st->cur.bssid, WLAN_ADDR_LEN)) st->cur.rssi = bss.rssi;
        return;
    }
    /* the rest: from the network being joined, to us (deauthentication perhaps to everyone) */
    if(st->state < ST_AUTH || st->state == ST_WAIT) return;
    if(memcmp(h->a2, st->cur.bssid, WLAN_ADDR_LEN) || memcmp(h->a3, st->cur.bssid, WLAN_ADDR_LEN)) return;
    if(memcmp(h->a1, w->mac, WLAN_ADDR_LEN) && memcmp(h->a1, group_bcast, WLAN_ADDR_LEN)) return;
    st->cur.rssi = it->info.rssi;
    switch(h->subtype) {
    case WLAN_ST_AUTH: rx_auth(st, it->frame, it->len); break;
    case WLAN_ST_ASSOC_RESP: case WLAN_ST_REASSOC_RESP: rx_assoc(st, it->frame, it->len); break;
    case WLAN_ST_DEAUTH: rx_gone(st, it->frame, it->len, true); break;
    case WLAN_ST_DISASSOC: rx_gone(st, it->frame, it->len, false); break;
    case WLAN_ST_ACTION: rx_action(st, it->frame, it->len, h); break;
    default: break;
    }
}

static void rx_data(struct wlan_state *st, struct rx_item *it, const struct ieee80211_hdr *h) {
    struct wlan_dev *w = st->w;
    struct ieee80211_msdu m[MSDU_MAX];
    u_int tid = h->qos ? (h->qos_ctrl & 15) : 16;
    bool wpa = is_wpa2(st->cur.security);
    int n, i;

    if(st->state != ST_KEYS && st->state != ST_UP) return;
    if((h->fc & (WLAN_FC_TODS | WLAN_FC_FROMDS)) != WLAN_FC_FROMDS) return;
    if(memcmp(h->a2, st->cur.bssid, WLAN_ADDR_LEN)) return;
    if(memcmp(h->a1, w->mac, WLAN_ADDR_LEN) && !(h->a1[0] & 1)) return;
    if((h->fc & WLAN_FC_PROTECTED) && !it->info.decrypted) { st->rx_dropped++; return; }   /* not ours to read */
    if(tid > 16) tid = 16;
    if((h->fc & WLAN_FC_RETRY) && st->last_seq_valid[tid] && st->last_seq[tid] == h->seq_ctrl) return;   /* seen */
    st->last_seq[tid] = h->seq_ctrl;
    st->last_seq_valid[tid] = true;
    st->cur.rssi = it->info.rssi;

    n = ieee80211_msdus(it->frame, it->len, h, m, MSDU_MAX);
    if(n < 0) { st->rx_bad++; return; }
    for(i = 0; i < n; i++) {
        if(!memcmp(m[i].sa, w->mac, WLAN_ADDR_LEN)) continue;   /* our own broadcast, sent back by the AP */
        if(m[i].type == ETH_P_EAPOL) {
            if(st->sm) {
                wpa_rx_eapol(st->sm, m[i].payload, m[i].len);
                wpa_check(st);
                if(!st->sm) return;                         /* the handshake failed: everything went with it */
            }
            continue;
        }
        if(st->state != ST_UP) continue;                    /* the port is closed until the keys are in */
        if(wpa && !it->info.decrypted) { st->rx_clear++; continue; }   /* unprotected on a protected network */
        if(m[i].len + ETH_HLEN > NETBUF_SIZE) { st->rx_dropped++; continue; }
        memcpy(st->ethbuf, m[i].da, ETH_ALEN);
        memcpy(st->ethbuf + 6, m[i].sa, ETH_ALEN);
        st->ethbuf[12] = (u8)(m[i].type >> 8);
        st->ethbuf[13] = (u8)m[i].type;
        memcpy(st->ethbuf + ETH_HLEN, m[i].payload, m[i].len);
        netdev_rx(st->nd, st->ethbuf, ETH_HLEN + m[i].len);
    }
}

static void rx_all(struct wlan_state *st) {
    for(;;) {
        struct ieee80211_hdr h;
        struct rx_item *it;
        u_long flags = irq_save();
        it = st->rxq_head;
        if(it) {
            st->rxq_head = it->next;
            if(!st->rxq_head) st->rxq_tail = NULL;
            st->rxq_count--;
        }
        irq_restore(flags);
        if(!it) return;
        st->rx_frames++;
        if(st->state == ST_DOWN) { kfree(it); continue; }
        if(ieee80211_parse_hdr(it->frame, it->len, &h)) st->rx_bad++;
        else if(h.type == WLAN_FC_TYPE_MGMT) rx_mgmt(st, it, &h);
        else rx_data(st, it, &h);
        kfree(it);
    }
}

/**** requests, in the thread ****/

static int do_up(struct wlan_state *st) {
    struct wlan_dev *w = st->w;
    char m[18];
    int r;
    if(w->up) return 0;
    hw_lock(st);
    r = w->ops->start(w);
    hw_unlock(st);
    if(r) {
        kprintf("%s: the chip did not start (%d)\n", w->name, r);
        return r;
    }
    w->up = true;
    memcpy(st->nd->mac, w->mac, ETH_ALEN);
    st->state = ST_IDLE;
    kprintf("%s: up, MAC %s\n", w->name, macfmt(w->mac, m));
    return 0;
}

static void do_down(struct wlan_state *st) {
    struct wlan_dev *w = st->w;
    if(!w->up) return;
    scan_abort(st);
    leave_now(st, "wifi down");
    hw_lock(st);
    w->ops->stop(w);
    hw_unlock(st);
    w->up = false;
    st->state = ST_DOWN;
    st->nd->link_up = false;
    forget_address(st);
    list_clear(st);
    kprintf("%s: down\n", w->name);
}

static void req_start(struct wlan_state *st) {
    struct wlan_dev *w = st->w;
    int r;
    st->req_started = true;
    switch(st->req) {
    case REQ_UP:
        req_complete(st, do_up(st));
        break;
    case REQ_DOWN:
        do_down(st);
        req_complete(st, 0);
        break;
    case REQ_LEAVE:
        if(w->up) {
            leave_now(st, "wifi leave");
            forget_address(st);
        }
        req_complete(st, 0);
        break;
    case REQ_SCAN:
        if(!w->up) { req_complete(st, -ENODEV); break; }
        if(st->state >= ST_SCAN && st->state <= ST_KEYS) { req_complete(st, -EBUSY); break; }   /* a reconnect is joining */
        r = scan_begin(st, NULL, 0, st->req_timeout_ms);
        if(r) req_complete(st, r);
        break;
    case REQ_JOIN:
        if(!w->up) { req_complete(st, -ENODEV); break; }
        if(st->req_has_pmk && !wpa_available()) { req_complete(st, -ENOSYS); break; }
        if(st->state != ST_IDLE) leave_now(st, "joining another network");
        scan_abort(st);
        if(st->ssid_len && (st->ssid_len != st->req_ssid_len || memcmp(st->ssid, st->req_ssid, st->ssid_len)))
            forget_address(st);                         /* another network than the last one: its address is no good */
        memcpy(st->ssid, st->req_ssid, st->req_ssid_len);
        st->ssid_len = st->req_ssid_len;
        st->has_pin = st->req_has_bssid;
        if(st->has_pin) memcpy(st->pin, st->req_bssid, WLAN_ADDR_LEN);
        st->has_pmk = st->req_has_pmk;
        if(st->has_pmk) memcpy(st->pmk, st->req_pmk, WPA_PMK_LEN);
        wipe(st->req_pmk, sizeof(st->req_pmk));
        st->reconnects = 0;
        join_start(st, true);
        break;
    default:
        req_complete(st, -EINVAL);
        break;
    }
}

/* the command went (Ctrl+C, a kill): what it started stops */
static void req_cancel(struct wlan_state *st) {
    if(!st->req_started) {
        req_complete(st, -EINTR);
        return;
    }
    if(st->req == REQ_SCAN) {
        scan_abort(st);
        req_complete(st, -EINTR);
    } else if(st->req == REQ_JOIN) {
        if(st->state == ST_SCAN) scan_abort(st);
        join_fail(st, -EINTR, WLAN_REASON_LEAVING, "cancelled");
        if(st->req == REQ_JOIN) req_complete(st, -EINTR);
    }
}

/**** the thread ****/

static void tick(struct wlan_state *st) {
    u_long now = getticks();
    list_age(st);
    if(st->scanning && (long)(now - st->scan_deadline) >= 0) {
        scan_abort(st);
        scan_finished(st, -ETIMEDOUT);
    }
    switch(st->state) {
    case ST_AUTH:
        if((long)(now - st->deadline) < 0) break;
        if(st->tries >= AUTH_TRIES) { join_fail(st, -ETIMEDOUT, 0, "no answer to authentication"); break; }
        st->tries++;
        send_auth(st);
        break;
    case ST_ASSOC:
        if((long)(now - st->deadline) < 0) break;
        if(st->tries >= ASSOC_TRIES) { join_fail(st, -ETIMEDOUT, WLAN_REASON_LEAVING, "no answer to association"); break; }
        st->tries++;
        send_assoc(st);
        break;
    case ST_KEYS:
        if(st->sm) {
            wpa_tick(st->sm);
            wpa_check(st);
        }
        if(st->state == ST_KEYS && (long)(now - st->deadline) >= 0)
            join_fail(st, -EACCES, WLAN_REASON_4WAY_TIMEOUT, "the 4-way handshake took too long (the password may be wrong)");
        break;
    case ST_UP:
        if(st->sm) wpa_tick(st->sm);                    /* group key renewals */
        break;
    case ST_WAIT:
        if(st->scanning || (long)(now - st->reconnect_at) < 0) break;
        st->reconnects++;
        join_start(st, false);
        break;
    default:
        break;
    }
}

static bool work_waiting(struct wlan_state *st) {
    return st->rxq_head || st->scan_done_flag || st->link_lost_flag || (st->req != REQ_NONE && (!st->req_started || st->req_cancel));
}

static void wlan_thread(void *arg) {
    struct wlan_dev *w = (struct wlan_dev *)arg;
    struct wlan_state *st = w->st;
    u_long last_tick = getticks();
    for(;;) {
        event_clear(&st->kick);
        if(!work_waiting(st)) event_wait_timeout(&st->kick, 100);
        rx_all(st);
        if(st->scan_done_flag) {
            st->scan_done_flag = false;
            if(st->scanning) scan_finished(st, st->scan_done_status);
        }
        if(st->link_lost_flag) {
            st->link_lost_flag = false;
            if(st->state == ST_UP) link_down(st, "the access point is gone (beacons lost)");
            else if(st->state >= ST_AUTH && st->state <= ST_KEYS) join_fail(st, -ECONNRESET, 0, "the access point is gone");
        }
        if(st->req != REQ_NONE) {
            if(st->req_cancel) {
                st->req_cancel = false;
                req_cancel(st);
            } else if(!st->req_started) {
                req_start(st);
            }
        }
        if(getticks() - last_tick >= HZ / 10) {
            last_tick = getticks();
            tick(st);
        }
    }
}

/**** wlan0, the netdev ****/

/* an Ethernet frame from the IP stack, under net_lock: a data frame to the AP, never waiting */
static int wlan_netdev_send(struct netdev *dev, struct netbuf *nb) {
    struct wlan_dev *w = (struct wlan_dev *)dev->priv;
    struct wlan_state *st = w->st;
    struct netbuf *out;
    u8 bssid[WLAN_ADDR_LEN];
    bool up, qos, protect;
    u_long flags;
    u16 seq;
    u_int n;
    int r;
    flags = irq_save();
    up = st->tx_up;
    qos = st->tx_qos;
    protect = st->tx_protect;
    memcpy(bssid, st->tx_bssid, WLAN_ADDR_LEN);
    irq_restore(flags);
    if(!up || nb->len < ETH_HLEN || memcmp(netbuf_data(nb) + 6, w->mac, ETH_ALEN)) return -1;   /* only ours: no bridging */
    out = netbuf_alloc();
    if(!out) return -1;
    out->off = 0;
    seq = next_seq(st, qos);
    n = ieee80211_from_eth(out->data, NETBUF_SIZE, netbuf_data(nb), nb->len, bssid, qos, seq);
    if(!n) {
        netbuf_free(out);
        return -1;
    }
    out->len = n;
    r = w->ops->tx_data(w, out, protect);
    netbuf_free(out);
    return r ? -1 : 0;
}

static const struct netdev_ops wlan_netdev_ops = { wlan_netdev_send, NULL };

/**** what the drivers call ****/

struct wlan_dev *wlan_register(const struct wlan_hw_ops *ops, void *priv) {
    struct wlan_dev *w;
    struct wlan_state *st;
    struct netdev *nd;
    if(!ops || !ops->start || !ops->stop || !ops->scan || !ops->join || !ops->tx_mgmt || !ops->associated ||
       !ops->set_key || !ops->tx_data || !ops->leave) return NULL;
    if(ndevs >= WLAN_DEVS) return NULL;
    w = (struct wlan_dev *)kcalloc(1, sizeof(*w));
    st = (struct wlan_state *)kcalloc(1, sizeof(*st));
    nd = (struct netdev *)kcalloc(1, sizeof(*nd));
    if(!w || !st || !nd) {
        kfree(w);
        kfree(st);
        kfree(nd);
        return NULL;
    }
    snprintf(w->name, sizeof(w->name), "wlan%u", ndevs);
    w->ops = ops;
    w->priv = priv;
    w->st = st;
    w->netdev = nd;
    st->w = w;
    st->nd = nd;
    st->state = ST_DOWN;
    mutex_init(&st->hw_lock, "wlan-hw");
    mutex_init(&st->list_lock, "wlan-list");
    event_init(&st->kick);
    event_init(&st->req_event);
    strlcpy(nd->name, w->name, sizeof(nd->name));       /* netdev_register() keeps it */
    nd->ops = &wlan_netdev_ops;
    nd->priv = w;
    nd->mtu = ETH_MTU;
    if(!kthread_create("wlan", wlan_thread, w, PL_HIGH, TF_KTHREAD | TF_DETACHED)) {
        kfree(w);
        kfree(st);
        kfree(nd);
        return NULL;
    }
    netdev_register(nd);
    devs[ndevs++] = w;
    kprintf("%s: Wi-Fi station (wifi up starts it)\n", w->name);
    return w;
}

void wlan_rx(struct wlan_dev *w, const u8 *frame, u_int len, const struct wlan_rx_info *info) {
    struct wlan_state *st;
    struct rx_item *it;
    u_long flags;
    if(!w || !(st = w->st) || !frame || !info) return;
    if(len < WLAN_HDR_LEN || len > FRAME_MAX || st->rxq_count >= RXQ_MAX) {
        st->rx_dropped++;
        return;
    }
    it = (struct rx_item *)kalloc(sizeof(*it) + len);
    if(!it) {
        st->rx_dropped++;
        return;
    }
    it->next = NULL;
    it->info = *info;
    it->len = len;
    memcpy(it->frame, frame, len);
    flags = irq_save();
    if(st->rxq_tail) st->rxq_tail->next = it;
    else st->rxq_head = it;
    st->rxq_tail = it;
    st->rxq_count++;
    irq_restore(flags);
    event_set(&st->kick);
}

void wlan_scan_done(struct wlan_dev *w, int status) {
    if(!w || !w->st) return;
    w->st->scan_done_status = status;
    w->st->scan_done_flag = true;
    event_set(&w->st->kick);
}

void wlan_link_lost(struct wlan_dev *w) {
    if(!w || !w->st) return;
    w->st->link_lost_flag = true;
    event_set(&w->st->kick);
}

/**** what the wifi command calls ****/

struct wlan_dev *wlan_find(const char *name) {
    u_int i;
    if(!name) return ndevs ? devs[0] : NULL;
    for(i = 0; i < ndevs; i++) if(!strcmp(devs[i]->name, name)) return devs[i];
    return NULL;
}

int wlan_up(struct wlan_dev *w) {
    if(!w) return -ENODEV;
    return request(w, REQ_UP, NULL, 0, NULL, NULL, 0);
}

int wlan_down_wait(struct wlan_dev *w) {
    if(!w) return -ENODEV;
    return request(w, REQ_DOWN, NULL, 0, NULL, NULL, 0);
}

void wlan_down(struct wlan_dev *w) {
    wlan_down_wait(w);
}

int wlan_scan(struct wlan_dev *w, u_int timeout_ms) {
    if(!w) return -ENODEV;
    return request(w, REQ_SCAN, NULL, 0, NULL, NULL, timeout_ms ? timeout_ms : SCAN_TIMEOUT_MS);
}

u_int wlan_bss_list(struct wlan_dev *w, struct wlan_bss *out, u_int max) {
    struct wlan_state *st;
    u_long now = getticks();
    u_int n = 0, i, j;
    if(!w || !(st = w->st)) return 0;
    mutex_lock(&st->list_lock);
    for(i = 0; i < SCAN_MAX && n < max; i++) {
        struct wlan_bss *b = &st->list[i].bss;
        if(!st->list[i].used || now - b->seen > SCAN_AGE) continue;
        for(j = n; j > 0 && out[j - 1].rssi < b->rssi; j--) out[j] = out[j - 1];   /* strongest first */
        out[j] = *b;
        n++;
    }
    mutex_unlock(&st->list_lock);
    return n;
}

int wlan_join(struct wlan_dev *w, const u8 *ssid, u_int ssid_len, const u8 *bssid, const u8 *pmk) {
    if(!w) return -ENODEV;
    if(!ssid || !ssid_len || ssid_len > WLAN_SSID_MAX) return -EINVAL;
    return request(w, REQ_JOIN, ssid, ssid_len, bssid, pmk, 0);
}

int wlan_leave_wait(struct wlan_dev *w) {
    if(!w) return -ENODEV;
    return request(w, REQ_LEAVE, NULL, 0, NULL, NULL, 0);
}

void wlan_leave(struct wlan_dev *w) {
    wlan_leave_wait(w);
}

/* state, SSID, BSSID, channel, signal; then lines for the last failure, the driver and the counters */
void wlan_describe(struct wlan_dev *w, char *buf, size_t size) {
    static const char *const joining[] = { "", "", "looking for", "authenticating with", "associating with",
                                           "4-way handshake with", "", "" };
    struct wlan_state *st;
    char s[WLAN_SSID_MAX + 1], m[18], drv[160];
    int n = 0, state;
    if(!size) return;
    buf[0] = 0;
    if(!w || !(st = w->st)) return;
    state = st->state;
    switch(state) {
    case ST_DOWN:
        n = snprintf(buf, size, "down (wifi up starts it)");
        break;
    case ST_IDLE:
        n = snprintf(buf, size, "up, not associated%s", st->scanning ? ", scanning" : "");
        break;
    case ST_UP:
        n = snprintf(buf, size, "associated with \"%s\" (%s), channel %u, %s GHz, signal %d dBm, %s%s%s",
                     ssid_text(st->ssid, st->ssid_len, s), macfmt(st->cur.bssid, m), st->cur.channel,
                     st->cur.band == WLAN_BAND_5GHZ ? "5" : "2.4", st->cur.rssi, ieee80211_security_name(st->cur.security),
                     st->cur_wmm ? ", QoS" : "", st->cur.ht ? ", HT" : "");
        break;
    case ST_WAIT:
        n = snprintf(buf, size, "link lost: joining \"%s\" again (try %u of %u)", ssid_text(st->ssid, st->ssid_len, s),
                     st->reconnects + 1, RECONNECT_TRIES);
        break;
    default:
        n = snprintf(buf, size, "%s \"%s\"", joining[state], ssid_text(st->ssid, st->ssid_len, s));
        if(state > ST_SCAN) n += snprintf(buf + n, size - n, " (%s), channel %u", macfmt(st->cur.bssid, m), st->cur.channel);
        break;
    }
    if(n < 0 || (size_t)n >= size) return;
    if(st->last[0] && state != ST_UP) n += snprintf(buf + n, size - n, "\nlast: %s", st->last);
    if((size_t)n >= size) return;
    if(w->ops->status) {
        drv[0] = 0;
        hw_lock(st);
        w->ops->status(w, drv, sizeof(drv));
        hw_unlock(st);
        if(drv[0]) n += snprintf(buf + n, size - n, "\ndriver: %s", drv);
        if((size_t)n >= size) return;
    }
    snprintf(buf + n, size - n, "\nframes: %lu received, %lu dropped, %lu malformed, %lu unprotected dropped",
             st->rx_frames, st->rx_dropped, st->rx_bad, st->rx_clear);
}
