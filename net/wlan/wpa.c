/*
SurfOS Wi-Fi: the WPA2-Personal supplicant
--------------------
File: wpa.c     Date: 10/9/26 (Wi-Fi: WPA2)
--------------------
include/net/wpa.h: the passphrase to the PMK, our RSN element, and one supplicant per
association running the 4-way handshake (IEEE 802.11-2016 12.7.6) and the group key
handshake (12.7.7) for RSN with AKM 2 (PSK) and CCMP, in EAPOL-Key frames of key
descriptor version 2 (HMAC-SHA1-128 MICs, AES key wrap). The brief is
docs/superpowers/plans/2026-10-09-wifi.md (track C).

The access point drives both handshakes and retransmits; the supplicant answers every
valid message, sends nothing on its own, and keeps time with wpa_tick(): no message 1
within 5 s of wpa_start(), or no message 3 within 5 s of our last message 2, ends it with
WPA_E_TIMEOUT. An AP that sends message 1 again three times after our message 2 has taken
none of them, which means their MIC did not check with its PMK: a wrong password, almost
always (WPA_E_MIC); three message 3s whose MIC fails end it the same way.

Everything the AP sends is checked against the length it came in, never only against its
own length fields. A frame whose MIC has not checked changes nothing but counters: five
malformed or unexpected ones end a handshake (WPA_E_PROTOCOL). Replay counters follow
12.7.2: only a frame whose MIC checked moves ours, and nothing at or below it is taken.
Once message 3's MIC has checked, what is wrong in it is the AP's doing and ends the
handshake at once (message 3's RSN element differing from the beacon's: WPA_E_RSN).

A key the driver already has is never installed again: a retransmitted message 3 (our
message 4 was lost) or group message 1 is answered and nothing more, against the key
reinstallation attacks of 2017. Message 4 goes out before the keys go in (the pairwise
key, then the group key), then done(WPA_OK).

done() is the last thing any call does: the station layer may wpa_free() the supplicant
inside it. The other callbacks must not call back into the supplicant. No secret is ever
logged; wpa_free() wipes the PMK, the PTKs, the GTK and the nonces. tests/host/test_wpa.c
runs it against a simulated access point and a captured handshake.
*/

#include <surfos/types.h>
#include <surfos/console.h>
#include <mm/kalloc.h>
#include <net/wpa.h>
#include <crypto/crypto.h>
#include <blibc_common.h>

/* EAPOL (IEEE 802.1X-2004 11.3): version, packet type, body length; an EAPOL-Key body
   (802.11-2016 12.7.2, Figure 12-32) follows, the offsets below counting from the version */
#define EAPOL_VERSION   1           /* what we send (802.1X-2004's 2 upsets some APs); any is taken */
#define EAPOL_TYPE_KEY  3
#define EAPOL_HDR       4

#define EK_DESC         4           /* descriptor type */
#define EK_INFO         5           /* key information */
#define EK_KEYLEN       7           /* key length */
#define EK_REPLAY       9           /* key replay counter */
#define EK_NONCE        17          /* key nonce */
#define EK_RSC          65          /* key RSC (after the 16-byte IV) */
#define EK_MIC          81          /* key MIC (after 8 reserved bytes) */
#define EK_DATALEN      97          /* key data length */
#define EK_DATA         99          /* key data */

#define DESC_RSN        2
#define KI_VERSION      0x0007
#define KI_VERSION_2    0x0002      /* HMAC-SHA1-128 MIC, AES key wrap */
#define KI_PAIRWISE     0x0008
#define KI_INSTALL      0x0040
#define KI_ACK          0x0080
#define KI_MIC          0x0100
#define KI_SECURE       0x0200
#define KI_ERROR        0x0400
#define KI_REQUEST      0x0800
#define KI_ENCRYPTED    0x1000
#define KI_SMK          0x2000

#define NONCE_LEN       32
#define REPLAY_LEN      8
#define MIC_LEN         16
#define KCK_LEN         16
#define TK_LEN          16          /* CCMP-128 */
#define GTK_LEN         16
#define RSN_IE_MAX      257         /* an element: id, length and up to 255 bytes */
#define KEY_DATA_MAX    1024        /* the longest Key Data field taken (wrapped) */
#define FRAME_MAX       (EK_DATA + KEY_DATA_MAX)

#define TIMEOUT_TICKS   50          /* wpa_tick() comes every 100 ms */
#define M1_REPEATS_MAX  3
#define MIC_FAILURES_MAX 3
#define BAD_FRAMES_MAX  5
#define NOTES_MAX       32          /* log lines per association */

#define ELEM_RSN        48
#define ELEM_VENDOR     0xdd
#define KDE_GTK         1           /* 00-0f-ac:1 */

enum { WPA_WAIT_M1, WPA_WAIT_M3, WPA_DONE, WPA_FAILED };

struct wpa_ptk { u8 kck[KCK_LEN], kek[16], tk[TK_LEN]; };

struct wpa_sm {
    const struct wpa_ops *ops;
    void *ctx;
    int state;
    bool reported;                  /* done() was called */
    u_int ticks;                    /* since the wait for message 1 or 3 began */
    u_int notes;                    /* log lines so far */
    u8 pmk[WPA_PMK_LEN];
    u8 aa[WLAN_ADDR_LEN], spa[WLAN_ADDR_LEN];
    u8 our_rsn[RSN_IE_MAX], ap_rsn[RSN_IE_MAX];
    u_int our_rsn_len, ap_rsn_len;
    u8 anonce[NONCE_LEN], snonce[NONCE_LEN];
    bool new_snonce;                /* the next message 1 begins a handshake: draw a new SNonce */
    struct wpa_ptk ptk, tptk;       /* confirmed by a message 3; from the last message 1 answered */
    bool ptk_set, tptk_set;
    u8 replay[REPLAY_LEN];          /* the highest counter of a frame whose MIC checked */
    bool replay_set;
    u8 m1_replay[REPLAY_LEN];       /* the last message 1's, in this handshake */
    bool m1_seen;
    u_int m1_repeats, mic_failures, bad_frames;
    u8 tk_in[TK_LEN];               /* what the driver has */
    bool tk_installed;
    u8 gtk_in[GTK_LEN], gtk_index;
    bool gtk_installed;
    u8 kd[KEY_DATA_MAX];            /* key data, unwrapped */
    u8 out[EK_DATA + RSN_IE_MAX];   /* the frame we send */
};

/* an EAPOL-Key frame as received: f and len cover the EAPOL frame as its MIC does */
struct ekey {
    const u8 *f;
    u_int len;
    u16 info, key_len;
    const u8 *replay, *nonce, *rsc, *mic, *data;
    u_int data_len;
};

/* what unwrapped key data holds (12.7.2, Table 12-6) */
struct kd_info {
    const u8 *rsn, *rsn2, *gtk;
    u_int rsn_len, rsn2_len, gtk_len;
    u8 gtk_id;
};

static u16 get_be16(const u8 *p) { return (u16)((p[0] << 8) | p[1]); }
static u16 get_le16(const u8 *p) { return (u16)(p[0] | (p[1] << 8)); }
static void put_be16(u8 *p, u16 v) { p[0] = (u8)(v >> 8); p[1] = (u8)v; }

/* 8-byte big-endian counters: below 0, equal 0, above 0 */
static int replay_cmp(const u8 *a, const u8 *b) {
    int i;
    for(i = 0; i < REPLAY_LEN; i++)
        if(a[i] != b[i]) return a[i] < b[i] ? -1 : 1;
    return 0;
}

/* a line in the kernel log, never with a secret in it; at most NOTES_MAX per association */
static void note(struct wpa_sm *sm, const char *what, const char *more) {
    if(sm->notes++ < NOTES_MAX) kprintf("+WPA: %s%s\n", what, more);
}

/* the end of a handshake that failed; done() last */
static void fail(struct wpa_sm *sm, int status, const char *why) {
    sm->state = WPA_FAILED;
    kprintf("+WPA: %s\n", why);
    if(!sm->reported) {
        sm->reported = true;
        sm->ops->done(sm->ctx, status);
    }
}

/* a malformed or unexpected frame, dropped; after the handshake they are not counted */
static void bad(struct wpa_sm *sm, const char *why) {
    note(sm, "dropped ", why);
    if(!sm->reported && ++sm->bad_frames >= BAD_FRAMES_MAX)
        fail(sm, WPA_E_PROTOCOL, "too many bad EAPOL-Key frames");
}

static void mic_failed(struct wpa_sm *sm, const char *which) {
    note(sm, which, "");
    if(!sm->reported && ++sm->mic_failures >= MIC_FAILURES_MAX)
        fail(sm, WPA_E_MIC, "MICs keep failing: a wrong password?");
}

/**** frames ****/

/* 0: an EAPOL-Key frame to look at; 1: another EAPOL packet, not ours; -1: malformed (why says
   how). Only len is trusted: the body length and key data length must fit in it. */
static int parse(const u8 *f, u_int len, struct ekey *k, const char **why) {
    u_int body;
    if(len < EAPOL_HDR) { *why = "an EAPOL frame shorter than its header"; return -1; }
    if(f[1] != EAPOL_TYPE_KEY) return 1;
    body = get_be16(f + 2);
    if(body > len - EAPOL_HDR) { *why = "a truncated EAPOL-Key frame"; return -1; }
    len = EAPOL_HDR + body;                         /* what follows the body is padding */
    if(len < EK_DATA) { *why = "an EAPOL-Key frame too short for its fields"; return -1; }
    if(len > FRAME_MAX) { *why = "an EAPOL-Key frame too long"; return -1; }
    if(f[EK_DESC] != DESC_RSN) { *why = "an EAPOL-Key frame that is not an RSN key descriptor"; return -1; }
    k->f = f;
    k->len = len;
    k->info = get_be16(f + EK_INFO);
    k->key_len = get_be16(f + EK_KEYLEN);
    k->replay = f + EK_REPLAY;
    k->nonce = f + EK_NONCE;
    k->rsc = f + EK_RSC;
    k->mic = f + EK_MIC;
    k->data = f + EK_DATA;
    k->data_len = get_be16(f + EK_DATALEN);
    if(k->data_len > len - EK_DATA) { *why = "an EAPOL-Key frame whose key data runs past its end"; return -1; }
    if((k->info & KI_VERSION) != KI_VERSION_2) { *why = "an EAPOL-Key frame not of descriptor version 2"; return -1; }
    if(k->info & (KI_REQUEST | KI_ERROR | KI_SMK)) { *why = "an EAPOL-Key frame with a supplicant's bits"; return -1; }
    if(!(k->info & KI_ACK)) { *why = "an EAPOL-Key frame without the Ack bit"; return -1; }
    return 0;
}

/* HMAC-SHA1-128 over the frame with its MIC field taken as zeros */
static void mic_of(const u8 *kck, const u8 *f, u_int len, u8 *mic) {
    static const u8 zeros[MIC_LEN];
    struct hmac_sha1_ctx c;
    u8 h[SHA1_LEN];
    hmac_sha1_init(&c, kck, KCK_LEN);
    hmac_sha1_update(&c, f, EK_MIC);
    hmac_sha1_update(&c, zeros, MIC_LEN);
    hmac_sha1_update(&c, f + EK_MIC + MIC_LEN, len - EK_MIC - MIC_LEN);
    hmac_sha1_final(&c, h);
    memcpy(mic, h, MIC_LEN);
    crypto_wipe(h, sizeof(h));
}

static bool mic_ok(const u8 *kck, const struct ekey *k) {
    u8 m[MIC_LEN];
    bool ok;
    mic_of(kck, k->f, k->len, m);
    ok = crypto_equal(m, k->mic, MIC_LEN);
    crypto_wipe(m, sizeof(m));
    return ok;
}

/* one of our frames: no key length (RSN), the replay counter we answer, a nonce or zeros, key data
   or none, MIC'd with kck. Sent last: the station layer only queues it. */
static void send_key(struct wpa_sm *sm, u16 info, const u8 *replay, const u8 *nonce, const u8 *data,
                     u_int data_len, const u8 *kck) {
    u8 *f = sm->out;
    u_int len = EK_DATA + data_len;
    memset(f, 0, EK_DATA);
    f[0] = EAPOL_VERSION;
    f[1] = EAPOL_TYPE_KEY;
    put_be16(f + 2, (u16)(len - EAPOL_HDR));
    f[EK_DESC] = DESC_RSN;
    put_be16(f + EK_INFO, info);
    memcpy(f + EK_REPLAY, replay, REPLAY_LEN);
    if(nonce) memcpy(f + EK_NONCE, nonce, NONCE_LEN);
    put_be16(f + EK_DATALEN, (u16)data_len);
    if(data_len) memcpy(f + EK_DATA, data, data_len);
    mic_of(kck, f, len, f + EK_MIC);
    sm->ops->send_eapol(sm->ctx, f, len);
}

/* the elements and KDEs of unwrapped key data; 0xdd followed by nothing or zeros is the padding
   and ends it (12.7.2) */
static int parse_key_data(const u8 *p, u_int len, struct kd_info *ki) {
    const u8 *end = p + len;
    u_int n;
    memset(ki, 0, sizeof(*ki));
    while(p < end) {
        if(p[0] == ELEM_VENDOR && (end - p == 1 || p[1] == 0)) break;
        if(end - p < 2) return -1;
        n = p[1];
        if(n > (u_int)(end - p - 2)) return -1;
        if(p[0] == ELEM_RSN) {
            if(!ki->rsn) { ki->rsn = p; ki->rsn_len = n + 2; }
            else if(!ki->rsn2) { ki->rsn2 = p; ki->rsn2_len = n + 2; }
        } else if(p[0] == ELEM_VENDOR && n >= 4 && p[2] == 0x00 && p[3] == 0x0f && p[4] == 0xac && p[5] == KDE_GTK) {
            if(n < 6) return -1;                    /* the key id byte and a reserved byte come first */
            if(!ki->gtk) {
                ki->gtk_id = p[6] & 3;
                ki->gtk = p + 8;
                ki->gtk_len = n - 6;
            }
        }
        p += 2 + n;
    }
    return 0;
}

/* a second RSN element in message 3 names the pairwise cipher (12.7.6.4): only CCMP will do */
static bool rsn2_ccmp(const u8 *e, u_int len) {
    static const u8 ccmp[4] = { 0x00, 0x0f, 0xac, 0x04 };
    if(len < 14 || get_le16(e + 8) != 1) return false;      /* id, length, version, group, count */
    return memcmp(e + 10, ccmp, 4) == 0;
}

/* the Key Data field unwrapped with kek into sm->kd; -1 when it is no RFC 3394 wrapping or fails */
static int unwrap_key_data(struct wpa_sm *sm, const u8 *kek, const struct ekey *k, u_int *len) {
    if(k->data_len < 24 || k->data_len % 8 || k->data_len > KEY_DATA_MAX) return -1;
    if(aes_unwrap(kek, k->data, k->data_len, sm->kd)) return -1;
    *len = k->data_len - 8;
    return 0;
}

/* the PTK: PRF-384(PMK, "Pairwise key expansion", Min(AA, SPA) || Max(AA, SPA) ||
   Min(ANonce, SNonce) || Max(ANonce, SNonce)) = KCK || KEK || TK (12.7.1.3) */
static void derive_ptk(struct wpa_sm *sm, struct wpa_ptk *ptk) {
    u8 data[2 * WLAN_ADDR_LEN + 2 * NONCE_LEN], out[KCK_LEN + 16 + TK_LEN];
    bool aa_first = memcmp(sm->aa, sm->spa, WLAN_ADDR_LEN) < 0;
    bool an_first = memcmp(sm->anonce, sm->snonce, NONCE_LEN) < 0;
    memcpy(data, aa_first ? sm->aa : sm->spa, WLAN_ADDR_LEN);
    memcpy(data + WLAN_ADDR_LEN, aa_first ? sm->spa : sm->aa, WLAN_ADDR_LEN);
    memcpy(data + 2 * WLAN_ADDR_LEN, an_first ? sm->anonce : sm->snonce, NONCE_LEN);
    memcpy(data + 2 * WLAN_ADDR_LEN + NONCE_LEN, an_first ? sm->snonce : sm->anonce, NONCE_LEN);
    ieee80211_prf_sha1(sm->pmk, WPA_PMK_LEN, "Pairwise key expansion", data, sizeof(data), out, sizeof(out));
    memcpy(ptk->kck, out, KCK_LEN);
    memcpy(ptk->kek, out + KCK_LEN, 16);
    memcpy(ptk->tk, out + KCK_LEN + 16, TK_LEN);
    crypto_wipe(out, sizeof(out));
}

/**** keys: 1 when installed, 0 when the driver already has it, -1 when it refused it ****/

static int install_ptk(struct wpa_sm *sm) {
    struct wlan_key key;
    int r;
    if(sm->tk_installed && crypto_equal(sm->tk_in, sm->ptk.tk, TK_LEN)) return 0;
    memset(&key, 0, sizeof(key));
    key.cipher = WLAN_CIPHER_CCMP;
    key.pairwise = true;
    key.index = 0;
    memcpy(key.key, sm->ptk.tk, TK_LEN);
    key.len = TK_LEN;
    r = sm->ops->install_key(sm->ctx, &key);
    crypto_wipe(&key, sizeof(key));
    if(r) return -1;
    memcpy(sm->tk_in, sm->ptk.tk, TK_LEN);
    sm->tk_installed = true;
    return 1;
}

static int install_gtk(struct wpa_sm *sm, const struct kd_info *ki, const u8 *rsc) {
    struct wlan_key key;
    int r;
    if(sm->gtk_installed && sm->gtk_index == ki->gtk_id && crypto_equal(sm->gtk_in, ki->gtk, GTK_LEN)) return 0;
    memset(&key, 0, sizeof(key));
    key.cipher = WLAN_CIPHER_CCMP;
    key.pairwise = false;
    key.index = ki->gtk_id;
    memcpy(key.key, ki->gtk, GTK_LEN);
    key.len = GTK_LEN;
    memcpy(key.rsc, rsc, sizeof(key.rsc));     /* the PN, low byte first, as the Key RSC field has it */
    r = sm->ops->install_key(sm->ctx, &key);
    crypto_wipe(&key, sizeof(key));
    if(r) return -1;
    memcpy(sm->gtk_in, ki->gtk, GTK_LEN);
    sm->gtk_index = ki->gtk_id;
    sm->gtk_installed = true;
    return 1;
}

/**** the handshakes ****/

/* message 1: EAPOL-Key(0, 0, 1, 0, P, 0, 0, ANonce, 0, -) */
static void rx_m1(struct wpa_sm *sm, const struct ekey *k) {
    int newer;
    if(k->info & (KI_INSTALL | KI_ENCRYPTED)) {
        bad(sm, "message 1 with Install or Encrypted Key Data");
        return;
    }
    newer = sm->m1_seen ? replay_cmp(k->replay, sm->m1_replay) : 1;
    if(newer < 0) return;                           /* older than one already answered: stale */
    if(newer > 0 && sm->m1_seen && sm->state == WPA_WAIT_M3 && !sm->reported &&
       ++sm->m1_repeats >= M1_REPEATS_MAX) {
        fail(sm, WPA_E_MIC, "the access point took none of our message 2s: a wrong password?");
        return;
    }
    if(sm->new_snonce) {
        crypto_random(sm->snonce, NONCE_LEN);
        sm->new_snonce = false;
    }
    memcpy(sm->anonce, k->nonce, NONCE_LEN);
    derive_ptk(sm, &sm->tptk);
    sm->tptk_set = true;
    memcpy(sm->m1_replay, k->replay, REPLAY_LEN);
    sm->m1_seen = true;
    sm->state = WPA_WAIT_M3;
    sm->ticks = 0;
    send_key(sm, KI_VERSION_2 | KI_PAIRWISE | KI_MIC, k->replay, sm->snonce, sm->our_rsn, sm->our_rsn_len,
             sm->tptk.kck);
}

/* message 3: EAPOL-Key(1, 1, 1, 1, P, 0, KeyRSC, ANonce, MIC, {RSNE, GTK[N]}) */
static void rx_m3(struct wpa_sm *sm, const struct ekey *k) {
    struct kd_info ki;
    u_int kd_len = 0;
    int rp, rg;
    bool first;
    if(!sm->tptk_set) { bad(sm, "message 3 before any message 1"); return; }
    if((k->info & (KI_INSTALL | KI_ENCRYPTED)) != (KI_INSTALL | KI_ENCRYPTED)) {
        bad(sm, "message 3 without Install or Encrypted Key Data");
        return;
    }
    if(memcmp(k->nonce, sm->anonce, NONCE_LEN)) { bad(sm, "message 3 for another message 1 (its ANonce)"); return; }
    if(!mic_ok(sm->tptk.kck, k)) { mic_failed(sm, "message 3's MIC did not check"); return; }
    memcpy(sm->replay, k->replay, REPLAY_LEN);
    sm->replay_set = true;

    /* the AP's own frame from here on */
    if(k->key_len != TK_LEN) { fail(sm, WPA_E_PROTOCOL, "message 3: not a CCMP key length"); return; }
    if(unwrap_key_data(sm, sm->tptk.kek, k, &kd_len)) {
        fail(sm, WPA_E_PROTOCOL, "message 3: its key data does not unwrap");
        return;
    }
    if(parse_key_data(sm->kd, kd_len, &ki)) {
        crypto_wipe(sm->kd, kd_len);
        fail(sm, WPA_E_PROTOCOL, "message 3: malformed key data");
        return;
    }
    if(!ki.rsn || ki.rsn_len != sm->ap_rsn_len || memcmp(ki.rsn, sm->ap_rsn, ki.rsn_len) ||
       (ki.rsn2 && !rsn2_ccmp(ki.rsn2, ki.rsn2_len))) {
        crypto_wipe(sm->kd, kd_len);
        fail(sm, WPA_E_RSN, "message 3: the access point's RSN element is not the one in its beacon");
        return;
    }
    if(!ki.gtk || ki.gtk_len != GTK_LEN || !ki.gtk_id) {
        crypto_wipe(sm->kd, kd_len);
        fail(sm, WPA_E_PROTOCOL, "message 3: no CCMP group key");
        return;
    }

    /* message 4: EAPOL-Key(1, 1, 0, 0, P, 0, 0, 0, MIC, -), then the keys */
    sm->ptk = sm->tptk;
    sm->ptk_set = true;
    first = !sm->reported;
    send_key(sm, KI_VERSION_2 | KI_PAIRWISE | KI_MIC | KI_SECURE, k->replay, NULL, NULL, 0, sm->ptk.kck);
    rp = install_ptk(sm);
    rg = rp < 0 ? 0 : install_gtk(sm, &ki, k->rsc);
    crypto_wipe(sm->kd, kd_len);
    if(rp < 0 || rg < 0) { fail(sm, WPA_E_KEY, "the driver refused a key"); return; }
    sm->state = WPA_DONE;
    sm->new_snonce = true;
    sm->m1_seen = false;
    sm->m1_repeats = sm->mic_failures = sm->bad_frames = 0;
    if(first) {
        sm->reported = true;
        note(sm, "4-way handshake done: pairwise and group keys in", "");
        sm->ops->done(sm->ctx, WPA_OK);
    } else if(rp > 0) note(sm, "pairwise key renewed", "");
}

/* group message 1: EAPOL-Key(1, 1, 1, 0, G, 0, Key RSC, 0, MIC, GTK[N]) */
static void rx_g1(struct wpa_sm *sm, const struct ekey *k) {
    struct kd_info ki;
    u_int kd_len = 0;
    int r;
    if(!sm->ptk_set) { bad(sm, "a group key message before the 4-way handshake"); return; }
    if(!(k->info & KI_ENCRYPTED)) { bad(sm, "group message 1 without Encrypted Key Data"); return; }
    if(!mic_ok(sm->ptk.kck, k)) { mic_failed(sm, "group message 1's MIC did not check"); return; }
    memcpy(sm->replay, k->replay, REPLAY_LEN);
    sm->replay_set = true;
    if(unwrap_key_data(sm, sm->ptk.kek, k, &kd_len) || parse_key_data(sm->kd, kd_len, &ki) || !ki.gtk ||
       ki.gtk_len != GTK_LEN || !ki.gtk_id) {
        crypto_wipe(sm->kd, sizeof(sm->kd));
        note(sm, "dropped group message 1: no CCMP group key in it", "");
        return;
    }
    /* group message 2: EAPOL-Key(1, 1, 0, 0, G, 0, 0, 0, MIC, -) */
    send_key(sm, KI_VERSION_2 | KI_MIC | KI_SECURE, k->replay, NULL, NULL, 0, sm->ptk.kck);
    r = install_gtk(sm, &ki, k->rsc);
    crypto_wipe(sm->kd, kd_len);
    if(r < 0) { fail(sm, WPA_E_KEY, "the driver refused the new group key"); return; }
    if(r > 0) note(sm, "group key renewed", "");
}

/**** the interface ****/

int wpa_passphrase_to_pmk(const char *passphrase, const u8 *ssid, u_int ssid_len, u8 pmk[WPA_PMK_LEN]) {
    u_int n;
    if(!passphrase || !pmk || ssid_len > WLAN_SSID_MAX || (ssid_len && !ssid)) return -1;
    for(n = 0; n < 64 && passphrase[n]; n++)
        if((u8)passphrase[n] < 32 || (u8)passphrase[n] > 126) return -1;
    if(n < 8 || n > 63) return -1;
    return pbkdf2_sha1((const u8 *)passphrase, n, ssid, ssid_len, 4096, pmk, WPA_PMK_LEN);
}

u_int wpa_rsn_ie(u8 *buf, u_int size) {
    static const u8 ie[] = {
        ELEM_RSN, 20,
        1, 0,                       /* version 1 */
        0x00, 0x0f, 0xac, 4,        /* group data cipher: CCMP */
        1, 0, 0x00, 0x0f, 0xac, 4,  /* one pairwise cipher: CCMP */
        1, 0, 0x00, 0x0f, 0xac, 2,  /* one AKM: PSK */
        0, 0                        /* RSN capabilities: none (no management frame protection) */
    };
    if(!buf || size < sizeof(ie)) return 0;
    memcpy(buf, ie, sizeof(ie));
    return sizeof(ie);
}

/* NULL too when something it needs is missing or an element is longer than an element can be */
struct wpa_sm *wpa_start(const u8 pmk[WPA_PMK_LEN], const u8 aa[WLAN_ADDR_LEN], const u8 spa[WLAN_ADDR_LEN],
                         const u8 *our_rsn, u_int our_rsn_len, const u8 *ap_rsn, u_int ap_rsn_len,
                         const struct wpa_ops *ops, void *ctx) {
    struct wpa_sm *sm;
    if(!pmk || !aa || !spa || !ops || !ops->send_eapol || !ops->install_key || !ops->done) return NULL;
    if(!our_rsn || our_rsn_len < 2 || our_rsn_len > RSN_IE_MAX) return NULL;
    if((ap_rsn_len && !ap_rsn) || ap_rsn_len > RSN_IE_MAX) return NULL;
    sm = kcalloc(1, sizeof(*sm));
    if(!sm) return NULL;
    sm->ops = ops;
    sm->ctx = ctx;
    memcpy(sm->pmk, pmk, WPA_PMK_LEN);
    memcpy(sm->aa, aa, WLAN_ADDR_LEN);
    memcpy(sm->spa, spa, WLAN_ADDR_LEN);
    memcpy(sm->our_rsn, our_rsn, our_rsn_len);
    sm->our_rsn_len = our_rsn_len;
    if(ap_rsn_len) memcpy(sm->ap_rsn, ap_rsn, ap_rsn_len);
    sm->ap_rsn_len = ap_rsn_len;
    sm->state = WPA_WAIT_M1;
    sm->new_snonce = true;
    return sm;
}

void wpa_rx_eapol(struct wpa_sm *sm, const u8 *frame, u_int len) {
    struct ekey k;
    const char *why = "";
    int r;
    if(!sm || !frame || sm->state == WPA_FAILED) return;
    r = parse(frame, len, &k, &why);
    if(r > 0) return;                               /* EAP, EAPOL-Start, ...: not for a PSK supplicant */
    if(r < 0) { bad(sm, why); return; }
    if(sm->replay_set && replay_cmp(k.replay, sm->replay) <= 0) return;    /* a replay */
    if(k.info & KI_PAIRWISE) {
        if(k.info & KI_MIC) rx_m3(sm, &k);
        else rx_m1(sm, &k);
    } else if(k.info & KI_MIC) rx_g1(sm, &k);
    else bad(sm, "a group key frame without a MIC");
}

void wpa_tick(struct wpa_sm *sm) {
    if(!sm || sm->reported || sm->state == WPA_FAILED) return;
    if(++sm->ticks < TIMEOUT_TICKS) return;
    fail(sm, WPA_E_TIMEOUT, sm->state == WPA_WAIT_M1 ? "no message 1 from the access point in 5 s"
                                                     : "no message 3 from the access point in 5 s");
}

void wpa_free(struct wpa_sm *sm) {
    if(!sm) return;
    crypto_wipe(sm, sizeof(*sm));
    kfree(sm);
}
