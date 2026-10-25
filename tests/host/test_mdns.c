/* Host-side unit tests for net/mdns_wire.c, the messages of the multicast DNS responder: a query for our name in
   any case answered to the group (A, cache-flush, TTL 120, an NSEC record with it), one with QU to the querier, a
   one-shot one (another port) with its ID, RD, its questions as written and a TTL of 10 s (and a plain "no
   record" for AAAA); ANY answered, AAAA with the NSEC record alone, other names and classes not; another opcode
   or a response code not looked at; a response never answered; a known answer with half its TTL left suppresses
   ours, a nearly expired one does not; a second question reached through a compression pointer; broken messages
   (a pointer loop, a label past the end, too short); probes, announcements and goodbyes as RFC 6762 has them;
   conflicts; ties between probes (section 8.2: class, type, data, the longer list); the names we may take. Built
   from the same file as the kernel and run by `make unittest`. */
#include <stdio.h>
#include <string.h>
#include <net/mdns.h>

static int failures, checks;

static void check(int ok, const char *what) {
    checks++;
    if(!ok) { failures++; printf("FAIL: %s\n", what); }
}

static u8 m[1600];
static u_int used;

static void w16(u16 v) { m[used++] = (u8)(v >> 8); m[used++] = (u8)v; }
static void w32(u32 v) { w16((u16)(v >> 16)); w16((u16)v); }

/* a name in labels, uncompressed */
static void name(const char *s) {
    while(*s) {
        const char *dot = strchr(s, '.');
        u_int n = dot ? (u_int)(dot - s) : (u_int)strlen(s);
        m[used++] = (u8)n;
        memcpy(m + used, s, n);
        used += n;
        s += n;
        if(*s) s++;
    }
    m[used++] = 0;
}

static void header(u16 id, u16 flags, u16 qd, u16 an, u16 ns, u16 ar) {
    used = 0;
    w16(id); w16(flags); w16(qd); w16(an); w16(ns); w16(ar);
}

static ipaddr_t addr(u8 a, u8 b, u8 c, u8 d) {
    ipaddr_t v;
    u8 *p = (u8 *)&v;
    p[0] = a; p[1] = b; p[2] = c; p[3] = d;
    return v;
}

int main(void) {
    u8 out[MDNS_MSG_MAX];
    ipaddr_t us = addr(10, 0, 2, 15), other = addr(10, 0, 2, 99);
    int uni, n;

    header(0, 0, 1, 0, 0, 0); name("SurfOS.LOCAL"); w16(1); w16(1);
    n = mdns_respond(m, used, 5353, "surfos", us, out, sizeof(out), &uni);
    check(n == 12 + 28 + 41 && !uni && out[2] == 0x84 && out[3] == 0 && out[7] == 1 && out[11] == 1 &&
          !memcmp(out + 36, &us, 4), "a query in another case: our A record, to the group, an NSEC record with it");
    check(out[12] == 6 && !memcmp(out + 13, "surfos\5local", 12) && out[25] == 0 && out[27] == 1 && out[28] == 0x80 &&
          out[29] == 1 && out[30] == 0 && out[31] == 0 && out[32] == 0 && out[33] == 120,
          "the answer: our name, A, IN with the cache-flush bit, 120 s");
    check(!memcmp(out + 40, "\6surfos\5local", 13) && out[53] == 0 && out[54] == 0 && out[55] == 47 && out[56] == 0x80 &&
          out[57] == 1 && out[61] == 120 && out[62] == 0 && out[63] == 17 && !memcmp(out + 64, "\6surfos\5local", 14) &&
          out[78] == 0 && out[79] == 1 && out[80] == 0x40,
          "the NSEC record (the additional section): its own name next, a bitmap of A alone");
    header(0, 0, 1, 0, 0, 0); name("surfos.local"); w16(1); w16(0x8001);
    check(mdns_respond(m, used, 5353, "surfos", us, out, sizeof(out), &uni) > 0 && uni, "QU: to the querier alone");
    header(0x1234, 0x0100, 1, 0, 0, 0); name("surfos.local"); w16(1); w16(1);
    n = mdns_respond(m, used, 54321, "surfos", us, out, sizeof(out), &uni);
    check(n == 12 + 18 + 28 && uni && out[0] == 0x12 && out[1] == 0x34 && out[2] == 0x85 && out[5] == 1 &&
          out[7] == 1 && out[11] == 0 && !memcmp(out + 12, m + 12, 18),
          "a one-shot query: its ID, RD and the question back, to it alone, no NSEC");
    check(out[n - 10] == 0 && out[n - 9] == 0 && out[n - 8] == 0 && out[n - 7] == 10 && out[n - 12] == 0 &&
          out[n - 11] == 1, "a one-shot query: TTL 10, no cache-flush bit");
    header(0x77, 0, 2, 0, 0, 0); name("other.local"); w16(1); w16(1);
    m[used++] = 6; memcpy(m + used, "SurfOS", 6); used += 6; m[used++] = 0xc0; m[used++] = 18; w16(255); w16(1);
    n = mdns_respond(m, used, 54321, "surfos", us, out, sizeof(out), &uni);
    check(n == 12 + 17 + 18 + 28 && out[5] == 2 && out[7] == 1 && !memcmp(out + 12, m + 12, 17) &&
          !memcmp(out + 29, "\6SurfOS\5local\0\0\xff\0\1", 18),
          "a one-shot query of two questions: both back as they were written (ANY, its case), uncompressed");
    header(0x55, 0, 1, 0, 0, 0); name("surfos.local"); w16(28); w16(1);
    n = mdns_respond(m, used, 54321, "surfos", us, out, sizeof(out), &uni);
    check(n == 12 + 18 && uni && out[5] == 1 && out[7] == 0 && out[3] == 0,
          "a one-shot query for AAAA: no record (a plain DNS answer), its question back");
    header(0, 0, 1, 0, 0, 0); name("surfos.local"); w16(255); w16(1);
    check(mdns_respond(m, used, 5353, "surfos", us, out, sizeof(out), &uni) > 0, "ANY: answered");
    header(0, 0, 1, 0, 0, 0); name("surfos.local"); w16(28); w16(1);
    n = mdns_respond(m, used, 5353, "surfos", us, out, sizeof(out), &uni);
    check(n == 12 + 41 && !uni && out[7] == 1 && out[11] == 0 && out[26] == 0 && out[27] == 47 && out[n - 1] == 0x40,
          "AAAA: the NSEC record alone, an answer (no IPv6 address, section 6.1)");
    header(0, 0, 1, 1, 0, 0); name("surfos.local"); w16(28); w16(1);
    m[used++] = 0xc0; m[used++] = 12; w16(47); w16(0x8001); w32(120); w16(3); m[used++] = 0; m[used++] = 1;
    m[used++] = 0x40;
    check(mdns_respond(m, used, 5353, "surfos", us, out, sizeof(out), &uni) == 0, "AAAA with our NSEC known: nothing");
    header(0, 0, 1, 0, 0, 0); name("other.local"); w16(1); w16(1);
    check(mdns_respond(m, used, 5353, "surfos", us, out, sizeof(out), &uni) == 0, "another name: nothing");
    header(0, 0, 1, 0, 0, 0); name("surfos.local"); w16(1); w16(3);
    check(mdns_respond(m, used, 5353, "surfos", us, out, sizeof(out), &uni) == 0, "another class: nothing");
    header(0, 0x2800, 1, 0, 0, 0); name("surfos.local"); w16(1); w16(1);
    check(mdns_respond(m, used, 5353, "surfos", us, out, sizeof(out), &uni) == 0, "another opcode: nothing");
    header(0, 0x0003, 1, 0, 0, 0); name("surfos.local"); w16(1); w16(1);
    check(mdns_respond(m, used, 5353, "surfos", us, out, sizeof(out), &uni) == 0, "a response code: nothing");
    header(0, 0x8400, 0, 1, 0, 0); name("surfos.local"); w16(1); w16(0x8001); w32(120); w16(4);
    memcpy(m + used, &other, 4); used += 4;
    check(mdns_respond(m, used, 5353, "surfos", us, out, sizeof(out), &uni) == 0, "a response: never answered");
    check(mdns_conflict(m, used, "surfos", us) == 1 && mdns_conflict(m, used, "SurfOS", us) == 1 &&
          mdns_conflict(m, used, "surfos", other) == 0 && mdns_conflict(m, used, "other", us) == 0,
          "a response giving our name another address: a conflict; with ours, or for another name: none");
    m[3] = 0x03;
    check(mdns_conflict(m, used, "surfos", us) == 0, "... with a response code: not looked at");
    m[3] = 0; m[2] = 0x8c;
    check(mdns_conflict(m, used, "surfos", us) == 0, "... with an opcode: not looked at");
    header(0, 0, 1, 0, 0, 0); name("surfos.local"); w16(1); w16(1);
    check(mdns_conflict(m, used, "surfos", us) == 0, "a query: no conflict");
    header(0, 0, 1, 1, 0, 0); name("surfos.local"); w16(1); w16(1);
    m[used++] = 0xc0; m[used++] = 12; w16(1); w16(0x8001); w32(100); w16(4); memcpy(m + used, &us, 4); used += 4;
    check(mdns_respond(m, used, 5353, "surfos", us, out, sizeof(out), &uni) == 0,
          "our record known with more than half its TTL: no answer");
    m[used - 10] = 0; m[used - 9] = 0; m[used - 8] = 0; m[used - 7] = 30;
    check(mdns_respond(m, used, 5353, "surfos", us, out, sizeof(out), &uni) > 0, "known, nearly expired: answered");
    header(0, 0, 2, 0, 0, 0); name("x.local"); w16(1); w16(1);
    m[used++] = 6; memcpy(m + used, "surfos", 6); used += 6; m[used++] = 0xc0; m[used++] = 14; w16(1); w16(1);
    check(mdns_respond(m, used, 5353, "surfos", us, out, sizeof(out), &uni) > 0,
          "a second question, .local through a pointer");
    header(0, 0, 1, 0, 0, 0); m[used++] = 0xc0; m[used++] = 12; w16(1); w16(1);
    check(mdns_respond(m, used, 5353, "surfos", us, out, sizeof(out), &uni) == -1, "a pointer to itself: broken");
    header(0, 0, 1, 0, 0, 0); m[used++] = 60; used += 3;
    check(mdns_respond(m, used, 5353, "surfos", us, out, sizeof(out), &uni) == -1, "a label past the end: broken");
    header(0, 0, 1, 0, 0, 0); name("surfos.local");
    check(mdns_respond(m, used, 5353, "surfos", us, out, sizeof(out), &uni) == -1, "a question cut short: broken");
    check(mdns_respond(m, 5, 5353, "surfos", us, out, sizeof(out), &uni) == -1, "shorter than a header: broken");
    n = mdns_probe("surfos", us, out, sizeof(out));
    check(n > 0 && out[2] == 0 && out[5] == 1 && out[7] == 0 && out[9] == 1 && out[11] == 0 &&
          out[12 + 14] == 0 && out[12 + 15] == 255 && out[12 + 16] == 0x80 && out[12 + 17] == 1,
          "a probe: a query for ANY with QU, our record in the authority section");
    n = mdns_announce("surfos", us, MDNS_TTL, out, sizeof(out));
    check(n == 12 + 14 + 14 && out[2] == 0x84 && out[5] == 0 && out[7] == 1 && !memcmp(out + n - 4, &us, 4) &&
          out[33] == 120, "an announcement: a response with our record");
    n = mdns_announce("surfos", us, 0, out, sizeof(out));
    check(n == 12 + 14 + 14 && out[28] == 0x80 && out[30] == 0 && out[31] == 0 && out[32] == 0 && out[33] == 0,
          "a goodbye: the record with a TTL of 0");
    check(mdns_announce("surfos", us, MDNS_TTL, out, 20) == 0 && mdns_probe("surfos", us, out, 30) == 0,
          "no room: nothing made");
    /* another host's probe for our name while we probe: its records against ours (an A record) */
    header(0, 0, 1, 0, 1, 0); name("surfos.local"); w16(255); w16(0x8001);
    name("surfos.local"); w16(1); w16(1); w32(120); w16(4); memcpy(m + used, &other, 4); used += 4;
    check(mdns_tiebreak(m, used, "surfos", us) == 1, "a probe with a later address than ours wins the tie");
    check(mdns_tiebreak(m, used, "surfos", addr(10, 0, 2, 100)) == -1 && mdns_tiebreak(m, used, "surfos", other) == -1,
          "against a later address of ours it loses; the same record is no conflict (both: a probe for the name)");
    check(mdns_tiebreak(m, used, "other", us) == 0, "a probe for another name: no tie");
    check(mdns_tiebreak(m, used - 1, "surfos", us) == 0, "a probe cut short: nothing");
    m[2] = 0x84;
    check(mdns_tiebreak(m, used, "surfos", us) == 0, "a response: no probe");
    header(0, 0, 1, 0, 1, 0); name("SURFOS.local"); w16(255); w16(0x8001);
    name("SurfOS.LOCAL"); w16(1); w16(0x8001); w32(120); w16(4);
    {
        ipaddr_t lower = addr(10, 0, 2, 3);
        memcpy(m + used, &lower, 4);
        used += 4;
    }
    check(mdns_tiebreak(m, used, "surfos", us) == -1, "an earlier address (another case, the cache-flush bit): ours wins");
    header(0, 0, 1, 0, 2, 0); name("surfos.local"); w16(255); w16(0x8001);
    m[used++] = 0xc0; m[used++] = 12; w16(28); w16(1); w32(120); w16(16); memset(m + used, 0, 16); used += 16;
    m[used++] = 0xc0; m[used++] = 12; w16(1); w16(1); w32(120); w16(4); memcpy(m + used, &us, 4); used += 4;
    check(mdns_tiebreak(m, used, "surfos", us) == 1,
          "an AAAA and then our own record, names through pointers: the longer list wins");
    m[9] = 1;                                           /* one record in the authority section: the AAAA */
    check(mdns_tiebreak(m, used, "surfos", us) == 1, "an AAAA alone: AAAA is the later type than A");
    header(0, 0, 1, 0, 1, 0); name("surfos.local"); w16(255); w16(0x8001);
    name("surfos.local"); w16(1); w16(3); w32(120); w16(4); memcpy(m + used, &us, 4); used += 4;
    check(mdns_tiebreak(m, used, "surfos", us) == 1, "the same data in a later class wins");
    check(mdns_name_valid("surfos") && mdns_name_valid("Lab-PC2") && mdns_name_valid("a") && !mdns_name_valid("-x") &&
          !mdns_name_valid("x-") && !mdns_name_valid("a.b") && !mdns_name_valid("") && !mdns_name_valid("a b") &&
          !mdns_name_valid("caf\xc3\xa9"), "names: letters, digits, hyphens inside");
    {
        char longname[80];
        memset(longname, 'n', 63);
        longname[63] = 0;
        check(mdns_name_valid(longname) && mdns_announce(longname, us, MDNS_TTL, out, sizeof(out)) == 12 + 71 + 14,
              "a name of 63 letters");
        header(0, 0, 1, 0, 0, 0); name(longname); used--; name("local");      /* <63 n's>.local, then A */
        w16(1); w16(1);
        check(mdns_respond(m, used, 5353, longname, us, out, sizeof(out), &uni) == 12 + 85 + 155,
              "... answered with its NSEC record too, in MDNS_MSG_MAX");
        longname[63] = 'n';
        longname[64] = 0;
        check(!mdns_name_valid(longname), "64 letters: too long for a label");
    }
    printf("test_mdns: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
