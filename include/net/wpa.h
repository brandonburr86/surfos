/*
SurfOS Wi-Fi: the WPA2-Personal supplicant
----------------------
File: wpa.h     Date: 10/9/26
----------------------
What the station layer (net/wlan/wlan.c) uses of net/wlan/wpa.c: the passphrase turned
into the PMK, our RSN element for the association request, and one supplicant per
association running the 4-way handshake (IEEE 802.11-2016 12.7.6) and the group key
handshake (12.7.7) for RSN with AKM 2 (PSK) and CCMP. The brief is
docs/superpowers/plans/2026-10-09-wifi.md.

EAPOL frames cross this interface without the 802.11 and LLC/SNAP headers: from the
EAPOL header (version, packet type, body length) on. The PMK and the keys are wiped when
the supplicant is freed; nothing here is ever logged.
*/

#ifndef _SURFOS_WPA_H
#define _SURFOS_WPA_H

#include <surfos/types.h>
#include <net/wlan.h>

#define WPA_PMK_LEN 32

/* errors given to done() */
#define WPA_OK          0
#define WPA_E_TIMEOUT   1   /* no message 1 or 3 in time */
#define WPA_E_MIC       2   /* a MIC did not check: almost always a wrong password */
#define WPA_E_RSN       3   /* message 3's RSN element differs from the beacon's */
#define WPA_E_PROTOCOL  4   /* a malformed or unexpected EAPOL-Key frame, too many times */
#define WPA_E_KEY       5   /* the driver refused a key */

/* what the station layer gives a supplicant; ctx is passed back */
struct wpa_ops {
    int (*send_eapol)(void *ctx, const u8 *frame, u_int len);     /* to the AP, unprotected */
    int (*install_key)(void *ctx, const struct wlan_key *key);   /* 0 ok */
    void (*done)(void *ctx, int status);    /* WPA_OK once the pairwise and group keys are in (the
                                               port opens), else why it failed; called once */
};

struct wpa_sm;

/* PBKDF2-HMAC-SHA1(passphrase, SSID, 4096, 32); 0, or -1 when the passphrase is not 8 to 63
   printable ASCII characters */
int wpa_passphrase_to_pmk(const char *passphrase, const u8 *ssid, u_int ssid_len, u8 pmk[WPA_PMK_LEN]);

/* our RSN element for an association request: version 1, group CCMP, pairwise CCMP, AKM PSK,
   capabilities 0; its length, or 0 when size is too small */
u_int wpa_rsn_ie(u8 *buf, u_int size);

/* a supplicant for one association: aa the AP's address, spa ours; the two RSN elements as they
   were sent (ours) and seen in the beacon (the AP's); NULL when out of memory */
struct wpa_sm *wpa_start(const u8 pmk[WPA_PMK_LEN], const u8 aa[WLAN_ADDR_LEN], const u8 spa[WLAN_ADDR_LEN],
                         const u8 *our_rsn, u_int our_rsn_len, const u8 *ap_rsn, u_int ap_rsn_len,
                         const struct wpa_ops *ops, void *ctx);
void wpa_rx_eapol(struct wpa_sm *sm, const u8 *frame, u_int len);   /* from the AP */
void wpa_tick(struct wpa_sm *sm);           /* every 100 ms from the station layer: timeouts */
void wpa_free(struct wpa_sm *sm);           /* wipes the PMK, the PTK and the GTK */

#endif
