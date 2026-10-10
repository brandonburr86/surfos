/*
SurfOS Wi-Fi: IEEE 802.11 frames
----------------------
File: ieee80211.h  Date: 10/9/26
----------------------
What the station layer (net/wlan/wlan.c) and the simulated chip (driver/wifi/wlansim.c)
read and write of 802.11 frames (IEEE 802.11-2016 clause 9): the MAC header, the elements of
beacons and probe responses, the RSN element, authentication, association, deauthentication
and Block Ack action frames, and data frames to and from Ethernet II (LLC/SNAP, QoS data,
A-MSDU). net/wlan/ieee80211.c is pure: no allocation, no I/O, no locks; it is host-tested
(tests/host/test_ieee80211.c). Every function checks every length it reads: a malformed
frame is refused (-1 or 0), never read past.

Frames are as wlan.h hands them over: the header first, no FCS. 802.11's multi-byte fields
are little-endian; the ethertype after LLC/SNAP is big-endian, as on Ethernet.
*/

#ifndef _NET_IEEE80211_H
#define _NET_IEEE80211_H

#include <surfos/types.h>
#include <net/wlan.h>

/**** the MAC header ****/

#define WLAN_FC_TYPE_MGMT 0
#define WLAN_FC_TYPE_CTRL 1
#define WLAN_FC_TYPE_DATA 2

/* management subtypes */
#define WLAN_ST_ASSOC_REQ    0
#define WLAN_ST_ASSOC_RESP   1
#define WLAN_ST_REASSOC_REQ  2
#define WLAN_ST_REASSOC_RESP 3
#define WLAN_ST_PROBE_REQ    4
#define WLAN_ST_PROBE_RESP   5
#define WLAN_ST_BEACON       8
#define WLAN_ST_DISASSOC     10
#define WLAN_ST_AUTH         11
#define WLAN_ST_DEAUTH       12
#define WLAN_ST_ACTION       13
/* data subtypes: bit 3 QoS, bit 2 no data */
#define WLAN_ST_DATA         0
#define WLAN_ST_NULL         4
#define WLAN_ST_QOS_DATA     8
#define WLAN_ST_QOS_NULL     12

/* the frame control field as a little-endian u16: version, type and subtype in the low byte,
   the flags in the high one */
#define WLAN_FC(type, subtype) ((u16)(((type) << 2) | ((subtype) << 4)))
#define WLAN_FC_TODS      0x0100
#define WLAN_FC_FROMDS    0x0200
#define WLAN_FC_MOREFRAG  0x0400
#define WLAN_FC_RETRY     0x0800
#define WLAN_FC_PWRMGT    0x1000
#define WLAN_FC_MOREDATA  0x2000
#define WLAN_FC_PROTECTED 0x4000
#define WLAN_FC_ORDER     0x8000            /* +HTC: an HT control field after the header (QoS data, management) */

#define WLAN_HDR_LEN      24                /* frame control, duration, three addresses, sequence control */
#define WLAN_QOS_HDR_LEN  26                /* and the QoS control field */
#define WLAN_QOS_AMSDU    0x0080            /* QoS control: the body is an A-MSDU */

/* a frame's header as ieee80211_parse_hdr() finds it */
struct ieee80211_hdr {
    u16 fc;
    u8 type, subtype;
    const u8 *a1, *a2, *a3, *a4;            /* a4: NULL unless To DS and From DS are both set */
    u16 seq_ctrl;                           /* sequence number << 4 | fragment number */
    bool qos;                               /* a QoS data frame: qos_ctrl holds its QoS control field */
    u16 qos_ctrl;
    u_int hdr_len;                          /* where the body begins */
};

/* management and data frames only (a control frame never comes from the drivers); 0, or -1 when
   the frame is shorter than its header says */
int ieee80211_parse_hdr(const u8 *frame, u_int len, struct ieee80211_hdr *h);

/* 10/9/26 (review L5): a fragment, More Fragments or a fragment number other than 0. Nothing here
   reassembles: a fragment is dropped, whatever its body looks like */
#define WLAN_SEQ_FRAG     0x000f            /* sequence control: the fragment number */
static inline bool ieee80211_is_fragment(const struct ieee80211_hdr *h) {
    return (h->fc & WLAN_FC_MOREFRAG) || (h->seq_ctrl & WLAN_SEQ_FRAG);
}

/**** elements ****/

#define WLAN_EID_SSID          0
#define WLAN_EID_RATES         1
#define WLAN_EID_DS_PARAMS     3
#define WLAN_EID_TIM           5
#define WLAN_EID_COUNTRY       7
#define WLAN_EID_HT_CAP        45
#define WLAN_EID_RSN           48
#define WLAN_EID_EXT_RATES     50
#define WLAN_EID_HT_OPERATION  61
#define WLAN_EID_EXT_CAP       127
#define WLAN_EID_VHT_CAP       191
#define WLAN_EID_VHT_OPERATION 192
#define WLAN_EID_VENDOR        221

#define WLAN_OUI_MICROSOFT     0x0050f2     /* vendor elements: WPA (type 1), WMM (type 2) */
#define WLAN_HT_CAP_LEN        26           /* the HT capabilities element's body */

/* every element of ies[0..len) fits: an id, a length, that many bytes, to the last byte */
bool ieee80211_elems_ok(const u8 *ies, u_int len);
/* 10/9/26 (review L6): how many bytes from the start of ies[0..len) are whole elements: all of them,
   or up to the first that does not fit (cut short, a stray byte at the end), where the list ends as
   Linux reads it. Beacons, probe and association responses are read that far */
u_int ieee80211_elems_whole(const u8 *ies, u_int len);
/* the first element with that id (its id byte: e[1] is its length, e + 2 its body), or NULL */
const u8 *ieee80211_find_elem(const u8 *ies, u_int len, u8 id);
/* the first vendor-specific element with that OUI and OUI type, or NULL */
const u8 *ieee80211_find_vendor(const u8 *ies, u_int len, u32 oui, u8 type);

/**** the RSN element (802.11-2016 9.4.2.25) ****/

#define RSN_SUITE(type) ((u32)0x000fac00 | (type))
#define RSN_CIPHER_TKIP  RSN_SUITE(2)
#define RSN_CIPHER_CCMP  RSN_SUITE(4)
#define RSN_CAP_MFPR     0x0040             /* management frame protection required */
#define RSN_CAP_MFPC     0x0080             /* management frame protection capable */

#define RSN_P_CCMP   1                      /* pairwise suites offered */
#define RSN_P_TKIP   2
#define RSN_P_OTHER  4
#define RSN_A_8021X  1                      /* AKMs offered: 1, 3, 5, 11, 12, 13 (enterprise) */
#define RSN_A_PSK    2                      /* 2 */
#define RSN_A_SAE    4                      /* 8, 9, 24, 25 */
#define RSN_A_OTHER  8                      /* 4 (FT-PSK), 6 (PSK-SHA256), 18 (OWE), ...: none we join with */

struct ieee80211_rsn {
    u16 version;
    u32 group;                              /* the group cipher suite: OUI << 8 | type */
    u_int pairwise;                         /* RSN_P_* */
    u_int akm;                              /* RSN_A_* */
    u16 caps;                               /* the RSN capabilities, 0 when the element ends before them */
};

/* the RSN element (id, length, body); fields left out at its end take the standard's defaults
   (CCMP, CCMP, 802.1X); 0, or -1 when malformed */
int ieee80211_parse_rsn(const u8 *elem, struct ieee80211_rsn *rsn);
/* what a network asks of a station (enum wlan_security): from its RSN element (or NULL), its
   WPA vendor element (or NULL) and the privacy bit of its capability field */
u8 ieee80211_security(const u8 *rsn_elem, const u8 *wpa_elem, u16 capability);
const char *ieee80211_security_name(u8 security);   /* "open", "WPA2-Personal", ... */

/**** beacons and probe responses ****/

#define WLAN_CAP_ESS            0x0001
#define WLAN_CAP_PRIVACY        0x0010
#define WLAN_CAP_SHORT_PREAMBLE 0x0020
#define WLAN_CAP_QOS            0x0200
#define WLAN_CAP_SHORT_SLOT     0x0400

#define IEEE80211_BSS_WMM        1          /* a WMM element, or QoS in the capability field: QoS data */
#define IEEE80211_BSS_PROBE_RESP 2          /* it came in a probe response (else a beacon) */

/* a beacon or probe response into bss (a network seen in a scan) at tick now; flags gets
   IEEE80211_BSS_*. Its elements are read up to the last whole one (ieee80211_elems_whole()), and a
   network with the privacy bit whose RSN element may be what was cut off is WLAN_SEC_OTHER, not WEP.
   The channel comes from the DS parameter set, else the HT operation element,
   else info's; a hidden SSID (none, empty or all zero bytes) leaves ssid_len 0. 0, or -1 when it
   is no beacon or probe response, or malformed */
int ieee80211_parse_bss(const u8 *frame, u_int len, const struct wlan_rx_info *info, u_long now,
                        struct wlan_bss *bss, u_int *flags);
u8 ieee80211_band_of(u8 channel);           /* enum wlan_band: 1-14 are 2.4 GHz */

/**** frames a station sends ****/

/* our rates for a band, as a rates element lists them (500 kb/s units): 1, 2, 5.5, 11, 6, 9, 12,
   18, 24, 36, 48, 54 Mb/s on 2.4 GHz, the eight OFDM ones on 5 GHz; their number */
u_int ieee80211_rates(u8 band, u8 *rates);
/* an HT capabilities element (id, length, 26 bytes) for streams 1 or 2, 20 or 20/40 MHz: short
   guard intervals, one RX STBC stream, no greenfield, no LDPC, 3839-byte A-MSDUs, 64 KB A-MPDUs
   with 8 us spacing; its length (28) */
u_int ieee80211_ht_cap(u8 *buf, u_int streams, bool ht40);

/* each builder writes a whole frame into buf and returns its length, or 0 when it does not fit;
   seq is the sequence number (12 bits) */
u_int ieee80211_build_probe_req(u8 *buf, u_int size, const u8 *sa, const u8 *ssid, u_int ssid_len, u8 band, u16 seq);
u_int ieee80211_build_auth(u8 *buf, u_int size, const u8 *da, const u8 *sa, const u8 *bssid, u16 algorithm,
                           u16 transaction, u16 status, u16 seq);
struct ieee80211_assoc_params {
    u16 capability;                         /* ours: ESS, and the AP's privacy bit, preamble and slot */
    u16 listen_interval;                    /* beacon intervals */
    const u8 *rsn;                          /* our RSN element (WPA2), NULL for an open network */
    u_int rsn_len;
    bool wmm;                               /* the AP has WMM: a WMM information element */
    bool ht;                                /* the AP has HT: our HT capabilities */
    u8 ht_streams;                          /* 1 or 2 */
    bool ht40;
};
u_int ieee80211_build_assoc_req(u8 *buf, u_int size, const struct wlan_bss *bss, const u8 *sa,
                                const struct ieee80211_assoc_params *p, u16 seq);
/* deauthentication or disassociation (subtype WLAN_ST_DEAUTH or WLAN_ST_DISASSOC) */
u_int ieee80211_build_deauth(u8 *buf, u_int size, u8 subtype, const u8 *da, const u8 *sa, const u8 *bssid,
                             u16 reason, u16 seq);
/* the answer to an ADDBA request (an action frame of category Block Ack): declined, with its
   dialog token and parameters; req is the request's body (from the category byte on) */
u_int ieee80211_build_addba_decline(u8 *buf, u_int size, const u8 *da, const u8 *sa, const u8 *bssid,
                                    const u8 *req, u_int req_len, u16 seq);

#define WLAN_AUTH_OPEN 0
#define WLAN_STATUS_SUCCESS           0
#define WLAN_STATUS_UNSPECIFIED       1
#define WLAN_STATUS_REFUSED           37    /* also an ADDBA request declined */
#define WLAN_REASON_UNSPECIFIED       1
#define WLAN_REASON_PREV_AUTH_INVALID 2
#define WLAN_REASON_LEAVING           3     /* the station is leaving */
#define WLAN_REASON_INACTIVITY        4
#define WLAN_REASON_CLASS3            7     /* a class 3 frame from a station that is not associated */
#define WLAN_REASON_4WAY_TIMEOUT      15
#define WLAN_REASON_8021X_FAILED      23
const char *ieee80211_status_name(u16 status);      /* a few words, "" for a code without a name here */
const char *ieee80211_reason_name(u16 reason);

/* authentication, association response, deauthentication and disassociation as they come */
struct ieee80211_auth {
    u16 algorithm, transaction, status;
};
int ieee80211_parse_auth(const u8 *frame, u_int len, struct ieee80211_auth *a);   /* 0 or -1 */
struct ieee80211_assoc_resp {
    u16 capability, status, aid;            /* aid without its two top bits */
    const u8 *ies;                          /* the elements, in the frame */
    u_int ies_len;                          /* up to the last whole one (10/9/26, review L6) */
};
int ieee80211_parse_assoc_resp(const u8 *frame, u_int len, struct ieee80211_assoc_resp *r);   /* 0 or -1 */
int ieee80211_parse_reason(const u8 *frame, u_int len, u16 *reason);   /* deauth, disassoc: 0 or -1 */

/**** data frames and Ethernet II ****/

#define WLAN_LLC_SNAP_LEN 8                 /* AA AA 03 00 00 00 and the ethertype */
#define ETH_P_EAPOL 0x888e

/* a data frame with one MSDU: header (To DS: a1 the BSSID, a2 the source, a3 the destination;
   From DS: a1 the destination, a2 the BSSID, a3 the source), QoS control (TID 0) when qos, then
   LLC/SNAP with type and the payload; its length, or 0 */
u_int ieee80211_build_data(u8 *buf, u_int size, bool to_ds, const u8 *a1, const u8 *a2, const u8 *a3, bool qos,
                           u16 seq, u16 type, const u8 *payload, u_int len);
/* an Ethernet II frame from this station (its source address is a2) to the AP: To DS, QoS data
   when qos; 0 when it does not fit or is no Ethernet II frame */
u_int ieee80211_from_eth(u8 *buf, u_int size, const u8 *eth, u_int eth_len, const u8 *bssid, bool qos, u16 seq);

/* one MSDU of a data frame: as an Ethernet II frame it is da, sa, type, then payload */
struct ieee80211_msdu {
    const u8 *da, *sa;
    u16 type;
    const u8 *payload;
    u_int len;
};
/* the MSDUs of a data frame whose header h describes (From DS or To DS; an A-MSDU has several):
   their number (up to max; a null data frame has none), or -1 when malformed: a body without
   LLC/SNAP, an A-MSDU subframe that runs past the frame, an A-MSDU that begins with an LLC/SNAP
   header where its first destination should be (CVE-2020-24588), a frame with four addresses;
   and -1 for a fragment (10/9/26, review L5): no MSDU is taken from one, not even a whole-looking
   one (CVE-2020-26145, -26146) */
int ieee80211_msdus(const u8 *frame, u_int len, const struct ieee80211_hdr *h, struct ieee80211_msdu *out, u_int max);

/**** writing frames: a buffer that refuses what does not fit ****/

struct wbuf {
    u8 *p;
    u_int len, size;
    bool full;                              /* something did not fit: the frame is no good */
};
void wbuf_init(struct wbuf *b, u8 *p, u_int size);
void wbuf_put(struct wbuf *b, const void *data, u_int n);
void wbuf_u8(struct wbuf *b, u8 v);
void wbuf_le16(struct wbuf *b, u16 v);
void wbuf_be16(struct wbuf *b, u16 v);
void wbuf_elem(struct wbuf *b, u8 id, const void *body, u_int n);   /* id, n, body (n <= 255) */
/* a management frame's header: fc, duration 0, a1 a2 a3, sequence number */
void wbuf_hdr(struct wbuf *b, u16 fc, const u8 *a1, const u8 *a2, const u8 *a3, u16 seq);
u_int wbuf_done(struct wbuf *b);            /* the length, or 0 when anything did not fit */

static inline u16 wlan_le16(const u8 *p) { return (u16)(p[0] | (p[1] << 8)); }
static inline u16 wlan_be16(const u8 *p) { return (u16)((p[0] << 8) | p[1]); }

#endif
