/* Host-side tests of the telnet client (11/01/26). user/telnet/tnclient.c, the protocol, alone: this end's offers,
   and silence on another port until the server negotiates; options answered once and never in a loop (RFC 1143),
   ECHO refused on this end; NAWS, 255 doubled; commands and sub-negotiations split at every byte; CR NUL and NUL, in
   and out of binary mode; what a key sends and shows, a key at a time and a line at a time; what the terminal is left
   with. Then against the server's own module, user/telnetd/telnet.c, as it is: the negotiation in either order, at
   every split, ends and agrees; a window and keys get across. Then random bytes from a server, with exact buffers.
   Last the program whole, user/telnet/session.c and main.c as they are on a simulated kernel: a session, Ctrl+]'s
   prompt, a server that closes, resets, stops reading or floods, a pipe's input, the window changing, and the terminal
   put back on every way out. Run by `make unittest`, and with clang's sanitizers on the Mac (the account says how). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <errno.h>
#include <poll.h>
#include "tnclient.h"
#include "telnetc/sim.h"

int telnet_main(int argc, char **argv);         /* user/telnet/main.c, built with -Dmain=telnet_main */

static int failures, checks;

static void check(int ok, const char *what) {
    checks++;
    if(!ok) {
        failures++;
        printf("FAIL: %s\n", what);
    }
}

/**** the module alone ****/

static unsigned char term_buf[65536], net_buf[65536], echo_buf[65536];
static struct tn_buf T, N, E;

static void clear_bufs(void) {
    T.p = term_buf;
    T.len = 0;
    T.size = sizeof(term_buf);
    N.p = net_buf;
    N.len = 0;
    N.size = sizeof(net_buf);
    E.p = echo_buf;
    E.len = 0;
    E.size = sizeof(echo_buf);
}

static void recv_(struct tnc *t, const void *p, int n) {
    clear_bufs();
    tnc_recv(t, (const unsigned char *)p, n, &T, &N);
}

static void keys_(struct tnc *t, const void *p, int n) {
    clear_bufs();
    tnc_keys(t, (const unsigned char *)p, n, &N, &E);
}

#define RECV(t, s) recv_(t, s, (int)sizeof(s) - 1)
#define KEYS(t, s) keys_(t, s, (int)sizeof(s) - 1)
#define SAID(b, s) ((b).len == (int)sizeof(s) - 1 && !memcmp((b).p, s, sizeof(s) - 1))
#define IAC "\xff"
#define WILL "\xfb"
#define WONT "\xfc"
#define DO "\xfd"
#define DONT "\xfe"
#define SB "\xfa"
#define SE "\xf0"

/* a client that has agreed to everything a good server asks */
static void agreed(struct tnc *t, int cols, int rows) {
    tnc_init(t, cols, rows, 1);
    RECV(t, IAC WILL "\x01" IAC WILL "\x03" IAC DO "\x03" IAC DO "\x1f" IAC DO "\x00");
}

static void test_options(void) {
    struct tnc t;
    tnc_init(&t, 80, 25, 1);
    clear_bufs();
    tnc_opening(&t, &N);
    check(SAID(N, IAC DO "\x03" IAC DO "\x01" IAC DO "\x00" IAC WILL "\x00" IAC WILL "\x1f"), "the offers on port 23: DO SGA ECHO BINARY, WILL BINARY NAWS");
    check(N.len <= TNC_OPENING, "... in the room said");
    clear_bufs();
    tnc_opening(&t, &N);
    check(!N.len, "made once");
    RECV(&t, IAC WILL "\x03" IAC WILL "\x01");
    check(!N.len && tnc_by_key(&t) && tnc_remote_echo(&t), "the answers to our own offers get no answer");
    RECV(&t, IAC DO "\x1f");
    check(SAID(N, IAC SB "\x1f\x00\x50\x00\x19" IAC SE), "the server's DO NAWS after our WILL: the window, no WILL again");
    RECV(&t, IAC DO "\x1f");
    check(!N.len, "asked again: nothing changes, nothing is said");

    tnc_init(&t, 80, 25, 1);
    RECV(&t, "hello\r\n");
    check(!N.len && !t.speaks && SAID(T, "hello\r\n"), "on another port this end says nothing while the server speaks no telnet");
    RECV(&t, IAC IAC);
    check(!t.speaks && SAID(T, IAC), "a byte 255 is no negotiation");
    RECV(&t, IAC WILL "\x01");
    check(t.speaks && tnc_remote_echo(&t), "the server's first negotiation");
    check(SAID(N, IAC DO "\x03" IAC DO "\x01" IAC DO "\x00" IAC WILL "\x00" IAC WILL "\x1f"),
          "... brings this end's offers; DO ECHO is among them, so WILL ECHO gets no second answer");

    tnc_init(&t, 80, 25, 1);
    t.speaks = 1;
    RECV(&t, IAC DO "\x01");
    check(SAID(N, IAC WONT "\x01"), "this end never echoes");
    RECV(&t, IAC DO "\x01");
    check(SAID(N, IAC WONT "\x01"), "... each time it is asked");
    RECV(&t, IAC WILL "\x63" IAC DO "\x63" IAC DO "\x18");
    check(SAID(N, IAC DONT "\x63" IAC WONT "\x63" IAC WONT "\x18"), "anything else is refused, the terminal type too");
    RECV(&t, IAC WONT "\x63" IAC DONT "\x63");
    check(!N.len, "a refusal of what is off gets no answer");
    RECV(&t, IAC WILL "\x01");
    check(SAID(N, IAC DO "\x01") && tnc_remote_echo(&t), "WILL ECHO: yes");
    RECV(&t, IAC WONT "\x01");
    check(SAID(N, IAC DONT "\x01") && !tnc_remote_echo(&t), "WONT ECHO: the echo is this end's again");
    RECV(&t, IAC WILL "\x00" IAC DO "\x00");
    check(SAID(N, IAC DO "\x00" IAC WILL "\x00"), "BINARY both ways");

    {   /* a server that says one thing and then the other, without end: one answer for each, no more */
        int i;
        tnc_init(&t, 80, 25, 1);
        t.speaks = 1;
        for(i = 0; i < 1000; i++) {
            RECV(&t, IAC WILL "\x01");
            check(N.len == 3, "toggling: an answer for a change");
            RECV(&t, IAC WONT "\x01");
            check(N.len == 3, "toggling: and for the change back");
        }
    }
}

static void test_naws(void) {
    struct tnc t;
    agreed(&t, 80, 25);
    check(t.told_cols == 80 && t.told_rows == 25, "the window, told");
    clear_bufs();
    tnc_size(&t, 80, 25, &N);
    check(!N.len, "the same size: nothing");
    tnc_size(&t, 132, 43, &N);
    check(SAID(N, IAC SB "\x1f\x00\x84\x00\x2b" IAC SE), "a new size");
    clear_bufs();
    tnc_size(&t, 255, 511, &N);
    check(SAID(N, IAC SB "\x1f\x00\xff\xff\x01\xff\xff" IAC SE), "a byte 255 in it is doubled");
    clear_bufs();
    tnc_size(&t, 65535, 65535, &N);
    check(N.len == TNC_NAWS_MAX, "the longest it can be");
    clear_bufs();
    tnc_size(&t, 100000, 100000, &N);
    check(!N.len && t.cols == 65535, "more than 16 bits holds: the most it can say");

    tnc_init(&t, 80, 25, 1);
    clear_bufs();
    tnc_size(&t, 100, 30, &N);
    check(!N.len, "before NAWS is agreed: not said");
    RECV(&t, IAC DO "\x1f");
    check(N.len >= 12 && !memcmp(N.p + N.len - 9, IAC SB "\x1f\x00\x64\x00\x1e" IAC SE, 9), "said when it is, with the size of then");

    agreed(&t, 80, 25);
    RECV(&t, IAC DONT "\x1f");
    check(SAID(N, IAC WONT "\x1f"), "DONT NAWS");
    clear_bufs();
    tnc_size(&t, 100, 30, &N);
    check(!N.len, "and it is not said any more");
    RECV(&t, IAC DO "\x1f");
    check(N.len == 12 && !memcmp(N.p, IAC WILL "\x1f" IAC SB "\x1f\x00\x64\x00\x1e" IAC SE, 12), "asked again, it is said again");

    tnc_init(&t, 0, 0, 0);
    RECV(&t, IAC DO "\x1f");
    check(N.len > 0 && !strstr((char *)N.p, IAC SB), "no window known: WILL, and nothing to tell");

    {
        unsigned char small[6];
        struct tn_buf b;
        agreed(&t, 80, 25);
        b.p = small;
        b.len = 0;
        b.size = sizeof(small);
        tnc_size(&t, 100, 30, &b);
        check(!b.len && t.told_cols == 80, "no room for the whole of it: nothing goes, and it stays to be told");
    }
}

static void test_reader(void) {
    struct tnc t;
    static const unsigned char whole[] = "a\r\0b\r\nc" IAC IAC "d" IAC "\xf1" "e" IAC SB "\x18\x01zz" IAC IAC "zz" IAC SE "f" IAC WILL "\x03" "g\0h";
    int step;
    tnc_init(&t, 80, 25, 1);
    RECV(&t, "a\r\0b");
    check(SAID(T, "a\rb"), "CR NUL: a CR alone");
    RECV(&t, "a\0b");
    check(SAID(T, "ab"), "a NUL alone is NVT's no-op");
    RECV(&t, "a\r\nb");
    check(SAID(T, "a\r\nb"), "CR LF stays, for the terminal");
    RECV(&t, IAC IAC);
    check(SAID(T, IAC), "IAC IAC");
    RECV(&t, "\xc3\xa9");
    check(SAID(T, "\xc3\xa9"), "8 bits pass, in or out of binary mode");
    RECV(&t, IAC "\xf6");
    check(!N.len && !T.len, "IAC AYT from a server: nothing is said back");
    RECV(&t, IAC "\xf4" IAC "\xf3" IAC "\xf5" IAC "\xf7" IAC "\xf8" IAC "\xf9" IAC "\xf1" IAC "\xf2" IAC SE);
    check(!N.len && !T.len, "IP, BRK, AO, EC, EL, GA, NOP, DM and a stray SE change nothing");

    t.speaks = 1;
    RECV(&t, IAC SB "\x18\x01" IAC SE "x");
    check(SAID(T, "x") && !N.len, "a sub-negotiation is dropped");
    RECV(&t, IAC SB "\x18" "a" IAC "\xf1" "b");
    check(SAID(T, "b"), "one cut short by a command: that command counts, data goes on");
    {
        static unsigned char big[70000];
        memset(big, 'z', sizeof(big));
        memcpy(big, IAC SB "\x18", 3);
        memcpy(big + sizeof(big) - 3, IAC SE "y", 3);
        recv_(&t, big, sizeof(big));
        check(SAID(T, "y"), "one of 70000 bytes: read to its end, nothing kept");
    }

    RECV(&t, IAC WILL "\x00");
    RECV(&t, "a\r\0b\0c\r\n");
    check(T.len == 8 && !memcmp(T.p, "a\r\0b\0c\r\n", 8), "in binary mode every byte is data");
    RECV(&t, IAC IAC);
    check(SAID(T, IAC), "... IAC IAC still one byte");
    RECV(&t, IAC WONT "\x00");
    RECV(&t, "\r\0");
    check(SAID(T, "\r"), "out of binary mode again: CR NUL");

    /* the same bytes in pieces of every size: the same data, and the same answers */
    for(step = 1; step < (int)sizeof(whole); step++) {
        unsigned char out[64], ans[64];
        int off, o = 0, a = 0, n = (int)sizeof(whole) - 1;
        tnc_init(&t, 80, 25, 1);
        t.speaks = 1;
        for(off = 0; off < n; off += step) {
            struct tn_buf d, r;
            d.p = out + o;
            d.len = 0;
            d.size = (int)sizeof(out) - o;
            r.p = ans + a;
            r.len = 0;
            r.size = (int)sizeof(ans) - a;
            tnc_recv(&t, whole + off, n - off < step ? n - off : step, &d, &r);
            o += d.len;
            a += r.len;
        }
        check(o == 11 && !memcmp(out, "a\rb\r\nc\xff" "defg", 11) ? 1 : (o == 12 && !memcmp(out, "a\rb\r\nc\xff" "defgh", 12)), "split at every byte: the same data");
        check(a == 3 && !memcmp(ans, IAC DO "\x03", 3), "... and the same answer");
    }
}

static void test_keys(void) {
    struct tnc t;
    agreed(&t, 80, 25);                         /* the server echoes, keys one at a time, this end sends in binary */
    check(t.us[TO_BINARY] == TN_YES, "agreed");
    KEYS(&t, "ab");
    check(SAID(N, "ab") && !E.len, "keys go as they are typed, and the server shows them");
    KEYS(&t, "\r");
    check(SAID(N, "\r"), "Return in binary mode: a CR alone");
    KEYS(&t, "\n");
    check(!N.len, "an LF right after a CR is that Return's end");
    KEYS(&t, "\n");
    check(SAID(N, "\r"), "an LF alone is a Return");
    KEYS(&t, "\xff");
    check(SAID(N, IAC IAC), "a byte 255 is doubled");
    KEYS(&t, "\x03\x1b[A\x7f\t");
    check(SAID(N, "\x03\x1b[A\x7f\t"), "Ctrl+C, an arrow's sequence, Backspace and Tab go as they are");
    RECV(&t, IAC DONT "\x00");
    KEYS(&t, "\r");
    check(SAID(N, "\r\n"), "out of binary mode Return is CR LF");
    {
        int used;
        clear_bufs();
        used = tnc_keys(&t, (const unsigned char *)"ab\x1d" "cd", 5, &N, &E);
        check(used == 3 && t.escaped && SAID(N, "ab"), "Ctrl+] stops there, and is never sent");
        t.escaped = 0;
        tnc_init(&t, 80, 25, 0);
        RECV(&t, IAC WILL "\x03");
        clear_bufs();
        used = tnc_keys(&t, (const unsigned char *)"ab\x1d" "cd", 5, &N, &E);
        check(used == 5 && !t.escaped && N.len == 5, "from a pipe it is a byte like another");
    }

    /* the server does not echo: this end shows what is typed */
    tnc_init(&t, 80, 25, 1);
    RECV(&t, IAC WILL "\x03");
    check(tnc_by_key(&t) && !tnc_remote_echo(&t), "one at a time, with no echo from the server");
    KEYS(&t, "a\xc3\xa9\r");
    check(SAID(E, "a\xc3\xa9\r\n"), "... shown here");
    KEYS(&t, "\x7f");
    check(SAID(E, "\b \b") && SAID(N, "\x7f"), "... Backspace too");
    KEYS(&t, "\x03\x1b");
    check(!E.len, "... but no control characters");
}

static void test_line_mode(void) {
    struct tnc t;
    int i;
    static unsigned char big[3000];
    tnc_init(&t, 80, 25, 1);
    KEYS(&t, "abc");
    check(!N.len && SAID(E, "abc"), "no agreement: the line waits here, and is shown here");
    KEYS(&t, "\x7f");
    check(!N.len && SAID(E, "\b \b"), "Backspace edits it");
    KEYS(&t, "d\r");
    check(SAID(N, "abd\r\n") && SAID(E, "d\r\n"), "Return sends it, with CR LF");
    KEYS(&t, "\n");
    check(!N.len, "an LF right after that CR is nothing");
    KEYS(&t, "\xc3\xa9\xe2\x89\x8b\xf0\x9f\x8f\x84" "\x7f\x7f\x7f\r");
    check(SAID(N, "\r\n"), "Backspace takes a whole character, whatever its bytes");
    KEYS(&t, "\xff\x7fz\xff\r");
    check(SAID(N, "z" IAC IAC "\r\n"), "an invalid byte counts one; a byte 255 is doubled when it goes");
    KEYS(&t, "abc\x15x\r");
    check(SAID(N, "x\r\n"), "Ctrl+U clears it");
    KEYS(&t, "a\x1b[Ab\x1b[1;5Cc\x1bOd\r");
    check(SAID(N, "abcd\r\n"), "a special key's sequence would be text to the server: dropped");
    KEYS(&t, "a\x01\x04\tb\r");
    check(SAID(N, "a\tb\r\n"), "other controls are dropped, a Tab kept");
    KEYS(&t, "abc\x03" "d\r");
    check(SAID(N, "d\r\n") && !t.speaks, "Ctrl+C clears the line; to a server that speaks no telnet nothing more goes");
    t.speaks = 1;
    KEYS(&t, "abc\x03");
    check(SAID(N, IAC "\xf4"), "to one that does: IAC IP");

    /* a line that fills the room goes out in pieces, none lost */
    memset(big, 'k', sizeof(big));
    tnc_init(&t, 80, 25, 1);
    keys_(&t, big, sizeof(big));
    check(N.len == 2 * TNC_LINE_MAX && t.edit.len == 3000 - 2 * TNC_LINE_MAX, "a line past the room goes in pieces");
    KEYS(&t, "\r");
    check(N.len == 3000 - 2 * TNC_LINE_MAX + 2, "and its end with Return");

    /* the agreement comes while a line waits */
    tnc_init(&t, 80, 25, 1);
    KEYS(&t, "ab");
    RECV(&t, IAC WILL "\x03");
    check(N.len >= 2 && !memcmp(N.p + N.len - 2, "ab", 2) && !t.edit.len, "the agreement sends what waited");
    KEYS(&t, "c");
    check(SAID(N, "c"), "and keys go one at a time from then on");

    /* the server echoes but wants lines */
    tnc_init(&t, 80, 25, 1);
    RECV(&t, IAC WILL "\x01");
    KEYS(&t, "a\r");
    check(SAID(N, "a\r\n") && !E.len, "lines, with the server's echo: nothing is shown here");

    /* an edit, as the prompt uses it */
    {
        struct tnc_edit e;
        memset(&e, 0, sizeof(e));
        for(i = 0; i < TNC_LINE_MAX - 1; i++) check(tnc_edit_key(&e, 'k', NULL) == TNC_E_MORE, "room");
        check(tnc_edit_key(&e, 'k', NULL) == TNC_E_FULL && e.len == TNC_LINE_MAX, "full, and the key kept");
        memset(&e, 0, sizeof(e));
        check(tnc_edit_key(&e, '\r', NULL) == TNC_E_LINE && tnc_edit_key(&e, '\n', NULL) == TNC_E_MORE, "Return, and the LF of a CR LF");
        check(tnc_edit_key(&e, 0x7f, NULL) == TNC_E_MORE, "Backspace with nothing there");
    }
}

static void test_restore(void) {
    static const char end[] = "\x1b\\\x1b[m\x1b[?25h\x1b" "7\x1b[?1049l";
    struct tnc t;
    char undo[TNC_RESTORE_MAX];
    int n, e = (int)sizeof(end) - 1;
    tnc_init(&t, 80, 25, 1);
    RECV(&t, "plain text\r\n");
    n = tnc_restore(&t, undo);
    check(n == e && !memcmp(undo, end, (size_t)e), "the end is one and the same, whatever came: strings ended, then the screen set");
    RECV(&t, "no line end");
    n = tnc_restore(&t, undo);
    check(n == e + 2 && !memcmp(undo, end, (size_t)e) && !memcmp(undo + e, "\r\n", 2), "a line not ended: the next words start on their own");
    check(n <= TNC_RESTORE_MAX, "all of it fits");
    RECV(&t, "\x1b]0;a title that goes on and on");
    n = tnc_restore(&t, undo);
    check(n == e + 2, "what the server left open changes nothing in it");
    check(tnc_restore(&t, undo) == e, "and it counts as a line end from then on");
}

/**** against the server's own module ****/

#include "../../user/telnetd/telnet.h"

/* the two ends, joined; step: the bytes a read takes. Returns how many bytes crossed */
static long converse(struct tnc *c, struct telnet *s, unsigned char *a, int *alen, unsigned char *b, int *blen, int step,
                     unsigned char *to_term, int *tlen, unsigned char *to_pty, int *plen) {
    long crossed = 0;
    int rounds = 0;
    while((*alen || *blen) && rounds++ < 10000) {
        if(*alen) {                             /* client to server */
            int n = *alen < step ? *alen : step;
            struct tn_buf d, r;
            d.p = to_pty + *plen;
            d.len = 0;
            d.size = 4096 - *plen;
            r.p = b + *blen;
            r.len = 0;
            r.size = 4096 - *blen;
            telnet_recv(s, a, n, &d, &r);
            *plen += d.len;
            *blen += r.len;
            memmove(a, a + n, (size_t)(*alen - n));
            *alen -= n;
            crossed += n;
        }
        if(*blen) {                             /* server to client */
            int n = *blen < step ? *blen : step;
            struct tn_buf d, r;
            d.p = to_term + *tlen;
            d.len = 0;
            d.size = 4096 - *tlen;
            r.p = a + *alen;
            r.len = 0;
            r.size = 4096 - *alen;
            tnc_recv(c, b, n, &d, &r);
            *tlen += d.len;
            *alen += r.len;
            memmove(b, b + n, (size_t)(*blen - n));
            *blen -= n;
            crossed += n;
        }
    }
    check(rounds < 10000, "the negotiation ends");
    return crossed;
}

static void test_against_server(void) {
    static unsigned char a[4096], b[4096], term[4096], pty[4096];
    int step, first;
    for(first = 0; first < 2; first++) {        /* the client speaks first (port 23), or waits for the server */
        for(step = 1; step < 40; step++) {
            struct tnc c;
            struct telnet s;
            struct tn_buf t;
            int alen = 0, blen = 0, tlen = 0, plen = 0;
            long crossed;
            tnc_init(&c, 100, 37, 1);
            telnet_init(&s);
            if(first) {
                t.p = a;
                t.len = 0;
                t.size = sizeof(a);
                tnc_opening(&c, &t);
                alen = t.len;
            }
            blen = telnet_opening(&s, b);
            crossed = converse(&c, &s, a, &alen, b, &blen, step, term, &tlen, pty, &plen);
            check(crossed < 200, "a few bytes settle it");
            check(!tlen && !plen, "no negotiation reaches the terminal or the shell as data");
            check(tnc_remote_echo(&c) && tnc_by_key(&c), "the server echoes, keys one at a time");
            check(c.us[TO_BINARY] == TN_YES && c.him[TO_BINARY] == TN_NO, "this end sends 8 bits; the server's output stays NVT, as it says");
            check(s.cols == 100 && s.rows == 37, "the window reached the server");
            check(!c.edit.len, "no line waits");

            /* keys across: Return a CR, 8 bits whole, 255 as 255 */
            t.p = a;
            t.len = 0;
            t.size = sizeof(a);
            {
                struct tn_buf e;
                static unsigned char shown[256];
                e.p = shown;
                e.len = 0;
                e.size = sizeof(shown);
                tnc_keys(&c, (const unsigned char *)"ls \xc3\xa9\xff\r", 7, &t, &e);
                alen = t.len;
                check(!e.len, "nothing shown here");
            }
            plen = 0;
            converse(&c, &s, a, &alen, b, &blen, step, term, &tlen, pty, &plen);
            check(plen == 7 && !memcmp(pty, "ls \xc3\xa9\xff\r", 7), "keys arrive as typed");

            /* a new window, what the server prints, IAC AYT */
            t.p = a;
            t.len = 0;
            t.size = sizeof(a);
            tnc_size(&c, 120, 33, &t);
            tnc_ayt(&c, &t);
            alen = t.len;
            tlen = 0;
            converse(&c, &s, a, &alen, b, &blen, step, term, &tlen, pty, &plen);
            check(s.cols == 120 && s.rows == 33 && s.resized, "a new window");
            check(tlen == (int)sizeof(TN_AYT_ANSWER) - 1 && !memcmp(term, TN_AYT_ANSWER, sizeof(TN_AYT_ANSWER) - 1), "AYT answered, as data");
            {
                static const unsigned char out[] = "one\ntwo\rthree\xff\n";
                blen = telnet_send(&s, out, (int)sizeof(out) - 1, b);
                tlen = 0;
                converse(&c, &s, a, &alen, b, &blen, step, term, &tlen, pty, &plen);
                check(tlen == 17 && !memcmp(term, "one\r\ntwo\rthree\xff\r\n", 17), "what the server prints reaches the terminal");
            }
        }
    }
}

/**** random bytes from a server ****/

static unsigned int seed;

static unsigned int rnd(unsigned int n) {
    seed = seed * 1103515245u + 12345u;
    return (seed >> 16) % n;
}

static void test_random(int rounds) {
    int round;
    for(round = 0; round < rounds; round++) {
        struct tnc t;
        int pieces = 1 + (int)rnd(40), i;
        seed = (unsigned int)round * 2654435761u + 7;
        tnc_init(&t, 80 + (int)rnd(300), 25 + (int)rnd(300), (int)rnd(2));
        if(rnd(2)) {
            unsigned char *o = malloc(TNC_OPENING);
            struct tn_buf b;
            b.p = o;
            b.len = 0;
            b.size = TNC_OPENING;
            tnc_opening(&t, &b);
            check(b.len <= TNC_OPENING, "the offers fit");
            free(o);
        }
        for(i = 0; i < pieces; i++) {
            int n = 1 + (int)rnd(200), k;
            unsigned char *in = malloc((size_t)n);
            unsigned char *term = malloc((size_t)n);        /* exact: a byte past the room is caught on the Mac */
            int room = TNC_ROOM(n);
            unsigned char *net = malloc((size_t)room);
            struct tn_buf d, r;
            for(k = 0; k < n; k++) {
                unsigned int w = rnd(10);
                in[k] = w < 4 ? (unsigned char)(0xf0 + rnd(16)) : w < 6 ? (unsigned char)rnd(40) : (unsigned char)rnd(256);
            }
            d.p = term;
            d.len = 0;
            d.size = n;
            r.p = net;
            r.len = 0;
            r.size = room;
            tnc_recv(&t, in, n, &d, &r);
            check(d.len <= n, "never more for the terminal than came");
            check(r.len <= room, "the answers fit TNC_ROOM");
            if(rnd(2)) {                                    /* keys meanwhile */
                int kn = 1 + (int)rnd(60), used;
                int nroom = TNC_KEYS_ROOM(kn), eroom = TNC_ECHO_ROOM(kn);
                unsigned char *ks = malloc((size_t)kn), *kout = malloc((size_t)nroom), *eout = malloc((size_t)eroom);
                struct tn_buf kb, eb;
                for(k = 0; k < kn; k++) ks[k] = (unsigned char)rnd(256);
                kb.p = kout;
                kb.len = 0;
                kb.size = nroom;
                eb.p = eout;
                eb.len = 0;
                eb.size = eroom;
                used = tnc_keys(&t, ks, kn, &kb, &eb);
                t.escaped = 0;
                check(used >= 1 && used <= kn && kb.len <= nroom && eb.len <= eroom, "keys fit their room");
                free(ks);
                free(kout);
                free(eout);
            }
            if(rnd(4) == 0) {
                unsigned char *s = malloc(TNC_NAWS_MAX);
                struct tn_buf b;
                b.p = s;
                b.len = 0;
                b.size = TNC_NAWS_MAX;
                tnc_size(&t, (int)rnd(70000), (int)rnd(70000), &b);
                check(b.len <= TNC_NAWS_MAX, "a window fits");
                free(s);
            }
            free(in);
            free(term);
            free(net);
        }
        {
            char *undo = malloc(TNC_RESTORE_MAX);
            check(tnc_restore(&t, undo) <= TNC_RESTORE_MAX, "the end fits");
            free(undo);
        }
        if(failures) {
            printf("  (round %d)\n", round);
            return;
        }
    }
}

/**** the program, on a simulated kernel ****/

enum { A_NONE, A_CLOSE, A_RESET, A_STALL, A_TAKE, A_KEYS_END, A_RESIZE, A_SCREEN_BROKEN };

struct step {
    const char *wait;                   /* this on the screen first; NULL: at once */
    const char *keys;                   /* then these are typed */
    int keys_len;                       /* 0: strlen */
    const char *server;                 /* and the server sends these */
    int server_len;
    int action;
    int cols, rows;                     /* A_RESIZE */
    const char *wait_net;               /* and these bytes sent to the server first */
    unsigned int wait_ms;               /* and this much time since the session began */
};

#define SOCK 100

static const struct step *steps;
static int nsteps, at_step;
static unsigned char *keyq, *inq;
static size_t key_len, key_off, in_len, in_off, key_cap, in_cap;
static int keys_ended, peer_fin, peer_rst, peer_full, sock_open;
static unsigned char to_server[1 << 20];
static size_t server_len;
static char screen_text[1 << 22];
static size_t screen_len;
static char err_text[4096];
static size_t err_len;
static int raw, was_raw, stdin_tty, connect_err, screen_broken, sizes = (80 << 16) | 25;
static unsigned int now_ms;
static long polls, idle_after;
static int last_port;
static unsigned int max_net_out;
static size_t read_while_stalled;
static size_t max_write;                /* a write to the connection queues at most this, as the kernel's may */
static int stdout_tty = 1;              /* standard output is a terminal */
static size_t flood_per_line;           /* a server that answers each line received with this much output */
static size_t flood_cap;                /* and stops reading while more than this of its output waits to be read */

static void grow(unsigned char **p, size_t *cap, size_t need) {
    if(need <= *cap) return;
    *cap = need * 2 + 256;
    *p = realloc(*p, *cap);
    if(!*p) abort();
}

static int sent_to_server(const char *text) {
    size_t i, n = strlen(text);
    for(i = 0; i + n <= server_len; i++) if(!memcmp(to_server + i, text, n)) return 1;
    return 0;
}

static void advance(void) {
    while(at_step < nsteps) {
        const struct step *s = &steps[at_step];
        if(s->wait && !strstr(screen_text, s->wait)) return;
        if(s->wait_net && !sent_to_server(s->wait_net)) return;
        if(s->wait_ms && now_ms < 1000 + s->wait_ms) return;
        at_step++;
        if(s->keys) {
            size_t n = s->keys_len ? (size_t)s->keys_len : strlen(s->keys);
            grow(&keyq, &key_cap, key_len + n);
            memcpy(keyq + key_len, s->keys, n);
            key_len += n;
        }
        if(s->server) {
            size_t n = s->server_len ? (size_t)s->server_len : strlen(s->server);
            grow(&inq, &in_cap, in_len + n);
            memcpy(inq + in_len, s->server, n);
            in_len += n;
        }
        switch(s->action) {
        case A_CLOSE: peer_fin = 1; break;
        case A_RESET: peer_rst = 1; break;
        case A_STALL: peer_full = 1; break;
        case A_TAKE:
            peer_full = 0;
            read_while_stalled = in_off;
            break;
        case A_KEYS_END: keys_ended = 1; break;
        case A_RESIZE: sizes = s->cols << 16 | s->rows; break;
        case A_SCREEN_BROKEN: screen_broken = 1; break;
        }
    }
}

int sim_poll(struct pollfd *p, unsigned int n, int timeout) {
    unsigned int i;
    int pass;
    if(++polls > 300000) {
        failures++;
        printf("FAIL: the session polls without end\n");
        printf("step %d of %d; keys %zu of %zu; server's bytes %zu of %zu; fin %d rst %d full %d; to the server %zu\n--- screen:\n%.2000s\n---\n",
               at_step, nsteps, key_off, key_len, in_off, in_len, peer_fin, peer_rst, peer_full, server_len, screen_text);
        for(i = 0; i < n; i++) printf("poll %u: fd %d events %d\n", i, p[i].fd, p[i].events);
        exit(1);
    }
    for(pass = 0; pass < 2; pass++) {
        int count = 0;
        advance();
        for(i = 0; i < n; i++) {
            int r = 0;
            p[i].revents = 0;
            if(p[i].fd == 0) {
                if((p[i].events & POLLIN) && (key_off < key_len || keys_ended)) r |= POLLIN;
            } else if(p[i].fd == SOCK && sock_open) {
                if((p[i].events & POLLIN) && (in_off < in_len || peer_fin || peer_rst)) r |= POLLIN;
                if((p[i].events & POLLOUT) && !peer_full && !peer_rst && !(flood_cap && in_len - in_off > flood_cap)) r |= POLLOUT;
                if(peer_fin || peer_rst) r |= POLLHUP;
                if(peer_rst) r |= POLLERR;
            } else r = POLLNVAL;
            p[i].revents = (short)r;
            if(r) count++;
        }
        if(count) return count;
        if(!pass) {
            if(!timeout) return 0;
            if(timeout < 0) {
                if(at_step >= nsteps) {
                    failures++;
                    printf("FAIL: a wait without a limit, with nothing to come\n");
                    printf("step %d of %d; keys %zu of %zu; server's bytes %zu of %zu; fin %d rst %d full %d ended %d; to the server %zu\n--- screen:\n%.600s\n---\n",
                           at_step, nsteps, key_off, key_len, in_off, in_len, peer_fin, peer_rst, peer_full, keys_ended, server_len, screen_text);
                    for(i = 0; i < n; i++) printf("poll %u: fd %d events %d\n", i, p[i].fd, p[i].events);
                    peer_rst = 1;               /* the test ends it, and goes on to the next */
                    return 0;
                }
                now_ms += 1000;
            } else now_ms += (unsigned int)timeout;
            if(at_step >= nsteps && ++idle_after == 200) {
                failures++;
                printf("FAIL: the session does not end\n");
                peer_rst = 1;
            }
        }
    }
    return 0;
}

ssize_t sim_read(int fd, void *buf, size_t n) {
    if(fd == 0) {
        size_t left = key_len - key_off;
        if(!left) {
            if(keys_ended) return 0;
            advance();
            left = key_len - key_off;
            if(!left) {
                if(at_step >= nsteps) return 0; /* a read of keys that will never come: the terminal has gone */
                failures++;
                printf("FAIL: the prompt reads with no key left to come first\n");
                return 0;
            }
        }
        if(n > left) n = left;
        memcpy(buf, keyq + key_off, n);
        key_off += n;
        return (ssize_t)n;
    }
    if(fd == SOCK) {
        size_t left = in_len - in_off;
        if(left) {
            if(n > left) n = left;
            memcpy(buf, inq + in_off, n);
            in_off += n;
            return (ssize_t)n;
        }
        if(peer_rst) {
            errno = ECONNRESET;
            return -1;
        }
        if(peer_fin) return 0;
        failures++;
        printf("FAIL: a read of the connection that would wait\n");
        errno = EAGAIN;
        return -1;
    }
    errno = EBADF;
    return -1;
}

ssize_t sim_write(int fd, const void *buf, size_t n) {
    if(fd == 1) {
        if(screen_broken) {
            errno = EIO;
            return -1;
        }
        if(screen_len + n + 1 < sizeof(screen_text)) {
            memcpy(screen_text + screen_len, buf, n);
            screen_len += n;
            screen_text[screen_len] = 0;
        }
        return (ssize_t)n;
    }
    if(fd == SOCK) {
        if(peer_rst) {
            errno = ECONNRESET;
            return -1;
        }
        if(peer_full || (flood_cap && in_len - in_off > flood_cap)) {
            failures++;
            printf("FAIL: a write to a connection that takes nothing\n");
            errno = EAGAIN;
            return -1;
        }
        if(max_write && n > max_write) n = max_write;
        if(n > max_net_out) max_net_out = (unsigned int)n;
        if(server_len + n < sizeof(to_server)) {
            memcpy(to_server + server_len, buf, n);
            server_len += n;
        }
        if(flood_per_line) {                    /* each line ended: a page of output back */
            size_t i;
            for(i = 0; i < n; i++) if(((const char *)buf)[i] == '\n') {
                size_t k;
                grow(&inq, &in_cap, in_len + flood_per_line);
                for(k = 0; k < flood_per_line; k++) inq[in_len + k] = (unsigned char)(k % 64 == 63 ? '\n' : 'o');
                in_len += flood_per_line;
            }
        }
        return (ssize_t)n;
    }
    errno = EBADF;
    return -1;
}

int sim_close(int fd) {
    if(fd == SOCK) sock_open = 0;
    return 0;
}

int sim_ttyctl(int op, unsigned int arg) {
    if(op == 1) {
        raw = (int)arg;
        if(arg) was_raw = 1;
        return 0;
    }
    if(op == 2) return sizes;
    return -1;
}

int sim_isatty(int fd) {
    return fd == 0 ? stdin_tty : fd == 1 ? stdout_tty : 0;
}

unsigned int sim_uptime(void) {
    return ++now_ms;
}

int sim_tcp_connect(unsigned int ip, unsigned short port) {
    (void)ip;
    last_port = port;
    if(connect_err) {
        errno = connect_err;
        return -1;
    }
    sock_open = 1;
    return SOCK;
}

int sim_net_resolve(const char *host, unsigned int *ip) {
    (void)host;
    (void)ip;
    errno = ENOENT;
    return -1;
}

int sim_ipparse(const char *s, unsigned int *ip) {
    int a, b, c, d;
    char end;
    if(sscanf(s, "%d.%d.%d.%d%c", &a, &b, &c, &d, &end) != 4) return -1;
    *ip = (unsigned int)a | (unsigned int)b << 8 | (unsigned int)c << 16 | (unsigned int)d << 24;
    return 0;
}

char *sim_ipstr(unsigned int ip, char buf[16]) {
    snprintf(buf, 16, "%u.%u.%u.%u", ip & 255, ip >> 8 & 255, ip >> 16 & 255, ip >> 24);
    return buf;
}

int sim_fprintf(FILE *f, const char *fmt, ...) {
    char text[1024];
    va_list ap;
    int n;
    (void)f;
    va_start(ap, fmt);
    n = vsnprintf(text, sizeof(text), fmt, ap);
    va_end(ap);
    if(n > 0 && err_len + (size_t)n + 1 < sizeof(err_text)) {
        memcpy(err_text + err_len, text, (size_t)n);
        err_len += (size_t)n;
        err_text[err_len] = 0;
    }
    return n;
}

int sim_fflush(FILE *f) {
    (void)f;
    return 0;
}

static int run(const char *args, const struct step *s, int n, int terminal) {
    char copy[128], *argv[8], *p;
    int argc = 0;
    steps = s;
    nsteps = n;
    at_step = 0;
    key_len = key_off = in_len = in_off = 0;
    keys_ended = peer_fin = peer_rst = peer_full = sock_open = 0;
    server_len = screen_len = err_len = 0;
    screen_text[0] = err_text[0] = 0;
    raw = was_raw = 0;
    stdin_tty = terminal;
    screen_broken = 0;
    now_ms = 1000;
    polls = idle_after = 0;
    max_net_out = 0;
    argv[argc++] = "telnet";
    snprintf(copy, sizeof(copy), "%s", args);
    for(p = strtok(copy, " "); p && argc < 7; p = strtok(NULL, " ")) argv[argc++] = p;
    argv[argc] = NULL;
    argc = telnet_main(argc, argv);
    check(!raw, "raw mode has ended, whatever way the session ended");
    check(!sock_open, "the connection is closed");
    return argc;
}

static int on_screen(const char *s) {
    return strstr(screen_text, s) != NULL;
}

static int said_err(const char *s) {
    return strstr(err_text, s) != NULL;
}

static int server_got(const void *p, size_t n) {
    size_t i;
    for(i = 0; i + n <= server_len; i++) if(!memcmp(to_server + i, p, n)) return 1;
    return 0;
}

#define NSTEPS(s) ((int)(sizeof(s) / sizeof(s[0])))
#define GOOD_SERVER IAC WILL "\x01" IAC WILL "\x03" IAC DO "\x03" IAC DO "\x1f" IAC DO "\x00"
#define BYTES(s) s, (int)sizeof(s) - 1  /* a server's bytes and their count: BINARY is option 0, a NUL */

static void test_program(void) {
    int r;
    {
        static const struct step s[] = {
            { NULL, 0, 0, BYTES(GOOD_SERVER "\r\nlogin shell> "), 0, 0, 0 },
            { "shell> ", "ls\r", 0, "ls\r\na.txt\r\nshell> ", 0, 0, 0, 0 },
            { "a.txt", "\x1dstatus\r", 0, 0, 0, 0, 0, 0 },
            { "bytes received", "\x1dquit\r", 0, 0, 0, 0, 0, 0 },
        };
        r = run("10.0.2.2", s, NSTEPS(s), 1);
        check(r == 0 && was_raw && last_port == 23, "a session on port 23, quit at the prompt: status 0");
        check(server_got(IAC DO "\x03" IAC DO "\x01", 6), "... this end made its offers");
        check(server_got("ls\r", 3) && !server_got("\x1d", 1), "... the keys went, Ctrl+] did not");
        check(on_screen("telnet> ") && on_screen("The other end echoes") && on_screen("one at a time") && on_screen("80 by 25"), "... status said how things stand");
        check(said_err("connected to 10.0.2.2 port 23; Ctrl+] gives a prompt"), "... and how to get out");
    }
    {
        static const struct step s[] = {
            { NULL, 0, 0, "220 smtp here\r\n", 0, 0, 0, 0 },
            { "smtp here", "HELO x\r", 0, 0, 0, 0, 0, 0 },
            { NULL, 0, 0, 0, 0, A_CLOSE, 0, 0, "HELO x\r\n" },
        };
        r = run("10.0.2.2 2525", s, NSTEPS(s), 1);
        check(r == 0 && last_port == 2525 && said_err("the other end closed"), "another port, and the other end closes: status 0");
        check(server_len == 8 && !memcmp(to_server, "HELO x\r\n", 8), "... not a byte but the line typed goes to a server that speaks no telnet");
        check(on_screen("HELO x\r\n"), "... and this end shows what is typed");
    }
    {
        static const struct step s[] = {
            { NULL, 0, 0, BYTES(GOOD_SERVER "x"), A_NONE, 0, 0 },
            { "x", 0, 0, "\x1b[?1049h\x1b[?25l\x1b[31mscreen", 0, 0, 0, 0 },
            { "screen", 0, 0, 0, 0, A_CLOSE, 0, 0 },
        };
        r = run("10.0.2.2", s, NSTEPS(s), 1);
        check(r == 0 && on_screen("\x1b\\\x1b[m\x1b[?25h\x1b" "7\x1b[?1049l"), "the server leaves full-screen and closes: the terminal is put back");
        check(said_err("closed"), "... and only then the last words");
    }
    {
        static const struct step s[] = {
            { NULL, 0, 0, BYTES(GOOD_SERVER "\x1b[?1049hscreen"), 0, 0, 0 },
            { "screen", 0, 0, 0, 0, A_RESET, 0, 0 },
        };
        r = run("10.0.2.2", s, NSTEPS(s), 1);
        check(r == 2 && on_screen("\x1b[?1049l") && said_err("the connection was lost"), "a reset: the terminal is put back, status 2");
    }
    {
        static const struct step s[] = {
            { NULL, 0, 0, BYTES(GOOD_SERVER "\x1b[?1049hscreen"), 0, 0, 0 },
            { "screen", "\x1dquit\r", 0, 0, 0, 0, 0, 0 },
        };
        r = run("10.0.2.2", s, NSTEPS(s), 1);
        check(r == 0 && on_screen("\x1b[?1049l"), "quit from full-screen: the terminal is put back");
    }
    {
        static const struct step s[] = {
            { NULL, 0, 0, BYTES(GOOD_SERVER "hello"), 0, 0, 0 },
            { "hello", "\x1dsend ayt\r", 0, 0, 0, 0, 0, 0 },
            { NULL, 0, 0, "\r\n[yes]\r\n", 0, 0, 0, 0 },
            { "[yes]", "\x1dnonsense\rquit\r", 0, 0, 0, 0, 0, 0 },
        };
        r = run("10.0.2.2", s, NSTEPS(s), 1);
        check(r == 0 && server_got(IAC "\xf6", 2) && on_screen("[yes]") && on_screen("quit leaves"), "send ayt; a word the prompt does not know is explained");
    }
    {
        static const struct step s[] = {
            { NULL, 0, 0, BYTES(GOOD_SERVER "hello"), 0, 0, 0 },
            { "hello", "\x1d\r", 0, 0, 0, 0, 0, 0 },
            { "telnet> ", "more\r", 0, 0, 0, 0, 0, 0 },
            { NULL, 0, 0, 0, 0, A_CLOSE, 0, 0, "more\r" },
        };
        r = run("10.0.2.2", s, NSTEPS(s), 1);
        check(r == 0 && server_got("more\r", 5), "an empty line goes back to the session");
    }
    {
        static const struct step s[] = {
            { NULL, 0, 0, BYTES(GOOD_SERVER "hello"), 0, 0, 0 },
            { "hello", "\x1dqu\x03", 0, 0, 0, 0, 0, 0 },
            { "telnet> ", "z", 0, 0, 0, A_CLOSE, 0, 0 },
        };
        r = run("10.0.2.2", s, NSTEPS(s), 1);
        check(r == 0 && said_err("closed"), "Ctrl+C at the prompt goes back to the session");
    }
    {   /* the terminal goes while the prompt waits */
        static const struct step s[] = { { NULL, 0, 0, BYTES(GOOD_SERVER "hello"), 0, 0, 0 }, { "hello", "\x1dqu", 0, 0, 0, 0, 0, 0 } };
        r = run("10.0.2.2", s, NSTEPS(s), 1);
        check(r == 0, "the terminal gone at the prompt ends the session");
    }
    {   /* a server that stops reading: the keys are dropped, Ctrl+] still leaves */
        static char lots[20000];
        static struct step s[] = {
            { NULL, 0, 0, BYTES(GOOD_SERVER "hello"), A_NONE, 0, 0 },
            { "hello", 0, 0, 0, 0, A_STALL, 0, 0 },
            { NULL, lots, sizeof(lots), 0, 0, 0, 0, 0 },
            { "keys are dropped", "\x1dquit\r", 0, 0, 0, 0, 0, 0 },
        };
        memset(lots, 'k', sizeof(lots));
        r = run("10.0.2.2", s, NSTEPS(s), 1);
        check(r == 0, "a server that stops reading: Ctrl+] gets out");
        check(on_screen("keys are dropped"), "... and the dropped keys are said, once");
    }
    {   /* a flood of negotiations, into a server that does not read: nothing grows, and it is not read further */
        static char flood[300000];
        static struct step s[] = {
            { NULL, 0, 0, flood, sizeof(flood), A_STALL, 0, 0 },
            { NULL, 0, 0, 0, 0, A_NONE, 0, 0 },
        };
        size_t i;
        for(i = 0; i + 3 <= sizeof(flood); i += 3) memcpy(flood + i, i & 8 ? IAC DONT "\x1f" : IAC DO "\x1f", 3);
        s[1].keys = "\x1dquit\r";
        r = run("10.0.2.2", s, NSTEPS(s), 1);
        check(r == 0 && in_off < sizeof(flood) / 2, "a flood of negotiations into a server that stops reading: reading stops, Ctrl+] gets out");
    }
    {   /* the flood at a server that stops reading for a minute: reading stops as the buffer of answers nears its end, the
           flood waits in the connection, and when the server reads again every answer goes. Without that the
           answers would overflow, and be lost */
        static char flood[60000];
        static const struct step s[] = {
            { NULL, 0, 0, flood, sizeof(flood), A_STALL, 0, 0, NULL, 0 },
            { NULL, 0, 0, 0, 0, A_TAKE, 0, 0, NULL, 60000 },
            { NULL, 0, 0, 0, 0, A_CLOSE, 0, 0, NULL, 120000 },
        };
        size_t i, n = 0, got = 0, k;
        for(i = 0; i + 3 <= sizeof(flood); i += 3) {
            memcpy(flood + i, IAC DO "\x63", 3);
            n++;
        }
        r = run("10.0.2.2", s, NSTEPS(s), 1);
        for(k = 0; k + 3 <= server_len; k++) if(!memcmp(to_server + k, IAC WONT "\x63", 3)) got++;
        check(read_while_stalled > 1000 && read_while_stalled < 12000, "a flood into a server that does not read: reading stops, near the buffer's end");
        check(r == 0 && got == n, "... and when it reads again every answer goes, none lost");
    }
    {   /* writes that queue only a little at a time, as a full send buffer does: what waits moves up, none lost or doubled */
        static char flood[12000];
        static const struct step s[] = {
            { NULL, 0, 0, flood, sizeof(flood), 0, 0, 0, NULL, 0 },
            { NULL, 0, 0, 0, 0, A_CLOSE, 0, 0, NULL, 60000 },
        };
        size_t i, n = 0, got = 0, k;
        for(i = 0; i + 3 <= sizeof(flood); i += 3) {
            memcpy(flood + i, IAC DO "\x63", 3);
            n++;
        }
        max_write = 7;
        r = run("10.0.2.2", s, NSTEPS(s), 1);
        max_write = 0;
        for(k = 0; k + 3 <= server_len; k++) if(!memcmp(to_server + k, IAC WONT "\x63", 3)) got++;
        check(r == 0 && got == n && server_len == 3 * n + 15, "writes of 7 bytes at a time: every answer goes, whole and in order");
    }
    {   /* the same flood at a server that reads: every answer goes, in order, none lost */
        static char flood[30000];
        static const struct step s[] = {
            { NULL, 0, 0, flood, sizeof(flood), 0, 0, 0 },
            { NULL, 0, 0, 0, 0, A_CLOSE, 0, 0 },
        };
        size_t i, n = 0;
        for(i = 0; i + 3 <= sizeof(flood); i += 3) {
            memcpy(flood + i, IAC DO "\x63", 3);
            n++;
        }
        r = run("10.0.2.2", s, NSTEPS(s), 1);
        {
            size_t got = 0, k;
            for(k = 0; k + 3 <= server_len; k++) if(!memcmp(to_server + k, IAC WONT "\x63", 3)) got++;
            check(r == 0 && got == n, "every refusal goes, none lost to a full buffer");
        }
    }
    {   /* a server gone with answers still waiting: poll says POLLHUP at once for ever unless they are let go */
        static char flood[300000];
        static struct step s[] = {
            { NULL, 0, 0, flood, sizeof(flood), A_STALL, 0, 0 },
            { NULL, 0, 0, 0, 0, A_CLOSE, 0, 0 },
        };
        memset(flood, 0, sizeof(flood));
        {
            size_t i;
            for(i = 0; i + 3 <= sizeof(flood); i += 3) memcpy(flood + i, IAC DO "\x63", 3);
        }
        r = run("10.0.2.2", s, NSTEPS(s), 1);
        check(r == 0 && said_err("closed"), "the server closes with answers waiting: no spin, the session ends");
    }
    {   /* the window changes during the session, and is told within the time a look takes */
        static const struct step s[] = {
            { NULL, 0, 0, BYTES(GOOD_SERVER "hello"), A_RESIZE, 132, 43 },
            { NULL, 0, 0, "\r\ndone", 0, 0, 0, 0 },
            { "done", 0, 0, 0, 0, A_CLOSE, 0, 0 },
        };
        sizes = 80 << 16 | 25;
        r = run("10.0.2.2", s, NSTEPS(s), 1);
        check(r == 0 && server_got(IAC SB "\x1f\x00\x84\x00\x2b" IAC SE, 9), "a new window size is told");
        sizes = 80 << 16 | 25;
    }

    /* a pipe's input */
    {
        static const struct step s[] = {
            { NULL, "ls\nexit\n", 0, BYTES(GOOD_SERVER), A_KEYS_END, 0, 0 },
            { NULL, 0, 0, "bye\r\n", 0, 0, 0, 0 },
        };
        struct step c[3];
        memcpy(c, s, sizeof(s));
        memset(&c[2], 0, sizeof(c[2]));
        c[2].wait = "bye";
        c[2].action = A_CLOSE;
        r = run("10.0.2.2", c, 3, 0);
        check(r == 0 && !was_raw, "input from a pipe: no raw mode");
        check(server_got("ls", 2) && server_got("exit", 4), "... its lines go");
        check(!said_err("Ctrl+]"), "... and no prompt is offered");
    }
    {
        static const struct step s[] = { { NULL, "hello\n", 0, 0, 0, A_KEYS_END, 0, 0 } };
        r = run("10.0.2.2 7", s, NSTEPS(s), 0);
        check(r == 0 && server_got("hello\r\n", 7) && now_ms >= 2000, "the input's end, and a server that does not close: over in 2 seconds with no byte");
    }
    {
        static const struct step s[] = { { NULL, "hello\n", 0, 0, 0, A_STALL, 0, 0 }, { NULL, 0, 0, 0, 0, A_KEYS_END, 0, 0 } };
        r = run("10.0.2.2 7", s, NSTEPS(s), 0);
        check(r == 2 && said_err("took nothing"), "the input's end, and a server that takes nothing: it did not all go, status 2");
    }
    {
        static const struct step s[] = { { NULL, 0, 0, BYTES(GOOD_SERVER "x"), 0, 0, 0 }, { "x", 0, 0, "more", 0, A_SCREEN_BROKEN, 0, 0 } };
        r = run("10.0.2.2", s, NSTEPS(s), 1);
        check(r == 2 && said_err("the output cannot be written"), "a screen that cannot be written: status 2");
    }

    /* the command line */
    r = run("", NULL, 0, 1);
    check(r == 2 && said_err("usage: telnet") && !was_raw, "no host");
    r = run("-x host", NULL, 0, 1);
    check(r == 2 && said_err("usage: telnet"), "an option there is not");
    r = run("a b c", NULL, 0, 1);
    check(r == 2 && said_err("usage: telnet"), "too many words");
    r = run("10.0.2.2 0", NULL, 0, 1);
    check(r == 2 && said_err("0 is no port"), "port 0");
    r = run("10.0.2.2 65536", NULL, 0, 1);
    check(r == 2 && said_err("65536 is no port"), "past the last port");
    r = run("10.0.2.2 2x", NULL, 0, 1);
    check(r == 2 && said_err("2x is no port"), "not a number");
    r = run("nosuch.example", NULL, 0, 1);
    check(r == 2 && said_err("no such host") && !was_raw, "a name that does not resolve");
    connect_err = ECONNREFUSED;
    r = run("10.0.2.2", NULL, 0, 1);
    connect_err = 0;
    check(r == 2 && said_err("10.0.2.2 port 23") && !was_raw, "a connection refused: the terminal never left line mode");
}

/**** the review of 69203a8: each of these failed before its fix ****/

#define THE_END "\x1b\\\x1b[m\x1b[?25h\x1b" "7\x1b[?1049l"

static void test_review(void) {
    static char big[240000], want[260000];
    size_t i, n = 0, k = 0;
    int r, line = 0;

    /* 1: input from a pipe is never dropped: it waits for room */
    while(n + 60 < sizeof(big)) {
        int m = snprintf(big + n, 60, "line %06d of the file being piped into telnet\n", line++);
        n += (size_t)m;
    }
    for(i = 0; i < n; i++) {
        if(big[i] == '\n') want[k++] = '\r';
        want[k++] = big[i];
    }
    {
        static struct step s[2];
        memset(s, 0, sizeof(s));
        s[0].keys = big;
        s[0].keys_len = (int)n;
        s[0].action = A_KEYS_END;
        stdout_tty = 0;
        max_write = 512;
        r = run("10.0.2.2 2000", s, 1, 0);
        max_write = 0;
        stdout_tty = 1;
        check(r == 0 && server_len == k && !memcmp(to_server, want, k), "240000 bytes from a pipe, a server taking 512 at a time: every byte, in order");
        check(!said_err("dropped") && !on_screen("dropped"), "... and none dropped");
    }
    {   /* a server that stops reading for good, and nobody there to press Ctrl+] */
        static struct step s[2];
        memset(s, 0, sizeof(s));
        s[0].keys = big;
        s[0].keys_len = (int)n;
        s[0].action = A_STALL;
        stdout_tty = 0;
        r = run("10.0.2.2 2000", s, 1, 0);
        stdout_tty = 1;
        check(r == 2 && said_err("took nothing") && now_ms < 200000, "a pipe into a server that stops reading: status 2, in a minute");
    }
    {   /* the server closes with input still to go */
        static struct step s[2];
        memset(s, 0, sizeof(s));
        s[0].keys = big;
        s[0].keys_len = (int)n;
        s[0].action = A_CLOSE;
        stdout_tty = 0;
        r = run("10.0.2.2 2000", s, 1, 0);
        stdout_tty = 1;
        check(r == 2 && said_err("before all"), "the server closes before all the input went: said, status 2");
    }

    /* 3: what is left in the line goes out at the input's end */
    {
        static const struct step s[] = { { NULL, "a\nno newline", 0, 0, 0, A_KEYS_END, 0, 0, NULL, 0 } };
        stdout_tty = 0;
        r = run("10.0.2.2 2000", s, 1, 0);
        stdout_tty = 1;
        check(r == 0 && server_len == 13 && !memcmp(to_server, "a\r\nno newline", 13), "the input's end: a last line with no end goes too, as it is");
        check(!screen_len, "... and telnet does not print its own input");
    }

    /* 13: the end is the same whatever the server left open */
    {
        static const struct step s[] = { { NULL, 0, 0, BYTES(GOOD_SERVER "hello\x1b]0;a title") , A_CLOSE, 0, 0, NULL, 0 } };
        r = run("10.0.2.2", s, 1, 1);
        check(r == 0 && on_screen(THE_END), "a server leaves a string open: it is ended, and the screen reset");
    }
    {
        static const struct step s[] = { { NULL, 0, 0, BYTES(GOOD_SERVER "\x1b([?1049hX\x1b[?10490hY\x1b[3"), A_CLOSE, 0, 0, NULL, 0 } };
        r = run("10.0.2.2", s, 1, 1);
        check(r == 0 && on_screen(THE_END), "forms no tracker would see: the same end");
    }
    {
        static const struct step s[] = { { NULL, 0, 0, BYTES(GOOD_SERVER "plain text\r\n"), A_CLOSE, 0, 0, NULL, 0 } };
        r = run("10.0.2.2", s, 1, 1);
        check(r == 0 && on_screen(THE_END), "and after a session that left nothing on");
        stdout_tty = 0;
        r = run("10.0.2.2", s, 1, 1);
        stdout_tty = 1;
        check(r == 0 && !strchr(screen_text, 0x1b), "but nothing of it goes into a file or a pipe");
    }
}

/**** the review of 856c4d8: this failed on it ****/

static void test_review2(void) {
    static char lines[40000], want[45000];
    size_t i, n = 0, k = 0;
    int r, line = 0;
    while(n + 40 < sizeof(lines)) n += (size_t)snprintf(lines + n, 40, "a command line %06d\n", line++);
    for(i = 0; i < n; i++) {
        if(lines[i] == '\n') want[k++] = '\r';
        want[k++] = lines[i];
    }
    {   /* a server that answers each line with 30 KB and reads no more while 16 KB of that wait: a shell behind a telnetd, a
           router's command line. Its output must always be read, or it reads none of the input: the two would wait for each other */
        static struct step s[2];
        memset(s, 0, sizeof(s));
        s[0].keys = lines;
        s[0].keys_len = (int)n;
        s[0].action = A_KEYS_END;
        stdout_tty = 0;
        max_write = 512;
        flood_per_line = 30000;
        flood_cap = 16384;
        r = run("10.0.2.2 2000", s, 1, 0);
        flood_per_line = flood_cap = 0;
        max_write = 0;
        stdout_tty = 1;
        check(r == 0 && server_len == k && !memcmp(to_server, want, k), "a pipe into a server that floods back while it reads slowly: every line goes, none waits for ever");
        check(!said_err("took nothing"), "... and nothing stalls");
    }
}

/**** hostile sessions at random ****/

static void test_program_random(int rounds) {
    static char blob[3][6000];
    int round;
    for(round = 0; round < rounds; round++) {
        struct step s[5];
        int i, r, k;
        seed = (unsigned int)round * 2654435761u + 99;
        for(i = 0; i < 3; i++) for(k = 0; k < (int)sizeof(blob[i]); k++) {
            unsigned int w = rnd(10);
            blob[i][k] = (char)(w < 3 ? 0xf0 + rnd(16) : w < 5 ? rnd(40) : w < 6 ? 0x1b : rnd(256));
        }
        memset(s, 0, sizeof(s));
        s[0].server = blob[0];
        s[0].server_len = 1 + (int)rnd(sizeof(blob[0]) - 1);
        s[0].action = rnd(4) ? A_NONE : A_STALL;
        s[1].server = blob[1];
        s[1].server_len = 1 + (int)rnd(sizeof(blob[1]) - 1);
        s[1].keys = "abc\rdef\x7f\r";
        s[2].action = rnd(2) ? A_TAKE : A_RESIZE;
        s[2].cols = 1 + (int)rnd(300);
        s[2].rows = 1 + (int)rnd(300);
        s[3].server = blob[2];
        s[3].server_len = 1 + (int)rnd(sizeof(blob[2]) - 1);
        switch(rnd(4)) {
        case 0: s[4].action = A_CLOSE; break;
        case 1: s[4].action = A_RESET; break;
        case 2: s[4].keys = "\x1dstatus\r\x1dquit\r"; break;
        default: s[4].keys = "\x1dsend ayt\r\x1dquit\r"; break;
        }
        sizes = 80 << 16 | 25;
        r = run(rnd(2) ? "10.0.2.2" : "10.0.2.2 2323", s, 5, 1);
        check(r == 0 || r == 2, "a hostile session ends with a status");
        check(!screen_len || !strstr(screen_text, "telnet> telnet> telnet> "), "and sane");
        if(failures) {
            printf("  (round %d)\n", round);
            return;
        }
    }
    sizes = 80 << 16 | 25;
}

int main(int argc, char **argv) {
    int rounds = argc > 1 ? atoi(argv[1]) : 2000;
    test_options();
    test_naws();
    test_reader();
    test_keys();
    test_line_mode();
    test_restore();
    test_against_server();
    test_random(rounds);
    test_program();
    test_review();
    test_review2();
    test_program_random(rounds);
    printf("test_telnetc: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
