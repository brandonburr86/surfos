/*
SurfOS Wi-Fi: the wifi command
--------------------
File: wifi.c    Date: 10/9/26 (Wi-Fi, track S)
--------------------
    wifi up | down | scan | join <ssid> [bssid] | status | leave

The station layer's commands (include/net/wlan.h, net/wlan/wlan.c) on the first Wi-Fi
device, wlan0. `scan` waits for the end of a scan and lists what is in range, strongest
first: SSID (a hidden network as such), BSSID, channel, band, signal and the security in
words. `join` picks the network from that list (scanning first when it is not there, with
its SSID, which finds a hidden one too), refuses a security SurfOS does not speak and says
why, and for WPA2 asks "Password for <ssid>:" with shell_ask_secret(): nothing typed is
shown, kept or logged, over telnet too. The password becomes the PMK at once
(wpa_passphrase_to_pmk(), PBKDF2 with the SSID) and is wiped; the PMK goes to the station,
which keeps it only while it may have to join again, and is wiped here as soon as
wlan_join() returns. The SSID is the words after `join` (a BSSID last, if given), so a
name with spaces works in the ring-0 shell too. `status` gives the station's state, the
last failure, the driver's line and wlan0's address.

A kernel command (CLAUDE.md): it prints on its own console, keeps nothing in statics,
asks through shell_ask_secret(), and returns when it is cut short (the station layer
cancels what it asked for; Ctrl+C at the password prompt joins nothing). 0, 1 or 2 as the
other commands.
*/

#include <surfos/types.h>
#include <surfos/console.h>
#include <surfos/task.h>
#include <surfos/tty.h>
#include <mm/kalloc.h>
#include <fs/vfs.h>
#include <net/net.h>
#include <net/wlan.h>
#include <net/wlan_cmd.h>
#include <net/wpa.h>
#include <net/ieee80211.h>
#include <blibc_common.h>
#include "shell.h"

#pragma weak wpa_passphrase_to_pmk          /* track C's (net/wlan/wpa.c): absent until it lands */

#define LIST_MAX   48
#define SCAN_MS    10000
#define PASS_MAX   72                       /* room for 63 characters and a few too many, to say so */

/* not optimised away */
static void wipe(void *p, size_t n) {
    volatile u8 *q = (volatile u8 *)p;
    while(n--) *q++ = 0;
}

/* an SSID as text: printable ASCII as it is, UTF-8 where the terminal draws it. 10/10/26 (review nit):
   any other byte as \xHH, a C1 control in UTF-8 too, which a terminal would act on (ieee80211_ssid_text()) */
static const char *ssid_str(const u8 *ssid, u_int len, char *out, size_t size) {
    int caps = tty_ctl(curTask->tty, TTY_CAPS, 0);
    if(!len) return "(hidden network)";
    return ieee80211_ssid_text(ssid, len, caps > 0 && (caps & TTY_CAP_UTF8), out, size);
}

static bool parse_mac(const char *s, u8 *mac) {
    int i;
    for(i = 0; i < 6; i++) {
        char *end;
        u_long v;
        if(!((s[0] >= '0' && s[0] <= '9') || (s[0] >= 'a' && s[0] <= 'f') || (s[0] >= 'A' && s[0] <= 'F'))) return false;
        v = strtoul(s, &end, 16);
        if(end - s != 2 || v > 255) return false;
        if((i < 5 && *end != ':') || (i == 5 && *end)) return false;
        mac[i] = (u8)v;
        s = end + 1;
    }
    return true;
}

static struct wlan_dev *device(void) {
    struct wlan_dev *w = wlan_find(NULL);
    if(!w) printf("    wifi: no Wi-Fi device (no driver found a card; `wlansim` on the kernel command line simulates one)\n");
    return w;
}

static int fail(const char *what, int r) {
    if(r == -EBUSY) printf("    wifi: busy: another wifi command is running on this device\n");
    else if(r == -EINTR) printf("    wifi: %s cut short\n", what);
    else if(r == -ENODEV) printf("    wifi: the device is down (wifi up first)\n");
    else printf("    wifi: %s failed: %s\n", what, strerror(r));
    return CMD_FAIL;
}

/* the station's description, a line each, indented, the first after the device's name */
static void describe(struct wlan_dev *w) {
    char *buf = (char *)kalloc(768), *line, *nl;
    if(!buf) return;
    wlan_describe(w, buf, 768);
    for(line = buf; line && *line; line = nl) {
        nl = strchr(line, '\n');
        if(nl) *nl++ = 0;
        if(line == buf) printf("    %s: %s\n", w->name, line);
        else printf("    %s\n", line);
    }
    kfree(buf);
}

static const char *band_str(u8 band) {
    return band == WLAN_BAND_5GHZ ? "5 GHz" : "2.4 GHz";
}

/**** scan ****/

static int wifi_scan(struct wlan_dev *w) {
    struct wlan_bss *list;
    char s[4 * WLAN_SSID_MAX + 1], m[18];
    u_int n, i;
    int r = wlan_scan(w, SCAN_MS);
    if(r && r != -ETIMEDOUT) return fail("scan", r);
    list = (struct wlan_bss *)kalloc(LIST_MAX * sizeof(*list));
    if(!list) {
        printf("    wifi: no memory for the list\n");
        return CMD_FAIL;
    }
    n = wlan_bss_list(w, list, LIST_MAX);
    if(r == -ETIMEDOUT) printf("    (the scan did not end within %u s: what was heard until then)\n", SCAN_MS / 1000);
    if(!n) {
        printf("    no networks in range\n");
        kfree(list);
        return CMD_OK;
    }
    printf("    %-32s  %-17s  %3s  %-7s  %-8s  %s\n", "SSID", "BSSID", "CH", "BAND", "SIGNAL", "SECURITY");
    for(i = 0; i < n && !task_interrupted(); i++) {
        printf("    %-32s  %-17s  %3u  %-7s  %4d dBm  %s\n", ssid_str(list[i].ssid, list[i].ssid_len, s, sizeof(s)),
               macfmt(list[i].bssid, m), list[i].channel, band_str(list[i].band), list[i].rssi,
               ieee80211_security_name(list[i].security));
    }
    printf("    %u network%s\n", n, n == 1 ? "" : "s");
    kfree(list);
    return CMD_OK;
}

/**** join ****/

/* what the networks named ssid (and bssid, when given) in the list ask: WPA2 if any of them is
   WPA2 (an open network with the same name is no reason to send the password in the clear, nor to
   go without it), else open if one is open, else the strongest one's; -1 when none is there */
static int security_of(struct wlan_bss *list, u_int n, const u8 *ssid, u_int len, const u8 *bssid) {
    int best = -1, strongest = -1;
    u_int i;
    for(i = 0; i < n; i++) {                            /* strongest first already */
        struct wlan_bss *b = &list[i];
        if(bssid && memcmp(b->bssid, bssid, WLAN_ADDR_LEN)) continue;
        if(b->ssid_len != len || memcmp(b->ssid, ssid, len)) continue;
        if(strongest < 0) strongest = b->security;
        if(b->security == WLAN_SEC_WPA2_PSK || b->security == WLAN_SEC_WPA2_WPA3) return b->security;
        if(b->security == WLAN_SEC_OPEN && best < 0) best = WLAN_SEC_OPEN;
    }
    return best >= 0 ? best : strongest;
}

static int lookup(struct wlan_dev *w, struct wlan_bss *list, const u8 *ssid, u_int len, const u8 *bssid) {
    return security_of(list, wlan_bss_list(w, list, LIST_MAX), ssid, len, bssid);
}

static int refuse(const char *name, int security) {
    switch(security) {
    case WLAN_SEC_WEP:
        printf("    wifi: %s uses WEP, which SurfOS does not support (it is broken: the network should use WPA2)\n", name);
        break;
    case WLAN_SEC_WPA3_SAE:
        printf("    wifi: %s is WPA3-Personal only (SAE): not supported yet\n", name);
        break;
    case WLAN_SEC_ENTERPRISE:
        printf("    wifi: %s is an enterprise network (802.1X with a login): not supported\n", name);
        break;
    default:
        printf("    wifi: %s uses a security SurfOS does not support (WPA, TKIP, OWE or protected management frames)\n", name);
        break;
    }
    return CMD_FAIL;
}

/* the outcome of a join; ap: the BSSID asked for, as text, or NULL */
static int joined(struct wlan_dev *w, const char *name, const char *ap, int r) {
    if(!r) {
        describe(w);
        printf("    joined %s: dhcp %s gets an address\n", name, w->name);
        return CMD_OK;
    }
    if(r == -EACCES) printf("    wifi: could not join %s: the 4-way handshake failed: the password may be wrong\n", name);
    else if(r == -ENOENT && ap) printf("    wifi: no access point %s named %s in range\n", ap, name);
    else if(r == -ENOENT) printf("    wifi: no network named %s in range\n", name);
    else if(r == -ENOSYS) printf("    wifi: WPA2 needs the supplicant (net/wlan/wpa.c), which this kernel does not have yet\n");
    else if(r == -EINTR || r == -EBUSY || r == -ENODEV) return fail("join", r);
    else printf("    wifi: could not join %s: %s\n", name, r == -ETIMEDOUT ? "no answer" : r == -ECONNREFUSED ? "refused" :
                r == -ECONNRESET ? "the access point sent us away" : strerror(r));
    describe(w);
    return CMD_FAIL;
}

static int wifi_join(struct wlan_dev *w, int argc, char **argv) {
    u8 ssid[WLAN_SSID_MAX], bssid[WLAN_ADDR_LEN], pmk[WPA_PMK_LEN];
    char name[4 * WLAN_SSID_MAX + 1], pass[PASS_MAX], question[4 * WLAN_SSID_MAX + 32], apbuf[18];
    const char *ap = NULL;
    struct wlan_bss *list;
    bool has_bssid = false;
    u_int len = 0;
    int i, last = argc - 1, security, r;

    if(argc < 3) {
        printf("    usage: wifi join <ssid> [bssid]   (the SSID may have spaces; a BSSID is aa:bb:cc:dd:ee:ff)\n");
        return CMD_USAGE;
    }
    if(argc > 3 && parse_mac(argv[last], bssid)) {
        has_bssid = true;
        ap = macfmt(bssid, apbuf);
        last--;
    }
    for(i = 2; i <= last; i++) {                        /* the words of the SSID, a space between */
        u_int n = strlen(argv[i]);
        if(len + (i > 2) + n > WLAN_SSID_MAX) {
            printf("    wifi: an SSID is 32 bytes at most\n");
            return CMD_USAGE;
        }
        if(i > 2) ssid[len++] = ' ';
        memcpy(ssid + len, argv[i], n);
        len += n;
    }
    ssid_str(ssid, len, name, sizeof(name));
    if(!w->up) {
        printf("    wifi: %s is down (wifi up first)\n", w->name);
        return CMD_FAIL;
    }

    list = (struct wlan_bss *)kalloc(LIST_MAX * sizeof(*list));
    if(!list) {
        printf("    wifi: no memory for the list\n");
        return CMD_FAIL;
    }
    security = lookup(w, list, ssid, len, has_bssid ? bssid : NULL);
    if(security < 0) {                                  /* not heard yet: scan */
        r = wlan_scan(w, SCAN_MS);
        if(r && r != -ETIMEDOUT) {
            kfree(list);
            return fail("scan", r);
        }
        security = lookup(w, list, ssid, len, has_bssid ? bssid : NULL);
    }
    if(security < 0) {
        /* perhaps hidden: the station scans for the SSID itself; an open network is joined so, and a
           WPA2 one it found is in the list now */
        r = wlan_join(w, ssid, len, has_bssid ? bssid : NULL, NULL);
        if(r != -ENOENT) {
            kfree(list);
            return joined(w, name, ap, r);
        }
        security = lookup(w, list, ssid, len, has_bssid ? bssid : NULL);
        if(security < 0 || security == WLAN_SEC_OPEN) {
            kfree(list);
            return joined(w, name, ap, -ENOENT);
        }
    }
    kfree(list);

    if(security == WLAN_SEC_OPEN) {
        printf("    joining %s (open)\n", name);
        return joined(w, name, ap, wlan_join(w, ssid, len, has_bssid ? bssid : NULL, NULL));
    }
    if(security != WLAN_SEC_WPA2_PSK && security != WLAN_SEC_WPA2_WPA3) return refuse(name, security);
    if(!wpa_passphrase_to_pmk) return joined(w, name, ap, -ENOSYS);

    snprintf(question, sizeof(question), "    Password for %s: ", name);
    shell_ask_secret(question, pass, sizeof(pass));
    if(task_interrupted()) {                            /* 10/9/26: Ctrl+C at the prompt: nothing is joined */
        wipe(pass, sizeof(pass));
        return fail("join", -EINTR);
    }
    if(!pass[0]) {
        wipe(pass, sizeof(pass));
        printf("    wifi: no password: not joined\n");
        return CMD_FAIL;
    }
    r = wpa_passphrase_to_pmk(pass, ssid, len, pmk);
    wipe(pass, sizeof(pass));
    if(r) {
        wipe(pmk, sizeof(pmk));
        printf("    wifi: a WPA2 password is 8 to 63 characters (letters, digits, spaces and punctuation): not joined\n");
        return CMD_FAIL;
    }
    printf("    joining %s (%s)\n", name, ieee80211_security_name((u8)security));
    r = wlan_join(w, ssid, len, has_bssid ? bssid : NULL, pmk);
    wipe(pmk, sizeof(pmk));
    return joined(w, name, ap, r);
}

/**** the command ****/

int cmd_wifi(int argc, char **argv) {
    struct wlan_dev *w;
    struct netdev *nd;
    char a[16], b[16], c[16], m[18];
    int r;
    if(argc < 2) {
        printf("    usage: wifi up | down | scan | join <ssid> [bssid] | status | leave\n");
        return CMD_USAGE;
    }
    if(strcmp(argv[1], "up") && strcmp(argv[1], "down") && strcmp(argv[1], "scan") && strcmp(argv[1], "join") &&
       strcmp(argv[1], "status") && strcmp(argv[1], "leave")) {
        printf("    usage: wifi up | down | scan | join <ssid> [bssid] | status | leave\n");
        return CMD_USAGE;
    }
    if(strcmp(argv[1], "join") && argc > 2) {
        printf("    usage: wifi %s   (no arguments)\n", argv[1]);
        return CMD_USAGE;
    }
    w = device();
    if(!w) return CMD_FAIL;
    nd = w->netdev;

    if(!strcmp(argv[1], "up")) {
        r = wlan_up(w);
        if(r) return fail("up", r);
        describe(w);
        return CMD_OK;
    }
    if(!strcmp(argv[1], "down")) {
        r = wlan_down_wait(w);
        if(r) return fail("down", r);
        printf("    %s: down\n", w->name);
        return CMD_OK;
    }
    if(!strcmp(argv[1], "scan")) {
        if(!w->up) {
            printf("    wifi: %s is down (wifi up first)\n", w->name);
            return CMD_FAIL;
        }
        return wifi_scan(w);
    }
    if(!strcmp(argv[1], "join")) return wifi_join(w, argc, argv);
    if(!strcmp(argv[1], "leave")) {
        r = wlan_leave_wait(w);
        if(r) return fail("leave", r);
        printf("    %s: not associated\n", w->name);
        return CMD_OK;
    }
    /* status */
    describe(w);
    printf("    %s: MAC %s, link %s, inet %s netmask %s gateway %s\n", nd->name, macfmt(nd->mac, m), nd->link_up ? "up" : "down",
           ipfmt(nd->ip, a), ipfmt(nd->netmask, b), ipfmt(nd->gateway, c));
    return CMD_OK;
}
