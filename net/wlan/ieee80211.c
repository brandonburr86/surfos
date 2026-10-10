/*
SurfOS Wi-Fi: IEEE 802.11 frames
--------------------
File: ieee80211.c  Date: 10/9/26 (Wi-Fi, track S)
--------------------
The frames of a station (include/net/ieee80211.h; IEEE 802.11-2016 clause 9): the MAC
header; beacons and probe responses parsed into a struct wlan_bss (SSID, channel, signal,
rates, HT and VHT, security from the RSN element and the privacy bit); the RSN element;
probe requests, open system authentication, association requests (our rates for the band,
our RSN element, HT capabilities and a WMM element when the AP has them), deauthentication
and the answer that declines an ADDBA request; authentication and association responses;
data frames to and from Ethernet II (LLC/SNAP, QoS data with TID 0, A-MSDU on receive).

Pure: no allocation, no I/O, no locks, so tests/host/test_ieee80211.c runs this file as it
is. Lengths come from the air and are never trusted: every field is read only after the
bytes it needs are known to be there, and a frame that is shorter than it says, whose
elements run past its end or whose SSID is longer than 32 bytes is refused whole.
*/

#include <surfos/types.h>
#include <net/net.h>
#include <net/wlan.h>
#include <net/ieee80211.h>
#include <blibc_common.h>

static const u8 broadcast[WLAN_ADDR_LEN] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };
static const u8 llc_snap[6] = { 0xaa, 0xaa, 0x03, 0x00, 0x00, 0x00 };

/**** writing ****/

void wbuf_init(struct wbuf *b, u8 *p, u_int size) {
    b->p = p;
    b->len = 0;
    b->size = size;
    b->full = false;
}

void wbuf_put(struct wbuf *b, const void *data, u_int n) {
    if(b->full || n > b->size - b->len) {
        b->full = true;
        return;
    }
    memcpy(b->p + b->len, data, n);
    b->len += n;
}

void wbuf_u8(struct wbuf *b, u8 v) {
    wbuf_put(b, &v, 1);
}

void wbuf_le16(struct wbuf *b, u16 v) {
    u8 x[2];
    x[0] = (u8)v;
    x[1] = (u8)(v >> 8);
    wbuf_put(b, x, 2);
}

void wbuf_be16(struct wbuf *b, u16 v) {
    u8 x[2];
    x[0] = (u8)(v >> 8);
    x[1] = (u8)v;
    wbuf_put(b, x, 2);
}

void wbuf_elem(struct wbuf *b, u8 id, const void *body, u_int n) {
    if(n > 255) {
        b->full = true;
        return;
    }
    wbuf_u8(b, id);
    wbuf_u8(b, (u8)n);
    if(n) wbuf_put(b, body, n);
}

void wbuf_hdr(struct wbuf *b, u16 fc, const u8 *a1, const u8 *a2, const u8 *a3, u16 seq) {
    wbuf_le16(b, fc);
    wbuf_le16(b, 0);                                    /* duration: the hardware's */
    wbuf_put(b, a1, WLAN_ADDR_LEN);
    wbuf_put(b, a2, WLAN_ADDR_LEN);
    wbuf_put(b, a3, WLAN_ADDR_LEN);
    wbuf_le16(b, (u16)((seq & 0x0fff) << 4));           /* fragment 0 */
}

u_int wbuf_done(struct wbuf *b) {
    return b->full ? 0 : b->len;
}

/**** the header ****/

int ieee80211_parse_hdr(const u8 *f, u_int len, struct ieee80211_hdr *h) {
    u_int need = WLAN_HDR_LEN;
    memset(h, 0, sizeof(*h));
    if(!f || len < WLAN_HDR_LEN) return -1;
    h->fc = wlan_le16(f);
    if(h->fc & 3) return -1;                            /* protocol version 0 only */
    h->type = (u8)((h->fc >> 2) & 3);
    h->subtype = (u8)((h->fc >> 4) & 15);
    if(h->type != WLAN_FC_TYPE_MGMT && h->type != WLAN_FC_TYPE_DATA) return -1;
    h->a1 = f + 4;
    h->a2 = f + 10;
    h->a3 = f + 16;
    h->seq_ctrl = wlan_le16(f + 22);
    if(h->type == WLAN_FC_TYPE_DATA) {
        if((h->fc & (WLAN_FC_TODS | WLAN_FC_FROMDS)) == (WLAN_FC_TODS | WLAN_FC_FROMDS)) {
            if(len < need + WLAN_ADDR_LEN) return -1;
            h->a4 = f + need;
            need += WLAN_ADDR_LEN;
        }
        if(h->subtype & 8) {                            /* QoS: the QoS control field, then +HTC with Order */
            if(len < need + 2) return -1;
            h->qos = true;
            h->qos_ctrl = wlan_le16(f + need);
            need += 2;
            if(h->fc & WLAN_FC_ORDER) need += 4;
        }
    } else if(h->fc & WLAN_FC_ORDER) {
        need += 4;                                      /* a management frame's HT control field */
    }
    if(len < need) return -1;
    h->hdr_len = need;
    return 0;
}

/**** elements ****/

bool ieee80211_elems_ok(const u8 *ies, u_int len) {
    u_int off = 0;
    while(off < len) {
        if(len - off < 2 || ies[off + 1] > len - off - 2) return false;
        off += 2 + ies[off + 1];
    }
    return true;
}

const u8 *ieee80211_find_elem(const u8 *ies, u_int len, u8 id) {
    u_int off = 0;
    while(len - off >= 2 && ies[off + 1] <= len - off - 2) {
        if(ies[off] == id) return ies + off;
        off += 2 + ies[off + 1];
        if(off >= len) break;
    }
    return NULL;
}

const u8 *ieee80211_find_vendor(const u8 *ies, u_int len, u32 oui, u8 type) {
    u_int off = 0;
    while(len - off >= 2 && ies[off + 1] <= len - off - 2) {
        const u8 *e = ies + off;
        if(e[0] == WLAN_EID_VENDOR && e[1] >= 4 && e[2] == (u8)(oui >> 16) && e[3] == (u8)(oui >> 8) &&
           e[4] == (u8)oui && e[5] == type) return e;
        off += 2 + e[1];
        if(off >= len) break;
    }
    return NULL;
}

/**** the RSN element ****/

static u32 suite(const u8 *p) {
    return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3];
}

static u_int pairwise_bit(u32 s) {
    if(s == RSN_CIPHER_CCMP) return RSN_P_CCMP;
    if(s == RSN_CIPHER_TKIP) return RSN_P_TKIP;
    return RSN_P_OTHER;
}

static u_int akm_bit(u32 s) {
    if((s & 0xffffff00) != RSN_SUITE(0)) return RSN_A_OTHER;
    switch(s & 0xff) {
    case 1: case 3: case 5: case 11: case 12: case 13: return RSN_A_8021X;
    case 2: return RSN_A_PSK;
    case 8: case 9: case 24: case 25: return RSN_A_SAE;
    default: return RSN_A_OTHER;
    }
}

/* a suite list (a count, then that many suites) at *p; its bits, or -1 when it does not fit */
static int suite_list(const u8 **p, const u8 *end, u_int (*bit)(u32), u_int *bits) {
    u_int n, i;
    if(end - *p < 2) return -1;
    n = wlan_le16(*p);
    *p += 2;
    if(!n || (u_int)(end - *p) / 4 < n) return -1;
    *bits = 0;
    for(i = 0; i < n; i++, *p += 4) *bits |= bit(suite(*p));
    return 0;
}

int ieee80211_parse_rsn(const u8 *e, struct ieee80211_rsn *r) {
    const u8 *p, *end;
    u_int n;
    memset(r, 0, sizeof(*r));
    if(!e || e[0] != WLAN_EID_RSN || e[1] < 2) return -1;
    p = e + 2;
    end = p + e[1];
    r->version = wlan_le16(p);
    p += 2;
    if(r->version != 1) return -1;
    r->group = RSN_CIPHER_CCMP;                         /* what a short element means (9.4.2.25.1) */
    r->pairwise = RSN_P_CCMP;
    r->akm = RSN_A_8021X;
    if(p == end) return 0;
    if(end - p < 4) return -1;
    r->group = suite(p);
    p += 4;
    if(p == end) return 0;
    if(suite_list(&p, end, pairwise_bit, &r->pairwise)) return -1;
    if(p == end) return 0;
    if(suite_list(&p, end, akm_bit, &r->akm)) return -1;
    if(p == end) return 0;
    if(end - p < 2) return -1;
    r->caps = wlan_le16(p);
    p += 2;
    if(p == end) return 0;
    if(end - p < 2) return -1;                          /* PMKIDs */
    n = wlan_le16(p);
    p += 2;
    if((u_int)(end - p) / 16 < n) return -1;
    p += 16 * n;
    if(p == end) return 0;
    if(end - p < 4) return -1;                          /* the group management cipher */
    return 0;                                           /* anything after it: later extensions */
}

u8 ieee80211_security(const u8 *rsn_elem, const u8 *wpa_elem, u16 capability) {
    struct ieee80211_rsn r;
    if(rsn_elem) {
        if(ieee80211_parse_rsn(rsn_elem, &r)) return WLAN_SEC_OTHER;
        /* WPA2-Personal as wpa.c does it: PSK, CCMP for both, no protected management frames
           required (a WPA3 transition network offers them, requires them not) */
        if((r.akm & RSN_A_PSK) && (r.pairwise & RSN_P_CCMP) && r.group == RSN_CIPHER_CCMP && !(r.caps & RSN_CAP_MFPR))
            return (r.akm & RSN_A_SAE) ? WLAN_SEC_WPA2_WPA3 : WLAN_SEC_WPA2_PSK;
        if(r.akm & RSN_A_SAE) return WLAN_SEC_WPA3_SAE;
        if(r.akm & RSN_A_8021X) return WLAN_SEC_ENTERPRISE;
        return WLAN_SEC_OTHER;
    }
    if(wpa_elem) return WLAN_SEC_OTHER;                 /* WPA (version 1) only */
    return (capability & WLAN_CAP_PRIVACY) ? WLAN_SEC_WEP : WLAN_SEC_OPEN;
}

const char *ieee80211_security_name(u8 security) {
    switch(security) {
    case WLAN_SEC_OPEN: return "open";
    case WLAN_SEC_WEP: return "WEP";
    case WLAN_SEC_WPA2_PSK: return "WPA2-Personal";
    case WLAN_SEC_WPA2_WPA3: return "WPA2/WPA3-Personal";
    case WLAN_SEC_WPA3_SAE: return "WPA3-Personal";
    case WLAN_SEC_ENTERPRISE: return "Enterprise (802.1X)";
    default: return "other (WPA, TKIP, OWE)";
    }
}

/**** beacons and probe responses ****/

u8 ieee80211_band_of(u8 channel) {
    return channel <= 14 ? WLAN_BAND_2GHZ : WLAN_BAND_5GHZ;
}

static bool all_zero(const u8 *p, u_int n) {
    u_int i;
    for(i = 0; i < n; i++) if(p[i]) return false;
    return true;
}

static void add_rates(struct wlan_bss *bss, const u8 *e) {
    u_int i;
    for(i = 0; e && i < e[1] && bss->nrates < sizeof(bss->rates); i++) bss->rates[bss->nrates++] = e[2 + i];
}

int ieee80211_parse_bss(const u8 *f, u_int len, const struct wlan_rx_info *info, u_long now,
                        struct wlan_bss *bss, u_int *flags) {
    struct ieee80211_hdr h;
    const u8 *ies, *e, *rsn;
    u_int ies_len;
    u8 channel = 0;

    *flags = 0;
    if(ieee80211_parse_hdr(f, len, &h) || h.type != WLAN_FC_TYPE_MGMT) return -1;
    if(h.subtype != WLAN_ST_BEACON && h.subtype != WLAN_ST_PROBE_RESP) return -1;
    if(len - h.hdr_len < 12) return -1;                 /* timestamp, beacon interval, capability */
    if(h.a3[0] & 1) return -1;                          /* a group address is no BSSID */
    ies = f + h.hdr_len + 12;
    ies_len = len - h.hdr_len - 12;
    if(!ieee80211_elems_ok(ies, ies_len)) return -1;

    memset(bss, 0, sizeof(*bss));
    memcpy(bss->bssid, h.a3, WLAN_ADDR_LEN);
    bss->beacon_interval = wlan_le16(f + h.hdr_len + 8);
    bss->capability = wlan_le16(f + h.hdr_len + 10);
    if(!(bss->capability & WLAN_CAP_ESS)) return -1;   /* an IBSS or a mesh: nothing a station joins */

    e = ieee80211_find_elem(ies, ies_len, WLAN_EID_SSID);
    if(e) {
        if(e[1] > WLAN_SSID_MAX) return -1;
        if(!all_zero(e + 2, e[1])) {                    /* hidden: no SSID, an empty one, or zero bytes */
            memcpy(bss->ssid, e + 2, e[1]);
            bss->ssid_len = e[1];
        }
    }

    e = ieee80211_find_elem(ies, ies_len, WLAN_EID_DS_PARAMS);
    if(e && e[1] >= 1) channel = e[2];
    if(!channel) {
        e = ieee80211_find_elem(ies, ies_len, WLAN_EID_HT_OPERATION);
        if(e && e[1] >= 1) channel = e[2];              /* its primary channel */
    }
    if(!channel && info) channel = info->channel;
    bss->channel = channel;
    bss->band = channel ? ieee80211_band_of(channel) : (info ? info->band : WLAN_BAND_2GHZ);
    bss->rssi = info ? info->rssi : 0;

    add_rates(bss, ieee80211_find_elem(ies, ies_len, WLAN_EID_RATES));
    add_rates(bss, ieee80211_find_elem(ies, ies_len, WLAN_EID_EXT_RATES));

    e = ieee80211_find_elem(ies, ies_len, WLAN_EID_HT_CAP);
    if(e && e[1] >= WLAN_HT_CAP_LEN) {
        bss->ht = true;
        memcpy(bss->ht_cap, e + 2, WLAN_HT_CAP_LEN);
    }
    e = ieee80211_find_elem(ies, ies_len, WLAN_EID_VHT_CAP);
    if(e && e[1] >= 12) bss->vht = true;

    rsn = ieee80211_find_elem(ies, ies_len, WLAN_EID_RSN);
    bss->security = ieee80211_security(rsn, ieee80211_find_vendor(ies, ies_len, WLAN_OUI_MICROSOFT, 1), bss->capability);
    if(rsn) {
        if((u_int)rsn[1] + 2 <= sizeof(bss->rsn_ie)) {
            memcpy(bss->rsn_ie, rsn, rsn[1] + 2);
            bss->rsn_ie_len = (u8)(rsn[1] + 2);
        } else if(bss->security == WLAN_SEC_WPA2_PSK || bss->security == WLAN_SEC_WPA2_WPA3) {
            bss->security = WLAN_SEC_OTHER;             /* message 3 is checked against it: it must be kept whole */
        }
    }

    if(ieee80211_find_vendor(ies, ies_len, WLAN_OUI_MICROSOFT, 2) || (bss->capability & WLAN_CAP_QOS))
        *flags |= IEEE80211_BSS_WMM;
    if(h.subtype == WLAN_ST_PROBE_RESP) *flags |= IEEE80211_BSS_PROBE_RESP;
    bss->seen = now;
    return 0;
}

/**** what a station sends ****/

static const u8 rates_2ghz[12] = { 0x02, 0x04, 0x0b, 0x16, 0x0c, 0x12, 0x18, 0x24, 0x30, 0x48, 0x60, 0x6c };
static const u8 rates_5ghz[8] = { 0x0c, 0x12, 0x18, 0x24, 0x30, 0x48, 0x60, 0x6c };

u_int ieee80211_rates(u8 band, u8 *rates) {
    if(band == WLAN_BAND_5GHZ) {
        memcpy(rates, rates_5ghz, sizeof(rates_5ghz));
        return sizeof(rates_5ghz);
    }
    memcpy(rates, rates_2ghz, sizeof(rates_2ghz));
    return sizeof(rates_2ghz);
}

/* the supported rates element and, past eight rates, the extended one */
static void put_rates(struct wbuf *b, u8 band) {
    u8 r[12];
    u_int n = ieee80211_rates(band, r);
    wbuf_elem(b, WLAN_EID_RATES, r, n < 8 ? n : 8);
    if(n > 8) wbuf_elem(b, WLAN_EID_EXT_RATES, r + 8, n - 8);
}

u_int ieee80211_ht_cap(u8 *buf, u_int streams, bool ht40) {
    u16 info = 0x000c | 0x0020 | 0x0100;                /* SM power save off, short GI 20 MHz, RX STBC 1 */
    if(ht40) info |= 0x0002 | 0x0040;                   /* 20/40 MHz, short GI 40 MHz */
    memset(buf, 0, 2 + WLAN_HT_CAP_LEN);
    buf[0] = WLAN_EID_HT_CAP;
    buf[1] = WLAN_HT_CAP_LEN;
    buf[2] = (u8)info;
    buf[3] = (u8)(info >> 8);
    buf[4] = 3 | (6 << 2);                              /* A-MPDU: 64 KB at most, 8 us apart at least */
    buf[5] = 0xff;                                      /* MCS 0-7 */
    if(streams >= 2) buf[6] = 0xff;                     /* MCS 8-15 */
    buf[5 + 12] = 0x01;                                 /* the TX MCS set is the RX set */
    return 2 + WLAN_HT_CAP_LEN;
}

u_int ieee80211_build_probe_req(u8 *buf, u_int size, const u8 *sa, const u8 *ssid, u_int ssid_len, u8 band, u16 seq) {
    struct wbuf b;
    if(ssid_len > WLAN_SSID_MAX) return 0;
    wbuf_init(&b, buf, size);
    wbuf_hdr(&b, WLAN_FC(WLAN_FC_TYPE_MGMT, WLAN_ST_PROBE_REQ), broadcast, sa, broadcast, seq);
    wbuf_elem(&b, WLAN_EID_SSID, ssid, ssid_len);
    put_rates(&b, band);
    return wbuf_done(&b);
}

u_int ieee80211_build_auth(u8 *buf, u_int size, const u8 *da, const u8 *sa, const u8 *bssid, u16 algorithm,
                           u16 transaction, u16 status, u16 seq) {
    struct wbuf b;
    wbuf_init(&b, buf, size);
    wbuf_hdr(&b, WLAN_FC(WLAN_FC_TYPE_MGMT, WLAN_ST_AUTH), da, sa, bssid, seq);
    wbuf_le16(&b, algorithm);
    wbuf_le16(&b, transaction);
    wbuf_le16(&b, status);
    return wbuf_done(&b);
}

/* the WMM information element of a station: version 1, no U-APSD */
static const u8 wmm_info[7] = { 0x00, 0x50, 0xf2, 0x02, 0x00, 0x01, 0x00 };

u_int ieee80211_build_assoc_req(u8 *buf, u_int size, const struct wlan_bss *bss, const u8 *sa,
                                const struct ieee80211_assoc_params *p, u16 seq) {
    struct wbuf b;
    if(bss->ssid_len > WLAN_SSID_MAX) return 0;
    wbuf_init(&b, buf, size);
    wbuf_hdr(&b, WLAN_FC(WLAN_FC_TYPE_MGMT, WLAN_ST_ASSOC_REQ), bss->bssid, sa, bss->bssid, seq);
    wbuf_le16(&b, p->capability);
    wbuf_le16(&b, p->listen_interval);
    wbuf_elem(&b, WLAN_EID_SSID, bss->ssid, bss->ssid_len);
    put_rates(&b, bss->band);
    if(p->rsn && p->rsn_len) {                          /* the whole element, as the supplicant made it */
        if(p->rsn_len < 2 || p->rsn[0] != WLAN_EID_RSN || (u_int)p->rsn[1] + 2 != p->rsn_len) return 0;
        wbuf_put(&b, p->rsn, p->rsn_len);
    }
    if(p->ht) {
        u8 ht[2 + WLAN_HT_CAP_LEN];
        wbuf_put(&b, ht, ieee80211_ht_cap(ht, p->ht_streams, p->ht40));
    }
    if(p->wmm) wbuf_elem(&b, WLAN_EID_VENDOR, wmm_info, sizeof(wmm_info));   /* vendor elements come last */
    return wbuf_done(&b);
}

u_int ieee80211_build_deauth(u8 *buf, u_int size, u8 subtype, const u8 *da, const u8 *sa, const u8 *bssid,
                             u16 reason, u16 seq) {
    struct wbuf b;
    if(subtype != WLAN_ST_DEAUTH && subtype != WLAN_ST_DISASSOC) return 0;
    wbuf_init(&b, buf, size);
    wbuf_hdr(&b, WLAN_FC(WLAN_FC_TYPE_MGMT, subtype), da, sa, bssid, seq);
    wbuf_le16(&b, reason);
    return wbuf_done(&b);
}

u_int ieee80211_build_addba_decline(u8 *buf, u_int size, const u8 *da, const u8 *sa, const u8 *bssid,
                                    const u8 *req, u_int req_len, u16 seq) {
    struct wbuf b;
    /* the request: category 3 (Block Ack), action 0, dialog token, parameters, timeout, starting sequence */
    if(req_len < 9 || req[0] != 3 || req[1] != 0) return 0;
    wbuf_init(&b, buf, size);
    wbuf_hdr(&b, WLAN_FC(WLAN_FC_TYPE_MGMT, WLAN_ST_ACTION), da, sa, bssid, seq);
    wbuf_u8(&b, 3);
    wbuf_u8(&b, 1);                                     /* ADDBA response */
    wbuf_u8(&b, req[2]);
    wbuf_le16(&b, WLAN_STATUS_REFUSED);
    wbuf_put(&b, req + 3, 4);                           /* its parameters and timeout */
    return wbuf_done(&b);
}

/**** codes in words ****/

const char *ieee80211_status_name(u16 status) {
    switch(status) {
    case 0: return "success";
    case 1: return "unspecified failure";
    case 10: return "it cannot support everything asked for";
    case 12: return "denied for another reason";
    case 13: return "authentication algorithm not supported";
    case 14: return "authentication out of sequence";
    case 16: return "authentication timed out";
    case 17: return "the access point has too many stations";
    case 18: return "a basic rate is missing";
    case 30: return "rejected for now, try again later";
    case 31: return "management frame protection policy";
    case 37: return "request declined";
    case 40: return "an element is not valid";
    case 41: return "the group cipher is not valid";
    case 42: return "the pairwise cipher is not valid";
    case 43: return "the AKM is not valid";
    case 44: return "RSN element version not supported";
    case 45: return "RSN capabilities not valid";
    case 46: return "cipher suite rejected by policy";
    default: return "";
    }
}

const char *ieee80211_reason_name(u16 reason) {
    switch(reason) {
    case 1: return "unspecified";
    case 2: return "the authentication is no longer valid";
    case 3: return "leaving";
    case 4: return "inactivity";
    case 5: return "the access point is busy";
    case 6: return "not authenticated";
    case 7: return "not associated";
    case 8: return "leaving the network";
    case 9: return "not authenticated";
    case 13: return "an element is not valid";
    case 14: return "MIC failure";
    case 15: return "4-way handshake timeout";
    case 16: return "group key handshake timeout";
    case 17: return "the 4-way handshake's elements differ";
    case 23: return "802.1X authentication failed";
    case 34: return "too many frames unacknowledged";
    default: return "";
    }
}

/**** what a station receives ****/

/* a management frame of one of the subtypes, with at least min bytes of body; the body, or NULL */
static const u8 *mgmt_body(const u8 *f, u_int len, u8 st1, u8 st2, u_int min, u_int *body_len) {
    struct ieee80211_hdr h;
    if(ieee80211_parse_hdr(f, len, &h) || h.type != WLAN_FC_TYPE_MGMT) return NULL;
    if(h.subtype != st1 && h.subtype != st2) return NULL;
    if(len - h.hdr_len < min) return NULL;
    *body_len = len - h.hdr_len;
    return f + h.hdr_len;
}

int ieee80211_parse_auth(const u8 *f, u_int len, struct ieee80211_auth *a) {
    u_int n;
    const u8 *p = mgmt_body(f, len, WLAN_ST_AUTH, WLAN_ST_AUTH, 6, &n);
    if(!p) return -1;
    a->algorithm = wlan_le16(p);
    a->transaction = wlan_le16(p + 2);
    a->status = wlan_le16(p + 4);
    return 0;
}

int ieee80211_parse_assoc_resp(const u8 *f, u_int len, struct ieee80211_assoc_resp *r) {
    u_int n;
    const u8 *p = mgmt_body(f, len, WLAN_ST_ASSOC_RESP, WLAN_ST_REASSOC_RESP, 6, &n);
    if(!p) return -1;
    r->capability = wlan_le16(p);
    r->status = wlan_le16(p + 2);
    r->aid = wlan_le16(p + 4) & 0x3fff;
    r->ies = p + 6;
    r->ies_len = n - 6;
    if(!ieee80211_elems_ok(r->ies, r->ies_len)) return -1;
    return 0;
}

int ieee80211_parse_reason(const u8 *f, u_int len, u16 *reason) {
    u_int n;
    const u8 *p = mgmt_body(f, len, WLAN_ST_DEAUTH, WLAN_ST_DISASSOC, 2, &n);
    if(!p) return -1;
    *reason = wlan_le16(p);
    return 0;
}

/**** data frames ****/

u_int ieee80211_build_data(u8 *buf, u_int size, bool to_ds, const u8 *a1, const u8 *a2, const u8 *a3, bool qos,
                           u16 seq, u16 type, const u8 *payload, u_int len) {
    struct wbuf b;
    u16 fc = WLAN_FC(WLAN_FC_TYPE_DATA, qos ? WLAN_ST_QOS_DATA : WLAN_ST_DATA) | (to_ds ? WLAN_FC_TODS : WLAN_FC_FROMDS);
    wbuf_init(&b, buf, size);
    wbuf_hdr(&b, fc, a1, a2, a3, seq);
    if(qos) wbuf_le16(&b, 0);                           /* TID 0, normal acknowledgement, no A-MSDU */
    wbuf_put(&b, llc_snap, sizeof(llc_snap));
    wbuf_be16(&b, type);
    if(len) wbuf_put(&b, payload, len);
    return wbuf_done(&b);
}

u_int ieee80211_from_eth(u8 *buf, u_int size, const u8 *eth, u_int eth_len, const u8 *bssid, bool qos, u16 seq) {
    u16 type;
    if(eth_len < 14) return 0;
    type = wlan_be16(eth + 12);
    if(type < 0x0600) return 0;                         /* an 802.3 length: no Ethernet II frame */
    return ieee80211_build_data(buf, size, true, bssid, eth + 6, eth, qos, seq, type, eth + 14, eth_len - 14);
}

/* LLC/SNAP (RFC 1042, or 802.1H's bridge tunnel) at the start of an MSDU: its ethertype */
static int snap(const u8 *p, u_int n, u16 *type) {
    if(n < WLAN_LLC_SNAP_LEN) return -1;
    if(p[0] != 0xaa || p[1] != 0xaa || p[2] != 0x03 || p[3] != 0 || p[4] != 0 || (p[5] != 0 && p[5] != 0xf8)) return -1;
    *type = wlan_be16(p + 6);
    return *type >= 0x0600 ? 0 : -1;
}

static int msdu(struct ieee80211_msdu *m, const u8 *da, const u8 *sa, const u8 *p, u_int n) {
    if(snap(p, n, &m->type)) return -1;
    m->da = da;
    m->sa = sa;
    m->payload = p + WLAN_LLC_SNAP_LEN;
    m->len = n - WLAN_LLC_SNAP_LEN;
    return 0;
}

int ieee80211_msdus(const u8 *f, u_int len, const struct ieee80211_hdr *h, struct ieee80211_msdu *out, u_int max) {
    const u8 *body, *da, *sa;
    u_int blen, off = 0, n = 0;
    if(h->type != WLAN_FC_TYPE_DATA || h->a4 || len < h->hdr_len) return -1;
    if(ieee80211_is_fragment(h)) return -1;             /* not reassembled: nothing is taken from it */
    if(h->subtype & 4) return 0;                        /* null data: nothing in it */
    switch(h->fc & (WLAN_FC_TODS | WLAN_FC_FROMDS)) {
    case WLAN_FC_FROMDS: da = h->a1; sa = h->a3; break;
    case WLAN_FC_TODS: da = h->a3; sa = h->a2; break;
    default: return -1;                                 /* no BSS: an IBSS frame */
    }
    body = f + h->hdr_len;
    blen = len - h->hdr_len;
    if(!(h->qos && (h->qos_ctrl & WLAN_QOS_AMSDU))) {
        if(!max) return 0;
        return msdu(&out[0], da, sa, body, blen) ? -1 : 1;
    }
    /* an A-MSDU: subframes of DA, SA, length, an MSDU, padding to four bytes but after the last */
    if(blen >= 6 && !memcmp(body, llc_snap, 6)) return -1;   /* an MPDU's body passed off as an A-MSDU */
    while(off < blen) {
        u_int sub;
        if(blen - off < 14) return -1;
        sub = wlan_be16(body + off + 12);
        if(sub > blen - off - 14) return -1;
        if(n < max) {
            if(msdu(&out[n], body + off, body + off + 6, body + off + 14, sub)) return -1;
            n++;
        }
        off += 14 + sub;
        if(off < blen) {
            u_int pad = (4 - (14 + sub) % 4) % 4;
            if(pad > blen - off) break;                 /* padding after the last one: the end */
            off += pad;
        }
    }
    return (int)n;
}
