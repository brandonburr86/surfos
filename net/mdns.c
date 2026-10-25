/*
SurfOS Network Stack: the multicast DNS responder
--------------------
File: mdns.c    Date: 10/25/26 (the network tools)
--------------------
<name>.local answered on the local network (include/net/mdns.h; the messages are mdns_wire.c).
mdns on [name] opens a UDP socket on port 5353 and starts a kernel thread with it. Each
interface with an address and a link is asked to take the group 224.0.0.251
(NETDEV_ADD_MULTICAST: the NIC's multicast filter; a device without the call, wlan0, passes
multicast as it is), and for each, and again when its address changes, the thread probes for
the name (three queries 250 ms apart, the first after a random 0 to 250 ms, RFC 6762 section
8.1): an answer from another host for it is a conflict, and the thread takes the next name
(name-2, name-3, ...) and probes again on every interface. Another host's probe for the name
while ours go out is a tie (section 8.2): when its records are the later, the thread probes
again a second later, and that host, which has the name by then, answers; when ours are, it
goes on. Fifteen conflicts in a row, each less than 10 s after the last, make every probe
after them wait 5 s (section 8.1). Then it announces the name twice, a second apart (section
8.3). A query that asks for it is answered from the interface it came in on, with that
interface's address: to the group, or to the querier alone when it asked so, sent the query
to our address (section 5.5) or is a one-shot querier; to the group our record goes once a
second at most on an interface, an answer to another host's probe 250 ms after the last
(section 6). A conflict after that (another host announcing the name) sends every interface
back to probing (section 9), where the name may be lost. mdns off says goodbye where the name
was announced (the record with a TTL of 0, section 10.1), so other computers forget it at once.

What is looked at (sections 5.5, 6, 11 and 18): messages from the local link (to the group,
or to our address from the interface's own subnet) with opcode and response code 0; a
response only from port 5353, and while probing only once a probe for the name has gone out
(the random wait, a tie's or a hold's: the probes to come hear the host that has the name);
never one from our own addresses (with Ethernet and Wi-Fi on one network each interface hears
the other's). Everything sent goes with an IP TTL of 255 (section 11).

The thread waits on its socket at most PROBE_MS at a time, so mdns off ends it within that
and a quarter second; its lines go to the kernel log (+MDNS:). Nothing starts it but the
command: without it nothing here runs, and the boot is as it was.
*/

#include <surfos/types.h>
#include <surfos/console.h>
#include <surfos/kernel.h>
#include <surfos/task.h>
#include <surfos/klog.h>
#include <surfos/irq.h>
#include <net/net.h>
#include <net/ip.h>
#include <net/mdns.h>
#include <fs/vfs.h>
#include <crypto/crypto.h>
#include <blibc_common.h>

#define PROBE_MS      250               /* between probes */
#define PROBES        3
#define ANNOUNCE_MS   1000              /* between announcements */
#define ANNOUNCES     2
#define RATE_MS       1000              /* our record to the group at most this often on an interface, ... */
#define PROBE_RATE_MS 250               /* ... an answer to another host's probe this often (section 6) */
#define IFS           4                 /* interfaces followed */
#define SUFFIX_MAX    99                /* name-2 .. name-99, then the responder gives up */
#define TIE_MS        1000              /* a tie lost: probing again this much later (section 8.2) */
#define CONFLICTS     15                /* conflicts in a row, each CONFLICT_GAP or less after the last, ... */
#define CONFLICT_GAP  10000
#define HOLD_MS       5000              /* ... and each probe after them waits this long (section 8.1) */
#define STOP_MS       2000              /* mdns_stop() waits this long for the thread */

enum { P_IDLE, P_PROBE, P_ANNOUNCE, P_DONE };

struct ifs {
    struct netdev *dev;
    ipaddr_t ip;                        /* the address the name was probed and announced for */
    int phase, count;                   /* count: the probes or announcements of this phase sent */
    u_long at;                          /* uptime_ms() of the next probe or announcement */
    u_long last_mc;                     /* our record last sent to the group here */
    bool joined;                        /* the group asked of its NIC */
};

static struct {
    volatile bool running, stop;
    char base[MDNS_NAME_MAX + 1];       /* the name asked for */
    char name[MDNS_NAME_MAX + 1];       /* the one answered for now (base, or base-N after a conflict) */
    int suffix;
    int conflicts;                      /* in a row (see CONFLICTS) */
    u_long last_conflict;
    struct ifs ifs[IFS];
} st;

static u8 msg[1500], out[MDNS_MSG_MAX];

static void mlog(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void mlog(const char *fmt, ...) {
    char buf[200];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    klog_write(buf);
}

/* every interface with an address back to probing (a new name, or the name contested), from at on */
static void probe_all(u_long at) {
    int i;
    for(i = 0; i < IFS; i++) {
        if(!st.ifs[i].dev || st.ifs[i].phase == P_IDLE) continue;
        st.ifs[i].phase = P_PROBE;
        st.ifs[i].count = 0;
        st.ifs[i].at = at;
    }
}

/* a conflict, counted: after CONFLICTS in a row, each probe waits HOLD_MS (section 8.1: a host with a fault, ours or
   another's, floods nobody). How long the next probes wait */
static u_long conflict_hold(u_long now) {
    if(now - st.last_conflict >= CONFLICT_GAP) st.conflicts = 0;
    st.last_conflict = now;
    if(++st.conflicts == CONFLICTS)
        mlog("+MDNS: %d conflicts in a row: each probe from now on waits %d s\n", CONFLICTS, HOLD_MS / 1000);
    return st.conflicts >= CONFLICTS ? HOLD_MS : 0;
}

/* the interfaces with an address and a link, followed; a new address probes again */
static void follow(u_long now) {
    struct netdev *d;
    char a[16];
    int i;
    for(d = netdev_first(); d; d = d->next) {
        struct ifs *f = NULL;
        for(i = 0; i < IFS; i++) if(st.ifs[i].dev == d) f = &st.ifs[i];
        if(!f) {
            if(!d->ip || !d->link_up) continue;
            for(i = 0; i < IFS && st.ifs[i].dev; i++);
            if(i == IFS) continue;
            f = &st.ifs[i];
            f->dev = d;
        }
        if(!f->joined) {
            static const u8 group[ETH_ALEN] = { 0x01, 0x00, 0x5e, 0x00, 0x00, 0xfb };
            f->joined = true;
            if(d->ops->ioctl && d->ops->ioctl(d, NETDEV_ADD_MULTICAST, (void *)group) < 0)
                mlog("+MDNS: %s: its NIC took no multicast address: queries to the group may not arrive\n", d->name);
        }
        if(d->ip && d->link_up && d->ip != f->ip) {
            u16 r;
            crypto_random(&r, sizeof(r));
            f->ip = d->ip;
            f->phase = P_PROBE;
            f->count = 0;
            f->at = now + r % PROBE_MS;                 /* section 8.1: the first probe after 0 to 250 ms */
            mlog("+MDNS: %s: %s; probing for %s.local\n", d->name, ipfmt(d->ip, a), st.name);
        } else if((!d->ip || !d->link_up) && f->phase != P_IDLE) {
            f->phase = P_IDLE;
            f->ip = 0;
        }
    }
}

/* The probes and announcements that are due: three probes PROBE_MS apart, then (no answer having come) the
   announcements, the first a PROBE_MS after the last probe. The time until the next one, PROBE_MS at most */
static u_long due(struct udp_socket *s, u_long now) {
    u_long wait = PROBE_MS;
    int i, n;
    for(i = 0; i < IFS; i++) {
        struct ifs *f = &st.ifs[i];
        if(!f->dev || f->phase == P_IDLE) continue;
        while(f->phase != P_DONE && (long)(now - f->at) >= 0) {
            if(f->phase == P_PROBE && f->count == PROBES) {
                f->phase = P_ANNOUNCE;
                f->count = 0;
                continue;
            }
            if(f->phase == P_ANNOUNCE && f->count == ANNOUNCES) {
                f->phase = P_DONE;
                break;
            }
            if(f->phase == P_PROBE) n = mdns_probe(st.name, f->ip, out, sizeof(out));
            else n = mdns_announce(st.name, f->ip, MDNS_TTL, out, sizeof(out));
            if(n > 0) udp_sendto_dev(s, f->dev, MDNS_GROUP, MDNS_PORT, out, (u_int)n);
            if(f->phase == P_ANNOUNCE) {
                f->last_mc = now;
                if(!f->count) {
                    char a[16];
                    mlog("+MDNS: %s: answering for %s.local (%s)\n", f->dev->name, st.name, ipfmt(f->ip, a));
                }
            }
            f->count++;
            f->at = now + (f->phase == P_PROBE ? PROBE_MS : ANNOUNCE_MS);
        }
        if(f->phase != P_DONE && f->at - now < wait) wait = f->at - now;
    }
    return wait;
}

/* is a one of our interfaces' addresses? (an interface of ours on the same network hears the other's messages) */
static bool ours(ipaddr_t a) {
    struct netdev *d;
    for(d = netdev_first(); d; d = d->next) if(d->ip && d->ip == a) return true;
    return false;
}

/* section 11: from the local link: to the group (no router passes it on), or to our address from the interface's
   own subnet. A message from farther away is no one's on this network (sections 5.5 and 11) */
static bool on_link(const struct netdev *dev, ipaddr_t src, ipaddr_t dst) {
    if(dst == MDNS_GROUP) return true;
    return dev->netmask && (src & dev->netmask) == (dev->ip & dev->netmask);
}

/* another host has the name: the next one, probed for on every interface. false when there is none left */
static bool rename_next(u_long now) {
    if(st.suffix >= SUFFIX_MAX) return false;
    st.suffix = st.suffix ? st.suffix + 1 : 2;
    snprintf(st.name, sizeof(st.name), "%.*s-%d", MDNS_NAME_MAX - 3, st.base, st.suffix);
    mlog("+MDNS: another host answers for that name: probing for %s.local instead\n", st.name);
    probe_all(now + conflict_hold(now));
    return true;
}

/* the end: a goodbye (section 10.1) where the name was announced, so other computers forget it at once */
static void goodbye(struct udp_socket *s) {
    int i, n;
    for(i = 0; i < IFS; i++) {
        struct ifs *f = &st.ifs[i];
        if(!f->dev || !f->ip || f->dev->ip != f->ip || !(f->phase == P_DONE || (f->phase == P_ANNOUNCE && f->count)))
            continue;
        if((n = mdns_announce(st.name, f->ip, 0, out, sizeof(out))) > 0)
            udp_sendto_dev(s, f->dev, MDNS_GROUP, MDNS_PORT, out, (u_int)n);
    }
}

static void mdns_thread(void *arg) {
    struct udp_socket *s = (struct udp_socket *)arg;
    while(!st.stop) {
        u_long now = uptime_ms(), wait;
        struct netdev *dev = NULL;
        struct ifs *f;
        ipaddr_t src, dst = 0;
        u16 sport;
        int n, i;
        follow(now);
        wait = due(s, now);
        n = udp_recvfrom_dev(s, msg, sizeof(msg), &src, &sport, &dev, &dst, wait ? wait : 1);
        if(n <= 0 || !dev || ours(src)) continue;               /* ours: from another of our interfaces */
        for(i = 0; i < IFS && st.ifs[i].dev != dev; i++);
        if(i == IFS) continue;
        f = &st.ifs[i];
        if(!f->ip || f->phase == P_IDLE || !on_link(dev, src, dst)) continue;
        now = uptime_ms();
        if(sport == MDNS_PORT && mdns_conflict(msg, (u_int)n, st.name, f->ip)) {   /* section 6: responses from 5353 */
            if(f->phase != P_PROBE) {                           /* section 9: probed for again, where it may lose */
                mlog("+MDNS: another host claims %s.local: probing for it again\n", st.name);
                probe_all(now + conflict_hold(now));
            } else if(f->count && !rename_next(now)) {          /* count 0: no probe for the name out yet */
                mlog("+MDNS: no free name from %s-2 to %s-%d: the responder stops\n", st.base, st.base, SUFFIX_MAX);
                break;
            }
            continue;
        }
        if(f->phase == P_PROBE) {                               /* the name is not ours yet: nothing is answered */
            if(f->count && mdns_tiebreak(msg, (u_int)n, st.name, f->ip) > 0) {
                u_long hold = conflict_hold(now);               /* section 8.2: the other host's probe wins */
                mlog("+MDNS: %s: another host probes for %s.local too and wins the tie: probing again in a second\n",
                     dev->name, st.name);
                f->count = 0;                                   /* its next probes find 0: one wait a round */
                f->at = now + (hold > TIE_MS ? hold : TIE_MS);
            }
            continue;
        }
        {
            int uni, probe = mdns_tiebreak(msg, (u_int)n, st.name, f->ip) != 0;    /* another host's probe */
            int r = mdns_respond(msg, (u_int)n, sport, st.name, f->ip, out, sizeof(out), &uni);
            if(r <= 0) continue;
            if(uni || dst != MDNS_GROUP) udp_sendto_dev(s, dev, src, sport, out, (u_int)r);
            else if(now - f->last_mc >= (u_long)(probe ? PROBE_RATE_MS : RATE_MS)) {
                udp_sendto_dev(s, dev, MDNS_GROUP, MDNS_PORT, out, (u_int)r);
                f->last_mc = now;
            }
        }
    }
    goodbye(s);
    udp_close(s);
    mlog("+MDNS: stopped\n");
    st.running = false;
}

int mdns_start(const char *name) {
    struct udp_socket *s;
    u_long flags;
    if(!mdns_name_valid(name)) return -EINVAL;
    flags = irq_save();                                 /* two consoles at once: one responder */
    if(st.running) {
        irq_restore(flags);
        return -EBUSY;
    }
    st.running = true;
    st.stop = false;                                    /* an mdns off from here on ends this one */
    irq_restore(flags);
    if(!(s = udp_open(MDNS_PORT))) {
        st.running = false;
        return -EADDRINUSE;
    }
    udp_set_ttl(s, 255);                                /* section 11: the answers to one querier too */
    memset(st.ifs, 0, sizeof(st.ifs));
    snprintf(st.base, sizeof(st.base), "%s", name);
    snprintf(st.name, sizeof(st.name), "%s", name);
    st.suffix = st.conflicts = 0;
    st.last_conflict = 0;
    if(!kthread_create("mdns", mdns_thread, s, PL_NORMAL, TF_DETACHED)) {
        udp_close(s);
        st.running = false;
        return -ENOMEM;
    }
    return 0;
}

/* the thread is told to end (it says goodbye first) and waited for, STOP_MS at most (its socket waits PROBE_MS at
   most) */
int mdns_stop(void) {
    u_long start = uptime_ms();
    st.stop = true;
    while(st.running && uptime_ms() - start < STOP_MS) sleep_ms(20);
    return st.running ? -EBUSY : 0;
}

int mdns_running(char *name, u_int size) {
    if(!st.running) return 0;
    snprintf(name, size, "%s", st.name);
    return 1;
}

/* where it answers: "eth0 10.0.2.15, wlan0 10.0.0.9 (probing)", or "no interface with an address yet" */
void mdns_status(char *out, u_int size) {
    u_int n = 0;
    int i;
    out[0] = 0;
    for(i = 0; i < IFS && n + 1 < size; i++) {
        const struct ifs *f = &st.ifs[i];
        char a[16];
        if(!f->dev || !f->ip || f->phase == P_IDLE) continue;
        n += (u_int)snprintf(out + n, size - n, "%s%s %s%s", n ? ", " : "", f->dev->name, ipfmt(f->ip, a),
                             f->phase == P_PROBE ? " (probing)" : "");
    }
    if(!out[0]) snprintf(out, size, "no interface with an address yet");
}
