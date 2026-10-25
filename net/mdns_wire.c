/*
SurfOS Network Stack: multicast DNS messages
--------------------
File: mdns_wire.c   Date: 10/25/26 (the network tools)
--------------------
What the responder (net/mdns.c) reads and writes of multicast DNS (RFC 6762), with no I/O,
allocation or state, so the tests run it on the host (tests/host/test_mdns.c): a query's
questions for our name, <name>.local, in any case and through compression pointers (bounded as
dns_wire.c bounds them); the answer, an A record with the cache-flush bit and a TTL of 120 s,
sent to the group, or to the querier alone when it asked so (QU) or when it is a one-shot
querier (a port other than 5353, RFC 6762 section 6.7: a plain DNS answer, its ID and questions
echoed as they were, a TTL of 10 s and no cache-flush bit); for a type the name has none of
(AAAA: SurfOS has no IPv6) an NSEC record saying A is all there is (section 6.1), which also
goes with every A answer, so a computer asking for both learns at once; a probe (section 8.1:
ANY, QU, our record in the authority section), an announcement (section 8.3) and a goodbye (a
TTL of 0, section 10.1); whether a response claims our name for another address (a conflict
while probing, or later); and whether another host's probe for the name, sent while we probe,
wins the tie (section 8.2). Known-answer suppression (section 7.1): a query that already holds
our record with at least half its TTL left gets no answer. Messages with an opcode or a response
code other than 0 are not looked at (section 18).
*/

#include <net/mdns.h>
#include <blibc_common.h>

#define TYPE_A      1
#define TYPE_NSEC   47
#define TYPE_ANY    255
#define CLASS_IN    1
#define CLASS_ANY   255
#define CLASS_TOP   0x8000              /* QU in a question, cache-flush in a record */
#define TTL_HOST    MDNS_TTL
#define TTL_LEGACY  10
#define IGNORED     0x780f              /* the flags' opcode and response code: a message with either is ignored */

static u16 be16(const u8 *p) { return (u16)((p[0] << 8) | p[1]); }
static u32 be32(const u8 *p) { return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3]; }
static char lower(char c) { return c >= 'A' && c <= 'Z' ? (char)(c + ('a' - 'A')) : c; }

/* the name at off, its labels joined by dots, in lower case (fold) or as written, into out (MDNS_FULL_MAX + 1
   bytes); *next past the name as it is written (not where its pointers lead). At most 128 labels and pointers. 0,
   or -1 for a broken name */
static int expand(const u8 *p, u_int len, u_int off, char *out, u_int *next, int fold) {
    u_int n = 0, steps = 0, after = 0;
    while(off < len && steps++ < 128) {
        u_int l = p[off++], j;
        if((l & 0xc0) == 0xc0) {
            if(off >= len) return -1;
            if(!after) after = off + 1;
            off = ((l & 0x3f) << 8) | p[off];
            continue;
        }
        if(l & 0xc0) return -1;
        if(!l) {
            out[n] = 0;
            *next = after ? after : off;
            return 0;
        }
        if(l > len - off || n + (n != 0) + l > MDNS_FULL_MAX) return -1;
        if(n) out[n++] = '.';
        for(j = 0; j < l; j++) {
            if(!p[off + j] || p[off + j] == '.') return -1;
            out[n++] = fold ? lower((char)p[off + j]) : (char)p[off + j];
        }
        off += l;
    }
    return -1;
}

/* name.local, in lower case, into full */
static void our_name(const char *name, char *full) {
    u_int n = 0;
    while(*name && n < MDNS_NAME_MAX) full[n++] = lower(*name++);
    memcpy(full + n, ".local", 7);
}

/* name.local in the wire's labels at out: its length, or 0 when it does not fit in cap */
static u_int put_name(u8 *out, u_int cap, const char *name) {
    u_int n = (u_int)strlen(name);
    if(n > MDNS_NAME_MAX || cap < n + 8) return 0;
    out[0] = (u8)n;
    memcpy(out + 1, name, n);
    memcpy(out + 1 + n, "\5local", 7);                  /* the label "local" and the root's 0 */
    return n + 8;
}

/* a name as expand() wrote it (labels joined by dots; "" is the root) in labels again at out: its length, or 0
   when it does not fit in cap */
static u_int put_text(u8 *out, u_int cap, const char *s) {
    u_int n = 0;
    while(*s) {
        u_int l = 0;
        while(s[l] && s[l] != '.') l++;
        if(cap - n < l + 2) return 0;                   /* the label, its length and at least the root's 0 */
        out[n++] = (u8)l;
        memcpy(out + n, s, l);
        n += l;
        s += l;
        if(*s) s++;
    }
    if(cap - n < 1) return 0;
    out[n++] = 0;
    return n;
}

static void put16(u8 *p, u16 v) { p[0] = (u8)(v >> 8); p[1] = (u8)v; }
static void put32(u8 *p, u32 v) { p[0] = (u8)(v >> 24); p[1] = (u8)(v >> 16); p[2] = (u8)(v >> 8); p[3] = (u8)v; }

/* our A record (name.local -> addr) at out: its length, or 0 when it does not fit */
static u_int put_record(u8 *out, u_int cap, const char *name, ipaddr_t addr, u16 cls, u32 ttl) {
    u_int n = put_name(out, cap, name);
    if(!n || cap - n < 14) return 0;
    put16(out + n, TYPE_A);
    put16(out + n + 2, cls);
    put32(out + n + 4, ttl);
    put16(out + n + 8, 4);
    memcpy(out + n + 10, &addr, 4);                     /* addr is in network order already */
    return n + 14;
}

/* our NSEC record (section 6.1, the restricted form): name.local, the next name its own, and a bitmap with A
   alone, the one type the name has. Its length, or 0 when it does not fit */
static u_int put_nsec(u8 *out, u_int cap, const char *name) {
    u_int n = put_name(out, cap, name), m;
    if(!n || cap - n < 10) return 0;
    m = put_name(out + n + 10, cap - n - 10, name);
    if(!m || cap - n - 10 - m < 3) return 0;
    put16(out + n, TYPE_NSEC);
    put16(out + n + 2, CLASS_IN | CLASS_TOP);
    put32(out + n + 4, TTL_HOST);
    put16(out + n + 8, (u16)(m + 3));
    out[n + 10 + m] = 0;                                /* window 0, */
    out[n + 10 + m + 1] = 1;                            /* a bitmap of one byte, */
    out[n + 10 + m + 2] = 0x40;                         /* type 1 (A) its only bit */
    return n + 10 + m + 3;
}

/* a record at off: its owner name, type, class, TTL, data and where the next one starts. 0, or -1 */
struct rec {
    char owner[MDNS_FULL_MAX + 1];
    u16 type, cls, size;
    u32 ttl;
    u_int data, next;
};

static int record_at(const u8 *p, u_int len, u_int off, struct rec *r) {
    if(expand(p, len, off, r->owner, &off, 1) || off > len || len - off < 10) return -1;
    r->type = be16(p + off);
    r->cls = be16(p + off + 2);
    r->ttl = be32(p + off + 4);
    r->size = be16(p + off + 8);
    r->data = off + 10;
    if(r->size > len - r->data) return -1;
    r->next = r->data + r->size;
    return 0;
}

int mdns_respond(const u8 *p, u_int len, u16 sport, const char *name, ipaddr_t addr, u8 *out, u_int cap,
                 int *unicast) {
    char full[MDNS_FULL_MAX + 1], q[MDNS_FULL_MAX + 1];
    u_int off = 12, i, qd, an, n, k;
    int legacy = sport != MDNS_PORT, qu = 0, want_a = 0, want_nsec = 0, known_a = 0, known_nsec = 0;
    int a, nsec, nsec_extra;
    struct rec r;
    *unicast = 0;
    if(len < 12) return -1;
    if(be16(p + 2) & 0x8000) return 0;                  /* a response: nothing to answer */
    if(be16(p + 2) & IGNORED) return 0;                 /* another opcode, or a response code (section 18) */
    qd = be16(p + 4);
    an = be16(p + 6);
    our_name(name, full);
    for(i = 0; i < qd; i++) {                           /* the questions: which ask for our name */
        u16 type, cls;
        if(expand(p, len, off, q, &off, 1) || len - off < 4) return -1;
        type = be16(p + off);
        cls = be16(p + off + 2) & ~CLASS_TOP;
        if(!strcmp(q, full) && (cls == CLASS_IN || cls == CLASS_ANY)) {
            if(type == TYPE_A || type == TYPE_ANY) want_a = 1;
            else want_nsec = 1;                         /* a type it has none of (AAAA): section 6.1 */
            if(be16(p + off + 2) & CLASS_TOP) qu = 1;
        }
        off += 4;
    }
    if(!want_a && !want_nsec) return 0;
    for(i = 0; i < an; i++) {                           /* known answers: ours, with half its TTL left, is known */
        if(record_at(p, len, off, &r)) return -1;
        if(!strcmp(r.owner, full) && (r.cls & ~CLASS_TOP) == CLASS_IN && r.ttl >= TTL_HOST / 2) {
            if(r.type == TYPE_A && r.size == 4 && !memcmp(p + r.data, &addr, 4)) known_a = 1;
            if(r.type == TYPE_NSEC) known_nsec = 1;
        }
        off = r.next;
    }
    if(legacy) known_a = known_nsec = 0;                /* a one-shot querier gets its answer whatever it holds */
    a = want_a && !known_a;
    nsec = !legacy && want_nsec && !known_nsec;         /* a one-shot querier gets a plain "no such record" */
    nsec_extra = !legacy && a && !want_nsec && !known_nsec;     /* with the A record, in the additional section */
    if(!legacy && !a && !nsec) return 0;
    if(cap < 12) return 0;
    memset(out, 0, 12);
    put16(out + 2, (u16)(0x8400 | (legacy ? be16(p + 2) & 0x0100 : 0)));  /* a response, authoritative; RD as asked */
    put16(out + 6, (u16)(a + nsec));
    put16(out + 10, (u16)nsec_extra);
    n = 12;
    if(legacy) {                                        /* a one-shot querier: its ID, its questions as they were */
        u_int qoff = 12, m;
        put16(out, be16(p));
        put16(out + 4, (u16)qd);
        for(i = 0; i < qd; i++) {
            expand(p, len, qoff, q, &qoff, 0);          /* it parsed above */
            m = put_text(out + n, cap - n, q);
            if(!m || cap - n - m < 4) return 0;
            memcpy(out + n + m, p + qoff, 4);           /* its type and class */
            n += m + 4;
            qoff += 4;
        }
    }
    if(a) {
        k = legacy ? put_record(out + n, cap - n, name, addr, CLASS_IN, TTL_LEGACY)
                   : put_record(out + n, cap - n, name, addr, CLASS_IN | CLASS_TOP, TTL_HOST);
        if(!k) return 0;
        n += k;
    }
    if(nsec || nsec_extra) {
        if(!(k = put_nsec(out + n, cap - n, name))) return 0;
        n += k;
    }
    *unicast = qu || legacy;
    return (int)n;
}

int mdns_announce(const char *name, ipaddr_t addr, u32 ttl, u8 *out, u_int cap) {
    u_int n;
    if(cap < 12) return 0;
    memset(out, 0, 12);
    put16(out + 2, 0x8400);
    put16(out + 6, 1);
    n = put_record(out + 12, cap - 12, name, addr, CLASS_IN | CLASS_TOP, ttl);
    return n ? (int)(12 + n) : 0;
}

int mdns_probe(const char *name, ipaddr_t addr, u8 *out, u_int cap) {
    u_int n, m;
    if(cap < 12) return 0;
    memset(out, 0, 12);
    put16(out + 4, 1);                                  /* one question */
    put16(out + 8, 1);                                  /* one record in the authority section: ours */
    n = put_name(out + 12, cap - 12, name);
    if(!n || cap - 12 - n < 4) return 0;
    put16(out + 12 + n, TYPE_ANY);
    put16(out + 12 + n + 2, CLASS_IN | CLASS_TOP);      /* QU: answers come to us alone */
    n += 16;
    m = put_record(out + n, cap - n, name, addr, CLASS_IN, TTL_HOST);
    return m ? (int)(n + m) : 0;
}

int mdns_conflict(const u8 *p, u_int len, const char *name, ipaddr_t addr) {
    char full[MDNS_FULL_MAX + 1], q[MDNS_FULL_MAX + 1];
    u_int off = 12, i, records;
    struct rec r;
    if(len < 12 || !(be16(p + 2) & 0x8000) || (be16(p + 2) & IGNORED)) return 0;
    our_name(name, full);
    for(i = 0; i < be16(p + 4); i++) {                  /* questions in a response: skipped */
        if(expand(p, len, off, q, &off, 1) || len - off < 4) return 0;
        off += 4;
    }
    records = (u_int)be16(p + 6) + be16(p + 8) + be16(p + 10);
    for(i = 0; i < records; i++, off = r.next) {
        if(record_at(p, len, off, &r)) return 0;
        if(!strcmp(r.owner, full) && (r.cls & ~CLASS_TOP) == CLASS_IN &&
           !(r.type == TYPE_A && r.size == 4 && !memcmp(p + r.data, &addr, 4)))
            return 1;                                   /* our name, for something else */
    }
    return 0;
}

/* two records in the order of RFC 6762 section 8.2: class (the cache-flush bit left out), type, then the data byte
   by byte, the longer the later when one is the start of the other. <0 when the first is the earlier, 0, >0 */
static int rec_cmp(u16 c1, u16 t1, const u8 *d1, u_int n1, u16 c2, u16 t2, const u8 *d2, u_int n2) {
    u_int i;
    c1 &= ~CLASS_TOP;
    c2 &= ~CLASS_TOP;
    if(c1 != c2) return c1 < c2 ? -1 : 1;
    if(t1 != t2) return t1 < t2 ? -1 : 1;
    for(i = 0; i < n1 && i < n2; i++)
        if(d1[i] != d2[i]) return d1[i] < d2[i] ? -1 : 1;
    return n1 == n2 ? 0 : n1 < n2 ? -1 : 1;
}

int mdns_tiebreak(const u8 *p, u_int len, const char *name, ipaddr_t addr) {
    char full[MDNS_FULL_MAX + 1], q[MDNS_FULL_MAX + 1];
    u_int off = 12, i, theirs = 0, low_data = 0, low_size = 0;
    u16 low_cls = 0, low_type = 0;
    u8 ours[4];
    struct rec r;
    int c;
    if(len < 12 || (be16(p + 2) & (0x8000 | IGNORED))) return 0;   /* a response is no probe (mdns_conflict's) */
    our_name(name, full);
    for(i = 0; i < be16(p + 4); i++) {                  /* the questions: skipped */
        if(expand(p, len, off, q, &off, 1) || len - off < 4) return 0;
        off += 4;
    }
    for(i = 0; i < be16(p + 6); i++, off = r.next)      /* known answers: skipped */
        if(record_at(p, len, off, &r)) return 0;
    for(i = 0; i < be16(p + 8); i++, off = r.next) {    /* the authority section: what it probes for */
        if(record_at(p, len, off, &r)) return 0;
        if(strcmp(r.owner, full)) continue;
        if(!theirs++ || rec_cmp(r.cls, r.type, p + r.data, r.size, low_cls, low_type, p + low_data, low_size) < 0) {
            low_cls = r.cls;                            /* its earliest record for our name */
            low_type = r.type;
            low_data = r.data;
            low_size = r.size;
        }
    }
    if(!theirs) return 0;
    memcpy(ours, &addr, 4);                             /* in network order, as the wire has it */
    c = rec_cmp(CLASS_IN, TYPE_A, ours, 4, low_cls, low_type, p + low_data, low_size);
    return c < 0 || (!c && theirs > 1) ? 1 : -1;        /* the same first record: the longer list is the later */
}

int mdns_name_valid(const char *name) {
    u_int i;
    if(!name || !*name || strlen(name) > MDNS_NAME_MAX || name[0] == '-') return 0;
    for(i = 0; name[i]; i++) {
        char c = name[i];
        if(!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-')) return 0;
    }
    return name[i - 1] != '-';
}
