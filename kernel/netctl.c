/*
SurfOS network settings for programs
--------------------
File: netctl.c  Date: 11/05/26 (the desktop applications, track settings)
--------------------
SYS_netctl (include/surfos/netctl.h): the Wi-Fi station, the network manager's settings and saved
networks, and the interfaces' addresses, for a program with a window: Settings (user/wsettings/).
The commands do the same for a terminal (shell/wifi.c, shell/settings.c); this is their structured
side, on the same functions (net/wlan/wlan.c, kernel/netman.c, net/netcfg.c), with nothing of its own
to decide but what the wifi command decides before a join.

Reads. Each fills a structure of the kernel's, zeroed first so that nothing else of the kernel's
reaches the program, and copies it out whole: the caller's memory is checked first for exactly the
structure's size (written), and again just before the copy. They take only short locks (net_lock,
the network manager's nm_lock inside its calls, the job's) and never wait for the radio or the stick.

Jobs. What waits (a scan, a join and its DHCP, the card, a leave, a write of the stick) is a job: the
call checks its request, copies what it needs into the job's record and starts a kernel thread,
`netctl`, on F1 as the network manager's are, and returns the job's number. One job runs at a time,
for every program (job_lock around the record: a start while one runs is -EBUSY); the thread reads
the request alone while it runs, says its stage, and at its end its result, what else came of it and
why, in words that are never a secret, and one line in the kernel log (dmesg). It is a kernel thread,
so no kill reaches it: it ends when its job does, also when its program has gone. The station's own
requests are one at a time per device (net/wlan/wlan.c): a job meanwhile with the network manager's
keeper or a wifi command at work gets -EBUSY from it, and says so.

A join (NETCTL_JOIN) is decided as wifi join decides it (shell/wifi.c): the security the networks of
that name in the last scans ask, WPA2 first, then open, else the strongest one's, which is refused
when SurfOS does not join it. A name not heard in the last minute (the station's list forgets what it
has not heard for that long, and a person may look at a list for longer) is looked for first, by the
job, with a scan, as wifi join makes one, and then decided so; a name still not heard is joined as a
hidden network (the station probes for it), WPA2 when a password came. An open network while a server
without a password runs (telnetd, ftpd: netman_telnet_exposed(), netman_ftp_exposed()) is refused
(-EPERM) unless the request says the person was told (NETCTL_JOIN_EXPOSED, which Settings sets only
from its warning's answer): at the call, and again by the job just before it joins, since its scan
first takes up to 10 s and a server may start meanwhile (11/05/26, the review's M2). The password comes in the request: the caller's field is wiped as soon
as the structure is known to be the caller's to write, whatever the call answers; the PMK is made
from a copy at once (wpa_passphrase_to_pmk(): PBKDF2 with the SSID) and the copy wiped, on the stack
of the call. The PMK goes to the job's record, the station (wlan_join_unless(), quiet: its failure in
the log, the window says it), networks.conf when the request asks to save the network (as wifi save
keeps it: the key, never the password), and is wiped as soon as the join and that save are done, or
when the thread cannot start. Then DHCP for wlan0, as the network manager asks for an association's
address: three tries, 2 s apart, while the station keeps that association. The job is the station's
like a wifi join typed in a Terminal: the network manager's keeper leaves it alone (it keeps only its
own associations), and looks for a saved network again only once the station gives this one up.

NETCTL_APPLY is the one change that is no job: a step of the desktop's theme or pointer speed put in
force by netman_apply(), which writes nothing, so that a person stepping through them (an arrow held
down) has each step at once and the stick written once, by the NETCTL_SET job their program starts
when the steps stop. Only those two keys: they act at once and nothing else depends on them.

The card on (NETCTL_CARD) starts the card and, when the setting wifi is on and there are saved
networks, does what wifi connect does (netman_wifi_connect_quiet(): its lines in the kernel log, as
the keeper's, since the job's thread has no console of its own): the first saved network in range
joined with its address, and the keeper looking by itself again. A card stopped from a window is the
owner's as after wifi down: the keeper waits for that.

Nothing here prints, logs or copies out a password or a key: the job's record holds the PMK only
while it is the thread's, a read copies only struct netctl_job's fields, and the log line names the
operation, the network and the outcome.
*/

#include <surfos/types.h>
#include <surfos/console.h>
#include <surfos/task.h>
#include <surfos/sync.h>
#include <surfos/timer.h>
#include <surfos/klog.h>
#include <surfos/sysimpl.h>
#include <surfos/syscall.h>
#include <surfos/netctl.h>
#include <mm/kalloc.h>
#include <fs/vfs.h>
#include <net/net.h>
#include <net/dhcp.h>
#include <net/wlan.h>
#include <net/wlan_cmd.h>
#include <net/wpa.h>
#include <net/ieee80211.h>
#include <net/netcfg.h>
#include <net/netman.h>
#include <blibc_common.h>

#pragma weak wpa_passphrase_to_pmk          /* as shell/wifi.c: a kernel without the supplicant joins no WPA2 */

#define SCAN_MS         10000               /* as the wifi command waits for a scan */
#define DESCRIBE_MAX    768                 /* wlan_describe()'s text, as wifi status takes it */
#define DHCP_TRIES      3                   /* as the network manager asks for an association's address */
#define DHCP_MS         8000
#define DHCP_PAUSE_MS   2000
#define SEQ_MAX         0x7fffffff          /* a job's number is an int the call returns */

/* the structures programs see keep these sizes (the header says why: no pointers, the same everywhere) */
typedef char netctl_wifi_is_fixed[sizeof(struct netctl_wifi) == 388 ? 1 : -1];
typedef char netctl_bss_is_fixed[sizeof(struct netctl_bss) == 48 ? 1 : -1];
typedef char netctl_bss_list_is_fixed[sizeof(struct netctl_bss_list) == 2308 ? 1 : -1];
typedef char netctl_saved_is_fixed[sizeof(struct netctl_saved) == 584 ? 1 : -1];
typedef char netctl_setting_is_fixed[sizeof(struct netctl_setting) == 116 ? 1 : -1];
typedef char netctl_settings_is_fixed[sizeof(struct netctl_settings) == 2064 ? 1 : -1];
typedef char netctl_ifaces_is_fixed[sizeof(struct netctl_ifaces) == 392 ? 1 : -1];
typedef char netctl_job_is_fixed[sizeof(struct netctl_job) == 260 ? 1 : -1];
typedef char netctl_join_is_fixed[sizeof(struct netctl_join) == 104 ? 1 : -1];
typedef char netctl_req_is_fixed[sizeof(struct netctl_req) == 112 ? 1 : -1];
/* and the numbers are the station's, the network manager's and the files' own */
typedef char netctl_sec_is_wlan[NETCTL_SEC_OPEN == WLAN_SEC_OPEN && NETCTL_SEC_WEP == WLAN_SEC_WEP &&
                                NETCTL_SEC_WPA2 == WLAN_SEC_WPA2_PSK && NETCTL_SEC_WPA2_WPA3 == WLAN_SEC_WPA2_WPA3 &&
                                NETCTL_SEC_WPA3 == WLAN_SEC_WPA3_SAE && NETCTL_SEC_ENTERPRISE == WLAN_SEC_ENTERPRISE &&
                                NETCTL_SEC_OTHER == WLAN_SEC_OTHER ? 1 : -1];
typedef char netctl_sta_is_wlan[NETCTL_STA_DOWN == WLAN_STA_DOWN && NETCTL_STA_IDLE == WLAN_STA_IDLE &&
                                NETCTL_STA_JOINING == WLAN_STA_JOINING && NETCTL_STA_UP == WLAN_STA_UP &&
                                NETCTL_STA_RECONNECTING == WLAN_STA_RECONNECTING && NETCTL_IDLE_NEW == WLAN_IDLE_NEW &&
                                NETCTL_IDLE_LEFT == WLAN_IDLE_LEFT && NETCTL_IDLE_JOIN_FAILED == WLAN_IDLE_JOIN_FAILED &&
                                NETCTL_IDLE_GAVE_UP == WLAN_IDLE_GAVE_UP ? 1 : -1];
typedef char netctl_saved_is_netcfg[NETCTL_SAVED_OPEN == NETCFG_SEC_OPEN && NETCTL_SAVED_WPA2 == NETCFG_SEC_WPA2 &&
                                    NETCTL_SAVED_MAX >= NETCFG_NETS_MAX ? 1 : -1];
#ifdef SYS_netctl                       /* programs make the call by the number netctl.h gives them */
typedef char netctl_number_is_the_calls[SYS_netctl == NETCTL_SYSCALL ? 1 : -1];
#endif
typedef char netctl_sizes_fit[NETCTL_SSID_MAX == WLAN_SSID_MAX && NETCTL_SSID_MAX == NETCFG_SSID_MAX &&
                              NETCTL_KEYS_MAX >= NETCFG_NKEYS && NETCTL_VALUE_MAX > NETCFG_HOSTNAME_MAX + 8 &&
                              NETCTL_HOST_MAX > NETCFG_HOSTNAME_MAX && NETCFG_KEY_LEN == WPA_PMK_LEN &&
                              NETCTL_PASS_MAX == 64 ? 1 : -1];

/* a job's request: what the call checked and copied, the thread's alone while it runs */
struct job_req {
    int op;
    struct wlan_dev *w;
    u_int stops;                        /* the station's leaves and downs when a join was asked: one since calls it off */
    bool on, save, hidden, has_pmk;
    bool unheard;                       /* a join's name not heard when it was asked: the job scans first */
    bool exposed_ok;                    /* NETCTL_JOIN_EXPOSED: an open network while a server runs, the person told */
    u8 pmk[WPA_PMK_LEN];
    u8 ssid[WLAN_SSID_MAX];
    u_int ssid_len;
    int key;
    char value[NETCTL_VALUE_MAX];
};

static mutex_t job_lock = MUTEX_INIT("netctl");
static struct netctl_job job;           /* what NETCTL_JOB tells: under job_lock */
static struct job_req req;              /* the running job's request (its PMK wiped at the job's end) */
static u_int job_seq;                   /* numbers given so far */
static char job_who[TASK_NAME_LEN + 24];/* the program that started it, for the log line */

/* not optimised away */
static void wipe(void *p, size_t n) {
    volatile u8 *q = (volatile u8 *)p;
    while(n--) *q++ = 0;
}

static u8 exposed(void) {
    return (netman_telnet_exposed() ? NETCTL_EXPOSED_TELNET : 0) | (netman_ftp_exposed() ? NETCTL_EXPOSED_FTP : 0);
}

/**** reads ****/

/* the line of text that starts at s, up to its end or a newline, into out */
static const char *line_into(const char *s, char *out, size_t size) {
    size_t n = 0;
    while(s[n] && s[n] != '\n') n++;
    if(size) {
        size_t c = n < size - 1 ? n : size - 1;
        memcpy(out, s, c);
        out[c] = 0;
    }
    return s[n] ? s + n + 1 : s + n;
}

static int fill_wifi(void *out) {
    struct netctl_wifi *k = (struct netctl_wifi *)out;
    struct wlan_station_info si;
    struct wlan_dev *w = wlan_find(NULL);
    struct netdev *nd;
    const char *s;
    char *text;
    if(!w) return 0;                                    /* present 0: no driver found a card */
    k->present = 1;
    k->up = w->up ? 1 : 0;
    strlcpy(k->name, w->name, sizeof(k->name));
    if(wlan_station(w, &si, false) == 0) {             /* a snapshot: never busy, never a key */
        k->state = (u8)si.state;
        k->idle = (u8)si.idle;
        k->ssid_len = si.ssid_len <= NETCTL_SSID_MAX ? si.ssid_len : NETCTL_SSID_MAX;
        memcpy(k->ssid, si.ssid, k->ssid_len);
        k->security = si.security;
        k->hidden = si.hidden ? 1 : 0;
    }
    nd = w->netdev;
    if(nd) {
        mutex_lock(&net_lock);                          /* dhcp and ifconfig change it under the lock */
        k->link_up = nd->link_up ? 1 : 0;
        k->ip = nd->ip;
        k->netmask = nd->netmask;
        k->gateway = nd->gateway;
        k->dns = nd->dns;
        mutex_unlock(&net_lock);
    }
    k->exposed = exposed();
    text = (char *)kalloc(DESCRIBE_MAX);
    if(text) {                                          /* wifi status's lines: the first, and the last failure's */
        wlan_describe(w, text, DESCRIBE_MAX);
        for(s = line_into(text, k->state_text, sizeof(k->state_text)); *s; s = line_into(s, NULL, 0)) {
            if(!strncmp(s, "last: ", 6)) {
                line_into(s + 6, k->last, sizeof(k->last));
                break;
            }
        }
        kfree(text);
    }
    return 0;
}

static int fill_networks(void *out) {
    struct netctl_bss_list *k = (struct netctl_bss_list *)out;
    struct wlan_dev *w = wlan_find(NULL);
    struct wlan_bss *list;
    u_long now = getticks(), age;
    u_int n, i;
    if(!w) return 0;
    list = (struct wlan_bss *)kalloc(NETCTL_BSS_MAX * sizeof(*list));
    if(!list) return -ENOMEM;
    n = wlan_bss_list(w, list, NETCTL_BSS_MAX);        /* the last minute's, strongest first */
    for(i = 0; i < n && i < NETCTL_BSS_MAX; i++) {
        struct netctl_bss *b = &k->bss[i];
        b->ssid_len = list[i].ssid_len <= NETCTL_SSID_MAX ? list[i].ssid_len : NETCTL_SSID_MAX;
        memcpy(b->ssid, list[i].ssid, b->ssid_len);
        memcpy(b->bssid, list[i].bssid, sizeof(b->bssid));
        b->security = list[i].security;
        b->channel = list[i].channel;
        b->band = list[i].band;
        b->rssi = list[i].rssi;
        b->hidden = list[i].hidden ? 1 : 0;
        age = (now - list[i].seen) / HZ;
        b->age_s = (u16)(age < 0xffff ? age : 0xffff);
    }
    k->count = i;
    kfree(list);
    return 0;
}

static int fill_saved(void *out) {
    struct netctl_saved *k = (struct netctl_saved *)out;
    struct netman_net *list = (struct netman_net *)kcalloc(NETCFG_NETS_MAX, sizeof(*list));
    int n, kept = 0, i;
    if(!list) return -ENOMEM;
    n = netman_networks(list, NETCFG_NETS_MAX, &kept);  /* everything but the keys, which it never gives */
    for(i = 0; i < n && i < NETCTL_SAVED_MAX; i++) {
        struct netctl_saved_net *s = &k->net[i];
        s->ssid_len = list[i].ssid_len <= NETCTL_SSID_MAX ? list[i].ssid_len : NETCTL_SSID_MAX;
        memcpy(s->ssid, list[i].ssid, s->ssid_len);
        s->security = list[i].security;
        s->hidden = list[i].hidden ? 1 : 0;
        s->refused = list[i].refused ? 1 : 0;
    }
    k->count = (u_int)i;
    k->kept = kept > 0 ? (u_int)kept : 0;
    kfree(list);
    return 0;
}

static int fill_settings(void *out) {
    struct netctl_settings *k = (struct netctl_settings *)out;
    struct netcfg_settings s;
    int i;
    netman_settings(&s);
    for(i = 0; i < NETCFG_NKEYS && i < NETCTL_KEYS_MAX; i++) {
        struct netctl_setting *e = &k->key[i];
        strlcpy(e->name, netcfg_key_name(i), sizeof(e->name));
        netcfg_value(&s, i, e->value, sizeof(e->value));
        strlcpy(e->values, netcfg_key_values(i), sizeof(e->values));
        e->set = (s.set >> i) & 1;
    }
    k->count = (u_int)i;
    k->off = netman_settings_off() ? 1 : 0;
    k->can_save = netman_can_save() ? 1 : 0;
    k->exposed = exposed();
    netman_where(k->where, sizeof(k->where));
    return 0;
}

static int fill_ifaces(void *out) {
    struct netctl_ifaces *k = (struct netctl_ifaces *)out;
    struct netdev *d;
    u_long now = getticks(), gone;
    u_int n = 0;
    dhcp_hostname(k->hostname, sizeof(k->hostname));
    mutex_lock(&net_lock);
    for(d = netdev_first(); d && n < NETCTL_IF_MAX; d = d->next, n++) {
        struct netctl_if *e = &k->iface[n];
        struct wlan_dev *w = wlan_find(d->name);
        strlcpy(e->name, d->name, sizeof(e->name));
        memcpy(e->mac, d->mac, sizeof(e->mac));
        e->link_up = d->link_up ? 1 : 0;
        e->wifi = w && w->netdev == d;
        e->ip = d->ip;
        e->netmask = d->netmask;
        e->gateway = d->gateway;
        e->dns = d->dns;
        if(d->ip && d->lease_s) {                       /* as ifconfig says it */
            gone = (now - d->lease_at) / HZ;
            e->dhcp_server = d->dhcp_server;
            e->lease_s = d->lease_s;
            e->lease_left = gone < d->lease_s ? d->lease_s - (u32)gone : 0;
        }
    }
    mutex_unlock(&net_lock);
    k->count = n;
    return 0;
}

static int fill_job(void *out) {
    mutex_lock(&job_lock);
    memcpy(out, &job, sizeof(job));
    mutex_unlock(&job_lock);
    return 0;
}

/* a read: size exactly the structure's, the caller's to write; filled in the kernel's copy and copied out whole */
static int read_op(void *arg, u32 size, u32 want, int (*fill)(void *out)) {
    void *k;
    int r;
    if(size != want) return -EINVAL;
    if(!user_buf(arg, size, true)) return -EFAULT;
    k = kcalloc(1, want);
    if(!k) return -ENOMEM;
    r = fill(k);
    if(!r && !user_buf(arg, size, true)) r = -EFAULT;
    if(!r) memcpy(arg, k, want);
    kfree(k);
    return r;
}

/**** jobs ****/

static const char *op_name(int op) {
    switch(op) {
    case NETCTL_CARD:   return req.on ? "card on" : "card off";
    case NETCTL_SCAN:   return "scan";
    case NETCTL_JOIN:   return "join";
    case NETCTL_LEAVE:  return "leave";
    case NETCTL_SAVE:   return "save";
    case NETCTL_FORGET: return "forget";
    case NETCTL_SET:    return "set";
    default:            return "?";
    }
}

static void job_stage(int stage) {
    mutex_lock(&job_lock);
    job.stage = stage;
    mutex_unlock(&job_lock);
}

/* the job ends with r (flags, why): its PMK wiped, the record done, a line in the kernel log */
static void job_end(int r, u32 flags, const char *why) {
    char name[4 * WLAN_SSID_MAX + 1], line[320], what[NETCTL_KEY_MAX + NETCTL_VALUE_MAX + 8];
    int op = req.op;
    wipe(req.pmk, sizeof(req.pmk));
    req.has_pmk = false;
    what[0] = 0;
    if(op == NETCTL_SET) snprintf(what, sizeof(what), " %s = %s", netcfg_key_name(req.key), req.value);
    else if(req.ssid_len) snprintf(what, sizeof(what), " \"%s\"", ieee80211_ssid_text(req.ssid, req.ssid_len, false, name,
                                                                                      sizeof(name)));
    snprintf(line, sizeof(line), "+NET: %s: %s%s: %s%s%s%s%s%s\n", job_who, op_name(op), what, r ? strerror(r) : "done",
             flags & NETCTL_DONE_ADDRESS ? ", an address" : "", flags & NETCTL_DONE_SAVED ? ", saved" : "",
             flags & NETCTL_DONE_NOT_SAVED ? ", not saved" : "", flags & NETCTL_DONE_NONE_NEAR ?
             ", no saved network in range" : "", flags & NETCTL_DONE_EXPOSED ?
             ", open while a server without a password runs" : "");
    if((int)strlen(line) >= (int)sizeof(line) - 1) line[sizeof(line) - 2] = '\n';
    mutex_lock(&job_lock);
    job.result = r;
    job.flags = flags;
    strlcpy(job.why, why ? why : "", sizeof(job.why));
    job.stage = 0;
    job.state = NETCTL_JOB_DONE;
    mutex_unlock(&job_lock);
    klog_write(line);
}

/* the address for the association the job made (join: its number at the station), as the network manager asks for
   one: up to DHCP_TRIES tries while the station keeps that association. True when wlan0 has one */
static bool address(struct wlan_dev *w, u_int join) {
    struct wlan_station_info si;
    struct netdev *nd = w->netdev;
    int i;
    for(i = 0; i < DHCP_TRIES; i++) {
        if(i) sleep_ms(DHCP_PAUSE_MS);
        if(wlan_station(w, &si, false) || si.state != WLAN_STA_UP || si.join_requests != join) return false;
        if(!nd->link_up) return false;
        if(nd->ip) return true;                         /* a dhcp typed meanwhile got one, or it kept its own */
        if(dhcp_configure_quiet(nd, DHCP_MS, true) == 0) return true;
    }
    return nd->ip != 0;
}

/* the network wlan0 is joined to, saved first in networks.conf: joined by this job (join: the number its join request
   got, which the association must still have, or another network's name would be saved with this key; key: its PMK,
   NULL for an open network), or by anyone (join NULL: the key the station keeps, as wifi save takes it). 0, or -errno
   and why */
static int save_joined(struct wlan_dev *w, const u8 *key, const u_int *join, bool hidden, char *why, size_t n) {
    struct wlan_station_info si;
    struct netcfg_net net;
    int had = -1, count = 0, r;
    memset(&net, 0, sizeof(net));
    if(wlan_station(w, &si, join == NULL) || (si.state != WLAN_STA_UP && si.state != WLAN_STA_RECONNECTING) ||
       !si.ssid_len) {
        wipe(&si, sizeof(si));
        snprintf(why, n, "%s is not joined to a network", w->name);
        return -ENOTCONN;
    }
    if(join && si.join_requests != *join) {
        wipe(&si, sizeof(si));
        snprintf(why, n, "%s was asked to join another network meanwhile", w->name);
        return -EAGAIN;
    }
    memcpy(net.ssid, si.ssid, si.ssid_len);
    net.ssid_len = si.ssid_len;
    net.hidden = si.hidden || hidden;
    if(si.security == WLAN_SEC_OPEN) {
        net.security = NETCFG_SEC_OPEN;
    } else if(key || si.has_key) {
        net.security = NETCFG_SEC_WPA2;
        memcpy(net.psk, key ? key : si.key, NETCFG_KEY_LEN);
        net.has_psk = true;
    } else {
        wipe(&si, sizeof(si));
        snprintf(why, n, "the station keeps no key for it: join it again, then save it");
        return -EACCES;
    }
    wipe(&si, sizeof(si));
    r = netman_save_network(&net, &had, &count, why, n);
    wipe(&net, sizeof(net));
    return r;
}

static int do_card(u32 *flags) {
    struct wlan_dev *w = req.w;
    struct wlan_station_info si;
    struct netcfg_settings s;
    struct netman_net *nets;
    int r, n;
    if(!req.on) return wlan_down_wait(w);
    if(!w->up && (r = wlan_up(w)) != 0) return r;
    netman_settings(&s);
    if(!s.wifi || netman_settings_off()) return 0;      /* joining by itself is off: the card alone */
    nets = (struct netman_net *)kcalloc(NETCFG_NETS_MAX, sizeof(*nets));
    if(!nets) return 0;
    n = netman_networks(nets, NETCFG_NETS_MAX, NULL);
    kfree(nets);
    if(n <= 0 || wlan_station(w, &si, false) || si.state == WLAN_STA_UP || si.state == WLAN_STA_JOINING ||
       si.state == WLAN_STA_RECONNECTING)
        return 0;
    job_stage(NETCTL_STAGE_CONNECTING);
    r = netman_wifi_connect_quiet(w);                   /* as wifi connect: the keeper looks by itself again */
    if(r == 0) *flags |= NETCTL_DONE_JOINED | (w->netdev->ip ? NETCTL_DONE_ADDRESS : 0);
    else if(r == -ENOENT) *flags |= NETCTL_DONE_NONE_NEAR;
    return 0;                                           /* the card runs: busy keepers and the like are no failure */
}

static int security_of(struct wlan_dev *w, const u8 *ssid, u_int len);

/* a join's name not heard in the last minute when it was asked: a scan first, as wifi join makes one, and then what
   join_checked() decides from what was heard, in why when it is a refusal; still not heard, a hidden network */
static int scan_first(struct wlan_dev *w, char *why, size_t n) {
    int r, security;
    job_stage(NETCTL_STAGE_SCANNING);
    r = wlan_scan(w, SCAN_MS);
    if(r && r != -ETIMEDOUT) return r;                  /* -ETIMEDOUT: what it heard until then is in the list */
    security = security_of(w, req.ssid, req.ssid_len);
    if(security == -ENOMEM) return -ENOMEM;
    job_stage(NETCTL_STAGE_JOINING);
    switch(security) {
    case -1:                                            /* hidden: the station probes for its name */
        req.hidden = true;
        return 0;
    case WLAN_SEC_OPEN:
        if(!req.has_pmk) return 0;
        snprintf(why, n, "%s", "it is an open network, which takes no password: the password was not used");
        return -EINVAL;
    case WLAN_SEC_WPA2_PSK:
    case WLAN_SEC_WPA2_WPA3:
        if(req.has_pmk) return 0;
        snprintf(why, n, "%s", "it is a WPA2 network: join it with its password");
        return -EACCES;
    case WLAN_SEC_WEP:
        snprintf(why, n, "%s", "it uses WEP, which SurfOS does not support (it is broken: the network should use WPA2)");
        return -ENOSYS;
    case WLAN_SEC_WPA3_SAE:
        snprintf(why, n, "%s", "it is WPA3-Personal only (SAE): not supported yet");
        return -ENOSYS;
    case WLAN_SEC_ENTERPRISE:
        snprintf(why, n, "%s", "it is an enterprise network (802.1X with a login): not supported");
        return -ENOSYS;
    default:
        snprintf(why, n, "%s", "it uses a security SurfOS does not support (WPA, TKIP, OWE or protected management "
                 "frames)");
        return -ENOSYS;
    }
}

static int do_join(u32 *flags, char *why, size_t n) {
    struct wlan_dev *w = req.w;
    u_int join = 0;
    int r = req.unheard ? scan_first(w, why, n) : 0;
    if(r) return r;
    if(!req.has_pmk && exposed()) {                     /* open, and a server without a password runs now */
        *flags |= NETCTL_DONE_EXPOSED;
        if(!req.exposed_ok) {
            snprintf(why, n, "%s", "a server without a password runs (telnetd or ftpd), and anyone on an open "
                     "network reaches it: not joined without the person's word");
            return -EPERM;
        }
    }
    r = wlan_join_unless(w, req.ssid, req.ssid_len, req.has_pmk ? req.pmk : NULL, req.stops, true, &join);
    if(r) return r;
    if(req.save) {
        job_stage(NETCTL_STAGE_SAVING);
        if(save_joined(w, req.has_pmk ? req.pmk : NULL, &join, req.hidden, why, n) == 0) *flags |= NETCTL_DONE_SAVED;
        else *flags |= NETCTL_DONE_NOT_SAVED;
    }
    wipe(req.pmk, sizeof(req.pmk));                     /* its last use */
    req.has_pmk = false;
    job_stage(NETCTL_STAGE_ADDRESS);
    if(address(w, join)) *flags |= NETCTL_DONE_ADDRESS;
    return 0;
}

static int do_set(u32 *flags, char *why, size_t n) {
    int r = netman_set(req.key, req.value, why, n);
    if(r < 0) return -EINVAL;
    if(r > 0) *flags |= NETCTL_DONE_NOT_SAVED;          /* in force (or not, as why says) but not on the stick */
    else why[0] = 0;
    return 0;
}

static void worker(void *arg) {
    char why[NETCTL_WHY_MAX];
    u32 flags = 0;
    int r, count = 0;
    (void)arg;
    why[0] = 0;
    switch(req.op) {
    case NETCTL_CARD:
        r = do_card(&flags);
        break;
    case NETCTL_SCAN:
        r = wlan_scan(req.w, SCAN_MS);
        if(r == -ETIMEDOUT) {                           /* what it heard until then is in the list */
            flags |= NETCTL_DONE_PARTIAL;
            r = 0;
        }
        break;
    case NETCTL_JOIN:
        r = do_join(&flags, why, sizeof(why));
        break;
    case NETCTL_LEAVE:
        r = wlan_leave_wait(req.w);
        break;
    case NETCTL_SAVE:
        r = save_joined(req.w, NULL, NULL, false, why, sizeof(why));
        break;
    case NETCTL_FORGET:
        r = netman_forget_network(req.ssid, req.ssid_len, &count, why, sizeof(why));
        break;
    case NETCTL_SET:
        r = do_set(&flags, why, sizeof(why));
        break;
    default:
        r = -EINVAL;
        break;
    }
    job_end(r, flags, why);
}

static int first_stage(int op, bool on) {
    switch(op) {
    case NETCTL_CARD:   return on ? NETCTL_STAGE_STARTING : NETCTL_STAGE_STOPPING;
    case NETCTL_SCAN:   return NETCTL_STAGE_SCANNING;
    case NETCTL_JOIN:   return NETCTL_STAGE_JOINING;
    case NETCTL_LEAVE:  return NETCTL_STAGE_LEAVING;
    case NETCTL_SAVE:   return NETCTL_STAGE_SAVING;
    default:            return NETCTL_STAGE_WRITING;
    }
}

static bool job_running(void) {
    bool running;
    mutex_lock(&job_lock);
    running = job.state == NETCTL_JOB_RUNNING;
    mutex_unlock(&job_lock);
    return running;
}

/* the job r asks for, unless one runs: its number; r (the caller's copy) is the caller's to wipe */
static int job_start(const struct job_req *r) {
    int seq;
    mutex_lock(&job_lock);
    if(job.state == NETCTL_JOB_RUNNING) {
        mutex_unlock(&job_lock);
        return -EBUSY;
    }
    req = *r;
    memset(&job, 0, sizeof(job));
    job_seq = job_seq >= SEQ_MAX ? 1 : job_seq + 1;
    job.seq = job_seq;
    job.op = r->op;
    job.state = NETCTL_JOB_RUNNING;
    job.stage = r->unheard ? NETCTL_STAGE_SCANNING : first_stage(r->op, r->on);
    job.ssid_len = r->ssid_len <= NETCTL_SSID_MAX ? r->ssid_len : NETCTL_SSID_MAX;
    memcpy(job.ssid, r->ssid, job.ssid_len);
    snprintf(job_who, sizeof(job_who), "%s (pid %lu)", curTask ? curTask->name : "?", curTask ? curTask->pid : 0UL);
    seq = (int)job.seq;
    mutex_unlock(&job_lock);
    if(!kthread_create_on("netctl", &conArray[0], worker, NULL, PL_NORMAL, TF_DETACHED)) {
        job_end(-ENOMEM, 0, "no memory for the job's thread");
        return -ENOMEM;
    }
    return seq;
}

/* the networks named so in the last scans: the security wifi join would use (shell/wifi.c's security_of(): WPA2 if
   any of them is, since an open network with the same name is no reason to send the password in the clear nor to go
   without it; else open if one is; else the strongest one's); -1 when none was heard, -ENOMEM */
static int security_of(struct wlan_dev *w, const u8 *ssid, u_int len) {
    struct wlan_bss *list = (struct wlan_bss *)kalloc(NETCTL_BSS_MAX * sizeof(*list));
    int best = -1, strongest = -1;
    u_int n, i;
    if(!list) return -ENOMEM;
    n = wlan_bss_list(w, list, NETCTL_BSS_MAX);        /* strongest first */
    for(i = 0; i < n; i++) {
        if(list[i].ssid_len != len || memcmp(list[i].ssid, ssid, len)) continue;
        if(strongest < 0) strongest = list[i].security;
        if(list[i].security == WLAN_SEC_WPA2_PSK || list[i].security == WLAN_SEC_WPA2_WPA3) {
            best = list[i].security;
            break;
        }
        if(list[i].security == WLAN_SEC_OPEN && best < 0) best = WLAN_SEC_OPEN;
    }
    kfree(list);
    return best >= 0 ? best : strongest;
}

/* a join's request, checked (k: the kernel's copy, its password wiped here once the PMK is made) */
static int join_checked(struct netctl_join *k, struct job_req *r) {
    struct wlan_station_info si;
    struct wlan_dev *w;
    size_t plen = strnlen(k->passphrase, NETCTL_PASS_MAX);
    int security;
    if(!k->ssid_len || k->ssid_len > NETCTL_SSID_MAX || (k->flags & ~(NETCTL_JOIN_SAVE | NETCTL_JOIN_EXPOSED)) ||
       plen >= NETCTL_PASS_MAX)
        return -EINVAL;
    w = wlan_find(NULL);
    if(!w || !w->up) return -ENODEV;                   /* none, or its card off (wifi up first) */
    if(job_running()) return -EBUSY;                    /* before the PBKDF2, which would be in vain */
    security = security_of(w, k->ssid, k->ssid_len);
    if(security == -ENOMEM) return -ENOMEM;
    if(security < 0) {                                  /* not heard in the last minute: the job looks first */
        r->unheard = true;
        security = plen ? WLAN_SEC_WPA2_PSK : WLAN_SEC_OPEN;   /* as it is joined when still not heard (hidden) */
    }
    if(security == WLAN_SEC_OPEN && plen) return -EINVAL;  /* a password for an open network: a mistake, not sent */
    if(security == WLAN_SEC_OPEN && exposed() && !(k->flags & NETCTL_JOIN_EXPOSED))
        return -EPERM;                                  /* a server without a password runs: the person is to be told */
    if(security != WLAN_SEC_OPEN && security != WLAN_SEC_WPA2_PSK && security != WLAN_SEC_WPA2_WPA3) return -ENOSYS;
    if(security != WLAN_SEC_OPEN) {
        if(!plen) return -EACCES;
        if(!wpa_passphrase_to_pmk) return -ENOSYS;
        if(wpa_passphrase_to_pmk(k->passphrase, k->ssid, k->ssid_len, r->pmk)) return -EINVAL;  /* 8-63 printable */
        r->has_pmk = true;
    }
    wipe(k->passphrase, sizeof(k->passphrase));
    if(wlan_station(w, &si, false)) return -ENODEV;
    r->op = NETCTL_JOIN;
    r->w = w;
    r->stops = si.stops;                                /* a wifi leave or down from now on calls the join off */
    r->save = (k->flags & NETCTL_JOIN_SAVE) != 0;
    r->exposed_ok = (k->flags & NETCTL_JOIN_EXPOSED) != 0;
    r->ssid_len = k->ssid_len;
    memcpy(r->ssid, k->ssid, k->ssid_len);
    return job_start(r);
}

static int start_join(void *arg, u32 size) {
    struct netctl_join *u = (struct netctl_join *)arg, k;
    struct job_req r;
    int rc;
    if(size != sizeof(k)) return -EINVAL;
    if(!user_buf(arg, size, true)) return -EFAULT;     /* written: its password is wiped */
    memcpy(&k, u, sizeof(k));
    wipe(u->passphrase, sizeof(u->passphrase));         /* the program's copy goes whatever the answer */
    memset(&r, 0, sizeof(r));
    rc = join_checked(&k, &r);
    wipe(&k, sizeof(k));
    wipe(&r, sizeof(r));
    return rc;
}

/* NETCTL_APPLY: a step of the desktop's theme or pointer speed in force now (netman_apply()), the stick not written: only
   those two, which act at once and which a person steps through (the radio groups' arrows, held down) */
static int apply_op(void *arg, u32 size) {
    struct netctl_req k;
    char why[NETCTL_WHY_MAX];
    int key, r;
    if(size != sizeof(k)) return -EINVAL;
    if(!user_buf(arg, size, false)) return -EFAULT;
    memcpy(&k, arg, sizeof(k));
    if(strnlen(k.key, sizeof(k.key)) >= sizeof(k.key) || strnlen(k.value, sizeof(k.value)) >= sizeof(k.value))
        return -EINVAL;
    key = netcfg_key_find(k.key);
    if(key != NETCFG_THEME && key != NETCFG_POINTER) return -EINVAL;
    r = netman_apply(key, k.value, why, sizeof(why));
    return r < 0 ? -EINVAL : r;
}

/* card, scan, leave, save, forget, set: the request checked into r */
static int start_other(int op, void *arg, u32 size) {
    struct netctl_req k;
    struct netcfg_settings s;
    struct job_req r;
    struct wlan_dev *w = wlan_find(NULL);
    char why[NETCTL_WHY_MAX];
    int rc;
    memset(&r, 0, sizeof(r));
    r.op = op;
    r.w = w;
    if(op == NETCTL_SCAN || op == NETCTL_LEAVE || op == NETCTL_SAVE) {
        if(size) return -EINVAL;                        /* no argument: nothing is read */
        if(!w || !w->up) return -ENODEV;
        return job_start(&r);
    }
    if(size != sizeof(k)) return -EINVAL;
    if(!user_buf(arg, size, false)) return -EFAULT;
    memcpy(&k, arg, sizeof(k));                         /* checked here, as it is now: the program may change its own */
    switch(op) {
    case NETCTL_CARD:
        if(k.on > 1) return -EINVAL;
        if(!w) return -ENODEV;
        r.on = k.on != 0;
        break;
    case NETCTL_FORGET:
        if(!k.ssid_len || k.ssid_len > NETCTL_SSID_MAX) return -EINVAL;
        r.ssid_len = k.ssid_len;
        memcpy(r.ssid, k.ssid, k.ssid_len);
        break;
    default:                                            /* NETCTL_SET: a key of the table and a value it takes */
        if(strnlen(k.key, sizeof(k.key)) >= sizeof(k.key) || strnlen(k.value, sizeof(k.value)) >= sizeof(k.value))
            return -EINVAL;
        r.key = netcfg_key_find(k.key);
        netman_settings(&s);
        if(r.key < 0 || !k.value[0] || netcfg_set(&s, r.key, k.value, why, sizeof(why))) return -EINVAL;
        strlcpy(r.value, k.value, sizeof(r.value));
        break;
    }
    rc = job_start(&r);
    return rc;
}

int sys_netctl(int op, void *arg, u32 size) {
    switch(op) {
    case NETCTL_WIFI:     return read_op(arg, size, sizeof(struct netctl_wifi), fill_wifi);
    case NETCTL_NETWORKS: return read_op(arg, size, sizeof(struct netctl_bss_list), fill_networks);
    case NETCTL_SAVED:    return read_op(arg, size, sizeof(struct netctl_saved), fill_saved);
    case NETCTL_SETTINGS: return read_op(arg, size, sizeof(struct netctl_settings), fill_settings);
    case NETCTL_IFACES:   return read_op(arg, size, sizeof(struct netctl_ifaces), fill_ifaces);
    case NETCTL_JOB:      return read_op(arg, size, sizeof(struct netctl_job), fill_job);
    case NETCTL_APPLY:    return apply_op(arg, size);
    case NETCTL_JOIN:     return start_join(arg, size);
    case NETCTL_CARD:
    case NETCTL_SCAN:
    case NETCTL_LEAVE:
    case NETCTL_SAVE:
    case NETCTL_FORGET:
    case NETCTL_SET:      return start_other(op, arg, size);
    default:              return -EINVAL;
    }
}
