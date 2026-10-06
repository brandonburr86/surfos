/* Host-side unit tests for user/telnetd/telnet.c, telnetd's telnet protocol: the opening
   negotiation; options answered once and never in a loop (RFC 1143), also against a client
   that answers everything; commands, sub-negotiations and CR pairs split across reads at
   every byte; IAC IAC, IP and BRK as Ctrl+C, AYT; NAWS sizes; full reply buffers; and the
   output side (LF as CR LF, 255 doubled). Built from the same file as the program and run
   by `make unittest`. */
#include <stdio.h>
#include <string.h>
#include "telnet.h"

static int failures, checks;

static void check(int ok, const char *what) {
    checks++;
    if(!ok) { failures++; printf("FAIL: %s\n", what); }
}

static unsigned char data[4096], reply[4096];
static struct tn_buf D, R;

/* in, n bytes, as one read */
static void recv_all(struct telnet *t, const unsigned char *in, int n) {
    D.p = data; D.len = 0; D.size = sizeof(data);
    R.p = reply; R.len = 0; R.size = sizeof(reply);
    telnet_recv(t, in, n, &D, &R);
}

/* the same bytes in reads of `step` bytes, collected into D and R */
static void recv_split(struct telnet *t, const unsigned char *in, int n, int step) {
    int i;
    D.p = data; D.len = 0; D.size = sizeof(data);
    R.p = reply; R.len = 0; R.size = sizeof(reply);
    for(i = 0; i < n; i += step) {
        struct tn_buf d = { data + D.len, 0, (int)sizeof(data) - D.len }, r = { reply + R.len, 0, (int)sizeof(reply) - R.len };
        telnet_recv(t, in + i, n - i < step ? n - i : step, &d, &r);
        D.len += d.len;
        R.len += r.len;
    }
}

static int is(const struct tn_buf *b, const void *want, int n) {
    return b->len == n && !memcmp(b->p, want, (size_t)n);
}

static struct telnet fresh(void) {
    struct telnet t;
    unsigned char out[TN_OPENING];
    telnet_init(&t);
    telnet_opening(&t, out);
    return t;
}

static void test_opening(void) {
    static const unsigned char want[] = { 255, 251, 1, 255, 251, 3, 255, 253, 3, 255, 253, 31 };
    struct telnet t;
    unsigned char out[TN_OPENING];
    int n;
    telnet_init(&t);
    n = telnet_opening(&t, out);
    check(n == 12 && !memcmp(out, want, 12), "the opening: WILL ECHO, WILL SGA, DO SGA, DO NAWS");
    check(t.us[TO_ECHO] == TN_WANTYES && t.us[TO_SGA] == TN_WANTYES, "the opening: ECHO and SGA asked on our side");
    check(t.him[TO_SGA] == TN_WANTYES && t.him[TO_NAWS] == TN_WANTYES, "the opening: SGA and NAWS asked of the client");
    check(telnet_opening(&t, out) == 0, "a second opening asks nothing again");
}

static void test_answers(void) {
    struct telnet t = fresh();
    static const unsigned char accept[] = { 255, 253, 1, 255, 253, 3, 255, 251, 3, 255, 251, 31 };
    static const unsigned char ttype_do[] = { 255, 253, 24 }, ttype_will[] = { 255, 251, 24 };
    static const unsigned char wont24[] = { 255, 252, 24 }, dont24[] = { 255, 254, 24 };
    static const unsigned char dont_echo[] = { 255, 254, 1 }, wont_echo[] = { 255, 252, 1 };
    static const unsigned char wont_naws[] = { 255, 252, 31 }, dont_naws[] = { 255, 254, 31 };
    static const unsigned char do_echo[] = { 255, 253, 1 }, will_echo[] = { 255, 251, 1 };
    static const unsigned char will_binary[] = { 255, 251, 0 }, dont_binary[] = { 255, 254, 0 };

    recv_all(&t, accept, sizeof(accept));
    check(D.len == 0 && R.len == 0, "the client accepts the opening: no data, nothing to answer");
    check(t.us[TO_ECHO] == TN_YES && t.us[TO_SGA] == TN_YES && t.him[TO_SGA] == TN_YES && t.him[TO_NAWS] == TN_YES,
          "after the client's answers ECHO, SGA and NAWS are on");
    recv_all(&t, accept, sizeof(accept));
    check(R.len == 0, "the same answers again: nothing (they change nothing)");

    recv_all(&t, ttype_do, 3);
    check(is(&R, wont24, 3), "DO TERMINAL-TYPE (not ours): WONT");
    recv_all(&t, ttype_do, 3);
    check(is(&R, wont24, 3), "DO TERMINAL-TYPE again: WONT again (an answer to every request in NO)");
    recv_all(&t, ttype_will, 3);
    check(is(&R, dont24, 3), "WILL TERMINAL-TYPE: DONT");
    recv_all(&t, will_binary, 3);
    check(is(&R, dont_binary, 3), "WILL BINARY: DONT");

    recv_all(&t, dont_echo, 3);
    check(is(&R, wont_echo, 3) && t.us[TO_ECHO] == TN_NO, "DONT ECHO while on: WONT ECHO, off");
    recv_all(&t, dont_echo, 3);
    check(R.len == 0, "DONT ECHO again: nothing, so no loop");
    recv_all(&t, do_echo, 3);
    check(is(&R, will_echo, 3) && t.us[TO_ECHO] == TN_YES, "DO ECHO while off: WILL ECHO, on again");

    recv_all(&t, wont_naws, 3);
    check(is(&R, dont_naws, 3) && t.him[TO_NAWS] == TN_NO, "WONT NAWS while on: DONT NAWS");
    recv_all(&t, wont_naws, 3);
    check(R.len == 0, "WONT NAWS again: nothing");
}

static void test_refusals_of_the_opening(void) {
    struct telnet t = fresh();
    static const unsigned char refuse[] = { 255, 254, 1, 255, 254, 3, 255, 252, 3, 255, 252, 31 };
    recv_all(&t, refuse, sizeof(refuse));
    check(R.len == 0, "the client refuses the opening: nothing more is said");
    check(t.us[TO_ECHO] == TN_NO && t.us[TO_SGA] == TN_NO && t.him[TO_SGA] == TN_NO && t.him[TO_NAWS] == TN_NO,
          "and the options stay off");
}

/* A client that answers every request it sees with the opposite of what was last said
   to it, the kind of peer that makes careless servers loop: the exchange must end. */
static void test_no_loop(void) {
    struct telnet t;
    unsigned char out[TN_OPENING], wire[256], next[256];
    int n, rounds = 0;
    telnet_init(&t);
    n = telnet_opening(&t, out);
    memcpy(wire, out, (size_t)n);
    while(n > 0 && rounds < 50) {
        int i, k = 0;
        for(i = 0; i + 2 < n; i += 3) {             /* the client: WILL -> DONT, DO -> WONT, WONT -> DO, DONT -> WILL */
            int verb = wire[i + 1], answer = verb == TN_WILL ? TN_DONT : verb == TN_DO ? TN_WONT :
                                            verb == TN_WONT ? TN_DO : TN_WILL;
            next[k++] = TN_IAC;
            next[k++] = (unsigned char)answer;
            next[k++] = wire[i + 2];
        }
        recv_all(&t, next, k);
        memcpy(wire, reply, (size_t)R.len);
        n = R.len;
        rounds++;
    }
    check(rounds < 50, "a contrary client: the negotiation ends");
}

static void test_data(void) {
    struct telnet t = fresh();
    static const unsigned char plain[] = "ls -l\r\0pwd\r\ncd /\rx\r";
    static const unsigned char want[] = "ls -l\rpwd\rcd /\rx\r";
    static const unsigned char ff[] = { 'a', 255, 255, 'b' }, ff_want[] = { 'a', 255, 'b' };
    static const unsigned char ip[] = { 'x', 255, 244, 'y', 255, 243 }, ip_want[] = { 'x', 3, 'y', 3 };
    static const unsigned char nop[] = { 'a', 255, 241, 'b', 255, 249, 'c', 255, 242, 255, 248, 255, 247, 255, 245, 255, 240 };
    static const unsigned char ayt[] = { 'q', 255, 246 };
    static const unsigned char crlf_split[] = "a\r";
    static const unsigned char lf_next[] = "\nb";
    static const unsigned char cr_iac_lf[] = { '\r', 255, 241, '\n', 'z' };

    recv_all(&t, plain, sizeof(plain) - 1);
    check(is(&D, want, sizeof(want) - 1) && R.len == 0, "CR NUL and CR LF are a CR; a CR alone stays");
    recv_all(&t, ff, sizeof(ff));
    check(is(&D, ff_want, sizeof(ff_want)), "IAC IAC is the data byte 255");
    recv_all(&t, ip, sizeof(ip));
    check(is(&D, ip_want, sizeof(ip_want)), "IAC IP and IAC BRK are Ctrl+C");
    recv_all(&t, nop, sizeof(nop));
    check(is(&D, "abc", 3) && R.len == 0, "NOP, GA, DM, EL, EC, AO and a stray SE change nothing");
    recv_all(&t, ayt, sizeof(ayt));
    check(is(&D, "q", 1) && is(&R, TN_AYT_ANSWER, (int)strlen(TN_AYT_ANSWER)), "AYT is answered at once");
    recv_all(&t, crlf_split, 2);
    check(is(&D, "a\r", 2), "a CR at the end of a read");
    recv_all(&t, lf_next, 2);
    check(is(&D, "b", 1), "and the LF at the start of the next one goes");
    recv_all(&t, cr_iac_lf, sizeof(cr_iac_lf));
    check(is(&D, "\rz", 2), "a command between CR and LF does not keep the LF");
}

static void test_naws(void) {
    struct telnet t = fresh();
    static const unsigned char naws[] = { 255, 250, 31, 0, 120, 0, 40, 255, 240, 'k' };
    static const unsigned char wide[] = { 255, 250, 31, 0, 255, 255, 0, 33, 255, 240 };
    static const unsigned char zero[] = { 255, 250, 31, 0, 0, 0, 0, 255, 240 };
    static const unsigned char other[] = { 255, 250, 24, 1, 255, 240, 'm' };
    static const unsigned char longsb[64 + 6] = { 255, 250, 39 };
    static const unsigned char broken[] = { 255, 250, 31, 0, 80, 255, 244, 'r' };
    unsigned char big[sizeof(longsb)];

    recv_all(&t, naws, sizeof(naws));
    check(t.cols == 120 && t.rows == 40 && t.resized && is(&D, "k", 1), "NAWS 120x40, then data");
    t.resized = 0;
    recv_all(&t, naws, sizeof(naws));
    check(!t.resized, "the same size again: no resize");
    recv_all(&t, wide, sizeof(wide));
    check(t.cols == 255 && t.rows == 33 && t.resized, "NAWS with IAC IAC inside: 255 columns");
    t.resized = 0;
    recv_all(&t, zero, sizeof(zero));
    check(!t.resized && t.cols == 255, "NAWS 0x0 (the client does not know): ignored");
    recv_all(&t, other, sizeof(other));
    check(is(&D, "m", 1) && R.len == 0, "another option's sub-negotiation: ignored");
    memcpy(big, longsb, sizeof(big));
    memset(big + 3, 'x', sizeof(big) - 5);
    big[sizeof(big) - 2] = 255;
    big[sizeof(big) - 1] = 240;
    recv_all(&t, big, sizeof(big));
    check(D.len == 0 && t.state == 0, "an overlong sub-negotiation: dropped, the reader back at data");
    recv_all(&t, broken, sizeof(broken));
    check(is(&D, "\003r", 2) && t.cols == 255, "a sub-negotiation cut short by IAC IP: dropped, Ctrl+C counts");
}

/* everything at once, then the same in reads of every size: the same data and answers */
static void test_splits(void) {
    static const unsigned char in[] = {
        255, 253, 1, 255, 253, 3, 'h', 'i', '\r', 0, 255, 251, 31, 255, 250, 31, 0, 100, 0, 30, 255, 240,
        255, 255, 'x', '\r', '\n', 255, 253, 24, 255, 244, 255, 246, 'e', 'n', 'd', '\r'
    };
    unsigned char whole_d[256], whole_r[256];
    int wd, wr, step, ok = 1;
    struct telnet t = fresh();
    recv_all(&t, in, sizeof(in));
    memcpy(whole_d, data, (size_t)D.len);
    memcpy(whole_r, reply, (size_t)R.len);
    wd = D.len;
    wr = R.len;
    check(t.cols == 100 && t.rows == 30, "the mixed stream: NAWS 100x30");
    for(step = 1; step < (int)sizeof(in); step++) {
        struct telnet u = fresh();
        recv_split(&u, in, sizeof(in), step);
        if(!is(&D, whole_d, wd) || !is(&R, whole_r, wr) || u.cols != 100 || u.rows != 30) {
            ok = 0;
            printf("  reads of %d bytes differ\n", step);
        }
    }
    check(ok, "a stream split into reads of 1, 2, ... bytes: the same data, answers and size");
}

static void test_full_reply(void) {
    struct telnet t = fresh();
    unsigned char in[300], small[4];
    struct tn_buf d = { data, 0, sizeof(data) }, r = { small, 0, sizeof(small) };
    int i;
    for(i = 0; i < 100; i++) {                    /* 100 DO TERMINAL-TYPE: 300 bytes of WONT */
        in[3 * i] = 255;
        in[3 * i + 1] = 253;
        in[3 * i + 2] = 24;
    }
    telnet_recv(&t, in, sizeof(in), &d, &r);
    check(r.len == sizeof(small) && d.len == 0 && t.state == 0, "answers beyond the room are dropped, the reader goes on");
}

static void test_output(void) {
    struct telnet t = fresh();
    unsigned char out[64];
    static const unsigned char ff[] = { 'a', 255, 'b' }, ff_want[] = { 'a', 255, 255, 'b' };
    int n;
    n = telnet_send(&t, (const unsigned char *)"one\ntwo\r\nthree\r", 15, out);
    check(n == 16 && !memcmp(out, "one\r\ntwo\r\nthree\r", 16), "LF as CR LF; a CR LF stays one; a CR alone stays");
    n = telnet_send(&t, (const unsigned char *)"\nx", 2, out);
    check(n == 2 && !memcmp(out, "\nx", 2), "a CR at the end of one write and the LF at the start of the next: one CR");
    n = telnet_send(&t, ff, sizeof(ff), out);
    check(n == 4 && !memcmp(out, ff_want, 4), "the byte 255 as IAC IAC");
    n = telnet_send(&t, (const unsigned char *)"\n\n", 2, out);
    check(n == 4 && !memcmp(out, "\r\n\r\n", 4), "two LFs: two CR LFs");
    n = telnet_send(&t, (const unsigned char *)"\xe2\x89\x8b>", 4, out);
    check(n == 4 && !memcmp(out, "\xe2\x89\x8b>", 4), "UTF-8 (the wave) passes as it is");
}

/* random bytes, IAC-heavy: data never longer than what came, answers within their room,
   and afterwards the reader still reads plain data */
static void test_random(void) {
    static const unsigned char pool[] = { 255, 255, 255, 250, 240, 251, 252, 253, 254, 31, 1, 3, 24, 13, 10, 0, 'a', 244 };
    unsigned int seed = 12345;
    int round, ok = 1;
    for(round = 0; round < 20000; round++) {
        struct telnet t = fresh();
        unsigned char in[64], small[16];
        struct tn_buf d = { data, 0, sizeof(data) }, r = { small, 0, sizeof(small) };
        int n, i;
        seed = seed * 1103515245 + 12345;
        n = (int)(seed >> 16) % 64;
        for(i = 0; i < n; i++) {
            seed = seed * 1103515245 + 12345;
            in[i] = pool[(seed >> 16) % sizeof(pool)];
        }
        telnet_recv(&t, in, n, &d, &r);
        if(d.len > n || r.len > (int)sizeof(small)) ok = 0;
        recv_all(&t, (const unsigned char *)"\377\360\377\361ok", 6);   /* IAC SE, IAC NOP: whatever state, back to data */
        if(D.len < 2 || memcmp(data + D.len - 2, "ok", 2)) ok = 0;
    }
    check(ok, "20000 random streams: data within its input, answers within their room, the reader recovers");
}

int main(void) {
    test_opening();
    test_answers();
    test_refusals_of_the_opening();
    test_no_loop();
    test_data();
    test_naws();
    test_splits();
    test_full_reply();
    test_output();
    test_random();
    if(failures) {
        printf("TELNET UNIT TESTS FAILED: %d of %d checks\n", failures, checks);
        return 1;
    }
    printf("TELNET UNIT TESTS PASSED: %d checks, 0 failures\n", checks);
    return 0;
}
