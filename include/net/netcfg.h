/*
SurfOS settings: the files on the stick
----------------------
File: netcfg.h  Date: 10/17/26 (the network manager)
----------------------
The two files the network manager (kernel/netman.c) reads at boot from the stick SurfOS
was started from, and writes when a setting or a saved Wi-Fi network changes:

    surfos-config/settings.conf   "key = value" lines: telnetd, telnetd.port, wifi,
                                  ethernet, hostname (netcfg_key_*)
    surfos-config/networks.conf   the saved Wi-Fi networks, in the order they are tried

This is the pure part: parsing, editing and writing the text, quoting an SSID, and the
DHCP lease timers. It touches no device and keeps nothing, and is host-tested
(tests/host/test_netcfg.c). The brief is docs/superpowers/plans/2026-10-17-network-manager.md.
*/

#ifndef _NET_NETCFG_H
#define _NET_NETCFG_H

#include <surfos/types.h>

#define NETCFG_DIR           "surfos-config"
#define NETCFG_SETTINGS      "settings.conf"
#define NETCFG_NETWORKS      "networks.conf"
#define NETCFG_SETTINGS_MAX  8192   /* the longest settings.conf read or written */
#define NETCFG_NETWORKS_MAX  16384  /* the longest networks.conf */
#define NETCFG_NETS_MAX      16     /* saved networks */
#define NETCFG_SSID_MAX      32
#define NETCFG_KEY_LEN       32     /* a PSK (the PMK): 256 bits, 64 hex digits in the file */
#define NETCFG_HOSTNAME_MAX  32

/**** settings.conf ****/

#define NETCFG_TELNETD       0      /* on | off: the telnet server at startup */
#define NETCFG_TELNETD_PORT  1      /* 1-65535 */
#define NETCFG_WIFI          2      /* on | off: join a saved network at startup and keep joined */
#define NETCFG_ETHERNET      3      /* dhcp | off: an address whenever an Ethernet link comes up */
#define NETCFG_HOSTNAME      4      /* the DHCP host name */
#define NETCFG_NKEYS         5

struct netcfg_settings {
    bool telnetd;
    u16 telnetd_port;
    bool wifi;
    bool ethernet_dhcp;
    char hostname[NETCFG_HOSTNAME_MAX + 1];
    u32 set;                        /* bit k: key k was set by the file (else it has its default) */
};

/* what a line was found to be, for a reader's messages: (arg, line number from 1, why) */
typedef void (*netcfg_note_fn)(void *arg, u_int line, const char *why);

void netcfg_defaults(struct netcfg_settings *s);
int netcfg_key_find(const char *name);                  /* its number, or -1 */
const char *netcfg_key_name(int k);
const char *netcfg_key_values(int k);                   /* "on | off", "1-65535", ... */
const char *netcfg_key_help(int k);                     /* one line, for the settings command */
const char *netcfg_value(const struct netcfg_settings *s, int k, char *buf, size_t size);
const char *netcfg_default_value(int k, char *buf, size_t size);
/* value for key k into s: 0, or -1 and why (s unchanged) */
int netcfg_set(struct netcfg_settings *s, int k, const char *value, char *why, size_t size);

/* the settings of a file of len bytes: the defaults, then its "key = value" lines in order (the last one of a key
   wins); a line that sets nothing known is told to note (may be NULL) and skipped */
void netcfg_parse_settings(const char *text, u_int len, struct netcfg_settings *s, netcfg_note_fn note, void *arg);

/* the file with key k set to value (checked first: -2 and why in out when it is no value of k), into out (size
   bytes, NUL-terminated); its length, or -1 when it does not fit. The first line setting k gets the new value, its
   comment kept; other lines setting k go; with none, a commented-out "#key = ..." line (as the new file has them)
   becomes the setting, or it is added at the end. Everything else stays as it was, byte for byte. text NULL: no file
   yet: a new one, every key explained and commented out with its default, k set */
int netcfg_edit_settings(const char *text, u_int len, int k, const char *value, char *out, u_int size);

/**** networks.conf ****/

#define NETCFG_SEC_OPEN      0
#define NETCFG_SEC_WPA2      1      /* WPA2-Personal (also a WPA2/WPA3 network, joined as WPA2) */

struct netcfg_net {
    u8 ssid[NETCFG_SSID_MAX];
    u8 ssid_len;
    u8 security;                    /* NETCFG_SEC_* */
    bool hidden;                    /* its beacons carry no SSID: joined by a probe for the name */
    bool has_psk;
    u8 psk[NETCFG_KEY_LEN];
    bool has_password;              /* written by hand: password = "..." instead of psk. The reader turns it into the
                                       key (wpa_passphrase_to_pmk()) and wipes it; it is never written back */
    char password[64];
};

/* the networks of a file of len bytes, in order, into nets (max); how many. A network with a problem (no name, a
   bad key, WPA2 without a key, a name seen before) is told to note and left out */
int netcfg_parse_networks(const char *text, u_int len, struct netcfg_net *nets, int max, netcfg_note_fn note, void *arg);

/* the file for nets[0..n): the header that explains it, then a block per network (psk, never a password); its
   length, or -1 when it does not fit in size */
int netcfg_write_networks(const struct netcfg_net *nets, int n, char *out, u_int size);

int netcfg_find_net(const struct netcfg_net *nets, int n, const u8 *ssid, u_int len);   /* its index, or -1 */

/* an SSID (len bytes) as the file writes it: in double quotes, printable ASCII as it is but \ and ", which get a
   backslash, every other byte \xHH. The length, or -1 when it does not fit */
int netcfg_quote(const u8 *ssid, u_int len, char *out, size_t size);
/* the other way: a quoted value, or a bare one (as typed by hand: its spaces inside kept), into out (max bytes);
   0 and *len, or -1 */
int netcfg_unquote(const char *in, u8 *out, u_int max, u_int *len);

/**** DHCP leases (net/dhcp.c keeps them; kernel/netman.c asks this when to act) ****/

#define NETCFG_LEASE_WAIT    0      /* nothing to do yet */
#define NETCFG_LEASE_RENEW   1      /* past half its time: ask the server that granted it */
#define NETCFG_LEASE_REBIND  2      /* past seven eighths: ask any server (broadcast) */
#define NETCFG_LEASE_EXPIRED 3      /* over: the address must go */

/* a lease of lease_s seconds granted at tick granted; last_try: the tick of the last renewal tried for it (0: none
   yet); hz ticks a second. Between tries the wait is half the time left to the next stage, at least min(60 s, an
   eighth of the lease) and at least 1 s, as RFC 2131 4.4.5 suggests */
int netcfg_lease_step(u32 lease_s, u_long granted, u_long last_try, u_long now, u_int hz);

#endif
