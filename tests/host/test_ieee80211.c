/* Host-side unit tests for net/wlan/ieee80211.c (10/9/26, Wi-Fi track S): the 802.11 frames
   of the station layer. Beacons and probe responses are built by hand, shaped like the
   owner's gateway (an XB-style AP: WPA2/WPA3 transition on 2.4 GHz with TIM, country, ERP,
   HT, extended capabilities, WMM and vendor elements), like its 5 GHz radio (no DS element:
   the channel from HT operation, VHT), like its open hotspot, and like networks SurfOS will
   not join (WEP, WPA3 only, enterprise, TKIP, WPA1, PMF required). Every frame is also
   parsed cut short at every length, and with every byte changed, from the end of a page
   whose next page is unmapped: a read past the end of a frame is a crash here. Then the
   frames a station sends, byte for byte, and data frames to and from Ethernet II, A-MSDU
   included. Built like the kernel objects and run by `make unittest`. */
#include <stdio.h>
#include <string.h>
#include <surfos/types.h>
#include <net/wlan.h>
#include <net/ieee80211.h>

void exit(int status);
/* the host's mmap and mprotect (Linux i386/x86-64 numbers), for frames that end at an unmapped page */
void *mmap(void *addr, size_t len, int prot, int flags, int fd, long off);
int mprotect(void *addr, size_t len, int prot);
#define PROT_NONE 0
#define PROT_READ 1
#define PROT_WRITE 2
#define MAP_PRIVATE 2
#define MAP_ANONYMOUS 0x20
#define PAGE 4096

static int failures, checks;

static void check(int ok, const char *what) {
    checks++;
    if(!ok) { failures++; printf("FAIL: %s\n", what); }
}

static u8 *guard;               /* a page, then an unmapped one */

/* a copy of the frame that ends where the mapped memory does */
static u8 *at_edge(const u8 *f, u_int len) {
    u8 *p = guard + PAGE - len;
    memcpy(p, f, len);
    return p;
}

static const u8 ap1[6] = { 0x42, 0x75, 0xc3, 0x4b, 0x21, 0x09 };   /* the 2.4 GHz radio's BSSID */
static const u8 ap2[6] = { 0x42, 0x75, 0xc3, 0x52, 0x21, 0x0a };   /* 5 GHz */
static const u8 ap3[6] = { 0x42, 0x75, 0xc3, 0x52, 0x21, 0x0c };   /* the open hotspot */
static const u8 sta[6] = { 0x52, 0x54, 0x00, 0x12, 0x34, 0x57 };
static const u8 bcast[6] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };

/**** beacons, built element by element ****/

static void beacon_start(struct wbuf *b, u8 *buf, u_int size, u8 subtype, const u8 *bssid, u16 interval, u16 cap) {
    static const u8 tsf[8] = { 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x00 };
    wbuf_init(b, buf, size);
    wbuf_hdr(b, WLAN_FC(WLAN_FC_TYPE_MGMT, subtype), subtype == WLAN_ST_BEACON ? bcast : sta, bssid, bssid, 1234);
    wbuf_put(b, tsf, 8);
    wbuf_le16(b, interval);
    wbuf_le16(b, cap);
}

/* RSN: version 1, group, pairwise list, AKM list, capabilities (when caps >= 0) */
static u_int rsn(u8 *e, u8 group, const u8 *pw, u_int npw, const u8 *akm, u_int nakm, int caps) {
    u_int n = 2, i;
    e[0] = WLAN_EID_RSN;
    e[2] = 1; e[3] = 0;
    n = 4;
    e[n++] = 0x00; e[n++] = 0x0f; e[n++] = 0xac; e[n++] = group;
    e[n++] = (u8)npw; e[n++] = 0;
    for(i = 0; i < npw; i++) { e[n++] = 0x00; e[n++] = 0x0f; e[n++] = 0xac; e[n++] = pw[i]; }
    e[n++] = (u8)nakm; e[n++] = 0;
    for(i = 0; i < nakm; i++) { e[n++] = 0x00; e[n++] = 0x0f; e[n++] = 0xac; e[n++] = akm[i]; }
    if(caps >= 0) { e[n++] = (u8)caps; e[n++] = (u8)(caps >> 8); }
    e[1] = (u8)(n - 2);
    return n;
}

static const u8 ccmp[1] = { 4 }, tkip_ccmp[2] = { 2, 4 }, psk[1] = { 2 }, psk_sae[2] = { 2, 8 }, sae[1] = { 8 },
                dot1x[1] = { 1 }, ft_psk[1] = { 4 };
static const u8 rates_b[8] = { 0x82, 0x84, 0x8b, 0x96, 0x0c, 0x12, 0x18, 0x24 };
static const u8 rates_x[4] = { 0x30, 0x48, 0x60, 0x6c };
static const u8 rates_a[8] = { 0x8c, 0x12, 0x98, 0x24, 0xb0, 0x48, 0x60, 0x6c };
static const u8 tim[4] = { 0x00, 0x01, 0x00, 0x00 };
static const u8 country[6] = { 'U', 'S', 0x20, 0x01, 0x0b, 0x1e };
static const u8 ext_cap[8] = { 0x04, 0x00, 0x08, 0x00, 0x00, 0x00, 0x00, 0x40 };
static const u8 wmm_param[24] = { 0x00, 0x50, 0xf2, 0x02, 0x01, 0x01, 0x80, 0x00, 0x03, 0xa4, 0x00, 0x00, 0x27, 0xa4,
                                  0x00, 0x00, 0x42, 0x43, 0x5e, 0x00, 0x62, 0x32, 0x2f, 0x00 };
static const u8 wps[10] = { 0x00, 0x50, 0xf2, 0x04, 0x10, 0x4a, 0x00, 0x01, 0x10, 0x00 };
static const u8 bcm[9] = { 0x00, 0x10, 0x18, 0x02, 0x00, 0x00, 0x1c, 0x00, 0x00 };

static void ht_cap_elem(struct wbuf *b) {
    u8 ht[26];
    memset(ht, 0, sizeof(ht));
    ht[0] = 0xef; ht[1] = 0x19;                         /* LDPC, 20/40, SGI, ... */
    ht[2] = 0x17;
    ht[3] = 0xff; ht[4] = 0xff;
    wbuf_elem(b, WLAN_EID_HT_CAP, ht, sizeof(ht));
}

static void ht_op_elem(struct wbuf *b, u8 primary) {
    u8 op[22];
    memset(op, 0, sizeof(op));
    op[0] = primary;
    op[1] = 0x05;                                       /* secondary above, any width */
    wbuf_elem(b, WLAN_EID_HT_OPERATION, op, sizeof(op));
}

/* the gateway's 2.4 GHz radio: HouseOfBurr, WPA2/WPA3 transition, channel 11 */
static u_int beacon_2g(u8 *buf, u_int size, u8 subtype) {
    struct wbuf b;
    u8 r[40];
    u8 erp = 0;
    beacon_start(&b, buf, size, subtype, ap1, 100, WLAN_CAP_ESS | WLAN_CAP_PRIVACY | WLAN_CAP_SHORT_SLOT | 0x0001);
    wbuf_elem(&b, WLAN_EID_SSID, "HouseOfBurr", 11);
    wbuf_elem(&b, WLAN_EID_RATES, rates_b, 8);
    wbuf_elem(&b, WLAN_EID_DS_PARAMS, "\x0b", 1);
    if(subtype == WLAN_ST_BEACON) wbuf_elem(&b, WLAN_EID_TIM, tim, 4);
    wbuf_elem(&b, WLAN_EID_COUNTRY, country, 6);
    wbuf_elem(&b, 42, &erp, 1);                         /* ERP */
    wbuf_elem(&b, WLAN_EID_EXT_RATES, rates_x, 4);
    wbuf_put(&b, r, rsn(r, 4, ccmp, 1, psk_sae, 2, 0x0080));   /* MFP capable, not required */
    ht_cap_elem(&b);
    ht_op_elem(&b, 11);
    wbuf_elem(&b, WLAN_EID_EXT_CAP, ext_cap, 8);
    wbuf_elem(&b, WLAN_EID_VENDOR, wmm_param, sizeof(wmm_param));
    wbuf_elem(&b, WLAN_EID_VENDOR, wps, sizeof(wps));
    wbuf_elem(&b, WLAN_EID_VENDOR, bcm, sizeof(bcm));
    return wbuf_done(&b);
}

/* its 5 GHz radio: no DS element, the channel from HT operation; VHT; WPA2 only */
static u_int beacon_5g(u8 *buf, u_int size) {
    struct wbuf b;
    u8 r[40], vht[12], vhtop[5];
    memset(vht, 0, sizeof(vht));
    memset(vhtop, 0, sizeof(vhtop));
    vht[0] = 0x92; vht[1] = 0x01; vht[2] = 0x80; vht[3] = 0x33;
    vhtop[0] = 1; vhtop[1] = 155;
    beacon_start(&b, buf, size, WLAN_ST_BEACON, ap2, 100, WLAN_CAP_ESS | WLAN_CAP_PRIVACY | 0x0100);
    wbuf_elem(&b, WLAN_EID_SSID, "HouseOfBurr", 11);
    wbuf_elem(&b, WLAN_EID_RATES, rates_a, 8);
    wbuf_elem(&b, WLAN_EID_TIM, tim, 4);
    wbuf_elem(&b, WLAN_EID_COUNTRY, country, 6);
    wbuf_put(&b, r, rsn(r, 4, ccmp, 1, psk, 1, 0x000c));
    ht_cap_elem(&b);
    ht_op_elem(&b, 157);
    wbuf_elem(&b, WLAN_EID_VHT_CAP, vht, sizeof(vht));
    wbuf_elem(&b, WLAN_EID_VHT_OPERATION, vhtop, sizeof(vhtop));
    wbuf_elem(&b, WLAN_EID_VENDOR, wmm_param, sizeof(wmm_param));
    return wbuf_done(&b);
}

/* the open hotspot, xfinitywifi on 157, with its SSID given as ssid (n bytes) */
static u_int beacon_open(u8 *buf, u_int size, const char *ssid, u_int n, bool ds) {
    struct wbuf b;
    beacon_start(&b, buf, size, WLAN_ST_BEACON, ap3, 100, WLAN_CAP_ESS);
    if(ssid) wbuf_elem(&b, WLAN_EID_SSID, ssid, n);
    wbuf_elem(&b, WLAN_EID_RATES, rates_a, 8);
    if(ds) wbuf_elem(&b, WLAN_EID_DS_PARAMS, "\x9d", 1);
    return wbuf_done(&b);
}

/* a network with one RSN element (or a WPA1 one, or none) and the privacy bit */
static u_int beacon_sec(u8 *buf, u_int size, const u8 *rsn_e, u_int rsn_n, bool wpa1, bool privacy) {
    static const u8 wpa1_e[22] = { 0x00, 0x50, 0xf2, 0x01, 0x01, 0x00, 0x00, 0x50, 0xf2, 0x02, 0x01, 0x00,
                                   0x00, 0x50, 0xf2, 0x02, 0x01, 0x00, 0x00, 0x50, 0xf2, 0x02 };
    struct wbuf b;
    beacon_start(&b, buf, size, WLAN_ST_BEACON, ap1, 100, WLAN_CAP_ESS | (privacy ? WLAN_CAP_PRIVACY : 0));
    wbuf_elem(&b, WLAN_EID_SSID, "net", 3);
    wbuf_elem(&b, WLAN_EID_RATES, rates_b, 8);
    wbuf_elem(&b, WLAN_EID_DS_PARAMS, "\x01", 1);
    if(rsn_e) wbuf_put(&b, rsn_e, rsn_n);
    if(wpa1) wbuf_elem(&b, WLAN_EID_VENDOR, wpa1_e, sizeof(wpa1_e));
    return wbuf_done(&b);
}

static int parse(const u8 *f, u_int len, struct wlan_bss *bss, u_int *flags) {
    static const struct wlan_rx_info info = { 6, WLAN_BAND_2GHZ, -47, false };
    return ieee80211_parse_bss(at_edge(f, len), len, &info, 777, bss, flags);
}

/* 10/9/26 (review L6): every shorter copy is refused when cut within the fixed fields and parsed
   otherwise, as far as its last whole element; one whose RSN element was cut off is never WEP;
   nothing is read past. Every one-byte change is parsed or refused, never read past */
static void abuse(const u8 *f, u_int len, const char *what) {
    const u_int fixed = WLAN_HDR_LEN + 12;
    struct wlan_bss bss;
    u_int cut, i, flags, ok = 0, early = 0, wep = 0;
    u8 copy[1024];
    char msg[120];
    for(cut = 0; cut < len; cut++) {
        if(parse(f, cut, &bss, &flags) != 0) continue;
        if(cut < fixed) early++;
        else ok++;
        if(cut >= fixed && bss.security == WLAN_SEC_WEP && ieee80211_elems_whole(f + fixed, cut - fixed) < cut - fixed)
            wep++;
    }
    snprintf(msg, sizeof(msg), "%s: copies cut within the fixed fields refused (%u parsed)", what, early);
    check(!early, msg);
    memcpy(copy, f, len);
    for(i = 0; i < len; i++) {
        u8 was = copy[i];
        copy[i] = (u8)(was ^ 0xff);
        parse(copy, len, &bss, &flags);
        copy[i] = (u8)(was + 1);
        parse(copy, len, &bss, &flags);
        copy[i] = was;
    }
    snprintf(msg, sizeof(msg), "%s: the %u copies cut after the fixed fields parse (%u did)", what, len - fixed, ok);
    check(ok == len - fixed, msg);
    snprintf(msg, sizeof(msg), "%s: a copy cut within an element is never WEP (%u were)", what, wep);
    check(!wep, msg);
}

static void test_beacons(void) {
    u8 f[512], r[64];
    struct wlan_bss bss;
    u_int n, flags;

    n = beacon_2g(f, sizeof(f), WLAN_ST_BEACON);
    check(n > 150, "the 2.4 GHz beacon is built");
    check(parse(f, n, &bss, &flags) == 0, "the 2.4 GHz beacon parses");
    check(!memcmp(bss.bssid, ap1, 6), "its BSSID");
    check(bss.ssid_len == 11 && !memcmp(bss.ssid, "HouseOfBurr", 11), "its SSID");
    check(bss.channel == 11 && bss.band == WLAN_BAND_2GHZ, "channel 11 from the DS element, not the receive channel (6)");
    check(bss.rssi == -47 && bss.seen == 777, "the signal and the tick it was heard");
    check(bss.beacon_interval == 100 && (bss.capability & WLAN_CAP_PRIVACY), "beacon interval and capability");
    check(bss.nrates == 12 && bss.rates[0] == 0x82 && bss.rates[11] == 0x6c, "12 rates from both elements, as listed");
    check(bss.ht && !bss.vht && bss.ht_cap[0] == 0xef && bss.ht_cap[2] == 0x17, "HT and its capabilities, no VHT");
    check(bss.security == WLAN_SEC_WPA2_WPA3, "WPA2/WPA3 transition (AKM 2 and 8, MFP capable)");
    check(bss.rsn_ie_len == 2 + 2 + 4 + 2 + 4 + 2 + 8 + 2 && bss.rsn_ie[0] == WLAN_EID_RSN, "the RSN element kept whole");
    check(flags == IEEE80211_BSS_WMM, "WMM seen, from a beacon");
    abuse(f, n, "2.4 GHz beacon");

    n = beacon_2g(f, sizeof(f), WLAN_ST_PROBE_RESP);
    check(parse(f, n, &bss, &flags) == 0 && flags == (IEEE80211_BSS_WMM | IEEE80211_BSS_PROBE_RESP),
          "a probe response parses as one");

    n = beacon_5g(f, sizeof(f));
    check(parse(f, n, &bss, &flags) == 0, "the 5 GHz beacon parses");
    check(bss.channel == 157 && bss.band == WLAN_BAND_5GHZ, "channel 157 from HT operation, 5 GHz");
    check(bss.vht && bss.ht && bss.nrates == 8, "VHT, HT, 8 rates");
    check(bss.security == WLAN_SEC_WPA2_PSK, "WPA2-Personal");
    abuse(f, n, "5 GHz beacon");

    n = beacon_open(f, sizeof(f), "xfinitywifi", 11, true);
    check(parse(f, n, &bss, &flags) == 0 && bss.security == WLAN_SEC_OPEN && bss.channel == 157 && flags == 0,
          "the open hotspot: open, channel 157, no WMM");
    check(bss.rsn_ie_len == 0 && !bss.ht, "no RSN, no HT");
    n = beacon_open(f, sizeof(f), "xfinitywifi", 11, false);
    check(parse(f, n, &bss, &flags) == 0 && bss.channel == 6, "no DS or HT element: the receive channel");
    n = beacon_open(f, sizeof(f), "", 0, true);
    check(parse(f, n, &bss, &flags) == 0 && bss.ssid_len == 0, "hidden: an empty SSID");
    n = beacon_open(f, sizeof(f), "\0\0\0\0\0\0\0", 7, true);
    check(parse(f, n, &bss, &flags) == 0 && bss.ssid_len == 0, "hidden: an SSID of zero bytes");
    n = beacon_open(f, sizeof(f), NULL, 0, true);
    check(parse(f, n, &bss, &flags) == 0 && bss.ssid_len == 0, "hidden: no SSID element");
    n = beacon_open(f, sizeof(f), "123456789012345678901234567890123", 33, true);
    check(parse(f, n, &bss, &flags) == -1, "an SSID of 33 bytes: refused");
    n = beacon_open(f, sizeof(f), "12345678901234567890123456789012", 32, true);
    check(parse(f, n, &bss, &flags) == 0 && bss.ssid_len == 32, "an SSID of 32 bytes");

    /* broken frames. 10/9/26 (review L6): a malformed element at the end ends the list there */
    n = beacon_open(f, sizeof(f), "x", 1, true);
    f[n++] = WLAN_EID_VENDOR;
    f[n++] = 10;                                        /* says 10 bytes, has 2 */
    f[n++] = 0; f[n++] = 0;
    check(parse(f, n, &bss, &flags) == 0 && bss.ssid_len == 1 && bss.channel == 157 && bss.nrates == 8 &&
          bss.security == WLAN_SEC_OPEN, "an element that runs past the end: the ones before it are read");
    n = beacon_open(f, sizeof(f), "x", 1, true);
    f[n++] = 7;                                         /* a lone id byte */
    check(parse(f, n, &bss, &flags) == 0 && bss.ssid_len == 1 && bss.channel == 157, "a stray byte at the end: the same");
    {
        u8 e[40];
        u_int k = rsn(e, 4, ccmp, 1, psk, 1, 0);
        n = beacon_sec(f, sizeof(f), e, k, false, true);
        check(parse(f, n, &bss, &flags) == 0 && bss.security == WLAN_SEC_WPA2_PSK, "WPA2, its RSN element last");
        check(parse(f, n - 1, &bss, &flags) == 0 && bss.security == WLAN_SEC_OTHER && bss.rsn_ie_len == 0 &&
              bss.ssid_len == 3, "that RSN element cut short: other (not joinable), not WEP");
        n = beacon_sec(f, sizeof(f), e, k, true, true);
        check(parse(f, n - 3, &bss, &flags) == 0 && bss.security == WLAN_SEC_WPA2_PSK && bss.rsn_ie_len == k,
              "the RSN element whole, a vendor element after it cut short: WPA2");
        n = beacon_sec(f, sizeof(f), NULL, 0, false, true);
        f[n++] = WLAN_EID_COUNTRY;
        f[n++] = 6;
        check(parse(f, n, &bss, &flags) == 0 && bss.security == WLAN_SEC_OTHER,
              "the privacy bit and something cut off at the end: other, not WEP");
        check(parse(f, n - 2, &bss, &flags) == 0 && bss.security == WLAN_SEC_WEP, "...and with nothing cut off: WEP");
    }
    n = beacon_open(f, sizeof(f), "x", 1, true);
    f[WLAN_HDR_LEN + 10] = 0x02;                        /* IBSS, not ESS */
    f[WLAN_HDR_LEN + 11] = 0;
    check(parse(f, n, &bss, &flags) == -1, "an IBSS beacon: refused");
    n = beacon_open(f, sizeof(f), "x", 1, true);
    f[16] = 0x01;                                       /* a group address as the BSSID */
    check(parse(f, n, &bss, &flags) == -1, "a multicast BSSID: refused");
    check(parse(f, WLAN_HDR_LEN + 11, &bss, &flags) == -1, "fixed fields cut short: refused");
    n = beacon_open(f, sizeof(f), "x", 1, true);
    f[0] = WLAN_FC(WLAN_FC_TYPE_MGMT, WLAN_ST_AUTH);
    check(parse(f, n, &bss, &flags) == -1, "an authentication frame is no beacon");
    n = beacon_open(f, sizeof(f), "x", 1, true);
    f[0] |= 1;                                          /* protocol version 1 */
    check(parse(f, n, &bss, &flags) == -1, "protocol version 1: refused");

    /* the RSN element longer than wlan_bss keeps: WPA2 cannot use it then */
    {
        u8 big[80];
        u_int k = rsn(big, 4, ccmp, 1, psk, 1, 0), i;
        big[k++] = 3; big[k++] = 0;                     /* three PMKIDs */
        for(i = 0; i < 48; i++) big[k++] = (u8)i;
        big[1] = (u8)(k - 2);
        n = beacon_sec(f, sizeof(f), big, k, false, true);
        check(parse(f, n, &bss, &flags) == 0 && bss.security == WLAN_SEC_OTHER && bss.rsn_ie_len == 0,
              "an RSN element too long to keep whole: not joinable (other)");
    }
    (void)r;
}

static void test_security(void) {
    u8 f[256], e[64];
    struct wlan_bss bss;
    struct ieee80211_rsn rs;
    u_int n, flags, k;

    k = rsn(e, 4, ccmp, 1, psk, 1, -1);
    n = beacon_sec(f, sizeof(f), e, k, false, true);
    check(parse(f, n, &bss, &flags) == 0 && bss.security == WLAN_SEC_WPA2_PSK, "WPA2-PSK without capabilities");
    k = rsn(e, 4, ccmp, 1, sae, 1, 0x00c0);
    n = beacon_sec(f, sizeof(f), e, k, false, true);
    check(parse(f, n, &bss, &flags) == 0 && bss.security == WLAN_SEC_WPA3_SAE, "WPA3 only: SAE, MFP required");
    k = rsn(e, 4, ccmp, 1, psk, 1, 0x00c0);
    n = beacon_sec(f, sizeof(f), e, k, false, true);
    check(parse(f, n, &bss, &flags) == 0 && bss.security == WLAN_SEC_OTHER, "PSK with MFP required: not ours");
    k = rsn(e, 4, ccmp, 1, dot1x, 1, 0);
    n = beacon_sec(f, sizeof(f), e, k, false, true);
    check(parse(f, n, &bss, &flags) == 0 && bss.security == WLAN_SEC_ENTERPRISE, "802.1X: enterprise");
    k = rsn(e, 2, tkip_ccmp, 2, psk, 1, 0);
    n = beacon_sec(f, sizeof(f), e, k, false, true);
    check(parse(f, n, &bss, &flags) == 0 && bss.security == WLAN_SEC_OTHER, "TKIP group cipher (WPA/WPA2 mixed): other");
    k = rsn(e, 4, tkip_ccmp, 2, psk, 1, 0);
    n = beacon_sec(f, sizeof(f), e, k, false, true);
    check(parse(f, n, &bss, &flags) == 0 && bss.security == WLAN_SEC_WPA2_PSK, "TKIP and CCMP pairwise, CCMP group: WPA2");
    k = rsn(e, 4, ccmp, 1, ft_psk, 1, 0);
    n = beacon_sec(f, sizeof(f), e, k, false, true);
    check(parse(f, n, &bss, &flags) == 0 && bss.security == WLAN_SEC_OTHER, "FT-PSK only: other");
    n = beacon_sec(f, sizeof(f), NULL, 0, true, true);
    check(parse(f, n, &bss, &flags) == 0 && bss.security == WLAN_SEC_OTHER, "WPA1 only: other");
    n = beacon_sec(f, sizeof(f), NULL, 0, false, true);
    check(parse(f, n, &bss, &flags) == 0 && bss.security == WLAN_SEC_WEP, "privacy without RSN: WEP");
    n = beacon_sec(f, sizeof(f), NULL, 0, false, false);
    check(parse(f, n, &bss, &flags) == 0 && bss.security == WLAN_SEC_OPEN, "neither: open");

    /* the RSN element on its own */
    e[0] = WLAN_EID_RSN; e[1] = 2; e[2] = 1; e[3] = 0;
    check(ieee80211_parse_rsn(e, &rs) == 0 && rs.group == RSN_CIPHER_CCMP && rs.pairwise == RSN_P_CCMP &&
          rs.akm == RSN_A_8021X, "a version-only RSN element: the defaults (CCMP, CCMP, 802.1X)");
    e[2] = 2;
    check(ieee80211_parse_rsn(e, &rs) == -1, "RSN version 2: refused");
    k = rsn(e, 4, ccmp, 1, psk, 1, 0);
    e[8] = 5;                                           /* five pairwise suites, one there */
    check(ieee80211_parse_rsn(e, &rs) == -1, "a pairwise count larger than the element: refused");
    k = rsn(e, 4, ccmp, 1, psk, 1, 0);
    e[8] = 0;
    check(ieee80211_parse_rsn(e, &rs) == -1, "no pairwise suite: refused");
    k = rsn(e, 4, ccmp, 1, psk, 1, 0);
    e[1] = (u8)(k - 2 - 1);                             /* capabilities cut to one byte */
    check(ieee80211_parse_rsn(e, &rs) == -1, "half a capability field: refused");
    k = rsn(e, 4, ccmp, 1, psk, 1, 0);
    n = beacon_sec(f, sizeof(f), e, k - 1, false, true);   /* the element says one byte more than there is */
    check(parse(f, n, &bss, &flags) == 0 && bss.security == WLAN_SEC_OTHER && bss.rsn_ie_len == 0,
          "an RSN element cut by the end of the frame: the rest is read, the network other (10/9/26, review L6)");
    k = rsn(e, 4, ccmp, 1, psk_sae, 2, 0x0080);
    check(ieee80211_parse_rsn(e, &rs) == 0 && rs.akm == (RSN_A_PSK | RSN_A_SAE) && rs.caps == 0x0080, "AKMs and capabilities");
    check(ieee80211_security(e, NULL, 0) == WLAN_SEC_WPA2_WPA3, "ieee80211_security on the element alone");
    check(!strcmp(ieee80211_security_name(WLAN_SEC_WPA2_PSK), "WPA2-Personal"), "the name of WPA2-PSK");
}

/**** what a station sends ****/

static void test_build(void) {
    u8 f[512], our_rsn[22];
    struct wlan_bss bss;
    struct ieee80211_assoc_params p;
    struct ieee80211_hdr h;
    const u8 *ies, *e;
    u_int n, k;
    static const u8 auth_bytes[30] = {
        0xb0, 0x00, 0x00, 0x00, 0x42, 0x75, 0xc3, 0x4b, 0x21, 0x09, 0x52, 0x54, 0x00, 0x12, 0x34, 0x57,
        0x42, 0x75, 0xc3, 0x4b, 0x21, 0x09, 0x50, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00 };
    static const u8 ht2[28] = { 45, 26, 0x6e, 0x01, 0x1b, 0xff, 0xff, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x01, 0, 0, 0,
                                0, 0, 0, 0, 0, 0, 0 };

    n = ieee80211_build_auth(f, sizeof(f), ap1, sta, ap1, WLAN_AUTH_OPEN, 1, 0, 5);
    check(n == 30 && !memcmp(f, auth_bytes, 30), "authentication: open system, transaction 1, byte for byte");
    check(ieee80211_build_auth(f, 29, ap1, sta, ap1, 0, 1, 0, 5) == 0, "authentication into 29 bytes: 0");

    n = ieee80211_build_probe_req(f, sizeof(f), sta, (const u8 *)"HouseOfBurr", 11, WLAN_BAND_2GHZ, 9);
    check(n == 24 + 13 + 10 + 6, "a probe request on 2.4 GHz: SSID, 8 rates, 4 extended");
    check(!memcmp(f + 4, bcast, 6) && !memcmp(f + 10, sta, 6) && !memcmp(f + 16, bcast, 6), "to everyone, from us");
    check(f[24] == 0 && f[25] == 11 && f[37] == WLAN_EID_RATES && f[38] == 8 && f[47] == WLAN_EID_EXT_RATES, "its elements");
    n = ieee80211_build_probe_req(f, sizeof(f), sta, NULL, 0, WLAN_BAND_5GHZ, 9);
    check(n == 24 + 2 + 10, "a wildcard probe request on 5 GHz: 8 rates only");

    k = ieee80211_ht_cap(f, 2, true);
    check(k == 28 && !memcmp(f, ht2, 28), "HT capabilities, 2 streams, 20/40 MHz, byte for byte");
    k = ieee80211_ht_cap(f, 1, false);
    check(f[2] == 0x2c && f[3] == 0x01 && f[5] == 0xff && f[6] == 0 && f[17] == 1, "HT capabilities, 1 stream, 20 MHz");

    /* association on 2.4 GHz to the open network with HT and WMM */
    n = beacon_2g(f, sizeof(f), WLAN_ST_BEACON);
    parse(f, n, &bss, &k);
    memset(&p, 0, sizeof(p));
    p.capability = WLAN_CAP_ESS | WLAN_CAP_SHORT_SLOT | WLAN_CAP_SHORT_PREAMBLE;
    p.listen_interval = 10;
    p.wmm = true;
    p.ht = true;
    p.ht_streams = 2;
    p.ht40 = true;
    n = ieee80211_build_assoc_req(f, sizeof(f), &bss, sta, &p, 77);
    check(n > 0 && ieee80211_parse_hdr(f, n, &h) == 0 && h.subtype == WLAN_ST_ASSOC_REQ && !memcmp(h.a1, ap1, 6) &&
          !memcmp(h.a2, sta, 6) && !memcmp(h.a3, ap1, 6) && h.seq_ctrl == (77 << 4), "an association request's header");
    check(wlan_le16(f + 24) == p.capability && wlan_le16(f + 26) == 10, "capability and listen interval");
    ies = f + 28;
    check(ieee80211_elems_ok(ies, n - 28), "its elements are whole");
    e = ieee80211_find_elem(ies, n - 28, WLAN_EID_SSID);
    check(e && e[1] == 11 && !memcmp(e + 2, "HouseOfBurr", 11), "the SSID");
    e = ieee80211_find_elem(ies, n - 28, WLAN_EID_RATES);
    check(e && e[1] == 8 && e[2] == 0x02 && ieee80211_find_elem(ies, n - 28, WLAN_EID_EXT_RATES), "2.4 GHz rates");
    e = ieee80211_find_elem(ies, n - 28, WLAN_EID_HT_CAP);
    check(e && !memcmp(e, ht2, 28), "our HT capabilities");
    check(ieee80211_find_vendor(ies, n - 28, WLAN_OUI_MICROSOFT, 2) && ies[n - 28 - 9] == WLAN_EID_VENDOR,
          "a WMM element, last");
    check(!ieee80211_find_elem(ies, n - 28, WLAN_EID_RSN), "no RSN element for an open network");

    /* WPA2 on 5 GHz: our RSN element as given, 5 GHz rates only */
    n = beacon_5g(f, sizeof(f));
    parse(f, n, &bss, &k);
    k = rsn(our_rsn, 4, ccmp, 1, psk, 1, 0);
    bss.channel = 157;
    bss.band = WLAN_BAND_5GHZ;
    memset(&p, 0, sizeof(p));
    p.capability = WLAN_CAP_ESS | WLAN_CAP_PRIVACY;
    p.listen_interval = 10;
    p.rsn = our_rsn;
    p.rsn_len = k;
    n = ieee80211_build_assoc_req(f, sizeof(f), &bss, sta, &p, 78);
    ies = f + 28;
    e = ieee80211_find_elem(ies, n - 28, WLAN_EID_RSN);
    check(k == 22 && n > 0 && e && !memcmp(e, our_rsn, 22), "our RSN element, verbatim");
    e = ieee80211_find_elem(ies, n - 28, WLAN_EID_RATES);
    check(e && e[1] == 8 && e[2] == 0x0c && !ieee80211_find_elem(ies, n - 28, WLAN_EID_EXT_RATES), "5 GHz: the OFDM rates");
    check(!ieee80211_find_elem(ies, n - 28, WLAN_EID_HT_CAP) && !ieee80211_find_vendor(ies, n - 28, WLAN_OUI_MICROSOFT, 2),
          "no HT, no WMM when not asked");
    p.rsn_len = 21;
    check(ieee80211_build_assoc_req(f, sizeof(f), &bss, sta, &p, 78) == 0, "an RSN element whose length disagrees: 0");
    p.rsn_len = 22;
    for(k = 0; k < n; k++) if(ieee80211_build_assoc_req(f, k, &bss, sta, &p, 78) != 0) break;
    check(k == n, "an association request into any smaller buffer: 0");

    n = ieee80211_build_deauth(f, sizeof(f), WLAN_ST_DEAUTH, ap1, sta, ap1, WLAN_REASON_LEAVING, 3);
    check(n == 26 && f[0] == 0xc0 && f[24] == 3 && f[25] == 0, "deauthentication, reason 3");
    n = ieee80211_build_deauth(f, sizeof(f), WLAN_ST_DISASSOC, ap1, sta, ap1, 8, 3);
    check(n == 26 && f[0] == 0xa0 && f[24] == 8, "disassociation, reason 8");
    check(ieee80211_build_deauth(f, sizeof(f), WLAN_ST_AUTH, ap1, sta, ap1, 8, 3) == 0, "neither: 0");

    {
        static const u8 addba[9] = { 3, 0, 0x2a, 0x02, 0x10, 0x00, 0x00, 0x10, 0x00 };
        n = ieee80211_build_addba_decline(f, sizeof(f), ap1, sta, ap1, addba, 9, 4);
        check(n == 33 && f[0] == 0xd0 && f[24] == 3 && f[25] == 1 && f[26] == 0x2a && f[27] == 37 && f[28] == 0 &&
              f[29] == 0x02 && f[30] == 0x10 && f[31] == 0 && f[32] == 0, "an ADDBA request declined (status 37)");
        check(ieee80211_build_addba_decline(f, sizeof(f), ap1, sta, ap1, addba, 8, 4) == 0, "a request cut short: 0");
    }
}

static void test_responses(void) {
    u8 f[256];
    struct wbuf b;
    struct ieee80211_auth a;
    struct ieee80211_assoc_resp r;
    u16 reason;
    u_int n, k, bad = 0;

    n = ieee80211_build_auth(f, sizeof(f), sta, ap1, ap1, WLAN_AUTH_OPEN, 2, 0, 1);
    check(ieee80211_parse_auth(at_edge(f, n), n, &a) == 0 && a.algorithm == 0 && a.transaction == 2 && a.status == 0,
          "an authentication response");
    for(k = 0; k < n; k++) if(ieee80211_parse_auth(at_edge(f, k), k, &a) == 0) bad++;
    check(!bad, "authentication cut short: refused at every length");

    wbuf_init(&b, f, sizeof(f));
    wbuf_hdr(&b, WLAN_FC(WLAN_FC_TYPE_MGMT, WLAN_ST_ASSOC_RESP), sta, ap1, ap1, 2);
    wbuf_le16(&b, WLAN_CAP_ESS);
    wbuf_le16(&b, 0);
    wbuf_le16(&b, 0xc001);                              /* AID 1 with its two top bits */
    wbuf_elem(&b, WLAN_EID_RATES, rates_b, 8);
    ht_cap_elem(&b);
    n = wbuf_done(&b);
    check(ieee80211_parse_assoc_resp(at_edge(f, n), n, &r) == 0 && r.status == 0 && r.aid == 1 && r.ies_len == 10 + 28,
          "an association response: status 0, AID 1, its elements");
    bad = 0;
    for(k = 0; k < n; k++) if(ieee80211_parse_assoc_resp(at_edge(f, k), k, &r) == 0 && k < 30) bad++;
    check(!bad, "an association response cut before its elements: refused");
    check(ieee80211_parse_assoc_resp(at_edge(f, n - 1), n - 1, &r) == 0 && r.ies_len == 10,
          "one cut inside its last element: read up to it (review L6)");
    f[0] = WLAN_FC(WLAN_FC_TYPE_MGMT, WLAN_ST_REASSOC_RESP);
    check(ieee80211_parse_assoc_resp(at_edge(f, n), n, &r) == 0, "a reassociation response too");

    n = ieee80211_build_deauth(f, sizeof(f), WLAN_ST_DISASSOC, sta, ap1, ap1, 15, 1);
    check(ieee80211_parse_reason(at_edge(f, n), n, &reason) == 0 && reason == 15, "the reason of a disassociation");
    check(ieee80211_parse_reason(at_edge(f, n - 1), n - 1, &reason) == -1, "a reason cut short: refused");
    check(!strcmp(ieee80211_reason_name(15), "4-way handshake timeout") && !strcmp(ieee80211_status_name(17),
          "the access point has too many stations") && !strcmp(ieee80211_status_name(999), ""), "codes in words");
}

/**** data frames ****/

static void test_data(void) {
    u8 eth[64], f[512], amsdu[256];
    struct ieee80211_hdr h;
    struct ieee80211_msdu m[4];
    struct wbuf b;
    u_int n, k, i;
    int r;
    static const u8 dst[6] = { 0x10, 0x00, 0x00, 0x00, 0x00, 0x01 };
    static const u8 snap_ip[8] = { 0xaa, 0xaa, 0x03, 0x00, 0x00, 0x00, 0x08, 0x00 };

    memcpy(eth, dst, 6);
    memcpy(eth + 6, sta, 6);
    eth[12] = 0x08; eth[13] = 0x00;
    for(i = 0; i < 20; i++) eth[14 + i] = (u8)(0x45 + i);
    n = ieee80211_from_eth(f, sizeof(f), eth, 34, ap1, true, 0x123);
    check(n == 26 + 8 + 20, "Ethernet to QoS data: 26 + 8 + 20 bytes");
    check(f[0] == 0x88 && f[1] == 0x01, "QoS data, To DS");
    check(!memcmp(f + 4, ap1, 6) && !memcmp(f + 10, sta, 6) && !memcmp(f + 16, dst, 6), "A1 the BSSID, A2 us, A3 the destination");
    check(wlan_le16(f + 22) == 0x1230 && f[24] == 0 && f[25] == 0, "sequence number, TID 0");
    check(!memcmp(f + 26, snap_ip, 8) && f[34] == 0x45 && f[53] == 0x45 + 19, "LLC/SNAP, IPv4, the payload");
    check(ieee80211_parse_hdr(f, n, &h) == 0 && h.qos && h.hdr_len == 26, "it parses back as QoS data");
    r = ieee80211_msdus(f, n, &h, m, 4);
    check(r == 1 && !memcmp(m[0].da, dst, 6) && !memcmp(m[0].sa, sta, 6) && m[0].type == 0x0800 && m[0].len == 20 &&
          m[0].payload[0] == 0x45, "To DS seen by the AP: destination A3, source A2");
    n = ieee80211_from_eth(f, sizeof(f), eth, 34, ap1, false, 1);
    check(n == 24 + 8 + 20 && f[0] == 0x08 && f[1] == 0x01, "plain data without QoS");
    eth[12] = 0x05; eth[13] = 0xdc;
    check(ieee80211_from_eth(f, sizeof(f), eth, 34, ap1, false, 1) == 0, "an 802.3 length instead of a type: 0");
    check(ieee80211_from_eth(f, sizeof(f), eth, 13, ap1, false, 1) == 0, "13 bytes: no Ethernet frame");

    /* from the AP: From DS, EAPOL */
    n = ieee80211_build_data(f, sizeof(f), false, sta, ap1, ap1, true, 9, ETH_P_EAPOL, (const u8 *)"\x02\x03\x00\x05hello", 9);
    check(ieee80211_parse_hdr(at_edge(f, n), n, &h) == 0 && (h.fc & WLAN_FC_FROMDS), "From DS QoS data");
    r = ieee80211_msdus(f, n, &h, m, 4);
    check(r == 1 && !memcmp(m[0].da, sta, 6) && !memcmp(m[0].sa, ap1, 6) && m[0].type == ETH_P_EAPOL && m[0].len == 9 &&
          m[0].payload[0] == 2, "From DS: destination A1, source A3; EAPOL after LLC/SNAP");
    for(k = 0; k < n; k++) {
        if(ieee80211_parse_hdr(at_edge(f, k), k, &h) == 0) ieee80211_msdus(guard + PAGE - k, k, &h, m, 4);
    }
    check(1, "data frames cut short at every length: nothing read past");
    f[26 + 2] = 0x04;                                   /* not LLC */
    ieee80211_parse_hdr(f, n, &h);
    check(ieee80211_msdus(f, n, &h, m, 4) == -1, "no LLC/SNAP: refused");

    /* an A-MSDU with two subframes, the first padded */
    wbuf_init(&b, amsdu, sizeof(amsdu));
    wbuf_hdr(&b, WLAN_FC(WLAN_FC_TYPE_DATA, WLAN_ST_QOS_DATA) | WLAN_FC_FROMDS, sta, ap1, ap1, 10);
    wbuf_le16(&b, WLAN_QOS_AMSDU);
    wbuf_put(&b, sta, 6); wbuf_put(&b, dst, 6); wbuf_be16(&b, 8 + 3);
    wbuf_put(&b, snap_ip, 8); wbuf_put(&b, "abc", 3);
    wbuf_put(&b, "\0", 1);                               /* 14 + 11 = 25: three bytes of padding */
    wbuf_put(&b, "\0\0", 2);
    wbuf_put(&b, bcast, 6); wbuf_put(&b, ap1, 6); wbuf_be16(&b, 8 + 4);
    wbuf_put(&b, snap_ip, 8); wbuf_put(&b, "wxyz", 4);
    n = wbuf_done(&b);
    check(ieee80211_parse_hdr(at_edge(amsdu, n), n, &h) == 0 && (h.qos_ctrl & WLAN_QOS_AMSDU), "an A-MSDU's header");
    r = ieee80211_msdus(guard + PAGE - n, n, &h, m, 4);
    check(r == 2, "two MSDUs in the A-MSDU");
    check(r == 2 && !memcmp(m[0].da, sta, 6) && !memcmp(m[0].sa, dst, 6) && m[0].len == 3 && !memcmp(m[0].payload, "abc", 3),
          "the first: its own addresses and payload");
    check(r == 2 && !memcmp(m[1].da, bcast, 6) && m[1].len == 4 && !memcmp(m[1].payload, "wxyz", 4), "the second, after the padding");
    r = ieee80211_msdus(amsdu, n, &h, m, 1);
    check(r == 1, "room for one: one");
    for(k = WLAN_QOS_HDR_LEN; k < n; k++) {
        if(ieee80211_parse_hdr(at_edge(amsdu, k), k, &h) == 0) {
            r = ieee80211_msdus(guard + PAGE - k, k, &h, m, 4);
            if(r == 2) break;
        }
    }
    check(k == n, "an A-MSDU cut short never gives both MSDUs");
    memcpy(amsdu + 26, snap_ip, 6);                     /* the CVE-2020-24588 shape */
    ieee80211_parse_hdr(amsdu, n, &h);
    check(ieee80211_msdus(amsdu, n, &h, m, 4) == -1, "an A-MSDU starting with LLC/SNAP: refused");

    /* other shapes */
    wbuf_init(&b, f, sizeof(f));
    wbuf_hdr(&b, WLAN_FC(WLAN_FC_TYPE_DATA, WLAN_ST_QOS_NULL) | WLAN_FC_TODS, ap1, sta, ap1, 1);
    wbuf_le16(&b, 0);
    n = wbuf_done(&b);
    check(ieee80211_parse_hdr(f, n, &h) == 0 && ieee80211_msdus(f, n, &h, m, 4) == 0, "QoS null: no MSDU");
    wbuf_init(&b, f, sizeof(f));
    wbuf_hdr(&b, WLAN_FC(WLAN_FC_TYPE_DATA, WLAN_ST_DATA) | WLAN_FC_TODS | WLAN_FC_FROMDS, ap1, sta, ap1, 1);
    wbuf_put(&b, dst, 6);
    wbuf_put(&b, snap_ip, 8);
    n = wbuf_done(&b);
    check(ieee80211_parse_hdr(f, n, &h) == 0 && h.a4 && h.hdr_len == 30 && ieee80211_msdus(f, n, &h, m, 4) == -1,
          "four addresses: parsed, then refused");
    wbuf_init(&b, f, sizeof(f));
    wbuf_hdr(&b, WLAN_FC(WLAN_FC_TYPE_DATA, WLAN_ST_QOS_DATA) | WLAN_FC_FROMDS | WLAN_FC_ORDER, sta, ap1, ap1, 1);
    wbuf_le16(&b, 0);
    wbuf_put(&b, "\0\0\0\0", 4);                         /* HT control */
    wbuf_put(&b, snap_ip, 8);
    n = wbuf_done(&b);
    check(ieee80211_parse_hdr(f, n, &h) == 0 && h.hdr_len == 30 && ieee80211_msdus(f, n, &h, m, 4) == 1 && m[0].len == 0,
          "+HTC: the body after the HT control field");
    check(ieee80211_parse_hdr(f, 29, &h) == -1, "+HTC cut short: refused");
    f[0] = 0x04;                                        /* a control frame */
    check(ieee80211_parse_hdr(f, n, &h) == -1, "a control frame: refused");

    /* 10/9/26 (review L5): fragments, each with what looks like a whole MSDU in it */
    n = ieee80211_build_data(f, sizeof(f), false, sta, ap1, ap1, false, 7, 0x0800, (const u8 *)"abcd", 4);
    check(ieee80211_parse_hdr(f, n, &h) == 0 && !ieee80211_is_fragment(&h) && ieee80211_msdus(f, n, &h, m, 4) == 1,
          "a whole frame: one MSDU");
    f[1] |= WLAN_FC_MOREFRAG >> 8;
    check(ieee80211_parse_hdr(f, n, &h) == 0 && ieee80211_is_fragment(&h) && ieee80211_msdus(f, n, &h, m, 4) == -1,
          "More Fragments (the first fragment): refused");
    f[1] &= (u8)~(WLAN_FC_MOREFRAG >> 8);
    f[22] |= 1;
    check(ieee80211_parse_hdr(f, n, &h) == 0 && ieee80211_is_fragment(&h) && ieee80211_msdus(f, n, &h, m, 4) == -1,
          "fragment number 1 (the last fragment): refused");
    f[22] = (u8)((f[22] & 0xf0) | 0x0f);
    check(ieee80211_parse_hdr(f, n, &h) == 0 && (h.seq_ctrl >> 4) == 7 && ieee80211_msdus(f, n, &h, m, 4) == -1,
          "fragment number 15: refused, the sequence number unchanged");
    wbuf_init(&b, amsdu, sizeof(amsdu));                /* an A-MSDU of one subframe */
    wbuf_hdr(&b, WLAN_FC(WLAN_FC_TYPE_DATA, WLAN_ST_QOS_DATA) | WLAN_FC_FROMDS, sta, ap1, ap1, 11);
    wbuf_le16(&b, WLAN_QOS_AMSDU);
    wbuf_put(&b, sta, 6); wbuf_put(&b, dst, 6); wbuf_be16(&b, 8 + 3);
    wbuf_put(&b, snap_ip, 8); wbuf_put(&b, "abc", 3);
    n = wbuf_done(&b);
    check(ieee80211_parse_hdr(amsdu, n, &h) == 0 && ieee80211_msdus(amsdu, n, &h, m, 4) == 1, "an A-MSDU of one subframe");
    amsdu[1] |= WLAN_FC_MOREFRAG >> 8;
    check(ieee80211_parse_hdr(amsdu, n, &h) == 0 && ieee80211_msdus(amsdu, n, &h, m, 4) == -1, "...as a fragment: refused");
}

int main(void) {
    guard = (u8 *)mmap(NULL, 2 * PAGE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if(guard == (u8 *)-1 || mprotect(guard + PAGE, PAGE, PROT_NONE)) {
        printf("FAIL: no guard page\n");
        return 1;
    }
    test_beacons();
    test_security();
    test_build();
    test_responses();
    test_data();
    if(failures) {
        printf("IEEE80211 UNIT TESTS FAILED: %d of %d checks\n", failures, checks);
        exit(1);
    }
    printf("IEEE80211 UNIT TESTS PASSED: %d checks, 0 failures\n", checks);
    return 0;
}
