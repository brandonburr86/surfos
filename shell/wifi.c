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

10/17/26 (the network manager, kernel/netman.c): the saved networks, which SurfOS joins by
itself at startup (the setting wifi), in surfos-config/networks.conf on the stick it was
started from:

    wifi save            the network wlan0 is joined to, with the key it joined with (the
                         PMK the station keeps; the password is gone already), first in the list
    wifi save <ssid>     one not joined now: its security from the last scan (asked when it was
                         not heard), its password asked as join asks it and made into the key;
                         one the last scan did not hear by its name is saved as hidden, so
                         SurfOS probes for it by name
    wifi forget <ssid>   no longer joined by itself (wlan0 stays where it is)
    wifi saved           the list, in the order they are tried, and which are in range
    wifi connect         what the start does: the card up, the first saved network in range
                         joined, its address (DHCP)

Nothing here prints a key; wifi saved shows names and securities only. While a telnet server
runs or is to start by itself, SurfOS joins no open network by itself (kernel/netman.c), and
wifi save of an open network says so. 10/24/26: the same while an FTP server (ftpd) runs.
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
#include <net/netcfg.h>
#include <net/netman.h>
#include <sys/bdev.h>
#include <blibc_common.h>
#include "shell.h"

#pragma weak wpa_passphrase_to_pmk          /* track C's (net/wlan/wpa.c): absent until it lands */

#define LIST_MAX   48
#define SCAN_MS    10000
#define PASS_MAX   72                       /* room for 63 characters and a few too many, to say so */
#define USAGE      "usage: wifi up | down | scan | join <ssid> [bssid] | status | leave\n" \
                   "           wifi save [ssid] | forget <ssid> | saved | connect"

static bool is_saved(const u8 *ssid, u_int len);     /* 10/17/26: below, with the saved networks */

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
        struct wlan_station_info si;
        describe(w);
        printf("    joined %s: dhcp %s gets an address\n", name, w->name);
        if(wlan_station(w, &si, false) == 0) {          /* 10/17/26 */
            if(si.security == WLAN_SEC_OPEN && netman_telnet_exposed())
                printf("    (a telnet server runs or is to start, and it has no password: it answers on this open\n"
                       "    network too; telnetd off stops it)\n");
            if(si.security == WLAN_SEC_OPEN && netman_ftp_exposed())     /* 10/24/26 */
                printf("    (an FTP server runs, and it has no password: anyone on this open network may reach\n"
                       "    its files; ftpd off stops it)\n");
            if(netman_can_save() && !netman_settings_off() && !is_saved(si.ssid, si.ssid_len))
                printf("    (wifi save keeps it on the stick, for SurfOS to join by itself at startup)\n");
        }
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

/**** the saved networks (10/17/26) ****/

/* the words argv[from..to] as an SSID, a space between them: 0 and *len, or -1 (said) when longer than 32 bytes */
static int ssid_of_words(char **argv, int from, int to, u8 *ssid, u_int *len) {
    u_int n = 0;
    int i;
    for(i = from; i <= to; i++) {
        u_int l = strlen(argv[i]);
        if(n + (i > from) + l > WLAN_SSID_MAX) {
            printf("    wifi: an SSID is 32 bytes at most\n");
            return -1;
        }
        if(i > from) ssid[n++] = ' ';
        memcpy(ssid + n, argv[i], l);
        n += l;
    }
    *len = n;
    return 0;
}

static const char *sec_name(u8 security) {
    return security == NETCFG_SEC_WPA2 ? "WPA2" : "open";
}

static const char *stick_name(void) {
    struct bdev *v = bdev_boot_vol();
    return v ? v->name : "the stick";
}

/* is the network named so saved? */
static bool is_saved(const u8 *ssid, u_int len) {
    struct netman_net *list = (struct netman_net *)kalloc(NETCFG_NETS_MAX * sizeof(*list));
    int n, i;
    bool found = false;
    if(!list) return false;
    n = netman_networks(list, NETCFG_NETS_MAX, NULL);
    for(i = 0; i < n && !found; i++) found = list[i].ssid_len == len && !memcmp(list[i].ssid, ssid, len);
    kfree(list);
    return found;
}

/* net saved, and said so; checked: its password has been seen to work (or it has none); unheard: saved as hidden
   because the last scan did not hear it by its name */
static int saved_it(const struct netcfg_net *net, const char *name, bool checked, bool unheard) {
    struct netcfg_settings cfg;
    char why[200], what[48], order[48];
    int had = -1, n = 0, r = netman_save_network(net, &had, &n, why, sizeof(why));
    if(r) {
        printf("    wifi: %s not saved: %s\n", name, why);
        return CMD_FAIL;
    }
    netman_settings(&cfg);
    snprintf(what, sizeof(what), "%s%s", sec_name(net->security), net->hidden ? ", hidden" : "");
    if(n == 1) strlcpy(order, "its only saved network", sizeof(order));
    else snprintf(order, sizeof(order), "the first of %i saved networks it tries", n);
    if(netman_settings_off())
        printf("    saved %s (%s) on %s%s, %s: SurfOS joins it by itself from the next start (this one does not\n"
               "    read the settings: settings=off)\n", name, what, stick_name(), had >= 0 ? " again" : "", order);
    else if(!cfg.wifi)
        printf("    saved %s (%s) on %s%s, %s; SurfOS joins none by itself while settings wifi is off\n", name, what,
               stick_name(), had >= 0 ? " again" : "", order);
    else
        printf("    saved %s (%s) on %s%s: SurfOS joins it by itself at startup, %s\n", name, what, stick_name(),
               had >= 0 ? " again" : "", order);
    if(unheard)
        printf("    (the last scan did not hear %s by its name, so it is saved as a hidden network, which SurfOS\n"
               "    probes for by name; in range and not hidden? wifi scan, then wifi save %s again)\n", name, name);
    if(!checked) printf("    (its password is checked the first time SurfOS joins it)\n");
    if(net->security == NETCFG_SEC_OPEN && netman_telnet_exposed())
        printf("    (a telnet server runs or is to start by itself, and it has no password: meanwhile SurfOS joins\n"
               "    no open network by itself; wifi join %s joins it by hand)\n", name);
    if(net->security == NETCFG_SEC_OPEN && netman_ftp_exposed())         /* 10/24/26 */
        printf("    (an FTP server runs, and it has no password: meanwhile SurfOS joins no open network by itself;\n"
               "    wifi join %s joins it by hand)\n", name);
    return CMD_OK;
}

/* the network named so in the last scan: its security (-1: not heard) and whether it is hidden */
static int heard_as(struct wlan_dev *w, const u8 *ssid, u_int len, bool *hidden) {
    struct wlan_bss *list = (struct wlan_bss *)kalloc(LIST_MAX * sizeof(*list));
    u_int n, i;
    int security;
    *hidden = false;
    if(!list) return -1;
    n = wlan_bss_list(w, list, LIST_MAX);
    security = security_of(list, n, ssid, len, NULL);
    for(i = 0; i < n; i++)
        if(list[i].ssid_len == len && !memcmp(list[i].ssid, ssid, len) && list[i].hidden) *hidden = true;
    kfree(list);
    return security;
}

static int wifi_save(struct wlan_dev *w, int argc, char **argv) {
    struct netcfg_net net;
    struct wlan_station_info si;
    char name[4 * WLAN_SSID_MAX + 1], pass[PASS_MAX], question[4 * WLAN_SSID_MAX + 64];
    u_int len;
    int security, r;

    memset(&net, 0, sizeof(net));
    if(argc == 2) {                                     /* the network joined now, with the key it was joined with */
        if(wlan_station(w, &si, true) || (si.state != WLAN_STA_UP && si.state != WLAN_STA_RECONNECTING) || !si.ssid_len) {
            wipe(&si, sizeof(si));
            printf("    wifi: %s is not joined to a network: wifi join <ssid> first, or wifi save <ssid>\n", w->name);
            return CMD_FAIL;
        }
        memcpy(net.ssid, si.ssid, si.ssid_len);
        net.ssid_len = si.ssid_len;
        net.hidden = si.hidden;
        ssid_str(net.ssid, net.ssid_len, name, sizeof(name));
        if(si.security == WLAN_SEC_OPEN) {
            net.security = NETCFG_SEC_OPEN;
        } else if(si.has_key) {
            net.security = NETCFG_SEC_WPA2;
            memcpy(net.psk, si.key, WPA_PMK_LEN);
            net.has_psk = true;
        } else {
            wipe(&si, sizeof(si));
            printf("    wifi: the station keeps no key for %s: wifi join %s, then wifi save\n", name, name);
            return CMD_FAIL;
        }
        wipe(&si, sizeof(si));
        r = saved_it(&net, name, true, false);
        wipe(&net, sizeof(net));
        return r;
    }

    if(ssid_of_words(argv, 2, argc - 1, net.ssid, &len)) return CMD_USAGE;
    net.ssid_len = (u8)len;
    ssid_str(net.ssid, len, name, sizeof(name));
    security = heard_as(w, net.ssid, len, &net.hidden); /* as the last scan heard it, if it did */
    if(security < 0) net.hidden = true;                 /* not heard by its name: hidden, or not here (probed for) */
    if(security >= 0 && security != WLAN_SEC_OPEN && security != WLAN_SEC_WPA2_PSK && security != WLAN_SEC_WPA2_WPA3)
        return refuse(name, security);
    if(security == WLAN_SEC_OPEN) {
        net.security = NETCFG_SEC_OPEN;
        return saved_it(&net, name, true, false);
    }
    if(!wpa_passphrase_to_pmk) {
        printf("    wifi: WPA2 needs the supplicant (net/wlan/wpa.c), which this kernel does not have\n");
        return CMD_FAIL;
    }
    snprintf(question, sizeof(question), security < 0 ? "    Password for %s (nothing for an open network): " :
             "    Password for %s: ", name);
    shell_ask_secret(question, pass, sizeof(pass));
    if(task_interrupted()) {                            /* Ctrl+C at the prompt: nothing is saved */
        wipe(pass, sizeof(pass));
        return fail("save", -EINTR);
    }
    if(!pass[0]) {
        wipe(pass, sizeof(pass));
        if(security >= 0) {
            printf("    wifi: no password: not saved\n");
            return CMD_FAIL;
        }
        net.security = NETCFG_SEC_OPEN;
        return saved_it(&net, name, true, true);
    }
    r = wpa_passphrase_to_pmk(pass, net.ssid, len, net.psk);
    wipe(pass, sizeof(pass));
    if(r) {
        wipe(&net, sizeof(net));
        printf("    wifi: a WPA2 password is 8 to 63 characters (letters, digits, spaces and punctuation): not saved\n");
        return CMD_FAIL;
    }
    net.security = NETCFG_SEC_WPA2;
    net.has_psk = true;
    r = saved_it(&net, name, false, security < 0);
    wipe(&net, sizeof(net));
    return r;
}

static int wifi_forget(int argc, char **argv) {
    u8 ssid[WLAN_SSID_MAX];
    char name[4 * WLAN_SSID_MAX + 1], why[200];
    u_int len;
    int r, n = 0;
    if(ssid_of_words(argv, 2, argc - 1, ssid, &len)) return CMD_USAGE;
    ssid_str(ssid, len, name, sizeof(name));
    r = netman_forget_network(ssid, len, &n, why, sizeof(why));
    if(r == -ENOENT) {
        printf("    wifi: %s is not a saved network (wifi saved lists them)\n", name);
        return CMD_FAIL;
    }
    if(r) {
        printf("    wifi: %s not forgotten: %s\n", name, why);
        return CMD_FAIL;
    }
    if(n) printf("    forgot %s: SurfOS no longer joins it by itself (%i saved network%s left)\n", name, n,
                 n == 1 ? "" : "s");
    else printf("    forgot %s: SurfOS no longer joins it by itself (no saved network left)\n", name);
    return CMD_OK;
}

static int wifi_saved(struct wlan_dev *w) {
    struct netman_net *nets = (struct netman_net *)kalloc(NETCFG_NETS_MAX * sizeof(*nets));
    struct wlan_bss *list = (struct wlan_bss *)kalloc(LIST_MAX * sizeof(*list));
    struct wlan_station_info si;
    struct netcfg_settings cfg;
    char name[4 * WLAN_SSID_MAX + 1], state[64];
    int n, nb, i, j, kept = 0;
    if(!nets || !list) {
        kfree(nets);
        kfree(list);
        printf("    wifi: no memory for the list\n");
        return CMD_FAIL;
    }
    n = netman_networks(nets, NETCFG_NETS_MAX, &kept);
    netman_settings(&cfg);
    if(netman_settings_off()) {
        printf("    the saved Wi-Fi networks are not read at this start (settings=off): wifi saved lists them after\n"
               "    the next start (wifi save and wifi forget still change them on %s)\n", stick_name());
    } else if(!n) {
        printf("    no saved Wi-Fi networks (wifi save keeps the one %s is joined to)\n", w->name);
    } else {
        nb = (int)wlan_bss_list(w, list, LIST_MAX);
        if(wlan_station(w, &si, false)) memset(&si, 0, sizeof(si));
        printf("    saved Wi-Fi networks on %s (%s/%s), tried in this order%s:\n", stick_name(), NETCFG_DIR,
               NETCFG_NETWORKS, cfg.wifi ? "" : " (not at startup: settings wifi is off)");
        for(i = 0; i < n; i++) {
            state[0] = 0;
            if(si.state == WLAN_STA_UP && si.ssid_len == nets[i].ssid_len && !memcmp(si.ssid, nets[i].ssid, si.ssid_len))
                strlcpy(state, "joined now", sizeof(state));
            else if(nets[i].refused)
                strlcpy(state, "its key was refused: wifi join it, then wifi save", sizeof(state));
            for(j = 0; j < nb && !state[0]; j++) {
                if(list[j].ssid_len == nets[i].ssid_len && !memcmp(list[j].ssid, nets[i].ssid, list[j].ssid_len))
                    snprintf(state, sizeof(state), "in range, %d dBm", list[j].rssi);
            }
            if(!state[0] && nets[i].hidden) strlcpy(state, "hidden", sizeof(state));
            if(state[0]) printf("    %2i  %-32s  %-4s  %s\n", i + 1, ssid_str(nets[i].ssid, nets[i].ssid_len, name,
                                sizeof(name)), sec_name(nets[i].security), state);
            else printf("    %2i  %-32s  %s\n", i + 1, ssid_str(nets[i].ssid, nets[i].ssid_len, name, sizeof(name)),
                        sec_name(nets[i].security));
        }
        printf("    (in range: heard in the last minute; wifi scan, then wifi saved, to look again)\n");
    }
    if(kept && !netman_settings_off())
        printf("    %s/%s also has %i part%s SurfOS did not understand, kept there as %s: correct %s on another\n"
               "    computer, or delete %s (dmesg says what is wrong with %s)\n", NETCFG_DIR, NETCFG_NETWORKS, kept,
               kept == 1 ? "" : "s", kept == 1 ? "it was" : "they were", kept == 1 ? "it" : "them",
               kept == 1 ? "it" : "them", kept == 1 ? "it" : "them");
    kfree(nets);
    kfree(list);
    return CMD_OK;
}

static int wifi_connect(struct wlan_dev *w) {
    struct wlan_station_info si;
    char name[4 * WLAN_SSID_MAX + 1];
    int r;
    if(wlan_station(w, &si, false) == 0) {
        ssid_str(si.ssid, si.ssid_len, name, sizeof(name));
        if(si.state == WLAN_STA_UP) {
            printf("    %s is joined to %s already (wifi leave first, for a saved network instead)\n", w->name, name);
            return CMD_OK;
        }
        if(si.state == WLAN_STA_JOINING || si.state == WLAN_STA_RECONNECTING) {
            printf("    wifi: busy: %s is joining %s (wifi status says how it goes)\n", w->name, name);
            return CMD_FAIL;
        }
    }
    r = netman_wifi_connect(w);
    if(r == -EBUSY) printf("    wifi: busy: the network manager or another wifi command has %s; try again in a moment\n",
                           w->name);
    else if(r == -EINTR) printf("    wifi: connect cut short\n");
    else if(r == -ECANCELED) printf("    wifi: connect called off by a wifi leave or wifi down meanwhile\n");
    return r ? CMD_FAIL : CMD_OK;
}

/**** wlansim (10/17/26): the tests' access points off and on ****/

int cmd_wlansim(int argc, char **argv) {
    int r;
    if(argc != 3 || (strcmp(argv[1], "on") && strcmp(argv[1], "off"))) {
        printf("    usage: wlansim on | off <ssid>   (the simulated Wi-Fi's access points: the kernel word wlansim)\n");
        return CMD_USAGE;
    }
    r = wlansim_switch(argv[2], !strcmp(argv[1], "on"));
    if(r == -ENODEV) printf("    wlansim: no simulated Wi-Fi chip (the kernel word wlansim)\n");
    else if(r == -ENOENT) printf("    wlansim: no simulated access point named %s\n", argv[2]);
    return r ? CMD_FAIL : CMD_OK;
}

/**** the command ****/

int cmd_wifi(int argc, char **argv) {
    struct wlan_dev *w;
    struct netdev *nd;
    char a[16], b[16], c[16], m[18];
    int r;
    if(argc < 2) {
        printf("    " USAGE "\n");
        return CMD_USAGE;
    }
    if(strcmp(argv[1], "up") && strcmp(argv[1], "down") && strcmp(argv[1], "scan") && strcmp(argv[1], "join") &&
       strcmp(argv[1], "status") && strcmp(argv[1], "leave") && strcmp(argv[1], "save") && strcmp(argv[1], "forget") &&
       strcmp(argv[1], "saved") && strcmp(argv[1], "connect")) {
        printf("    " USAGE "\n");
        return CMD_USAGE;
    }
    if(strcmp(argv[1], "join") && strcmp(argv[1], "save") && strcmp(argv[1], "forget") && argc > 2) {
        printf("    usage: wifi %s   (no arguments)\n", argv[1]);
        return CMD_USAGE;
    }
    if(!strcmp(argv[1], "forget") && argc < 3) {
        printf("    usage: wifi forget <ssid>   (wifi saved lists them)\n");
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
    if(!strcmp(argv[1], "save")) return wifi_save(w, argc, argv);
    if(!strcmp(argv[1], "forget")) return wifi_forget(argc, argv);
    if(!strcmp(argv[1], "saved")) return wifi_saved(w);
    if(!strcmp(argv[1], "connect")) return wifi_connect(w);
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
