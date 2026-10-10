/* Host-side tests for net/wlan/wpa.c (10/9/26, track C of the Wi-Fi brief), the WPA2-Personal
   supplicant:
   * a simulated access point: an authenticator written here from IEEE 802.11-2016 12.7 with
     lib/crypto (its own PTK derivation, MICs, the GTK in AES key wrap), running whole 4-way and
     group key handshakes through the wpa_ops callbacks: success (frames and key order checked),
     a wrong password, replays and retransmissions (no key installed twice), message 3 with
     another RSN element, malformed, truncated and oversized frames, a fuzzer, group and pairwise
     rekeys, timeouts, refused keys, and that wpa_free() wipes everything;
   * a real handshake: Wireshark's sample capture wpa2linkuppassphraseiswireshark.pcap (wiki
     SampleCaptures; SSID ikeriri-5g, passphrase "wireshark"). Given the capture's SNonce, the
     supplicant must answer the AP's messages 1 and 3 with the station's messages 2 and 4 byte
     for byte (the MICs prove the PMK, the KCK and the order of addresses and nonces), and
     install the GTK the AP's key data carries (which proves the KEK) and the TK beside them.
   Frames are handed over flush against an inaccessible page, so a read past one faults.
   kalloc/kfree (kfree checks the block was wiped, then poisons and keeps it to catch writes
   after a free), kprintf (the log is checked for secrets) and crypto_random (scripted
   nonces) are this file's. Built like the kernel objects and run by `make unittest`. */
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <surfos/types.h>
#include <net/wpa.h>
#include <crypto/crypto.h>

void exit(int status);
void *malloc(size_t n);
void free(void *p);
void *mmap(void *addr, size_t len, int prot, int flags, int fd, long off);
int mprotect(void *addr, size_t len, int prot);

static int failures, checks;

static void check(int ok, const char *what) {
    checks++;
    if(!ok) { failures++; printf("FAIL: %s\n", what); }
}

static u_int unhex(const char *s, u8 *out) {
    u_int n = 0;
    int hi = -1, v;
    for(; *s; s++) {
        if(*s >= '0' && *s <= '9') v = *s - '0';
        else if(*s >= 'a' && *s <= 'f') v = *s - 'a' + 10;
        else continue;
        if(hi < 0) hi = v;
        else { out[n++] = (u8)(hi << 4 | v); hi = -1; }
    }
    return n;
}

static int all_zero(const u8 *p, u_int n) {
    while(n--) if(*p++) return 0;
    return 1;
}

static void put_be16(u8 *p, u16 v) { p[0] = (u8)(v >> 8); p[1] = (u8)v; }
static u16 get_be16(const u8 *p) { return (u16)(p[0] << 8 | p[1]); }
static void put_be64(u8 *p, u64 v) { int i; for(i = 7; i >= 0; i--) { p[i] = (u8)v; v >>= 8; } }
static u64 get_be64(const u8 *p) { u64 v = 0; int i; for(i = 0; i < 8; i++) v = v << 8 | p[i]; return v; }

/**** the kernel, as wpa.c sees it ****/

#define QMAX 600
static struct { u8 *p; size_t n; } quarantine[QMAX];
static u_int nquarantine, unwiped_frees, live_blocks;
static int fail_alloc;

void *kalloc(size_t n) {
    size_t *h;
    if(fail_alloc) return NULL;
    h = malloc(n + 16);
    if(!h) return NULL;
    h[0] = n;
    live_blocks++;
    return (u8 *)h + 16;
}

void *kcalloc(size_t count, size_t size) {
    u8 *p = kalloc(count * size);
    if(p) memset(p, 0, count * size);
    return p;
}

void kfree(void *mem) {
    u8 *p = mem;
    size_t n = *(size_t *)(p - 16);
    if(!all_zero(p, n)) unwiped_frees++;
    memset(p, 0xa5, n);
    live_blocks--;
    if(nquarantine < QMAX) {
        quarantine[nquarantine].p = p;
        quarantine[nquarantine].n = n;
        nquarantine++;
    } else free(p - 16);
}

static int quarantine_intact(void) {
    u_int i;
    size_t j;
    for(i = 0; i < nquarantine; i++)
        for(j = 0; j < quarantine[i].n; j++)
            if(quarantine[i].p[j] != 0xa5) return 0;
    return 1;
}

/* check what is kept, then really free it, so the next test's frees are kept too */
static int quarantine_flush(void) {
    int ok = quarantine_intact();
    while(nquarantine) {
        nquarantine--;
        free(quarantine[nquarantine].p - 16);
    }
    return ok;
}

static size_t block_size(const void *mem) { return *(const size_t *)((const u8 *)mem - 16); }

static char logbuf[32768];
static u_int loglen;

void kprintf(const char *fmt, ...) {
    va_list ap;
    int n;
    if(loglen >= sizeof(logbuf) - 1) return;
    va_start(ap, fmt);
    n = vsnprintf(logbuf + loglen, sizeof(logbuf) - loglen, fmt, ap);
    va_end(ap);
    if(n > 0) loglen += (u_int)n;
    if(loglen > sizeof(logbuf) - 1) loglen = sizeof(logbuf) - 1;
}

static void log_clear(void) { loglen = 0; logbuf[0] = 0; }

/* nothing secret in the log: no 8 bytes of it in hex, either case, nor raw */
static int log_has(const u8 *secret, u_int n) {
    char lo[17], up[17];
    u_int i, j;
    if(n > 8) n = 8;
    for(i = 0; i < n; i++) {
        snprintf(lo + 2 * i, 3, "%02x", secret[i]);
        snprintf(up + 2 * i, 3, "%02X", secret[i]);
    }
    if(strstr(logbuf, lo) || strstr(logbuf, up)) return 1;
    for(i = 0; i + n <= loglen; i++) {
        for(j = 0; j < n && (u8)logbuf[i + j] == secret[j]; j++);
        if(j == n) return 1;
    }
    return 0;
}

/* every line the supplicant logs starts with +WPA: */
static int log_tidy(void) {
    const char *p = logbuf;
    while(*p) {
        if(strncmp(p, "+WPA: ", 6)) return 0;
        p = strchr(p, '\n');
        if(!p) return 0;
        p++;
    }
    return 1;
}

static u8 next_rand[32];
static int rand_scripted;
static u_int rand_calls;

/* the SNonce: scripted when a test needs a known one, else a pattern that differs per call */
void crypto_random(void *buf, size_t len) {
    u8 *p = buf;
    size_t i;
    rand_calls++;
    for(i = 0; i < len; i++) p[i] = rand_scripted && i < 32 ? next_rand[i] : (u8)(0x5a ^ (rand_calls * 37 + i * 11));
    rand_scripted = 0;
}

/* frames go to the supplicant ending where an inaccessible page begins */
static u8 *guard_end;

static void guard_init(void) {
    u8 *m = mmap(NULL, 18 * 4096, 1 | 2, 0x02 | 0x20, -1, 0);       /* PROT_READ|WRITE, MAP_PRIVATE|ANONYMOUS */
    if(m == (u8 *)-1 || mprotect(m + 17 * 4096, 4096, 0)) {
        printf("FAIL: no guard page\n");
        exit(1);
    }
    guard_end = m + 17 * 4096;
}

/**** the station layer's side ****/

#define SENT_MAX 24

struct sta {
    struct wpa_sm *sm;
    u8 sent[SENT_MAX][400];
    u_int sent_len[SENT_MAX], nsent;
    struct wlan_key keys[8];
    u_int nkeys;
    int status;
    u_int ndone;
    char ev[64];
    u_int nev;
    int refuse;                     /* install_key fails: 1 for the pairwise key, 2 for a group key */
    int free_in_done;
};

static void event(struct sta *s, char c) {
    if(s->nev < sizeof(s->ev) - 1) s->ev[s->nev++] = c;
    s->ev[s->nev] = 0;
}

static int sta_send(void *ctx, const u8 *frame, u_int len) {
    struct sta *s = ctx;
    if(s->nsent < SENT_MAX && len <= sizeof(s->sent[0])) {
        memcpy(s->sent[s->nsent], frame, len);
        s->sent_len[s->nsent] = len;
    }
    s->nsent++;
    event(s, 'S');
    return 0;
}

static int sta_install(void *ctx, const struct wlan_key *key) {
    struct sta *s = ctx;
    event(s, key->pairwise ? 'P' : 'G');
    if((s->refuse == 1 && key->pairwise) || (s->refuse == 2 && !key->pairwise)) return -1;
    if(s->nkeys < 8) s->keys[s->nkeys] = *key;
    s->nkeys++;
    return 0;
}

static void sta_done(void *ctx, int status) {
    struct sta *s = ctx;
    event(s, 'D');
    s->status = status;
    s->ndone++;
    if(s->free_in_done) {
        wpa_free(s->sm);
        s->sm = NULL;
    }
}

static const struct wpa_ops sta_ops = { sta_send, sta_install, sta_done };

static void deliver(struct sta *s, const u8 *f, u_int len) {
    u8 *p = guard_end - len;
    memcpy(p, f, len);
    wpa_rx_eapol(s->sm, p, len);
}

static void ticks(struct sta *s, u_int n) {
    while(n--) wpa_tick(s->sm);
}

/**** the simulated access point (the authenticator of 12.7.6 and 12.7.7) ****/

struct ap {
    u8 pmk[32], aa[6], spa[6];
    u8 rsn[64];                     /* its beacon's RSN element */
    u_int rsn_len;
    u8 rsn_m3[96];                  /* the element(s) it puts in message 3: normally the beacon's */
    u_int rsn_m3_len;
    u8 sta_rsn[64];                 /* the station's, from its association request */
    u_int sta_rsn_len;
    u64 counter, last_m1, last_m3, last_g1;
    u8 anonce[32], snonce[32];
    u8 kck[16], kek[16], tk[16];
    u8 gtk[16], gtk_id, rsc[8];
    int careless;                   /* sends message 3 without checking message 2's MIC */
};

static const u8 beacon_rsn[22] = { 0x30, 0x14, 1, 0, 0, 0x0f, 0xac, 4, 1, 0, 0, 0x0f, 0xac, 4, 1, 0, 0, 0x0f, 0xac, 2,
                                   0x0c, 0 };      /* like a typical AP: 16 PTKSA replay counters */
static const u8 aa_addr[6] = { 0x42, 0x75, 0xc3, 0x4b, 0x21, 0x09 };
static const u8 spa_addr[6] = { 0x10, 0x20, 0x30, 0x40, 0x50, 0x60 };     /* below AA: Min/Max swap them */

static void ap_init(struct ap *ap, const char *passphrase, const char *ssid) {
    u_int i;
    memset(ap, 0, sizeof(*ap));
    pbkdf2_sha1((const u8 *)passphrase, strlen(passphrase), (const u8 *)ssid, strlen(ssid), 4096, ap->pmk, 32);
    memcpy(ap->aa, aa_addr, 6);
    memcpy(ap->spa, spa_addr, 6);
    memcpy(ap->rsn, beacon_rsn, sizeof(beacon_rsn));
    ap->rsn_len = sizeof(beacon_rsn);
    memcpy(ap->rsn_m3, beacon_rsn, sizeof(beacon_rsn));
    ap->rsn_m3_len = sizeof(beacon_rsn);
    ap->sta_rsn_len = wpa_rsn_ie(ap->sta_rsn, sizeof(ap->sta_rsn));
    ap->counter = 1;
    for(i = 0; i < 32; i++) ap->anonce[i] = (u8)(0xe0 + i);          /* above any SNonce of crypto_random's */
    for(i = 0; i < 16; i++) ap->gtk[i] = (u8)(0x90 + i);
    ap->gtk_id = 1;
    ap->rsc[0] = 0x2a;                                                   /* PN 0x0000000b012a, low byte first */
    ap->rsc[1] = 0x01;
    ap->rsc[2] = 0x0b;
}

/* PTK = PRF-384(PMK, "Pairwise key expansion", Min(AA, SPA) || Max || Min(ANonce, SNonce) || Max) */
static void ap_derive(struct ap *ap) {
    u8 data[76], out[48];
    const u8 *a1 = ap->aa, *a2 = ap->spa, *n1 = ap->anonce, *n2 = ap->snonce;
    if(memcmp(a1, a2, 6) > 0) { a1 = ap->spa; a2 = ap->aa; }
    if(memcmp(n1, n2, 32) > 0) { n1 = ap->snonce; n2 = ap->anonce; }
    memcpy(data, a1, 6);
    memcpy(data + 6, a2, 6);
    memcpy(data + 12, n1, 32);
    memcpy(data + 44, n2, 32);
    ieee80211_prf_sha1(ap->pmk, 32, "Pairwise key expansion", data, 76, out, 48);
    memcpy(ap->kck, out, 16);
    memcpy(ap->kek, out + 16, 16);
    memcpy(ap->tk, out + 32, 16);
}

static int mic_good(const u8 *kck, const u8 *f, u_int len) {
    u8 copy[1200], mic[20];
    if(len > sizeof(copy) || len < 99) return 0;
    memcpy(copy, f, len);
    memset(copy + 81, 0, 16);
    hmac_sha1(kck, 16, copy, len, mic);
    return memcmp(mic, f + 81, 16) == 0;
}

/* an EAPOL-Key frame from the AP: 802.1X-2004 (version 2) like hostapd's, the next replay counter */
static u_int ap_frame(struct ap *ap, u8 *f, u16 info, u16 key_len, const u8 *nonce, const u8 *rsc, const u8 *kd,
                      u_int kd_len, u64 *used) {
    u8 h[20];
    memset(f, 0, 99);
    f[0] = 2;
    f[1] = 3;
    put_be16(f + 2, (u16)(95 + kd_len));
    f[4] = 2;
    put_be16(f + 5, info);
    put_be16(f + 7, key_len);
    *used = ap->counter;
    put_be64(f + 9, ap->counter++);
    if(nonce) memcpy(f + 17, nonce, 32);
    if(rsc) memcpy(f + 65, rsc, 8);
    put_be16(f + 97, (u16)kd_len);
    if(kd_len) memcpy(f + 99, kd, kd_len);
    if(info & 0x0100) {
        hmac_sha1(ap->kck, 16, f, 99 + kd_len, h);
        memcpy(f + 81, h, 16);
    }
    return 99 + kd_len;
}

/* message 1, with the PMKID KDE hostapd adds: HMAC-SHA1-128(PMK, "PMK Name" || AA || SPA) */
static u_int ap_m1(struct ap *ap, u8 *f) {
    u8 in[20], h[20], kde[22] = { 0xdd, 20, 0x00, 0x0f, 0xac, 4 };
    memcpy(in, "PMK Name", 8);
    memcpy(in + 8, ap->aa, 6);
    memcpy(in + 14, ap->spa, 6);
    hmac_sha1(ap->pmk, 32, in, 20, h);
    memcpy(kde + 6, h, 16);
    return ap_frame(ap, f, 0x008a, 16, ap->anonce, NULL, kde, sizeof(kde), &ap->last_m1);
}

/* message 2 as the AP checks it: 0 taken (it has the PTK), -1 malformed, -2 its MIC did not check */
static int ap_take_m2(struct ap *ap, const u8 *f, u_int len) {
    if(len < 99 || f[0] != 1 || f[1] != 3 || get_be16(f + 2) != len - 4 || f[4] != 2) return -1;
    if(get_be16(f + 5) != 0x010a || get_be16(f + 7) != 0 || get_be64(f + 9) != ap->last_m1) return -1;
    if(get_be16(f + 97) != len - 99 || len - 99 != ap->sta_rsn_len || memcmp(f + 99, ap->sta_rsn, ap->sta_rsn_len)) return -1;
    if(!all_zero(f + 49, 32)) return -1;                                 /* IV, RSC and reserved */
    memcpy(ap->snonce, f + 17, 32);
    ap_derive(ap);
    if(!mic_good(ap->kck, f, len) && !ap->careless) return -2;
    return 0;
}

/* key data: the given elements, the GTK KDE, padding to 8 bytes (0xdd then zeros) */
static u_int gtk_key_data(const struct ap *ap, u8 *p, const u8 *elems, u_int elems_len, u_int gtk_len, u8 id) {
    u_int n = elems_len;
    if(elems_len) memcpy(p, elems, elems_len);
    p[n++] = 0xdd;
    p[n++] = (u8)(6 + gtk_len);
    p[n++] = 0x00;
    p[n++] = 0x0f;
    p[n++] = 0xac;
    p[n++] = 1;
    p[n++] = id;
    p[n++] = 0;
    memset(p + n, 0, gtk_len);
    memcpy(p + n, ap->gtk, gtk_len < 16 ? gtk_len : 16);
    n += gtk_len;
    if(n % 8 || n < 16) {
        p[n++] = 0xdd;
        while(n % 8 || n < 16) p[n++] = 0;
    }
    return n;
}

static u_int ap_m3_custom(struct ap *ap, u8 *f, u16 info, u16 key_len, u_int gtk_len, u8 id, int wrap) {
    u8 plain[200], wrapped[208];
    u_int pl = gtk_key_data(ap, plain, ap->rsn_m3, ap->rsn_m3_len, gtk_len, id);
    if(!wrap) return ap_frame(ap, f, info, key_len, ap->anonce, ap->rsc, plain, pl, &ap->last_m3);
    aes_wrap(ap->kek, plain, pl, wrapped);
    return ap_frame(ap, f, info, key_len, ap->anonce, ap->rsc, wrapped, pl + 8, &ap->last_m3);
}

static u_int ap_m3(struct ap *ap, u8 *f) { return ap_m3_custom(ap, f, 0x13ca, 16, 16, ap->gtk_id, 1); }

static int ap_take_m4(struct ap *ap, const u8 *f, u_int len) {
    if(len != 99 || f[1] != 3 || get_be16(f + 2) != 95 || f[4] != 2 || get_be16(f + 5) != 0x030a) return -1;
    if(get_be16(f + 7) || get_be64(f + 9) != ap->last_m3 || get_be16(f + 97) || !all_zero(f + 17, 64)) return -1;
    return mic_good(ap->kck, f, len) ? 0 : -2;
}

static u_int ap_g1(struct ap *ap, u8 *f) {
    u8 plain[64], wrapped[72];
    u_int pl = gtk_key_data(ap, plain, NULL, 0, 16, ap->gtk_id);
    aes_wrap(ap->kek, plain, pl, wrapped);
    return ap_frame(ap, f, 0x1382, 0, NULL, ap->rsc, wrapped, pl + 8, &ap->last_g1);
}

static int ap_take_g2(struct ap *ap, const u8 *f, u_int len) {
    if(len != 99 || f[1] != 3 || get_be16(f + 2) != 95 || f[4] != 2 || get_be16(f + 5) != 0x0302) return -1;
    if(get_be16(f + 7) || get_be64(f + 9) != ap->last_g1 || get_be16(f + 97) || !all_zero(f + 17, 64)) return -1;
    return mic_good(ap->kck, f, len) ? 0 : -2;
}

/* a supplicant for this AP; the PMK is the AP's unless another is given */
static void sta_start(struct sta *s, struct ap *ap, const u8 *pmk) {
    memset(s, 0, sizeof(*s));
    s->sm = wpa_start(pmk ? pmk : ap->pmk, ap->aa, ap->spa, ap->sta_rsn, ap->sta_rsn_len, ap->rsn, ap->rsn_len,
                      &sta_ops, s);
}

static void sta_end(struct sta *s) {
    if(s->sm) wpa_free(s->sm);
    s->sm = NULL;
}

/* messages 1 to 4; 0 when the AP took messages 2 and 4 */
static int handshake(struct sta *s, struct ap *ap) {
    u8 f[600];
    u_int n = s->nsent;
    deliver(s, f, ap_m1(ap, f));
    if(s->nsent != n + 1 || ap_take_m2(ap, s->sent[n], s->sent_len[n])) return -1;
    deliver(s, f, ap_m3(ap, f));
    if(s->nsent != n + 2 || ap_take_m4(ap, s->sent[n + 1], s->sent_len[n + 1])) return -2;
    return 0;
}

/**** the tests ****/

static void test_rsn_ie(void) {
    u8 b[64];
    static const u8 want[22] = { 0x30, 0x14, 0x01, 0x00, 0x00, 0x0f, 0xac, 0x04, 0x01, 0x00, 0x00, 0x0f, 0xac, 0x04,
                                 0x01, 0x00, 0x00, 0x0f, 0xac, 0x02, 0x00, 0x00 };
    memset(b, 0xee, sizeof(b));
    check(wpa_rsn_ie(b, sizeof(b)) == 22 && memcmp(b, want, 22) == 0 && b[22] == 0xee,
          "our RSN element: version 1, group CCMP, pairwise CCMP, AKM PSK, capabilities 0");
    check(wpa_rsn_ie(b, 21) == 0 && wpa_rsn_ie(NULL, 64) == 0, "... 0 when it does not fit");
}

static void test_passphrase(void) {
    u8 pmk[32], z[32];
    u_int i;
    char p[80];
    memset(z, 0, sizeof(z));
    check(wpa_passphrase_to_pmk("password", (const u8 *)"IEEE", 4, pmk) == 0, "\"password\", SSID \"IEEE\"");
    {
        u8 want[32];
        unhex("f42c6fc52df0ebef9ebb4b90b38a5f902e83fe1b135a70e23aed762e9710a12e", want);
        check(memcmp(pmk, want, 32) == 0, "... the PSK of 802.11-2016 J.4.3");
    }
    check(wpa_passphrase_to_pmk("ThisIsAPassword", (const u8 *)"ThisIsASSID", 11, pmk) == 0, "\"ThisIsAPassword\"");
    {
        u8 want[32];
        unhex("0dc0d6eb90555ed6419756b9a15ec3e3209b63df707dd508d14581f8982721af", want);
        check(memcmp(pmk, want, 32) == 0, "... J.4.3's second PSK");
    }
    memset(p, 'a', 32);
    p[32] = 0;
    check(wpa_passphrase_to_pmk(p, (const u8 *)"ZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZ", 32, pmk) == 0, "32 'a', a 32-byte SSID");
    {
        u8 want[32];
        unhex("becb93866bb8c3832cb777c2f559807c8c59afcb6eae734885001300a981cc62", want);
        check(memcmp(pmk, want, 32) == 0, "... J.4.3's third PSK");
    }
    check(wpa_passphrase_to_pmk("1234567", (const u8 *)"x", 1, pmk) == -1, "7 characters: -1");
    check(wpa_passphrase_to_pmk("12345678", (const u8 *)"x", 1, pmk) == 0, "8 characters: fine");
    for(i = 0; i < 63; i++) p[i] = (char)('!' + i);
    p[63] = 0;
    check(wpa_passphrase_to_pmk(p, (const u8 *)"x", 1, pmk) == 0, "63 printable characters: fine");
    p[63] = 'x';
    p[64] = 0;
    check(wpa_passphrase_to_pmk(p, (const u8 *)"x", 1, pmk) == -1, "64 characters: -1 (not a hex PSK here)");
    check(wpa_passphrase_to_pmk("pass\tword", (const u8 *)"x", 1, pmk) == -1, "a tab: -1");
    check(wpa_passphrase_to_pmk("passw\x7f" "rd", (const u8 *)"x", 1, pmk) == -1, "DEL: -1");
    check(wpa_passphrase_to_pmk("p\xc3\xa4sswort", (const u8 *)"x", 1, pmk) == -1, "UTF-8 (not ASCII): -1");
    check(wpa_passphrase_to_pmk("password", (const u8 *)"ZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZ", 33, pmk) == -1,
          "a 33-byte SSID: -1");
    check(wpa_passphrase_to_pmk(NULL, (const u8 *)"x", 1, pmk) == -1, "no passphrase: -1");
}

static void check_keys(struct sta *s, struct ap *ap, const char *what) {
    char w[160];
    snprintf(w, sizeof(w), "%s: the pairwise key installed is the AP's TK (CCMP, index 0, 16 bytes)", what);
    check(s->nkeys >= 1 && s->keys[0].pairwise && s->keys[0].cipher == WLAN_CIPHER_CCMP && s->keys[0].index == 0 &&
          s->keys[0].len == 16 && memcmp(s->keys[0].key, ap->tk, 16) == 0 && all_zero(s->keys[0].key + 16, 16) &&
          all_zero(s->keys[0].rsc, 8), w);
    snprintf(w, sizeof(w), "%s: then the group key: the AP's GTK, its key id and RSC", what);
    check(s->nkeys >= 2 && !s->keys[1].pairwise && s->keys[1].cipher == WLAN_CIPHER_CCMP && s->keys[1].index == ap->gtk_id &&
          s->keys[1].len == 16 && memcmp(s->keys[1].key, ap->gtk, 16) == 0 && memcmp(s->keys[1].rsc, ap->rsc, 8) == 0, w);
}

static void test_success(void) {
    struct ap ap;
    struct sta s;
    u8 f[600];
    u_int i;
    size_t n;
    const u8 *blk;
    int found;

    log_clear();
    ap_init(&ap, "correct horse battery", "HouseOfTest");
    sta_start(&s, &ap, NULL);
    check(s.sm != NULL, "wpa_start");
    for(i = 0; i < 32; i++) next_rand[i] = (u8)(0x10 + i);
    rand_scripted = 1;
    deliver(&s, f, ap_m1(&ap, f));
    check(s.nsent == 1 && s.sent_len[0] == 99 + 22, "message 1 is answered with message 2 (our RSN element in it)");
    check(s.sent[0][0] == 1 && s.sent[0][1] == 3 && get_be16(s.sent[0] + 2) == 117 && s.sent[0][4] == 2,
          "... EAPOL version 1, EAPOL-Key, its body length, RSN descriptor");
    check(get_be16(s.sent[0] + 5) == 0x010a && get_be16(s.sent[0] + 7) == 0,
          "... key information 0x010a (version 2, pairwise, MIC), key length 0");
    check(get_be64(s.sent[0] + 9) == ap.last_m1 && memcmp(s.sent[0] + 17, next_rand, 32) == 0,
          "... message 1's replay counter, the SNonce from crypto_random");
    check(ap_take_m2(&ap, s.sent[0], s.sent_len[0]) == 0, "... the AP checks its MIC with the PTK it derives");
    check(s.nkeys == 0 && s.ndone == 0, "... no keys yet");
    deliver(&s, f, ap_m3(&ap, f));
    check(s.nsent == 2 && ap_take_m4(&ap, s.sent[1], s.sent_len[1]) == 0,
          "message 3 is answered with message 4 (0x030a, message 3's counter, no nonce or data): the AP takes it");
    check_keys(&s, &ap, "success");
    check(s.nkeys == 2 && s.ndone == 1 && s.status == WPA_OK, "done(WPA_OK), once");
    check(strcmp(s.ev, "SSPGD") == 0, "in order: message 2, message 4, the pairwise key, the group key, done");
    check(strstr(logbuf, "4-way handshake done") != NULL && log_tidy(), "the log says so, every line +WPA:");

    /* the supplicant's memory holds the secrets until wpa_free wipes it */
    blk = (const u8 *)s.sm;
    n = block_size(s.sm);
    for(found = 0, i = 0; i + 16 <= n; i++)
        if(!memcmp(blk + i, ap.tk, 16) || !memcmp(blk + i, ap.pmk, 16)) found++;
    check(found >= 2, "the supplicant holds the PMK and the TK");
    check(!log_has(ap.pmk, 32) && !log_has(ap.kck, 16) && !log_has(ap.kek, 16) && !log_has(ap.tk, 16) &&
          !log_has(ap.gtk, 16) && !strstr(logbuf, "correct horse"), "nothing secret in the log");
    i = unwiped_frees;
    wpa_free(s.sm);
    s.sm = NULL;
    check(unwiped_frees == i, "wpa_free wipes the whole supplicant (PMK, PTKs, GTK, nonces) before freeing it");
}

/* the public capture: AP 50:0f:80:70:18:d0, station 40:40:a7:50:73:db */
static void test_capture(void) {
    static const char *m1 = "0203007502008a0010000000000000000115adf473164f43a34f211ebc34495b588af5b915c0dd4478f5fbc89d2f7b"
        "d0fa0000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000016dd14000fa"
        "c04b9c9f71f0c96f62b6c11f545d2dff41b";
    static const char *m2 = "0103007502010a000000000000000000011b9717293f9d9d6979d94b36dbc9d83418bbce09f72edc1e1ae4fd7982"
        "1ffda400000000000000000000000000000000000000000000000000000000000000002f8e7921e572afd75a7c898e625ffb430016301401"
        "00000fac040100000fac040100000fac023c00";
    static const char *m3 = "020300970213ca0010000000000000000215adf473164f43a34f211ebc34495b588af5b915c0dd4478f5fbc89d2f"
        "7bd0fa0000000000000000000000000000000000000000000000000000000000000000e481fe9d4a2e0a53dd1119fb361043300038b43c77"
        "37faedbf17306b5ea1d5059fd5379d0145fa6ddac1e08d460de93cd72c103a7fb2c21445d2b740139c80ac569ae1e865a198c79a21";
    static const char *m4 = "0103005f02030a000000000000000000020000000000000000000000000000000000000000000000000000000000"
        "000000000000000000000000000000000000000000000000000000000000000000000014ac2c3067058ee2c6fc3f5a7d5a58390000";
    u8 aa[6], spa[6], rsn[22], pmk[32], want[32], f[200], g[200];
    u_int len, glen;
    struct sta s;

    log_clear();
    unhex("500f807018d0", aa);
    unhex("4040a75073db", spa);
    unhex("30140100000fac040100000fac040100000fac023c00", rsn);    /* the beacon's and the station's */
    check(wpa_passphrase_to_pmk("wireshark", (const u8 *)"ikeriri-5g", 10, pmk) == 0, "capture: \"wireshark\" on ikeriri-5g");
    unhex("9b14886c1a4915a1a68baae91b67b903c356135bcb71ee44a4a6f5dad9af738f", want);
    check(memcmp(pmk, want, 32) == 0, "capture: the PMK");
    memset(&s, 0, sizeof(s));
    s.sm = wpa_start(pmk, aa, spa, rsn, 22, rsn, 22, &sta_ops, &s);
    unhex("1b9717293f9d9d6979d94b36dbc9d83418bbce09f72edc1e1ae4fd79821ffda4", next_rand);
    rand_scripted = 1;
    len = unhex(m1, f);
    deliver(&s, f, len);
    glen = unhex(m2, g);
    check(s.nsent == 1 && s.sent_len[0] == glen && memcmp(s.sent[0], g, glen) == 0,
          "capture: the AP's message 1 is answered with the station's message 2, byte for byte (MIC 2f8e7921...)");
    len = unhex(m3, f);
    deliver(&s, f, len);
    glen = unhex(m4, g);
    check(s.nsent == 2 && s.sent_len[1] == glen && memcmp(s.sent[1], g, glen) == 0,
          "capture: the AP's message 3 (MIC e481fe9d... checked) is answered with the station's message 4");
    unhex("99775e9a0854ac7899e11147547dd8f7", want);
    check(s.nkeys == 2 && s.keys[0].pairwise && memcmp(s.keys[0].key, want, 16) == 0, "capture: the TK installed");
    unhex("eab4e5b93588db11d1ecfda6eac5606b", want);
    check(s.nkeys == 2 && !s.keys[1].pairwise && s.keys[1].index == 1 && memcmp(s.keys[1].key, want, 16) == 0 &&
          all_zero(s.keys[1].rsc, 8), "capture: the GTK from the unwrapped key data, key 1, RSC 0");
    check(s.ndone == 1 && s.status == WPA_OK && strcmp(s.ev, "SSPGD") == 0, "capture: done(WPA_OK) after the keys");
    check(!log_has(pmk, 32) && !log_has(want, 16), "capture: nothing secret in the log");
    sta_end(&s);
}

static void test_wrong_password(void) {
    struct ap ap;
    struct sta s;
    u8 f[600], pmk[32];
    u_int i;

    /* a careful AP (as hostapd): message 2's MIC does not check, so it sends message 1 again */
    log_clear();
    ap_init(&ap, "the right password", "HouseOfTest");
    wpa_passphrase_to_pmk("the wrong password", (const u8 *)"HouseOfTest", 11, pmk);
    sta_start(&s, &ap, pmk);
    for(i = 0; i < 3; i++) {
        deliver(&s, f, ap_m1(&ap, f));
        check(s.nsent == i + 1 && ap_take_m2(&ap, s.sent[i], s.sent_len[i]) == -2,
              "wrong password: the AP finds message 2's MIC wrong");
        ticks(&s, 10);                                  /* hostapd waits 100 ms, then 1 s */
    }
    check(s.ndone == 0, "... two retransmissions of message 1 are answered");
    deliver(&s, f, ap_m1(&ap, f));
    check(s.ndone == 1 && s.status == WPA_E_MIC && s.nsent == 3 && s.nkeys == 0,
          "... the third: done(WPA_E_MIC), no fourth message 2, no keys");
    deliver(&s, f, ap_m1(&ap, f));
    ticks(&s, 100);
    check(s.ndone == 1 && s.nsent == 3, "... and nothing more after it");
    check(strstr(logbuf, "wrong password") != NULL && !strstr(logbuf, "the wrong password"), "... the log suggests why");
    sta_end(&s);

    /* a careless AP sends message 3 anyway: its MIC fails three times */
    ap_init(&ap, "the right password", "HouseOfTest");
    ap.careless = 1;
    sta_start(&s, &ap, pmk);
    deliver(&s, f, ap_m1(&ap, f));
    check(ap_take_m2(&ap, s.sent[0], s.sent_len[0]) == 0, "careless AP: takes message 2 without its MIC");
    deliver(&s, f, ap_m3(&ap, f));
    deliver(&s, f, ap_m3(&ap, f));
    check(s.ndone == 0 && s.nsent == 1, "... two message 3s whose MIC fails are dropped");
    deliver(&s, f, ap_m3(&ap, f));
    check(s.ndone == 1 && s.status == WPA_E_MIC && s.nsent == 1 && s.nkeys == 0, "... the third: done(WPA_E_MIC)");
    sta_end(&s);
}

static void test_replay(void) {
    struct ap ap;
    struct sta s;
    u8 f[600], m1[600], m3[600], g1[600];
    u_int m1_len, m3_len, g1_len;

    ap_init(&ap, "replay password", "HouseOfTest");
    sta_start(&s, &ap, NULL);
    m1_len = ap_m1(&ap, m1);
    deliver(&s, m1, m1_len);
    ap_take_m2(&ap, s.sent[0], s.sent_len[0]);
    m3_len = ap_m3(&ap, m3);
    deliver(&s, m3, m3_len);
    deliver(&s, m3, m3_len);
    check(s.nsent == 2 && s.nkeys == 2 && s.ndone == 1, "message 3 delivered twice: the copy is a replay, ignored");

    /* message 4 was lost: the AP retransmits message 3 with the next counter */
    deliver(&s, f, ap_m3(&ap, f));
    check(s.nsent == 3 && ap_take_m4(&ap, s.sent[2], s.sent_len[2]) == 0,
          "a retransmitted message 3 is answered with another message 4 for its counter");
    check(s.nkeys == 2 && s.ndone == 1, "... but no key is installed again (key reinstallation)");
    deliver(&s, m1, m1_len);
    deliver(&s, m3, m3_len);
    check(s.nsent == 3, "the first message 1 and 3 again: below the replay counter, ignored");

    g1_len = ap_g1(&ap, g1);
    deliver(&s, g1, g1_len);
    check(s.nsent == 4 && ap_take_g2(&ap, s.sent[3], s.sent_len[3]) == 0 && s.nkeys == 2,
          "group message 1 with the GTK it has: answered, not reinstalled");
    deliver(&s, g1, g1_len);
    check(s.nsent == 4, "... the same frame again: a replay, not answered");
    ap.gtk[0] ^= 0xff;
    ap.gtk_id = 2;
    deliver(&s, f, ap_g1(&ap, f));
    check(s.nsent == 5 && s.nkeys == 3 && s.keys[2].index == 2 && !s.keys[2].pairwise, "a new GTK is installed");
    deliver(&s, f, ap_g1(&ap, f));
    check(s.nsent == 6 && s.nkeys == 3, "... and its retransmission is answered, not reinstalled");
    sta_end(&s);
}

static void test_rsn_mismatch(void) {
    static const u8 tkip_too[26] = { 0x30, 0x18, 1, 0, 0, 0x0f, 0xac, 4, 2, 0, 0, 0x0f, 0xac, 2, 0, 0x0f, 0xac, 4,
                                     1, 0, 0, 0x0f, 0xac, 2, 0x0c, 0 };
    static const u8 second_ccmp[22] = { 0x30, 0x14, 1, 0, 0, 0x0f, 0xac, 4, 1, 0, 0, 0x0f, 0xac, 4, 1, 0, 0, 0x0f,
                                        0xac, 2, 0, 0 };
    struct ap ap;
    struct sta s;
    u8 f[600];

    ap_init(&ap, "rsn password", "HouseOfTest");
    memcpy(ap.rsn_m3, tkip_too, sizeof(tkip_too));                      /* a downgrade: TKIP offered too */
    ap.rsn_m3_len = sizeof(tkip_too);
    sta_start(&s, &ap, NULL);
    deliver(&s, f, ap_m1(&ap, f));
    ap_take_m2(&ap, s.sent[0], s.sent_len[0]);
    deliver(&s, f, ap_m3(&ap, f));
    check(s.ndone == 1 && s.status == WPA_E_RSN && s.nsent == 1 && s.nkeys == 0,
          "message 3's RSN element differs from the beacon's: done(WPA_E_RSN), no message 4, no keys");
    sta_end(&s);

    ap_init(&ap, "rsn password", "HouseOfTest");
    ap.rsn_m3[20] = 0;                                                   /* the capabilities differ */
    sta_start(&s, &ap, NULL);
    check(handshake(&s, &ap) == -2 && s.status == WPA_E_RSN, "... one bit of the capabilities is enough");
    sta_end(&s);

    ap_init(&ap, "rsn password", "HouseOfTest");
    memcpy(ap.rsn_m3 + 22, tkip_too, sizeof(tkip_too));                 /* a second element naming TKIP */
    ap.rsn_m3_len = 22 + sizeof(tkip_too);
    sta_start(&s, &ap, NULL);
    check(handshake(&s, &ap) == -2 && s.status == WPA_E_RSN, "a second RSN element with TKIP: WPA_E_RSN");
    sta_end(&s);

    ap_init(&ap, "rsn password", "HouseOfTest");
    memcpy(ap.rsn_m3 + 22, second_ccmp, sizeof(second_ccmp));
    ap.rsn_m3_len = 44;
    sta_start(&s, &ap, NULL);
    check(handshake(&s, &ap) == 0 && s.status == WPA_OK, "a second RSN element naming CCMP: fine");
    sta_end(&s);

    ap_init(&ap, "rsn password", "HouseOfTest");
    ap.rsn_len = 0;                                                      /* the station saw no RSN element */
    sta_start(&s, &ap, NULL);
    check(handshake(&s, &ap) == -2 && s.status == WPA_E_RSN, "no beacon RSN element to compare: WPA_E_RSN");
    sta_end(&s);
}

/* message 3 from the AP itself (its MIC checks) with something wrong: the handshake ends at once */
static void bad_m3(u16 info, u16 key_len, u_int gtk_len, u8 id, int wrap, int wrong_kek, const char *what) {
    struct ap ap;
    struct sta s;
    u8 f[600];
    char w[120];
    ap_init(&ap, "protocol password", "HouseOfTest");
    sta_start(&s, &ap, NULL);
    deliver(&s, f, ap_m1(&ap, f));
    ap_take_m2(&ap, s.sent[0], s.sent_len[0]);
    if(wrong_kek) ap.kek[0] ^= 1;
    deliver(&s, f, ap_m3_custom(&ap, f, info, key_len, gtk_len, id, wrap));
    snprintf(w, sizeof(w), "message 3 %s: done(WPA_E_PROTOCOL), no message 4, no keys", what);
    check(s.ndone == 1 && s.status == WPA_E_PROTOCOL && s.nsent == 1 && s.nkeys == 0, w);
    sta_end(&s);
}

static u_int mkframe(u8 *f, u_int body, u16 info, u_int data_len) {
    memset(f, 0, 4 + body);
    f[0] = 2;
    f[1] = 3;
    put_be16(f + 2, (u16)body);
    if(body >= 1) f[4] = 2;
    if(body >= 3) put_be16(f + 5, info);
    if(body >= 95) put_be16(f + 97, (u16)data_len);
    return 4 + body;
}

static void test_malformed(void) {
    static u8 big[70000];
    struct ap ap;
    struct sta s;
    u8 m1[600], f[600], eap[9] = { 2, 0, 0, 5, 1, 1, 0, 5, 1 };
    u_int m1_len, cut, i;
    int ok;

    bad_m3(0x13ca, 32, 16, 1, 1, 0, "with key length 32");
    bad_m3(0x13ca, 16, 16, 1, 0, 0, "whose key data is not wrapped");
    bad_m3(0x13ca, 16, 16, 1, 1, 1, "wrapped with another KEK");
    bad_m3(0x13ca, 16, 32, 1, 1, 0, "with a 32-byte (TKIP) group key");
    bad_m3(0x13ca, 16, 16, 0, 1, 0, "with group key id 0");

    /* every truncation of message 1 (its length fields left as they were) */
    ap_init(&ap, "malformed password", "HouseOfTest");
    m1_len = ap_m1(&ap, m1);
    for(ok = 1, cut = 0; cut < m1_len; cut++) {
        sta_start(&s, &ap, NULL);
        deliver(&s, m1, cut);
        if(s.nsent || s.ndone) ok = 0;
        deliver(&s, m1, m1_len);
        if(s.nsent != 1) ok = 0;
        sta_end(&s);
    }
    check(ok, "message 1 cut at each of its lengths: dropped, and the whole one is still answered");

    sta_start(&s, &ap, NULL);
    for(i = 0; i < 10; i++) deliver(&s, eap, sizeof(eap));
    deliver(&s, eap, 3);
    check(s.nsent == 0 && s.ndone == 0, "EAP packets (an 802.1X network's): ignored, not counted as bad");
    memcpy(f, m1, m1_len);
    memset(f + m1_len, 0x77, 40);
    deliver(&s, f, m1_len + 40);
    check(s.nsent == 1, "bytes after the EAPOL body (padding) are ignored");
    sta_end(&s);

    sta_start(&s, &ap, NULL);
    deliver(&s, f, mkframe(f, 94, 0x008a, 0));
    deliver(&s, f, mkframe(f, 95, 0x008a, 1));                          /* key data runs past the body */
    memcpy(f, m1, m1_len);
    put_be16(f + 97, 0xffff);
    deliver(&s, f, m1_len);
    memcpy(f, m1, m1_len);
    f[4] = 254;                                                          /* WPA1's descriptor */
    deliver(&s, f, m1_len);
    check(s.nsent == 0 && s.ndone == 0, "short bodies, key data past the end, another descriptor: dropped");
    memset(big, 0, sizeof(big));
    memcpy(big, m1, m1_len);
    put_be16(big + 2, 0xffff);
    deliver(&s, big, 4 + 0xffff);
    check(s.nsent == 0 && s.ndone == 1 && s.status == WPA_E_PROTOCOL, "an oversized one is the fifth: WPA_E_PROTOCOL");
    sta_end(&s);

    sta_start(&s, &ap, NULL);
    memcpy(f, m1, m1_len);
    put_be16(f + 5, 0x008b);                                             /* key descriptor version 3 */
    deliver(&s, f, m1_len);
    put_be16(f + 5, 0x088a);                                             /* the Request bit */
    deliver(&s, f, m1_len);
    put_be16(f + 5, 0x000a);                                             /* no Ack */
    deliver(&s, f, m1_len);
    put_be16(f + 5, 0x00ca);                                             /* Install in message 1 */
    deliver(&s, f, m1_len);
    check(s.nsent == 0 && s.ndone == 0, "version 3, Request, no Ack, Install: dropped (4 bad frames)");
    sta_end(&s);

    /* frames in the wrong order */
    ap_init(&ap, "malformed password", "HouseOfTest");
    sta_start(&s, &ap, NULL);
    deliver(&s, f, ap_m3(&ap, f));
    deliver(&s, f, ap_g1(&ap, f));
    check(s.nsent == 0 && s.ndone == 0 && s.nkeys == 0, "message 3 or a group message before message 1: dropped");
    deliver(&s, f, ap_m1(&ap, f));
    ap_take_m2(&ap, s.sent[0], s.sent_len[0]);
    ap.anonce[5] ^= 1;
    deliver(&s, f, ap_m3(&ap, f));
    ap.anonce[5] ^= 1;
    check(s.nsent == 1 && s.nkeys == 0, "message 3 with another ANonce than message 1's: dropped");
    deliver(&s, f, ap_m3(&ap, f));
    check(s.nsent == 2 && s.ndone == 1 && s.status == WPA_OK, "... the right one then completes the handshake");
    sta_end(&s);
}

static u32 rng = 0x2545f491;
static u32 rnd(void) {
    rng ^= rng << 13;
    rng ^= rng >> 17;
    rng ^= rng << 5;
    return rng;
}

static u_int mutate(u8 *f, u_int len, u_int cap) {
    u_int k = rnd() % 7, i, n;
    switch(k) {
    case 0: for(n = 1 + rnd() % 4; n; n--) { i = rnd() % len; f[i] ^= (u8)(1 << (rnd() % 8)); } break;
    case 1: f[rnd() % len] = (u8)rnd(); break;
    case 2: return rnd() % len;
    case 3: n = rnd() % 64; for(i = 0; i < n && len < cap; i++) f[len++] = (u8)rnd(); break;
    case 4: put_be16(f + 2, (u16)rnd()); break;
    case 5: put_be16(f + 97, (u16)(rnd() % 400)); break;
    default: f[5 + rnd() % 4] = (u8)rnd(); break;
    }
    return len;
}

static void test_fuzz(void) {
    struct ap ap, ap0;
    struct sta s;
    u8 m1[600], m3[600], f[1200];
    u_int m1_len, m3_len, len, iter, bad_m1 = 0, bad_m3 = 0;

    ap_init(&ap0, "fuzz password", "HouseOfTest");
    ap = ap0;
    m1_len = ap_m1(&ap, m1);
    for(iter = 0; iter < 1500; iter++) {
        sta_start(&s, &ap, NULL);
        memcpy(f, m1, m1_len);
        len = mutate(f, m1_len, sizeof(f));
        deliver(&s, f, len);
        if(s.nkeys || s.ndone || (s.nsent && (s.sent_len[0] != 121 || get_be16(s.sent[0] + 5) != 0x010a))) bad_m1++;
        sta_end(&s);
    }
    check(!bad_m1, "1500 mutated message 1s: never a key, never done(), only well-formed message 2s");

    for(iter = 0; iter < 1500; iter++) {
        ap = ap0;
        sta_start(&s, &ap, NULL);
        deliver(&s, f, ap_m1(&ap, f));
        ap_take_m2(&ap, s.sent[0], s.sent_len[0]);
        m3_len = ap_m3(&ap, m3);
        memcpy(f, m3, m3_len);
        len = mutate(f, m3_len, sizeof(f));
        deliver(&s, f, len);
        if(len >= m3_len && !memcmp(f, m3, m3_len)) {
            if(s.ndone != 1 || s.status != WPA_OK) bad_m3++;     /* only bytes after it changed */
        } else if(s.nkeys || s.ndone || s.nsent != 1) bad_m3++;
        sta_end(&s);
    }
    check(!bad_m3, "1500 mutated message 3s: a changed frame never installs a key, ends anything or is answered");
}

static void test_rekey(void) {
    struct ap ap;
    struct sta s;
    u8 f[600], old_tk[16];
    u_int i;

    log_clear();
    ap_init(&ap, "rekey password", "HouseOfTest");
    sta_start(&s, &ap, NULL);
    check(handshake(&s, &ap) == 0 && s.ndone == 1, "rekeys: the first handshake");

    /* group key handshake: a new GTK under key id 2 */
    for(i = 0; i < 16; i++) ap.gtk[i] = (u8)(0x33 * i);
    ap.gtk_id = 2;
    ap.rsc[0] = 0x99;
    deliver(&s, f, ap_g1(&ap, f));
    check(s.nsent == 3 && ap_take_g2(&ap, s.sent[2], s.sent_len[2]) == 0,
          "group message 1 is answered with group message 2 (0x0302, its counter, MIC with the KCK)");
    check(s.nkeys == 3 && !s.keys[2].pairwise && s.keys[2].index == 2 && memcmp(s.keys[2].key, ap.gtk, 16) == 0 &&
          memcmp(s.keys[2].rsc, ap.rsc, 8) == 0, "... and the new group key goes in: key 2, its RSC");
    check(s.ndone == 1 && strstr(logbuf, "group key renewed") != NULL, "... done() is not called again");
    ap.kck[0] ^= 1;
    ap.gtk_id = 1;
    deliver(&s, f, ap_g1(&ap, f));
    ap.kck[0] ^= 1;
    ap.gtk_id = 2;
    check(s.nsent == 3 && s.nkeys == 3, "a group message 1 whose MIC fails: dropped");

    /* a new 4-way handshake on the same association (a PTK rekey) */
    memcpy(old_tk, ap.tk, 16);
    for(i = 0; i < 32; i++) ap.anonce[i] = (u8)(0xf0 - i);
    i = rand_calls;
    check(handshake(&s, &ap) == 0, "a second 4-way handshake: the AP takes messages 2 and 4");
    check(rand_calls == i + 1 && memcmp(ap.tk, old_tk, 16) != 0, "... with a new SNonce, so a new PTK");
    check(s.nkeys == 4 && s.keys[3].pairwise && memcmp(s.keys[3].key, ap.tk, 16) == 0,
          "... the new pairwise key goes in; the group key it repeats does not");
    check(s.ndone == 1 && strstr(logbuf, "pairwise key renewed") != NULL, "... done() once in all");
    ap.gtk[3] ^= 0x40;
    deliver(&s, f, ap_g1(&ap, f));
    check(s.nkeys == 5 && !s.keys[4].pairwise && ap_take_g2(&ap, s.sent[s.nsent - 1], s.sent_len[s.nsent - 1]) == 0,
          "... and the next group key comes under the new PTK");
    check(!log_has(ap.tk, 16) && !log_has(ap.gtk, 16) && !log_has(old_tk, 16) && log_tidy(), "nothing secret logged");
    sta_end(&s);
}

static void test_timeouts(void) {
    struct ap ap;
    struct sta s;
    u8 f[600];

    ap_init(&ap, "timeout password", "HouseOfTest");
    sta_start(&s, &ap, NULL);
    ticks(&s, 49);
    check(s.ndone == 0, "no message 1 for 4.9 s: still waiting");
    ticks(&s, 1);
    check(s.ndone == 1 && s.status == WPA_E_TIMEOUT, "... at 5 s: done(WPA_E_TIMEOUT)");
    ticks(&s, 100);
    deliver(&s, f, ap_m1(&ap, f));
    check(s.ndone == 1 && s.nsent == 0, "... once: later ticks and frames do nothing");
    sta_end(&s);

    sta_start(&s, &ap, NULL);
    ticks(&s, 30);
    deliver(&s, f, ap_m1(&ap, f));
    ticks(&s, 40);
    deliver(&s, f, ap_m1(&ap, f));                                       /* the AP retransmits message 1 */
    ticks(&s, 49);
    check(s.ndone == 0 && s.nsent == 2, "message 2 sent again: the 5 s for message 3 start over");
    ticks(&s, 1);
    check(s.ndone == 1 && s.status == WPA_E_TIMEOUT, "... no message 3 within 5 s of the last message 2: WPA_E_TIMEOUT");
    sta_end(&s);

    ap_init(&ap, "timeout password", "HouseOfTest");
    sta_start(&s, &ap, NULL);
    handshake(&s, &ap);
    ticks(&s, 1000);
    check(s.ndone == 1 && s.status == WPA_OK, "after the handshake, ticks do nothing");
    wpa_tick(NULL);
    wpa_rx_eapol(NULL, f, 99);
    wpa_free(NULL);
    check(1, "NULL supplicants are ignored");
    sta_end(&s);
}

static void test_keys_refused(void) {
    struct ap ap;
    struct sta s;
    ap_init(&ap, "key password", "HouseOfTest");
    sta_start(&s, &ap, NULL);
    s.refuse = 1;
    handshake(&s, &ap);
    check(s.ndone == 1 && s.status == WPA_E_KEY && strcmp(s.ev, "SSPD") == 0,
          "the driver refuses the pairwise key: done(WPA_E_KEY), no group key tried");
    sta_end(&s);
    ap_init(&ap, "key password", "HouseOfTest");
    sta_start(&s, &ap, NULL);
    s.refuse = 2;
    handshake(&s, &ap);
    check(s.ndone == 1 && s.status == WPA_E_KEY && strcmp(s.ev, "SSPGD") == 0, "... refuses the group key: WPA_E_KEY");
    sta_end(&s);
}

static void test_too_many_bad(void) {
    struct ap ap;
    struct sta s;
    u8 f[600];
    u_int i;
    ap_init(&ap, "bad password", "HouseOfTest");
    sta_start(&s, &ap, NULL);
    for(i = 0; i < 4; i++) deliver(&s, f, mkframe(f, 50, 0x008a, 0));
    check(s.ndone == 0, "four malformed frames: dropped");
    deliver(&s, f, mkframe(f, 50, 0x008a, 0));
    check(s.ndone == 1 && s.status == WPA_E_PROTOCOL, "the fifth: done(WPA_E_PROTOCOL)");
    sta_end(&s);
}

/* done() may free the supplicant: nothing may touch it afterwards (kfree poisons and keeps it) */
static void test_free_in_done(void) {
    struct ap ap;
    struct sta s;
    u8 f[600], pmk[32];
    u_int before = unwiped_frees, i;

    check(quarantine_flush(), "nothing was written to a freed supplicant in the tests so far");
    ap_init(&ap, "free password", "HouseOfTest");
    sta_start(&s, &ap, NULL);
    s.free_in_done = 1;
    handshake(&s, &ap);
    check(s.ndone == 1 && s.status == WPA_OK && !s.sm, "freed in done(WPA_OK)");
    sta_start(&s, &ap, NULL);
    s.free_in_done = 1;
    ticks(&s, 60);
    check(s.ndone == 1 && s.status == WPA_E_TIMEOUT, "freed in done(WPA_E_TIMEOUT), from wpa_tick");
    ap.rsn_m3[21] ^= 1;
    sta_start(&s, &ap, NULL);
    s.free_in_done = 1;
    handshake(&s, &ap);
    check(s.ndone == 1 && s.status == WPA_E_RSN, "freed in done(WPA_E_RSN)");
    wpa_passphrase_to_pmk("not the password", (const u8 *)"HouseOfTest", 11, pmk);
    sta_start(&s, &ap, pmk);
    s.free_in_done = 1;
    for(i = 0; i < 4; i++) deliver(&s, f, ap_m1(&ap, f));
    check(s.ndone == 1 && s.status == WPA_E_MIC, "freed in done(WPA_E_MIC), from message 1");
    sta_start(&s, &ap, NULL);
    s.free_in_done = 1;
    for(i = 0; i < 5; i++) deliver(&s, f, mkframe(f, 50, 0x008a, 0));
    check(s.ndone == 1 && s.status == WPA_E_PROTOCOL, "freed in done(WPA_E_PROTOCOL)");
    check(unwiped_frees == before, "... each wiped before it was freed");
    check(quarantine_intact(), "... and nothing written to any freed supplicant afterwards");
}

static void test_start(void) {
    struct ap ap;
    struct sta s;
    u8 rsn[300];
    static const struct wpa_ops no_done = { sta_send, sta_install, NULL };
    ap_init(&ap, "start password", "HouseOfTest");
    memset(rsn, 0x30, sizeof(rsn));
    check(!wpa_start(NULL, ap.aa, ap.spa, ap.sta_rsn, 22, ap.rsn, 22, &sta_ops, &s), "wpa_start without a PMK: NULL");
    check(!wpa_start(ap.pmk, ap.aa, ap.spa, NULL, 0, ap.rsn, 22, &sta_ops, &s), "... without our RSN element: NULL");
    check(!wpa_start(ap.pmk, ap.aa, ap.spa, rsn, 258, ap.rsn, 22, &sta_ops, &s), "... a 258-byte element: NULL");
    check(!wpa_start(ap.pmk, ap.aa, ap.spa, ap.sta_rsn, 22, rsn, 258, &sta_ops, &s), "... the AP's too long: NULL");
    check(!wpa_start(ap.pmk, ap.aa, ap.spa, ap.sta_rsn, 22, ap.rsn, 22, &no_done, &s), "... without done(): NULL");
    fail_alloc = 1;
    check(!wpa_start(ap.pmk, ap.aa, ap.spa, ap.sta_rsn, 22, ap.rsn, 22, &sta_ops, &s), "... out of memory: NULL");
    fail_alloc = 0;
}

int main(void) {
    guard_init();
    test_rsn_ie();
    test_passphrase();
    test_success();
    test_capture();
    test_wrong_password();
    test_replay();
    test_rsn_mismatch();
    test_malformed();
    test_fuzz();
    test_rekey();
    test_timeouts();
    test_keys_refused();
    test_too_many_bad();
    test_free_in_done();
    test_start();
    check(live_blocks == 0, "every supplicant was freed");
    check(unwiped_frees == 0, "every one wiped first");
    printf("WPA UNIT TESTS %s: %d checks, %d failures\n", failures ? "FAILED" : "PASSED", checks, failures);
    exit(failures ? 1 : 0);
    return 0;
}
