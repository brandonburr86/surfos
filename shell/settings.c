/*
SurfOS settings: the settings command
--------------------
File: settings.c    Date: 10/17/26 (the network manager)
--------------------
    settings                    every setting with its value, and where they are kept
    settings <key>              one: its value, the values it takes and what it does
    settings <key> <value>      change it (also "settings <key> = <value>", as the file has it):
                                saved in surfos-config/settings.conf on the stick SurfOS was started
                                from, which every start reads (kernel/netman.c), and in force at
                                once where that means anything: telnetd and telnetd.port act at a
                                start, and under settings=off (the menu's entry without the
                                settings) nothing changes before the next start

The keys and their values are net/netcfg.c's: telnetd, telnetd.port, wifi, ethernet, hostname, and since
10/25/26 history (track A, A10: surfsh's history in surfos-config/history, fs/devfs.c's /dev/config/history).
The file is changed one line at a time, so what the owner wrote there on another computer stays.

A kernel command (CLAUDE.md): it prints on its own console, keeps nothing in statics, asks nothing.
0, 1 or 2 as the other commands: a key or a value it does not know is a usage error (2); a change
not saved on the stick is a failure (1), and says why (and whether it is in force meanwhile).
*/

#include <surfos/types.h>
#include <surfos/console.h>
#include <sys/bdev.h>
#include <net/netcfg.h>
#include <net/netman.h>
#include <blibc_common.h>
#include "shell.h"

#define VALUE_MAX (NETCFG_HOSTNAME_MAX + 8)     /* a value as the file writes it, the longest a host name */

/* when a change takes effect, after "saved" */
static const char *when(int k, const struct netcfg_settings *s) {
    switch(k) {
    case NETCFG_TELNETD:      return s->telnetd ? "at the next start (telnetd on starts it now)" :
                                                  "at the next start (telnetd off stops it now)";
    case NETCFG_TELNETD_PORT: return "at the next start";
    case NETCFG_WIFI:         return s->wifi ? "at every start a saved network is joined by itself (wifi connect joins one "
                                               "now)" : "no saved network is joined by itself, from now on (wifi down "
                                               "turns the card off)";
    case NETCFG_ETHERNET:     return s->ethernet_dhcp ? "now: an Ethernet link that comes up gets an address" :
                                                        "now: dhcp asks for an address by hand";
    case NETCFG_HISTORY:      return s->history ? "from now on surfsh saves its history there (plain text: a line that "
                                                  "starts with a space is never kept), and a surfsh started later "
                                                  "recalls it" :
                                                  "from now on surfsh saves no history (what is saved stays in "
                                                  "surfos-config/history until it is deleted)";
    default:                  return "in the next DHCP request";
    }
}

/* a key and its value; full: what it takes and does as well */
static void show(const struct netcfg_settings *s, int k, bool full) {
    char v[VALUE_MAX], d[VALUE_MAX];
    bool set = (s->set >> k) & 1;
    netcfg_value(s, k, v, sizeof(v));
    netcfg_default_value(k, d, sizeof(d));
    if(!full) {
        if(set) printf("    %-12s = %s\n", netcfg_key_name(k), v);
        else printf("    %-12s = %-8s  (the default)\n", netcfg_key_name(k), v);
        return;
    }
    if(set) printf("    %s = %s   (set; the default is %s; it takes %s)\n", netcfg_key_name(k), v, d, netcfg_key_values(k));
    else printf("    %s = %s   (the default; it takes %s)\n", netcfg_key_name(k), v, netcfg_key_values(k));
    printf("    %s\n", netcfg_key_help(k));
}

int cmd_settings(int argc, char **argv) {
    struct netcfg_settings s, t;
    struct netman_net nets[NETCFG_NETS_MAX];
    char where[200], why[200], key[24], v[VALUE_MAX];
    const char *value = NULL, *eq;
    bool off = netman_settings_off();
    int k, r, n;

    netman_settings(&s);
    if(argc == 1) {
        netman_where(where, sizeof(where));
        printf("    settings: %s\n", where);
        for(k = 0; k < NETCFG_NKEYS; k++) show(&s, k, false);
        n = netman_networks(nets, NETCFG_NETS_MAX, NULL);
        printf("    settings <key> says what one does, settings <key> <value> changes it; %i saved Wi-Fi network%s "
               "(wifi saved)\n", n, n == 1 ? "" : "s");
        return CMD_OK;
    }
    /* settings key, settings key value, settings key = value, settings key=value */
    eq = strchr(argv[1], '=');
    if(eq && argc == 2) {
        k = -1;
        if((size_t)(eq - argv[1]) < sizeof(key)) {
            memcpy(key, argv[1], (size_t)(eq - argv[1]));
            key[eq - argv[1]] = 0;
            k = netcfg_key_find(key);
        }
        value = eq + 1;
    } else {
        k = netcfg_key_find(argv[1]);
        if(argc == 3) value = argv[2];
        else if(argc == 4 && !strcmp(argv[2], "=")) value = argv[3];
        else if(argc != 2) {
            printf("    usage: settings [key [value]]   (settings alone lists them)\n");
            return CMD_USAGE;
        }
    }
    if(k < 0) {
        if(eq && argc == 2) printf("    settings: no setting called %.*s (settings lists them)\n", (int)(eq - argv[1]),
                                   argv[1]);
        else printf("    settings: no setting called %s (settings lists them)\n", argv[1]);
        return CMD_USAGE;
    }
    if(!value) {
        show(&s, k, true);
        return CMD_OK;
    }
    t = s;                                              /* the new value, as the file writes it */
    if(netcfg_set(&t, k, value, why, sizeof(why))) {
        printf("    settings: %s\n", why);
        return CMD_USAGE;
    }
    netcfg_value(&t, k, v, sizeof(v));
    r = netman_set(k, value, why, sizeof(why));
    if(r < 0) {
        printf("    settings: %s\n", why);
        return CMD_USAGE;
    }
    if(r > 0 && (off || k == NETCFG_TELNETD || k == NETCFG_TELNETD_PORT)) {     /* nothing in force: nothing changes */
        printf("    settings: %s = %s not saved: %s; %s, so nothing changes%s\n", netcfg_key_name(k), v, why,
               off ? "this start does not use the settings (settings=off)" : "it would act at the next start only",
               k == NETCFG_TELNETD ? (t.telnetd ? " (telnetd on starts the server now)" :
                                     " (telnetd off stops the server now)") : "");
        return CMD_FAIL;
    }
    if(r > 0) {
        printf("    settings: %s = %s in force until SurfOS restarts, but not saved: %s\n", netcfg_key_name(k), v, why);
        return CMD_FAIL;
    }
    printf("    %s = %s, saved on %s (%s/%s): %s\n", netcfg_key_name(k), v,
           bdev_boot_vol() ? bdev_boot_vol()->name : "the stick", NETCFG_DIR, NETCFG_SETTINGS,
           off ? "from the next start (this one does not use the settings: settings=off)" : when(k, &t));
    if(k == NETCFG_TELNETD && t.telnetd)
        printf("    telnetd has no password and no encryption: whoever reaches the laptop over a network SurfOS is on\n"
               "    gets a shell. It starts only once there is an address on Ethernet or a WPA2 network, and while it\n"
               "    runs SurfOS joins no open Wi-Fi network by itself. But SurfOS cannot tell whose network it is:\n"
               "    a cafe's WPA2 network, whose password others know, counts too. Save only your own networks.\n");
    return CMD_OK;
}
