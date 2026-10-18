/*
SurfOS Network Stack: DHCP client
--------------------
File: dhcp.c    Date: 10/3/26 (roadmap N2)
--------------------
DISCOVER, OFFER, REQUEST, ACK over a UDP socket on port 68; the lease is
applied to the interface. No renewal yet: the lease time is only reported.
10/9/26 (Wi-Fi, track S): the socket is bound to the interface (udp_bind_dev()): `dhcp wlan0`
asks through wlan0 and hears only wlan0's answers, as `dhcp eth0` does through eth0.
10/10/26: one dhcp at a time, there being one port 68: a second one (dhcp wlan0 typed while
dhcp runs on eth0, say) waits for the first within its own time limit, saying so, and Ctrl+C
ends the wait; before, it failed at once ("port 68 is busy").
10/17/26 (the network manager, kernel/netman.c): each interface keeps its lease (struct netdev:
the server, the length, when it was granted), and dhcp_renew() keeps the lease going: a
DHCPREQUEST with the address in ciaddr, to the server that granted it (RENEWING) or to anyone
(REBINDING), as RFC 2131 section 4.4.5 has it; a NAK gives the address up (dhcp_drop()). When to
renew is the manager's business (netcfg_lease_step(), net/netcfg.c); a renewal and a drop touch only
the lease they were asked about, never an address set by hand or a lease got meanwhile. When an
Ethernet link comes back, the manager has the address confirmed (DHCP_REBOOTING: the address
asked for, RFC 2131 4.3.2), and a NAK drops it. Requests carry the host
name of the settings (option 12), so a router can list SurfOS by name. The kernel word
dhcplease=<seconds> shortens every lease to that, for the tests: a real lease is a day or more.
An address set by hand (ifconfig) has no lease, and nothing renews it. A dhcp waiting for another
one now waits up to 30 s and counts its own time limit from when it has port 68, so a request
made while the manager asks in the background still gets all its tries; the manager's waits and
failures are said in the log only.
*/

#include <stdarg.h>
#include <surfos/types.h>
#include <surfos/console.h>
#include <surfos/timer.h>
#include <surfos/sync.h>
#include <surfos/task.h>
#include <surfos/irq.h>
#include <surfos/multiboot.h>
#include <surfos/klog.h>
#include <mm/kalloc.h>
#include <net/net.h>
#include <net/eth.h>
#include <net/ip.h>
#include <net/dhcp.h>
#include <blibc_common.h>

#define DHCP_SERVER_PORT 67
#define DHCP_CLIENT_PORT 68
#define DHCP_DISCOVER 1
#define DHCP_OFFER    2
#define DHCP_REQUEST  3
#define DHCP_ACK      5
#define DHCP_NAK      6
#define OPT_SUBNET    1
#define OPT_ROUTER    3
#define OPT_DNS       6
#define OPT_HOSTNAME  12
#define OPT_REQ_IP    50
#define OPT_LEASE     51
#define OPT_MSG_TYPE  53
#define OPT_SERVER_ID 54
#define OPT_PARAMS    55
#define OPT_END       255

struct dhcp_msg {
    u8 op, htype, hlen, hops;
    u32 xid;
    u16 secs, flags;
    ipaddr_t ciaddr, yiaddr, siaddr, giaddr;
    u8 chaddr[16], sname[64], file[128];
    u8 cookie[4];
    u8 options[312];
} __attribute__((packed));

#define HOSTNAME_MAX  32

static u_long lease_seconds;
static mutex_t dhcp_lock = MUTEX_INIT("dhcp");  /* 10/10/26: one dhcp at a time */
static struct netdev *dhcp_dev;                 /* whose dhcp holds it, to say so */
static char hostname[HOSTNAME_MAX + 1];         /* 10/17/26: option 12; "" sends none (under irq_save) */

u_long dhcp_lease_seconds(void) {
    return lease_seconds;
}

/* 10/17/26 (dhcp.h): letters, digits and '-' only, as a host name may have them; anything else sends none */
void dhcp_set_hostname(const char *name) {
    char v[HOSTNAME_MAX + 1];
    u_long flags;
    size_t i, n = name ? strlen(name) : 0;
    if(n > HOSTNAME_MAX) n = 0;
    for(i = 0; i < n; i++) {
        char c = name[i];
        if(!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-')) n = 0;
    }
    memcpy(v, name ? name : "", n);
    v[n] = 0;
    flags = irq_save();
    memcpy(hostname, v, n + 1);
    irq_restore(flags);
}

/* 10/17/26: ciaddr, the address being renewed (0 for a new lease: then the answer is asked to come broadcast) */
static u_int build(struct dhcp_msg *m, struct netdev *dev, u32 xid, u8 type, ipaddr_t req, ipaddr_t server,
                   ipaddr_t ciaddr) {
    u8 *o = m->options;
    char name[HOSTNAME_MAX + 1];
    u_long flags = irq_save();
    size_t n;
    memcpy(name, hostname, sizeof(name));
    irq_restore(flags);
    n = strlen(name);
    memset(m, 0, sizeof(*m));
    m->op = 1;
    m->htype = 1;
    m->hlen = ETH_ALEN;
    m->xid = xid;
    m->ciaddr = ciaddr;
    m->flags = ciaddr ? 0 : htons(0x8000);         /* please broadcast the reply (a renewal's comes to ciaddr) */
    memcpy(m->chaddr, dev->mac, ETH_ALEN);
    m->cookie[0] = 99; m->cookie[1] = 130; m->cookie[2] = 83; m->cookie[3] = 99;
    *o++ = OPT_MSG_TYPE; *o++ = 1; *o++ = type;
    if(req) { *o++ = OPT_REQ_IP; *o++ = 4; memcpy(o, &req, 4); o += 4; }
    if(server) { *o++ = OPT_SERVER_ID; *o++ = 4; memcpy(o, &server, 4); o += 4; }
    *o++ = OPT_PARAMS; *o++ = 4; *o++ = OPT_SUBNET; *o++ = OPT_ROUTER; *o++ = OPT_DNS; *o++ = OPT_LEASE;
    *o++ = 61; *o++ = 7; *o++ = 1; memcpy(o, dev->mac, ETH_ALEN); o += ETH_ALEN;   /* client identifier */
    if(n) { *o++ = OPT_HOSTNAME; *o++ = (u8)n; memcpy(o, name, n); o += n; }
    *o++ = OPT_END;
    return (u_int)(o - (u8 *)m) < 300 ? 300 : (u_int)(o - (u8 *)m);
}

/* the option, or NULL; *len gets its length */
static const u8 *option(const struct dhcp_msg *m, u_int size, u8 code, u_int *len) {
    const u8 *o = m->options, *end = (const u8 *)m + size;
    while(o + 2 <= end && *o != OPT_END) {
        if(*o == 0) { o++; continue; }
        if(o + 2 + o[1] > end) break;
        if(*o == code) { *len = o[1]; return o + 2; }
        o += 2 + o[1];
    }
    return NULL;
}

/* wait for a reply of the given type to our xid */
static int expect(struct udp_socket *s, u32 xid, u8 type, struct dhcp_msg *m, u_long deadline) {
    for(;;) {
        u_long now = getticks();
        const u8 *t;
        u_int n, l;
        int r;
        if((long)(now - deadline) >= 0) return -1;     /* 10/17/26: as ticks wrap */
        r = udp_recvfrom(s, m, sizeof(*m), NULL, NULL, (deadline - now) * (1000 / HZ));
        if(r < 0) return -1;
        n = (u_int)r;
        if(n < 240 || m->op != 2 || m->xid != xid) continue;
        if(memcmp(m->cookie, "\x63\x82\x53\x63", 4)) continue;
        t = option(m, n, OPT_MSG_TYPE, &l);
        if(!t || l != 1) continue;
        if(*t == type) return (int)n;
        if(*t == DHCP_NAK) return -2;
    }
}

/* a line on the screen and in the log, or (quiet: the network manager's background work) in the log only */
static void say(bool quiet, const char *fmt, ...) {
    char line[96];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    if(quiet) klog_write(line);
    else kprintf("%s", line);
}

/* 10/10/26: dhcp_lock for dev, waited for until the deadline or a Ctrl+C; 0, or -1 when not had. 10/17/26: the
   callers wait up to LOCK_WAIT_MS for it and count their own time from when they have it, so a dhcp waiting for
   another one (the network manager's in the background, say) still gets its tries */
#define LOCK_WAIT_MS 30000
static int one_at_a_time(struct netdev *dev, bool quiet) {
    u_long deadline = getticks() + LOCK_WAIT_MS / (1000 / HZ);
    bool said = false;
    while(!mutex_trylock(&dhcp_lock)) {
        struct netdev *busy = dhcp_dev;
        if(!said) say(quiet, "dhcp: %s: waiting for the dhcp on %s\n", dev->name, busy ? busy->name : "another interface");
        said = true;
        if((long)(getticks() - deadline) >= 0 || sleep_intr(50)) {
            say(quiet, "dhcp: %s: not tried: another dhcp was still running\n", dev->name);
            return -1;
        }
    }
    dhcp_dev = dev;
    return 0;
}

static void done(void) {
    dhcp_dev = NULL;
    mutex_unlock(&dhcp_lock);
}

/* 10/17/26: dhcplease=<seconds> on the command line: every lease counts as no longer than that (the tests); 0: as given */
static u32 lease_cap(void) {
    char v[12];
    u_long n = 0;
    const char *p;
    if(!bootinfo_arg(&bootinfo, "dhcplease=", v, sizeof(v))) return 0;
    for(p = v; *p >= '0' && *p <= '9' && n < 100000000; p++) n = n * 10 + (u_long)(*p - '0');
    return *p ? 0 : (u32)n;
}

/* the ACK (n bytes) in m: its address, netmask, gateway, DNS server and lease onto dev; the lease's length is said
   in *secs. server: who granted it, when the ACK does not say; announce: a gratuitous ARP for the address (a new
   lease; a renewal keeps the address it had). 10/17/26: for a renewal (keep), only while dev still has the lease
   at0 of address ip0 (an ifconfig, a forgotten address or a new lease meanwhile wins): false when it did not */
static bool take_lease(struct netdev *dev, const struct dhcp_msg *m, u_int n, ipaddr_t server, u32 *secs, bool announce,
                       bool keep, ipaddr_t ip0, u_long at0) {
    ipaddr_t mask = 0, gw = 0, dns = 0, id = 0;
    u32 lease = 0, cap = lease_cap();
    const u8 *o;
    u_int l;
    if((o = option(m, n, OPT_SUBNET, &l)) != NULL && l == 4) memcpy(&mask, o, 4);
    if((o = option(m, n, OPT_ROUTER, &l)) != NULL && l >= 4) memcpy(&gw, o, 4);
    if((o = option(m, n, OPT_DNS, &l)) != NULL && l >= 4) memcpy(&dns, o, 4);
    if((o = option(m, n, OPT_LEASE, &l)) != NULL && l == 4) { memcpy(&lease, o, 4); lease = ntohl(lease); }
    if((o = option(m, n, OPT_SERVER_ID, &l)) != NULL && l == 4) memcpy(&id, o, 4);
    lease_seconds = lease;
    *secs = lease;
    if(lease == 0xFFFFFFFF) lease = 0;              /* for ever: nothing to renew */
    if(cap && (!lease || lease > cap)) lease = cap;
    mutex_lock(&net_lock);
    if(keep && (dev->ip != ip0 || dev->lease_at != at0 || !dev->lease_s)) {
        mutex_unlock(&net_lock);
        return false;
    }
    dev->ip = m->yiaddr;
    dev->netmask = mask ? mask : IP4(255, 255, 255, 0);
    dev->gateway = gw;
    dev->dns = dns;
    dev->dhcp_server = id ? id : server;
    dev->lease_s = lease;
    dev->lease_at = getticks();
    if(announce) arp_announce(dev);
    mutex_unlock(&net_lock);
    return true;
}

int dhcp_configure(struct netdev *dev, u_long timeout_ms) {
    return dhcp_configure_quiet(dev, timeout_ms, false);
}

int dhcp_configure_quiet(struct netdev *dev, u_long timeout_ms, bool quiet) {
    struct udp_socket *s;
    struct dhcp_msg *m;
    u32 xid = (u32)getticks() * 2654435761UL ^ 0x5f0f1e2d, secs;
    u_long deadline;
    ipaddr_t offered = 0, server = 0;
    const u8 *o;
    u_int l, n;
    int attempt, r = -1;
    char a[16], b[16];

    if(one_at_a_time(dev, quiet)) return -1;
    deadline = getticks() + timeout_ms / (1000 / HZ);
    s = udp_open(DHCP_CLIENT_PORT);
    if(!s) { done(); kprintf("dhcp: port 68 is busy\n"); return -1; }
    udp_bind_dev(s, dev);
    m = (struct dhcp_msg *)kalloc(sizeof(*m));
    if(!m) { udp_close(s); done(); return -1; }
    for(attempt = 0; attempt < 3 && (long)(getticks() - deadline) < 0; attempt++) {
        n = build(m, dev, xid, DHCP_DISCOVER, 0, 0, 0);
        if(udp_sendto(s, IP_BROADCAST, DHCP_SERVER_PORT, m, n) != 0) break;
        {
            u_long step = getticks() + 2 * HZ;
            r = expect(s, xid, DHCP_OFFER, m, (long)(step - deadline) < 0 ? step : deadline);
        }
        if(r <= 0) continue;
        offered = m->yiaddr;
        o = option(m, (u_int)r, OPT_SERVER_ID, &l);
        if(o && l == 4) memcpy(&server, o, 4);
        n = build(m, dev, xid, DHCP_REQUEST, offered, server, 0);
        if(udp_sendto(s, IP_BROADCAST, DHCP_SERVER_PORT, m, n) != 0) break;
        {
            u_long step = getticks() + 2 * HZ;
            r = expect(s, xid, DHCP_ACK, m, (long)(step - deadline) < 0 ? step : deadline);
        }
        if(r > 0) break;
    }
    if(r > 0) {
        take_lease(dev, m, (u_int)r, server, &secs, true, false, 0, 0);
        kprintf("dhcp: %s: lease %s for %lu s from %s\n", dev->name, ipfmt(dev->ip, a), (u_long)secs, ipfmt(server, b));
        r = 0;
    } else {
        if(quiet) {                                 /* 10/17/26 */
            char line[64];
            snprintf(line, sizeof(line), "dhcp: %s: no %s\n", dev->name, offered ? "acknowledgement" : "offer");
            klog_write(line);
        } else {
            kprintf("dhcp: %s: no %s\n", dev->name, offered ? "acknowledgement" : "offer");
        }
        r = -1;
    }
    kfree(m);
    udp_close(s);
    done();
    return r;
}

/* 10/17/26: the address of the lease at0 goes, unless dev has another by now (0: whatever it has) */
static void drop_if(struct netdev *dev, const char *why, ipaddr_t ip0, u_long at0, bool check) {
    char a[16];
    mutex_lock(&net_lock);
    if(check && (dev->ip != ip0 || dev->lease_at != at0 || !dev->lease_s)) {
        mutex_unlock(&net_lock);
        return;
    }
    if(dev->ip) kprintf("dhcp: %s: address %s given up (%s)\n", dev->name, ipfmt(dev->ip, a), why);
    dev->ip = dev->netmask = dev->gateway = dev->dns = 0;
    dev->dhcp_server = 0;
    dev->lease_s = 0;
    arp_flush(dev);
    mutex_unlock(&net_lock);
}

/* 10/17/26 (dhcp.h): two tries, each waiting 2 s at most for the answer */
int dhcp_renew(struct netdev *dev, int how, u_long timeout_ms, u_long lease_at, bool quiet) {
    struct udp_socket *s;
    struct dhcp_msg *m;
    u32 xid = (u32)getticks() * 2654435761UL ^ 0x7e1ea5ed, secs;
    u_long deadline, at;
    ipaddr_t ip, server;
    u_int n;
    int attempt, r = -1;
    char a[16];

    if(one_at_a_time(dev, quiet)) return -1;
    deadline = getticks() + timeout_ms / (1000 / HZ);
    mutex_lock(&net_lock);                         /* the lease as it is once the lock is had: the one meant? */
    ip = dev->ip;
    server = dev->dhcp_server;
    at = dev->lease_at;
    if(!dev->lease_s || (lease_at && at != lease_at)) ip = 0;
    mutex_unlock(&net_lock);
    if(!ip || (how == DHCP_RENEWING && !server)) { done(); return -1; }
    s = udp_open(DHCP_CLIENT_PORT);
    if(!s) { done(); kprintf("dhcp: port 68 is busy\n"); return -1; }
    udp_bind_dev(s, dev);
    m = (struct dhcp_msg *)kalloc(sizeof(*m));
    if(!m) { udp_close(s); done(); return -1; }
    for(attempt = 0; attempt < 2 && (long)(getticks() - deadline) < 0 && r != -2; attempt++) {
        /* renewing and rebinding: no requested address, no server, ciaddr says it; rebooting (a link back): the
           address asked for in option 50, ciaddr 0 (RFC 2131 4.3.2) */
        n = how == DHCP_REBOOTING ? build(m, dev, xid, DHCP_REQUEST, ip, 0, 0) : build(m, dev, xid, DHCP_REQUEST, 0, 0, ip);
        if(udp_sendto(s, how == DHCP_RENEWING ? server : IP_BROADCAST, DHCP_SERVER_PORT, m, n) != 0) break;
        {
            u_long step = getticks() + 2 * HZ;
            r = expect(s, xid, DHCP_ACK, m, (long)(step - deadline) < 0 ? step : deadline);
        }
        if(r > 0) break;
    }
    if(r > 0 && m->yiaddr == ip) {
        if(take_lease(dev, m, (u_int)r, server, &secs, false, true, ip, at))
            kprintf("dhcp: %s: lease %s %s for %lu s\n", dev->name, ipfmt(ip, a),
                    how == DHCP_REBOOTING ? "still good" : "renewed", (u_long)secs);
        r = 0;
    } else if(r > 0 || r == -2) {
        kprintf("dhcp: %s: the server would not %s %s\n", dev->name, how == DHCP_REBOOTING ? "confirm" : "renew",
                ipfmt(ip, a));
        drop_if(dev, how == DHCP_REBOOTING ? "not this network's" : "the DHCP server would not renew it", ip, at, true);
        r = -2;
    } else {
        r = -1;
    }
    kfree(m);
    udp_close(s);
    done();
    return r;
}

void dhcp_drop(struct netdev *dev, const char *why, u_long lease_at) {
    ipaddr_t ip;
    mutex_lock(&net_lock);
    ip = dev->ip;
    mutex_unlock(&net_lock);
    drop_if(dev, why, ip, lease_at, lease_at != 0);
}
