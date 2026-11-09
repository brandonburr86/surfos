/*
SurfOS host tests: a simulated kernel for Settings
--------------------
File: wsettings_fake.h  Date: 11/05/26 (the desktop applications, track settings)
--------------------
SYS_netctl (include/surfos/netctl.h) as Settings' model (user/wsettings/settings.c) meets it, for
tests/host/test_wsettings.c (the model) and tests/host/test_wsettings_win.c (the window on the fake
desktop): the reads answered from structures the test sets, a job started (refused while one runs,
by another program's job, by a refusal the test plans, or as a kernel without the call) and ended
when the test says, and what each start was handed kept for the checks, a join's password as the
kernel saw it before it wiped the program's field. wlansim's six networks are there from the start,
with the test passphrase of tools/qemu-run.py, never a real one. Included by one test each: its
functions are its own.
*/
#ifndef _WSETTINGS_FAKE_H
#define _WSETTINGS_FAKE_H

#include <stdio.h>
#include <string.h>
#include <surfos/netctl.h>

#define PASSPHRASE "surfsim-wpa2-test-passphrase"     /* tools/qemu-run.py's test network's, never a real one */

static struct {
    struct netctl_wifi wifi;
    struct netctl_bss_list bss;
    struct netctl_saved saved;
    struct netctl_settings settings;
    struct netctl_ifaces ifaces;
    struct netctl_job job;
    int busy_other;                     /* another program's job runs */
    int no_call;                        /* a kernel without SYS_netctl */
    int refuse;                         /* the next start answers this */
    int starts, last_op, bad;           /* bad: a call with a size or an argument that is none */
    int applies, apply_r;               /* NETCTL_APPLY's calls, and what it answers (0: in force) */
    int job_read_r;                     /* what the next NETCTL_JOB read answers instead (-12: no memory just then) */
    int exposed_now;                    /* a server without a password runs, whatever k.wifi (read every 2 s) says */
    struct netctl_req last_apply;
    struct netctl_join last_join;       /* what a join was handed (its password as the kernel saw it) */
    struct netctl_req last_req;
    unsigned seq;
} k;

static int fake_first_stage(int op, int on) {
    switch(op) {
    case NETCTL_CARD:   return on ? NETCTL_STAGE_STARTING : NETCTL_STAGE_STOPPING;
    case NETCTL_SCAN:   return NETCTL_STAGE_SCANNING;
    case NETCTL_JOIN:   return NETCTL_STAGE_JOINING;
    case NETCTL_LEAVE:  return NETCTL_STAGE_LEAVING;
    case NETCTL_SAVE:   return NETCTL_STAGE_SAVING;
    default:            return NETCTL_STAGE_WRITING;
    }
}

static int fake_read(void *arg, unsigned size, const void *from, unsigned want) {
    if(size != want || !arg) {
        k.bad++;
        return -22;
    }
    memcpy(arg, from, want);
    return 0;
}

static int fake_netctl(void *ctx, int op, void *arg, unsigned size) {
    (void)ctx;
    if(k.no_call) return -38;
    switch(op) {
    case NETCTL_WIFI:     return fake_read(arg, size, &k.wifi, sizeof(k.wifi));
    case NETCTL_NETWORKS: return fake_read(arg, size, &k.bss, sizeof(k.bss));
    case NETCTL_SAVED:    return fake_read(arg, size, &k.saved, sizeof(k.saved));
    case NETCTL_SETTINGS: return fake_read(arg, size, &k.settings, sizeof(k.settings));
    case NETCTL_IFACES:   return fake_read(arg, size, &k.ifaces, sizeof(k.ifaces));
    case NETCTL_JOB:
        if(k.job_read_r) {
            int r = k.job_read_r;
            k.job_read_r = 0;
            return r;
        }
        return fake_read(arg, size, &k.job, sizeof(k.job));
    case NETCTL_APPLY:                                  /* at once, nothing written: the value in force */
        if(size != sizeof(struct netctl_req) || !arg) {
            k.bad++;
            return -22;
        }
        k.applies++;
        k.last_apply = *(struct netctl_req *)arg;
        if(!k.apply_r) {
            unsigned i;
            for(i = 0; i < k.settings.count; i++)
                if(!strcmp(k.settings.key[i].name, k.last_apply.key))
                    snprintf(k.settings.key[i].value, sizeof(k.settings.key[i].value), "%s", k.last_apply.value);
        }
        return k.apply_r;
    }
    k.starts++;
    if(op == NETCTL_JOIN) {
        struct netctl_join *j = (struct netctl_join *)arg;
        if(size != sizeof(*j) || !j) {
            k.bad++;
            return -22;
        }
        k.last_join = *j;
        memset(j->passphrase, 0, sizeof(j->passphrase));    /* the kernel's wipe of the program's field */
    } else if(op == NETCTL_CARD || op == NETCTL_FORGET || op == NETCTL_SET) {
        if(size != sizeof(struct netctl_req) || !arg) {
            k.bad++;
            return -22;
        }
        k.last_req = *(struct netctl_req *)arg;
    } else if(size || arg) {
        k.bad++;
        return -22;
    }
    if(k.refuse) {
        int r = k.refuse;
        k.refuse = 0;
        return r;
    }
    if(k.job.state == NETCTL_JOB_RUNNING || k.busy_other) return -16;
    if(op == NETCTL_JOIN && !k.last_join.passphrase[0] && (k.wifi.exposed || k.exposed_now) &&
       !(k.last_join.flags & NETCTL_JOIN_EXPOSED))
        return -1;                                      /* EPERM: open, a server without a password runs, nobody told */
    k.last_op = op;
    memset(&k.job, 0, sizeof(k.job));
    k.job.seq = ++k.seq;
    k.job.op = op;
    k.job.state = NETCTL_JOB_RUNNING;
    k.job.stage = fake_first_stage(op, op == NETCTL_CARD && k.last_req.on);
    if(op == NETCTL_JOIN) {                             /* a name not heard: the kernel's job looks for it first */
        unsigned i;
        for(i = 0; i < k.bss.count && (k.bss.bss[i].ssid_len != k.last_join.ssid_len ||
                                      memcmp(k.bss.bss[i].ssid, k.last_join.ssid, k.last_join.ssid_len)); i++) ;
        if(i == k.bss.count) k.job.stage = NETCTL_STAGE_SCANNING;
    }
    return (int)k.job.seq;
}

static void job_done(int result, unsigned flags, const char *why) {
    k.job.state = NETCTL_JOB_DONE;
    k.job.result = result;
    k.job.flags = flags;
    k.job.stage = 0;
    snprintf(k.job.why, sizeof(k.job.why), "%s", why ? why : "");
}

static void add_bss(const char *ssid, int security, int rssi, int channel) {
    struct netctl_bss *b = &k.bss.bss[k.bss.count++];
    memset(b, 0, sizeof(*b));
    b->ssid_len = (unsigned char)strlen(ssid);
    memcpy(b->ssid, ssid, b->ssid_len);
    b->security = (unsigned char)security;
    b->rssi = (signed char)rssi;
    b->channel = (unsigned char)channel;
    b->band = channel > 14;
}

static void add_setting(const char *name, const char *value, const char *values) {
    struct netctl_setting *s = &k.settings.key[k.settings.count++];
    snprintf(s->name, sizeof(s->name), "%s", name);
    snprintf(s->value, sizeof(s->value), "%s", value);
    snprintf(s->values, sizeof(s->values), "%s", values);
}

static struct netctl_setting *setting(const char *name) {
    unsigned i;
    for(i = 0; i < k.settings.count; i++) if(!strcmp(k.settings.key[i].name, name)) return &k.settings.key[i];
    return NULL;
}

static void add_saved(const char *ssid, int security, int hidden) {
    struct netctl_saved_net *s = &k.saved.net[k.saved.count++];
    memset(s, 0, sizeof(*s));
    s->ssid_len = (unsigned char)strlen(ssid);
    memcpy(s->ssid, ssid, s->ssid_len);
    s->security = (unsigned char)security;
    s->hidden = (unsigned char)hidden;
}

static unsigned ip4(int a, int b, int c, int d) {
    unsigned char x[4] = { (unsigned char)a, (unsigned char)b, (unsigned char)c, (unsigned char)d };
    unsigned v;
    memcpy(&v, x, 4);
    return v;
}

/* wlansim's networks (and two of the same name), the card on and idle, Ethernet with a lease, the network manager's
   table as it is before desk's keys */
static void fake_reset(void) {
    memset(&k, 0, sizeof(k));
    k.wifi.present = 1;
    k.wifi.up = 1;
    k.wifi.state = NETCTL_STA_IDLE;
    strcpy(k.wifi.name, "wlan0");
    add_bss("SurfSim", NETCTL_SEC_OPEN, -45, 6);
    add_bss("SurfSimWPA", NETCTL_SEC_WPA2, -52, 36);
    add_bss("Twins", NETCTL_SEC_OPEN, -54, 6);
    add_bss("SurfSimFlaky", NETCTL_SEC_OPEN, -58, 1);
    add_bss("SurfSimSAE", NETCTL_SEC_WPA3, -60, 149);
    add_bss("", NETCTL_SEC_OPEN, -67, 11);                 /* SurfSimHidden's beacons */
    add_bss("SurfSimWEP", NETCTL_SEC_WEP, -71, 1);
    add_bss("Twins", NETCTL_SEC_WPA2, -80, 11);
    add_setting("telnetd", "off", "on | off");
    add_setting("telnetd.port", "23", "1-65535");
    add_setting("wifi", "on", "on | off");
    add_setting("ethernet", "off", "dhcp | off");
    add_setting("hostname", "surfos", "letters, digits, -");
    add_setting("history", "off", "on | off");
    k.settings.can_save = 1;
    snprintf(k.settings.where, sizeof(k.settings.where), "%s", "surfos-config/settings.conf on usb1, the stick SurfOS "
             "was started from");
    k.ifaces.count = 2;
    strcpy(k.ifaces.hostname, "surfos");
    strcpy(k.ifaces.iface[0].name, "eth0");
    k.ifaces.iface[0].mac[0] = 0x52;
    k.ifaces.iface[0].mac[1] = 0x54;
    k.ifaces.iface[0].link_up = 1;
    k.ifaces.iface[0].ip = ip4(10, 0, 2, 15);
    k.ifaces.iface[0].netmask = ip4(255, 255, 255, 0);
    k.ifaces.iface[0].gateway = ip4(10, 0, 2, 2);
    k.ifaces.iface[0].dns = ip4(10, 0, 2, 3);
    k.ifaces.iface[0].dhcp_server = ip4(10, 0, 2, 2);
    k.ifaces.iface[0].lease_s = 86400;
    k.ifaces.iface[0].lease_left = 86300;
    strcpy(k.ifaces.iface[1].name, "wlan0");
    k.ifaces.iface[1].wifi = 1;
}

#endif
