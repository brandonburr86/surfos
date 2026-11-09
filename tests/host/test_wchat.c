/* Host tests for the Chat's model (user/wchat/: irc.c, ircfmt.c, chat.c, recv.c, input.c, link.c), built as it is
   with the host's C library, its memory through tests/host/wchat_alloc.h. They are: lines cut from a server's bytes
   at every split, over the limit and with a NUL; messages parsed, prefixes, tags, fifteen parameters; case mappings;
   names; lines put together and refused; base64 (RFC 4648's vectors); CTCP; text decoded and cleaned, the
   formatting codes as runs, stripped, the colours, the rows; registration with CAP, SASL PLAIN and a server password,
   the password gone from the model's memory once used; nicks taken at registration; channels, their lists, joins,
   parts, quits, nicks, kicks, topics, modes; messages, queries, notices, CTCP and DCC; PING and a flood of them;
   what is typed, its commands, a message split to fit, a paste of lines; the pacing; the keepalive; the
   end of a connection; every limit (rooms, nicks, lines, bytes) against a hostile server; a hostile server of random
   bytes cut at random; every allocation of a session failing in turn, with nothing left allocated; and the window's
   link to its helper, its frames cut at every byte and broken in every way. Built and run by `make unittest`; the
   same file runs under AddressSanitizer and UndefinedBehaviorSanitizer on the Mac (the account says how). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "irc.h"
#include "ircfmt.h"
#include "chat.h"
#include "link.h"
#include "chatint.h"

static int checks, failed;
#define CHECK(x) do { checks++; if(!(x)) { failed++; printf("FAIL %s line %d: %s\n", __FILE__, __LINE__, #x); } } while(0)

/**** the allocator the model's files use (wchat_alloc.h) ****/

static long live, calls, fail_at = -1;

static int refuse(void) {
    calls++;
    if(fail_at < 0) return 0;
    return fail_at-- == 0;
}

void *wchat_malloc(size_t n) {
    void *p;
    if(refuse()) return NULL;
    p = malloc(n ? n : 1);
    if(p) live++;
    return p;
}

void *wchat_calloc(size_t n, size_t size) {
    void *p;
    if(refuse()) return NULL;
    p = calloc(n ? n : 1, size ? size : 1);
    if(p) live++;
    return p;
}

void *wchat_realloc(void *p, size_t n) {
    void *q;
    if(!p) return wchat_malloc(n);
    if(refuse()) return NULL;
    q = realloc(p, n ? n : 1);
    return q;
}

void wchat_free(void *p) {
    if(p) live--;
    free(p);
}

/**** a random number generator of our own, so that every run is the same ****/

static unsigned long long seed = 0x5eed5eedULL;

static unsigned rnd(unsigned n) {
    seed ^= seed << 13;
    seed ^= seed >> 7;
    seed ^= seed << 17;
    return n ? (unsigned)(seed % n) : 0;
}

/**** lines ****/

/* every line of s, fed in pieces of step bytes, joined by '|' (L: a long one, B: a bad one) */
static const char *lines_of(const char *s, int n, int step) {
    static struct irc_reader r;
    static char got[3 * IRC_IN_MAX];
    int at = 0, used, o = 0, k;
    memset(&r, 0, sizeof(r));
    got[0] = 0;
    while(at < n) {
        int piece = n - at < step ? n - at : step, off = 0;
        while(off < piece) {
            k = irc_read(&r, s + at + off, piece - off, &used);
            off += used;
            if(k == IRC_LINE) o += snprintf(got + o, sizeof(got) - (size_t)o, "%s|", r.line);
            else if(k == IRC_LONG) o += snprintf(got + o, sizeof(got) - (size_t)o, "L|");
            else if(k == IRC_BAD) o += snprintf(got + o, sizeof(got) - (size_t)o, "B|");
        }
        at += piece;
    }
    return got;
}

static void reader(void) {
    static const char s[] = "PING :a\r\nPING :b\n\r\n\nPING :c\rd\r\n:x NOTICE * :e\r\r\n";
    static char big[3 * IRC_IN_MAX];
    static struct irc_reader r;
    int step, n, used, k, i;
    for(step = 1; step <= (int)sizeof(s); step++)       /* the same lines however the bytes come */
        CHECK(!strcmp(lines_of(s, (int)sizeof(s) - 1, step), "PING :a|PING :b|PING :c\rd|:x NOTICE * :e\r|"));
    CHECK(!strcmp(lines_of("A\0B\r\nC\r\n", 8, 3), "B|C|"));
    /* the longest line: IRC_IN_MAX - 2 bytes and CR LF; one byte more is dropped, the next line read */
    memset(big, 'x', sizeof(big));
    memcpy(big, "PRIVMSG #a :", 12);
    memcpy(big + IRC_IN_MAX - 2, "\r\nPING :1\r\n", 11);
    n = IRC_IN_MAX - 2 + 11;
    for(step = 1; step < 9000; step += 997) {
        const char *got = lines_of(big, n, step);
        CHECK(strlen(got) == IRC_IN_MAX - 2 + 1 + 8 && !strcmp(got + IRC_IN_MAX - 1, "PING :1|"));
    }
    memset(big, 'x', sizeof(big));
    memcpy(big + IRC_IN_MAX - 1, "\r\nPING :2\r\n", 11);
    CHECK(!strcmp(lines_of(big, IRC_IN_MAX - 1 + 11, 4096), "L|PING :2|"));
    memcpy(big + IRC_IN_MAX - 2, "\nPING :3\n", 9);             /* IRC_IN_MAX - 2 and LF alone: whole */
    CHECK(!strcmp(lines_of(big, IRC_IN_MAX - 2 + 9, 100) + IRC_IN_MAX - 1, "PING :3|"));
    big[IRC_IN_MAX - 2] = 'x';
    memcpy(big + IRC_IN_MAX - 1, "\nPING :4\n", 9);             /* one more without CR: too long */
    CHECK(!strcmp(lines_of(big, IRC_IN_MAX - 1 + 9, 100), "L|PING :4|"));
    /* a megabyte with no line end, then one: one long line, and the reader as new */
    memset(&r, 0, sizeof(r));
    memset(big, 'y', sizeof(big));
    for(i = 0; i < 40; i++) {
        k = irc_read(&r, big, (int)sizeof(big), &used);
        CHECK(k == IRC_MORE && used == (int)sizeof(big));
    }
    k = irc_read(&r, "\nPING :5\r\n", 10, &used);
    CHECK(k == IRC_LONG && used == 1);
    k = irc_read(&r, "PING :5\r\n", 9, &used);
    CHECK(k == IRC_LINE && !strcmp(r.line, "PING :5"));
}

/**** messages ****/

static void parser(void) {
    static char line[1024];
    struct irc_msg m;
    int i;
    strcpy(line, "@time=2026-11-05T10:00:00Z;x=y :nick!user@host.test PRIVMSG #surf :hello  there ");
    CHECK(!irc_parse(line, &m));
    CHECK(!strcmp(m.tags, "time=2026-11-05T10:00:00Z;x=y") && !strcmp(m.nick, "nick") && !strcmp(m.user, "user"));
    CHECK(!strcmp(m.host, "host.test") && !m.server && !strcmp(m.command, "PRIVMSG") && m.numeric == -1);
    CHECK(m.argc == 2 && !strcmp(m.argv[0], "#surf") && !strcmp(m.argv[1], "hello  there "));
    strcpy(line, ":irc.test 001 alice :Welcome");
    CHECK(!irc_parse(line, &m) && m.server && m.numeric == 1 && !strcmp(m.nick, "irc.test") && m.argc == 2);
    strcpy(line, "PING :token");
    CHECK(!irc_parse(line, &m) && m.server && !m.prefix && !strcmp(m.argv[0], "token"));
    strcpy(line, "ping   x   y  ");                         /* spaces apart, a command in lower case */
    CHECK(!irc_parse(line, &m) && !strcmp(m.command, "PING") && m.argc == 2 && !strcmp(m.argv[1], "y"));
    strcpy(line, ":n@h JOIN #a");                           /* a nick and a host, no user */
    CHECK(!irc_parse(line, &m) && !strcmp(m.nick, "n") && !strcmp(m.user, "") && !strcmp(m.host, "h") && !m.server);
    strcpy(line, ":services NOTICE x :y");                  /* no dot, no ! or @: a nick */
    CHECK(!irc_parse(line, &m) && !m.server);
    strcpy(line, "PRIVMSG #a :");                           /* an empty last parameter */
    CHECK(!irc_parse(line, &m) && m.argc == 2 && !strcmp(m.argv[1], ""));
    strcpy(line, "CMD 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 :17");    /* after 14, the rest is the 15th */
    CHECK(!irc_parse(line, &m) && m.argc == 15 && !strcmp(m.argv[13], "14") && !strcmp(m.argv[14], "15 16 :17"));
    strcpy(line, "CMD 1 2 3 4 5 6 7 8 9 10 11 12 13 14 :15 16");
    CHECK(!irc_parse(line, &m) && m.argc == 15 && !strcmp(m.argv[14], "15 16"));
    for(i = 0; i < 9; i++) {                                /* no message */
        static const char *const bad[] = { ":prefix", ": PRIVMSG x", "@tags", "@tags :p", "12 x", "1234 x",
                                           "PRIV-MSG x", "", ":p 1a2 x" };
        strcpy(line, bad[i]);
        CHECK(irc_parse(line, &m) == -1);
    }
    strcpy(line, "@ :p X");                                 /* tags that are empty: a message all the same */
    CHECK(!irc_parse(line, &m) && !strcmp(m.command, "X"));
    memset(line, 'A', 40);                                  /* a command of 40 letters: none */
    line[40] = 0;
    CHECK(irc_parse(line, &m) == -1);
}

static void names(void) {
    CHECK(irc_lower(IRC_CM_RFC1459, '[') == '{' && irc_lower(IRC_CM_RFC1459, '^') == '~');
    CHECK(irc_lower(IRC_CM_STRICT, ']') == '}' && irc_lower(IRC_CM_STRICT, '^') == '^');
    CHECK(irc_lower(IRC_CM_ASCII, '[') == '[' && irc_lower(IRC_CM_ASCII, 'Q') == 'q' && irc_lower(IRC_CM_ASCII, '{') == '{');
    CHECK(!irc_casecmp(IRC_CM_RFC1459, "Alice[^]", "alice{~}") && irc_casecmp(IRC_CM_ASCII, "Alice[", "alice{"));
    CHECK(!irc_casecmp(IRC_CM_STRICT, "A\\", "a|") && irc_casecmp(IRC_CM_STRICT, "a^", "a~"));
    CHECK(irc_casecmp(IRC_CM_ASCII, "a", "b") < 0 && irc_casecmp(IRC_CM_ASCII, "ab", "a") > 0);
    CHECK(irc_casemap_named("ascii") == IRC_CM_ASCII && irc_casemap_named("strict-rfc1459") == IRC_CM_STRICT);
    CHECK(irc_casemap_named("rfc1459") == IRC_CM_RFC1459 && irc_casemap_named("rfc8265") == IRC_CM_RFC1459);
    CHECK(irc_valid_nick("alice", 64, "#&") && irc_valid_nick("[a]`_^{|}-9", 64, "#") && irc_valid_nick("9z", 64, "#"));
    CHECK(!irc_valid_nick("", 64, "#") && !irc_valid_nick("a b", 64, "#") && !irc_valid_nick("a,b", 64, "#"));
    CHECK(!irc_valid_nick(":a", 64, "#") && !irc_valid_nick("#a", 64, "#") && !irc_valid_nick("a!b", 64, "#"));
    CHECK(!irc_valid_nick("a\rb", 64, "#") && !irc_valid_nick("a\x7f", 64, "#") && !irc_valid_nick("\xc3\xa9", 64, "#"));
    CHECK(!irc_valid_nick("abcdef", 5, "#") && irc_valid_nick("abcde", 5, "#") && irc_valid_nick("#a", 64, NULL));
    CHECK(irc_valid_channel("#surf", 200, "#&") && irc_valid_channel("&caf\xc3\xa9", 200, "#&"));
    CHECK(!irc_valid_channel("surf", 200, "#&") && !irc_valid_channel("#a b", 200, "#") && !irc_valid_channel("#a,b", 200, "#"));
    CHECK(!irc_valid_channel("#a\x07", 200, "#") && !irc_valid_channel("", 200, "#") && !irc_valid_channel("#a", 200, ""));
}

static void compose(void) {
    char out[600], big[600];
    const char *mid[3] = { "#surf", "key", NULL };
    CHECK(irc_compose(out, sizeof(out), "JOIN", mid, 2, NULL) == 16 && !strcmp(out, "JOIN #surf key\r\n"));
    CHECK(irc_compose(out, sizeof(out), "PRIVMSG", mid, 1, "hi there") == 25 && !strcmp(out, "PRIVMSG #surf :hi there\r\n"));
    CHECK(irc_compose(out, sizeof(out), "QUIT", NULL, 0, "") == 8 && !strcmp(out, "QUIT :\r\n"));
    mid[1] = "a b";
    CHECK(irc_compose(out, sizeof(out), "JOIN", mid, 2, NULL) == -1);       /* a space in a middle one */
    mid[1] = ":k";
    CHECK(irc_compose(out, sizeof(out), "JOIN", mid, 2, NULL) == -1);
    mid[1] = "";
    CHECK(irc_compose(out, sizeof(out), "JOIN", mid, 2, NULL) == -1);
    CHECK(irc_compose(out, sizeof(out), "PRIVMSG", mid, 1, "a\r\nQUIT") == -1);    /* no second command */
    CHECK(irc_compose(out, sizeof(out), "PRIVMSG", mid, 1, "a\nb") == -1);
    CHECK(irc_compose(out, sizeof(out), "PRIV MSG", NULL, 0, "x") == -1 && irc_compose(out, sizeof(out), "", NULL, 0, "x") == -1);
    mid[0] = "#a";
    memset(big, 'x', sizeof(big));                          /* "PRIVMSG #a :" is 12: 498 more make 510 */
    big[498] = 0;
    CHECK(irc_compose(out, sizeof(out), "PRIVMSG", mid, 1, big) == 512);
    big[498] = 'x';
    big[499] = 0;
    CHECK(irc_compose(out, sizeof(out), "PRIVMSG", mid, 1, big) == -1);
    CHECK(irc_compose(out, 20, "PRIVMSG", mid, 1, "a long line for twenty") == -1);
}

static void base64(void) {
    static const char *const in[] = { "", "f", "fo", "foo", "foob", "fooba", "foobar" };
    static const char *const want[] = { "", "Zg==", "Zm8=", "Zm9v", "Zm9vYg==", "Zm9vYmE=", "Zm9vYmFy" };
    char out[16];
    int i;
    for(i = 0; i < 7; i++)
        CHECK(irc_base64((const unsigned char *)in[i], (int)strlen(in[i]), out, sizeof(out)) == (int)strlen(want[i]) &&
              !strcmp(out, want[i]));
    CHECK(irc_base64((const unsigned char *)"foobar", 6, out, 8) == -1 && irc_base64((const unsigned char *)"foobar", 6, out, 9) == 8);
}

static void ctcp(void) {
    char cmd[16];
    const char *arg;
    int len;
    CHECK(irc_ctcp("\001ACTION waves hello\001", cmd, sizeof(cmd), &arg, &len) && !strcmp(cmd, "ACTION") &&
          len == 11 && !strncmp(arg, "waves hello", 11));
    CHECK(irc_ctcp("\001version\001", cmd, sizeof(cmd), &arg, &len) && !strcmp(cmd, "VERSION") && len == 0);
    CHECK(irc_ctcp("\001PING 123", cmd, sizeof(cmd), &arg, &len) && !strcmp(cmd, "PING") && len == 3);
    CHECK(!irc_ctcp("\001", cmd, sizeof(cmd), &arg, &len) && !irc_ctcp("ACTION x", cmd, sizeof(cmd), &arg, &len));
    CHECK(!irc_ctcp("\001A-B x\001", cmd, sizeof(cmd), &arg, &len));
    CHECK(!irc_ctcp("\001ABCDEFGHIJKLMNOPQRSTUVWXYZ\001", cmd, sizeof(cmd), &arg, &len));
}

/**** text ****/

static const char *cleaned(const char *s, int size) {
    static char out[IRC_TEXT_MAX];
    irc_text(out, size, s, (int)strlen(s));
    return out;
}

static char text_out[IRC_TEXT_MAX];

static void text(void) {
    static char in[IRC_LINE_MAX + 1];
    char out[64];
    int i, n, size;
    CHECK(!strcmp(cleaned("caf\xc3\xa9 \xe2\x82\xac", 64), "caf\xc3\xa9 \xe2\x82\xac"));     /* UTF-8 kept */
    CHECK(!strcmp(cleaned("caf\xe9 \x80 \x93q\x94", 64), "caf\xc3\xa9 \xe2\x82\xac \xe2\x80\x9cq\xe2\x80\x9d"));
    CHECK(!strcmp(cleaned("a\x81" "b\x8d" "c", 64), "abc"));      /* windows-1252's holes go */
    CHECK(!strcmp(cleaned("\xc0\xaf \xed\xa0\x80", 64), "\xc3\x80\xc2\xaf \xc3\xad\xc2\xa0\xe2\x82\xac"));  /* overlong, surrogate */
    CHECK(!strcmp(cleaned("a\tb\x01" "c\x07\x1b[31m\x7f" "d", 64), "a bc[31md"));
    CHECK(!strcmp(cleaned("\x02" "b\x03" "4,5c\x0f\x1d\x1e\x1f\x11\x16\x04" "ff0000x", 64), "\x02" "b\x03" "4,5c\x0f\x1d\x1e\x1f\x11\x16\x04" "ff0000x"));
    CHECK(!strcmp(cleaned("\xc2\x85" "a\xe2\x80\xae" "b\xef\xbb\xbf" "c\xe2\x80\x8b" "d", 64), "abcd"));   /* C1, bidi, BOM, ZWSP */
    for(size = 1; size < 12; size++) {                      /* cut between characters, at every size */
        n = irc_text(out, size, "\xe2\x82\xac\xc3\xa9" "a\xf0\x9f\x8c\x8a", 10);
        CHECK(n < size && out[n] == 0 && (n == 0 || n == 3 || n == 5 || n == 6 || n == 10));
        CHECK(n >= size - 4);
    }
    for(i = 0; i < 256; i++) in[i] = (char)(255 - i);     /* every byte, as windows-1252: valid UTF-8 out */
    in[256] = 0;
    n = irc_text(text_out, IRC_TEXT_MAX, in, 256);
    CHECK(n > 0 && n < IRC_TEXT_MAX);
}

/* the runs of s as "fg,bg,bius:text|" (b bold, i italic, u underline, s strike, r reverse, m mono) */
static const char *runs(const char *s) {
    static char got[512];
    struct irc_style st;
    int pos = 0, start, len, o = 0;
    irc_style_reset(&st);
    got[0] = 0;
    while(irc_run(s, &pos, &st, &start, &len))
        o += snprintf(got + o, sizeof(got) - (size_t)o, "%d,%d,%s%s%s%s%s%s:%.*s|", st.fg, st.bg, st.bold ? "b" : "",
                      st.italic ? "i" : "", st.underline ? "u" : "", st.strike ? "s" : "", st.reverse ? "r" : "",
                      st.mono ? "m" : "", len, s + start);
    return got;
}

static void formatting(void) {
    char out[64];
    int c, next, end;
    CHECK(!strcmp(runs("a\x02" "b\x02" "c"), "-1,-1,:a|-1,-1,b:b|-1,-1,:c|"));
    CHECK(!strcmp(runs("\x03" "4red\x03" "12,1blue\x03" "x"), "4,-1,:red|12,1,:blue|-1,-1,:x|"));
    CHECK(!strcmp(runs("\x03" "4,red"), "4,-1,:,red|"));              /* a comma with no digit is text */
    CHECK(!strcmp(runs("\x03" "123"), "12,-1,:3|"));                  /* two digits at most */
    CHECK(!strcmp(runs("\x03" "99,99a\x03" "5,7b\x03" "9c"), "-1,-1,:a|5,7,:b|9,7,:c|"));  /* 99 the default; bg kept */
    CHECK(!strcmp(runs("\x04" "ff8000,0000ffa\x04" "12345g"), "33521664,16777471,:a|-1,-1,:12345g|"));
    CHECK(!strcmp(runs("\x1d" "i\x1f" "u\x1e" "s\x16" "r\x11" "m\x0f" "z"), "-1,-1,i:i|-1,-1,iu:u|-1,-1,ius:s|"
                  "-1,-1,iusr:r|-1,-1,iusrm:m|-1,-1,:z|"));
    CHECK(!strcmp(runs("\x02\x03\x0f"), "") && !strcmp(runs(""), ""));
    CHECK(irc_strip(out, sizeof(out), "\x02" "bo\x03" "4,5ld\x04" "abcdef!") == 5 && !strcmp(out, "bold!"));
    CHECK(irc_strip(out, 4, "\x02\xe2\x82\xac" "ab") == 3 && !strcmp(out, "\xe2\x82\xac"));
    CHECK(irc_strip(out, 3, "\xe2\x82\xac" "ab") == 0 && !strcmp(out, ""));
    CHECK(irc_code_len("\x03") == 1 && irc_code_len("\x03" "1,") == 2 && irc_code_len("\x03" "12,34x") == 6);
    CHECK(irc_code_len("\x04" "abcdef,123456") == 14 && irc_code_len("\x04" "abcde") == 1 && irc_code_len("a") == 0);
    CHECK(irc_rgb(0) == 0xffffff && irc_rgb(1) == 0 && irc_rgb(88) == 0 && irc_rgb(98) == 0xffffff);
    CHECK(irc_rgb(IRC_RGB(0x123456)) == 0x123456 && irc_rgb(99) == 0 && irc_rgb(-1) == 0);
    for(c = 16; c < 88; c++) CHECK(irc_rgb(c) <= 0xffffff);
    CHECK(irc_rgb(16) != irc_rgb(17) && irc_rgb(16) != irc_rgb(28));
    CHECK(irc_readable(0x000000, 0xf4f1e6) && !irc_readable(0xefdf1f, 0xf4f1e6) && irc_readable(0xffffff, 0x102e3b));
    CHECK(irc_cells("\x02" "a\xc3\xa9\x03" "12,3b") == 3);
    /* rows */
    end = irc_wrap("hello world", 0, 8, &next);
    CHECK(end == 5 && next == 6);
    end = irc_wrap("hello world", 6, 8, &next);
    CHECK(end == 11 && next == 11);
    end = irc_wrap("abcdefghij", 0, 4, &next);              /* a word longer than a row is cut */
    CHECK(end == 4 && next == 4);
    end = irc_wrap("ab \x02" "cd", 0, 3, &next);            /* the code goes with its word */
    CHECK(end == 2 && next == 3);
    end = irc_wrap("abc   def", 0, 3, &next);               /* the spaces at the break are nobody's */
    CHECK(end == 3 && next == 6);
    end = irc_wrap("\xc3\xa9\xc3\xa9\xc3\xa9", 0, 2, &next);
    CHECK(end == 4 && next == 4);
}

/* every row of random texts at every width: rows of cols cells at most, every character in one, no row empty */
static void wrapping(void) {
    static const char *const bits[] = { "a", "b", " ", "  ", "\xc3\xa9", "\xe2\x82\xac", "\x02", "\0034,5", "\x0f", "word" };
    char s[400];
    int t, cols, at, next, end, total, rows, bad = 0;
    for(t = 0; t < 400; t++) {
        int o = 0;
        while(o < 300) {
            const char *b = bits[rnd(10)];
            memcpy(s + o, b, strlen(b));
            o += (int)strlen(b);
        }
        s[o] = 0;
        for(cols = 1; cols < 40; cols++) {
            at = total = rows = 0;
            while(s[at]) {
                char row[400];
                end = irc_wrap(s, at, cols, &next);
                if(end < at || next < end || next <= at || ++rows > 400) {
                    bad++;
                    break;
                }
                memcpy(row, s + at, (size_t)(end - at));
                row[end - at] = 0;
                if(irc_cells(row) > cols) bad++;
                total += irc_cells(row);
                for(; end < next; end++) if(s[end] == ' ') total++;     /* the spaces between rows */
                at = next;
            }
            if(total != irc_cells(s)) bad++;
        }
    }
    CHECK(!bad);
}

/**** the model ****/

static struct chat C;
static char sent[1 << 17];
static int made;

static void fresh(void) {
    if(made) chat_free(&C);
    CHECK(chat_init(&C) == 0);
    made = 1;
    C.now = 5000;
    C.minute = 10 * 60 + 30;
}

static void server(const char *s) {
    chat_recv(&C, s, (int)strlen(s));
}

static const char *out(void) {
    int n = chat_send(&C, sent, (int)sizeof(sent) - 1);
    sent[n] = 0;
    return sent;
}

static const struct chat_line *newest(int room, int back) {
    const struct chat_room *r = C.room[room];
    return chat_line_at(r, r->count - 1 - back);
}

static const char *said(int room, int back) {
    const struct chat_line *l = newest(room, back);
    return l ? l->text : "(none)";
}

static int plain_ok;                    /* the window's question answered Send: the password may go without TLS */
static int consistent(void);            /* every room's list sorted, its counts right (below) */

static void params(struct chat_params *p, const char *nick, int kind, const char *pass, int tls) {
    memset(p, 0, sizeof(*p));
    p->plain_ok = plain_ok;
    strcpy(p->host, "irc.test");
    p->port = tls ? 6697 : 6667;
    p->tls = tls;
    strcpy(p->nick, nick);
    p->pass_kind = kind;
    if(pass) strcpy(p->pass, pass);
}

static void connect_as(const char *nick, int kind, const char *pass, int tls) {
    struct chat_params p;
    char err[200];
    params(&p, nick, kind, pass, tls);
    CHECK(chat_connect(&C, &p, err, sizeof(err)) == 0);
    CHECK(C.state == CHAT_CONNECTING);
    chat_up(&C, tls ? "TLSv1.3" : "");
    CHECK(C.state == CHAT_REGISTERING);
}

static void online(void) {
    fresh();
    connect_as("alice", CHAT_PASS_NONE, NULL, 1);
    CHECK(!strcmp(out(), "CAP LS 302\r\nNICK alice\r\nUSER alice 0 * :alice\r\n"));
    server(":irc.test CAP * LS :multi-prefix sasl\r\n");
    CHECK(!strcmp(out(), "CAP END\r\n"));
    server(":irc.test 001 alice :Welcome to the test network, alice\r\n"
           ":irc.test 005 alice CHANTYPES=# PREFIX=(ohv)@%+ NETWORK=Test\\x20Net :are supported\r\n");
    CHECK(C.state == CHAT_ONLINE && !strcmp(C.network, "Test Net") && !strcmp(C.prefix_chars, "@%+"));
    CHECK(!strcmp(said(0, 0), "Welcome to the test network, alice"));
}

/* in #surf with bob, @carol and +dave */
static int in_surf(void) {
    int r;
    online();
    chat_input(&C, "/join surf");
    CHECK(!strcmp(out(), "JOIN #surf\r\n"));
    server(":alice!al@host.test JOIN :#surf\r\n:irc.test 332 alice #surf :Waves \x02today\r\n"
           ":irc.test 333 alice #surf carol!c@h 1700000000\r\n"
           ":irc.test 353 alice = #surf :alice bob @carol\r\n:irc.test 353 alice = #surf :+dave\r\n"
           ":irc.test 366 alice #surf :End of /NAMES list.\r\n");
    r = chat_find(&C, ROOM_CHANNEL, "#SURF");
    CHECK(r == 1 && C.active == 1 && C.room[1]->joined && C.room[1]->names_done && C.room[1]->nnicks == 4);
    CHECK(!strcmp(C.room[1]->topic, "Waves \x02today") && !strcmp(said(1, 0), "Set by carol"));
    CHECK(!strcmp(C.user, "al@host.test"));
    return r;
}

static void registration(void) {
    struct chat_params p;
    char err[200];
    int i;
    fresh();
    params(&p, "alice", CHAT_PASS_NONE, NULL, 1);           /* the dialog's checks */
    CHECK(chat_check(&p, err, sizeof(err)) == 0);
    strcpy(p.host, "irc test");
    CHECK(chat_check(&p, err, sizeof(err)) == -1 && strstr(err, "letters, digits"));
    strcpy(p.host, "");
    CHECK(chat_check(&p, err, sizeof(err)) == -1);
    strcpy(p.host, "irc.test");
    p.port = 0;
    CHECK(chat_check(&p, err, sizeof(err)) == -1);
    p.port = 70000;
    CHECK(chat_check(&p, err, sizeof(err)) == -1);
    p.port = 6697;
    strcpy(p.nick, "9lives");
    CHECK(chat_check(&p, err, sizeof(err)) == -1);
    strcpy(p.nick, "a b");
    CHECK(chat_check(&p, err, sizeof(err)) == -1);
    strcpy(p.nick, "abcdefghijabcdefghijabcdefghijabc");
    CHECK(chat_check(&p, err, sizeof(err)) == -1);
    strcpy(p.nick, "[surf]_er");
    p.pass_kind = CHAT_PASS_SASL;
    CHECK(chat_check(&p, err, sizeof(err)) == -1);          /* SASL and no password */
    strcpy(p.pass, "two\nlines");
    CHECK(chat_check(&p, err, sizeof(err)) == -1);
    strcpy(p.pass, "made-up");
    CHECK(chat_check(&p, err, sizeof(err)) == 0);
    /* a server that knows no CAP: registration all the same */
    fresh();
    connect_as("alice", CHAT_PASS_NONE, NULL, 0);
    out();
    server(":old.test 421 * CAP :Unknown command\r\n:old.test 001 alice :Hi\r\n");
    CHECK(C.state == CHAT_ONLINE && !strcmp(out(), ""));
    /* nicks taken: _ and __, then digits; a 433 about an old try changes nothing */
    fresh();
    connect_as("alice", CHAT_PASS_NONE, NULL, 1);
    out();
    server(":irc.test 433 * alice :Nickname is already in use\r\n");
    CHECK(!strcmp(out(), "NICK alice_\r\n") && !strcmp(C.nick, "alice_"));
    server(":irc.test 433 * alice :Nickname is already in use\r\n");
    CHECK(!strcmp(out(), ""));
    server(":irc.test 433 * alice_ :in use\r\n");
    CHECK(!strcmp(out(), "NICK alice__\r\n"));
    for(i = 0; i < 7; i++) {
        char want[32], tried[128];
        snprintf(tried, sizeof(tried), ":irc.test 437 * %s :in use\r\n", C.nick);
        server(tried);
        snprintf(want, sizeof(want), "NICK alice%d\r\n", i + 2);
        CHECK(!strcmp(out(), want));
    }
    server(":irc.test 433 * alice8 :in use\r\n");
    CHECK(!strcmp(out(), "") && strstr(said(0, 0), "/nick NAME"));
    chat_input(&C, "/nick surfer");                         /* registration takes the nick typed */
    CHECK(!strcmp(out(), "NICK surfer\r\n") && !strcmp(C.nick, "surfer"));
    server(":irc.test 433 * surfer :in use\r\n");
    CHECK(!strcmp(out(), "") && strstr(said(0, 0), "/nick NAME"));
    server(":irc.test 001 surfer :Welcome\r\n");
    CHECK(C.state == CHAT_ONLINE && !strcmp(C.nick, "surfer"));
    /* a nine-letter nick keeps nine with its ending */
    fresh();
    connect_as("abcdefghi", CHAT_PASS_NONE, NULL, 1);
    out();
    server(":irc.test 433 * abcdefghi :in use\r\n");
    CHECK(!strcmp(out(), "NICK abcdefgh_\r\n"));
    /* a nick refused: no guess */
    fresh();
    connect_as("alice", CHAT_PASS_NONE, NULL, 1);
    out();
    server(":irc.test 432 * alice :Erroneous nickname\r\n");
    CHECK(!strcmp(out(), "") && strstr(said(0, 0), "does not take the nick alice"));
    /* the server names the nick it took */
    server(":irc.test 001 Alice2 :Welcome\r\n");
    CHECK(!strcmp(C.nick, "Alice2"));
    /* CAP LS in several lines; the server's CAP that was not asked for changes nothing */
    fresh();
    connect_as("alice", CHAT_PASS_NONE, NULL, 1);
    out();
    server(":irc.test CAP * ACK :sasl\r\n:irc.test CAP * LS * :a b\r\n");
    CHECK(!strcmp(out(), ""));
    server(":irc.test CAP * LS :c d\r\n");
    CHECK(!strcmp(out(), "CAP END\r\n"));
    server(":irc.test CAP * LS :e\r\n:irc.test CAP * NAK :x\r\n");
    CHECK(!strcmp(out(), ""));
    /* no answer to CAP LS in 30 seconds: CAP END */
    fresh();
    connect_as("alice", CHAT_PASS_NONE, NULL, 1);
    out();
    C.now += CHAT_CAP_MS - 1;
    chat_tick(&C);
    CHECK(!strcmp(out(), ""));
    C.now += 1;
    chat_tick(&C);
    CHECK(!strcmp(out(), "CAP END\r\n"));
}

/* the password's bytes anywhere in the model's own memory */
static int holds(const void *at, size_t size, const char *pass) {
    const unsigned char *b = at;
    size_t n = strlen(pass), i;
    for(i = 0; i + n <= size; i++) if(!memcmp(b + i, pass, n)) return 1;
    return 0;
}

/* the password anywhere in the model: its struct, every file's static buffers, every room's lines (11/09/26) */
static int remembered(const char *pass) {
    const struct chat_buf *const tables[] = { chat_bufs, chat_recv_bufs, chat_input_bufs };
    int i, k;
    if(holds(&C, sizeof(C), pass)) return 1;
    for(i = 0; i < 3; i++)
        for(k = 0; tables[i][k].at; k++) if(holds(tables[i][k].at, tables[i][k].size, pass)) return 1;
    for(i = 0; i < C.rooms; i++)
        for(k = 0; k < C.room[i]->count; k++) {
            const struct chat_line *l = chat_line_at(C.room[i], k);
            if(strstr(l->text, pass) || strstr(l->from, pass)) return 1;
        }
    return 0;
}

static void passwords(void) {
    static char longpass[CHAT_PASS_MAX + 1];
    char want[600];
    /* SASL PLAIN */
    fresh();
    connect_as("alice", CHAT_PASS_SASL, "made-up-secret", 1);
    CHECK(!strcmp(out(), "CAP LS 302\r\nNICK alice\r\nUSER alice 0 * :alice\r\n"));
    server(":irc.test AUTHENTICATE +\r\n");                 /* not asked for yet: nothing goes */
    CHECK(!strcmp(out(), ""));
    server(":irc.test CAP * LS :multi-prefix sasl=EXTERNAL,PLAIN\r\n");
    CHECK(!strcmp(out(), "CAP REQ :sasl\r\n"));
    server(":irc.test CAP alice ACK :sasl\r\n");
    CHECK(!strcmp(out(), "AUTHENTICATE PLAIN\r\n"));
    CHECK(remembered("made-up-secret"));
    server("AUTHENTICATE +\r\n");
    CHECK(!strcmp(out(), "AUTHENTICATE AGFsaWNlAG1hZGUtdXAtc2VjcmV0\r\n"));
    CHECK(!remembered("made-up-secret"));
    server("AUTHENTICATE +\r\n");                           /* asked again: nothing */
    CHECK(!strcmp(out(), ""));
    server(":irc.test 900 alice alice!a@h alice :You are now logged in as alice\r\n:irc.test 903 alice :SASL successful\r\n");
    CHECK(!strcmp(out(), "CAP END\r\n") && strstr(said(0, 1), "logged in as alice"));
    /* the exact line, with the authorization identity empty */
    fresh();
    connect_as("alice", CHAT_PASS_SASL, "secret", 1);
    out();
    server(":irc.test CAP * LS :sasl\r\n:irc.test CAP * ACK :sasl\r\nAUTHENTICATE +\r\n");
    CHECK(!strcmp(out(), "CAP REQ :sasl\r\nAUTHENTICATE PLAIN\r\nAUTHENTICATE AGFsaWNlAHNlY3JldA==\r\n"));
    CHECK(!remembered("secret"));
    server(":irc.test 904 alice :SASL authentication failed\r\n");
    CHECK(!strcmp(out(), "CAP END\r\n") && strstr(said(0, 0), "not logged in"));
    /* the longest: 32 letters of nick and 256 of password, one line of 388 */
    fresh();
    memset(longpass, 'p', CHAT_PASS_MAX);
    connect_as("abcdefghijabcdefghijabcdefghijab", CHAT_PASS_SASL, longpass, 1);
    out();
    server(":irc.test CAP * LS :sasl\r\n:irc.test CAP * ACK :sasl\r\nAUTHENTICATE +\r\n");
    CHECK(strlen(out()) == strlen("CAP REQ :sasl\r\nAUTHENTICATE PLAIN\r\nAUTHENTICATE \r\n") + 388);
    CHECK(!remembered(longpass + 200));
    /* SASL not offered, or no PLAIN: said, and registration goes on; the password goes */
    fresh();
    connect_as("alice", CHAT_PASS_SASL, "made-up", 1);
    out();
    server(":irc.test CAP * LS :multi-prefix\r\n");
    CHECK(!strcmp(out(), "CAP END\r\n") && strstr(said(0, 0), "offers no SASL") && !remembered("made-up"));
    fresh();
    connect_as("alice", CHAT_PASS_SASL, "made-up", 1);
    out();
    server(":irc.test CAP * LS :sasl=EXTERNAL\r\n");
    CHECK(!strcmp(out(), "CAP END\r\n") && strstr(said(0, 0), "no PLAIN") && !remembered("made-up"));
    fresh();
    connect_as("alice", CHAT_PASS_SASL, "made-up", 1);
    out();
    server(":irc.test CAP * LS :sasl\r\n:irc.test CAP * NAK :sasl\r\n");
    CHECK(!strcmp(out(), "CAP REQ :sasl\r\nCAP END\r\n") && !remembered("made-up"));
    fresh();                                                /* a server that asks for something else */
    connect_as("alice", CHAT_PASS_SASL, "made-up", 1);
    out();
    server(":irc.test CAP * LS :sasl\r\n:irc.test CAP * ACK :sasl\r\nAUTHENTICATE :give me more\r\n");
    CHECK(!strcmp(out(), "CAP REQ :sasl\r\nAUTHENTICATE PLAIN\r\nAUTHENTICATE *\r\nCAP END\r\n") && !remembered("made-up"));
    /* a server's password goes first, and only once; without TLS only as the person agreed (11/09/26, the review) */
    fresh();
    connect_as("alice", CHAT_PASS_SERVER, "made-up", 0);
    CHECK(!strcmp(out(), "CAP LS 302\r\nNICK alice\r\nUSER alice 0 * :alice\r\n"));
    CHECK(!remembered("made-up") && !strcmp(said(0, 0), "The password was not sent: this connection has no encryption"));
    CHECK(!strcmp(said(0, 2), "The password will not be sent: this connection has no TLS"));
    fresh();
    connect_as("alice", CHAT_PASS_SASL, "made-up", 0);     /* SASL's PLAIN in base64 is the password itself */
    CHECK(!strcmp(out(), "CAP LS 302\r\nNICK alice\r\nUSER alice 0 * :alice\r\n"));
    server(":irc.test CAP * LS :sasl\r\n:irc.test AUTHENTICATE +\r\n");
    CHECK(!strcmp(out(), "CAP END\r\n") && !remembered("made-up") && strstr(said(0, 0), "SASL login was not sent"));
    plain_ok = 1;
    fresh();
    connect_as("alice", CHAT_PASS_SERVER, "made-up", 0);
    CHECK(!strcmp(out(), "CAP LS 302\r\nPASS made-up\r\nNICK alice\r\nUSER alice 0 * :alice\r\n"));
    CHECK(!remembered("made-up") && strstr(said(0, 1), "as you agreed"));
    fresh();
    connect_as("alice", CHAT_PASS_SASL, "made-up", 0);
    out();
    server(":irc.test CAP * LS :sasl\r\n:irc.test CAP * ACK :sasl\r\n:irc.test AUTHENTICATE +\r\n");
    CHECK(!strcmp(out(), "CAP REQ :sasl\r\nAUTHENTICATE PLAIN\r\nAUTHENTICATE AGFsaWNlAG1hZGUtdXA=\r\n"));
    CHECK(!remembered("made-up"));
    fresh();                                                /* the last check, as AUTHENTICATE is to go */
    connect_as("alice", CHAT_PASS_SASL, "made-up", 0);
    out();
    server(":irc.test CAP * LS :sasl\r\n:irc.test CAP * ACK :sasl\r\n");
    CHECK(!strcmp(out(), "CAP REQ :sasl\r\nAUTHENTICATE PLAIN\r\n"));
    C.p.plain_ok = 0;
    server(":irc.test AUTHENTICATE +\r\n");
    CHECK(!strcmp(out(), "AUTHENTICATE *\r\nCAP END\r\n") && !remembered("made-up"));
    plain_ok = 0;
    /* a login the server stops answering: each step has 30 seconds, then the password goes (L4) */
    fresh();
    connect_as("alice", CHAT_PASS_SASL, "made-up", 1);
    out();
    server(":irc.test CAP * LS :sasl\r\n:irc.test CAP * ACK :sasl\r\n");
    CHECK(!strcmp(out(), "CAP REQ :sasl\r\nAUTHENTICATE PLAIN\r\n") && remembered("made-up"));
    CHECK(chat_wait(&C) > 0 && chat_wait(&C) <= CHAT_CAP_MS);
    C.now += CHAT_CAP_MS - 1;
    chat_tick(&C);
    CHECK(!strcmp(out(), "") && remembered("made-up"));
    C.now += 1;
    chat_tick(&C);
    CHECK(!strcmp(out(), "AUTHENTICATE *\r\nCAP END\r\n") && !remembered("made-up") && C.cap == CAP_DONE);
    CHECK(!strcmp(said(0, 0), "The server did not answer the SASL login: you are not logged in"));
    fresh();                                                /* CAP REQ with no ACK or NAK */
    connect_as("alice", CHAT_PASS_SASL, "made-up", 1);
    out();
    server(":irc.test CAP * LS :sasl\r\n");
    CHECK(!strcmp(out(), "CAP REQ :sasl\r\n"));
    C.now += CHAT_CAP_MS;
    chat_tick(&C);
    CHECK(!strcmp(out(), "CAP END\r\n") && !remembered("made-up"));
    fresh();                                                /* the login sent, and no 903 or 904 */
    connect_as("alice", CHAT_PASS_SASL, "made-up", 1);
    out();
    server(":irc.test CAP * LS :sasl\r\n:irc.test CAP * ACK :sasl\r\n:irc.test AUTHENTICATE +\r\n");
    out();
    C.now += CHAT_CAP_MS;
    chat_tick(&C);
    CHECK(!strcmp(out(), "AUTHENTICATE *\r\nCAP END\r\n") && C.cap == CAP_DONE);
    fresh();
    connect_as("alice", CHAT_PASS_SERVER, ":two words", 1);
    CHECK(!strcmp(out(), "CAP LS 302\r\nPASS ::two words\r\nNICK alice\r\nUSER alice 0 * :alice\r\n"));
    /* the connection ending takes what is left of one */
    fresh();
    connect_as("alice", CHAT_PASS_SASL, "made-up", 1);
    chat_down(&C, "reset");
    CHECK(!remembered("made-up") && C.state == CHAT_OFF);
    snprintf(want, sizeof(want), "%s", said(0, 0));
    CHECK(!strcmp(want, "Disconnected: reset"));
}

static void channels(void) {
    int r = in_surf(), i, order[8];
    struct chat_room *s = C.room[r];
    char marks[16];
    chat_nick_order(&C, s, order);
    for(i = 0; i < 4; i++) marks[i] = chat_nick_mark(&C, &s->nicks[order[i]]);
    CHECK(!strncmp(marks, "@+  ", 4) && !strcmp(s->nicks[order[0]].name, "carol") && !strcmp(s->nicks[order[3]].name, "bob"));
    server(":erin!e@h JOIN #surf\r\n");
    CHECK(s->nnicks == 5 && !strcmp(said(r, 0), "erin (e@h) joined"));
    server(":erin!e@h PART #surf :bye now\r\n");
    CHECK(s->nnicks == 4 && !strcmp(said(r, 0), "erin left (bye now)"));
    server(":Bob!b@h NICK :robert\r\n");
    CHECK(chat_nick_find(&C, s, "ROBERT", NULL) >= 0 && chat_nick_find(&C, s, "bob", NULL) < 0);
    CHECK(!strcmp(said(r, 0), "Bob is now known as robert"));
    server(":carol!c@h MODE #surf +v-o+bk robert carol x!*@* key\r\n");
    i = chat_nick_find(&C, s, "carol", NULL);
    CHECK(i >= 0 && s->nicks[i].modes == 0);
    i = chat_nick_find(&C, s, "robert", NULL);
    CHECK(i >= 0 && chat_nick_mark(&C, &s->nicks[i]) == '+');
    server(":carol!c@h MODE #surf +lh 10 robert\r\n");     /* l takes one when set: h then names robert */
    CHECK(chat_nick_mark(&C, &s->nicks[i]) == '%');
    server(":carol!c@h MODE #surf -lh robert\r\n");         /* and none when unset */
    CHECK(chat_nick_mark(&C, &s->nicks[i]) == '+');
    server(":carol!c@h TOPIC #surf :High tide\r\n");
    CHECK(!strcmp(s->topic, "High tide") && !strcmp(said(r, 0), "carol changed the topic to: High tide"));
    server(":carol!c@h TOPIC #surf :\r\n");
    CHECK(!s->topic && !strcmp(said(r, 0), "carol removed the topic"));
    server(":carol!c@h KICK #surf robert :splash\r\n");
    CHECK(s->nnicks == 3 && !strcmp(said(r, 0), "carol kicked robert (splash)"));
    server(":dave!d@h QUIT :Ping timeout\r\n");
    CHECK(s->nnicks == 2 && !strcmp(said(r, 0), "dave quit (Ping timeout)"));
    server(":carol!c@h KICK #surf alice :out\r\n");         /* us: the room stays, not joined */
    CHECK(!s->joined && s->nnicks == 0 && strstr(said(r, 0), "You were kicked from #surf by carol (out)"));
    server(":alice!al@h JOIN #surf\r\n:irc.test 353 alice @ #surf :@alice carol\r\n");
    CHECK(s->joined && s->nnicks == 2 && chat_find(&C, ROOM_CHANNEL, "#surf") == r);
    chat_input(&C, "/part");                                /* our /part: the room goes with the PART */
    CHECK(!strcmp(out(), "PART #surf\r\n") && s->parting);
    server(":alice!al@h PART #surf\r\n");
    CHECK(chat_find(&C, ROOM_CHANNEL, "#surf") < 0 && C.rooms == 1 && C.active == 0);
    /* a PART that was not ours keeps the room */
    in_surf();
    server(":alice!al@h PART #surf :forced\r\n");
    CHECK(chat_find(&C, ROOM_CHANNEL, "#surf") == 1 && !C.room[1]->joined && strstr(said(1, 0), "You left #surf (forced)"));
    /* a JOIN nobody asked for: a room, not in front; a JOIN to a channel with no room for others is passed over */
    online();
    server(":alice!a@h JOIN #forced\r\n:bob!b@h JOIN #elsewhere\r\n");
    CHECK(chat_find(&C, ROOM_CHANNEL, "#forced") == 1 && C.active == 0 && C.room[1]->unread == 1 && C.rooms == 2);
    /* our nick changing; RFC 1459 names: <channel> :names without the symbol */
    server(":irc.test 353 alice #forced :@bob alice\r\n");
    CHECK(C.room[1]->nnicks == 2);
    server(":alice!a@h NICK alicia\r\n");
    CHECK(!strcmp(C.nick, "alicia") && chat_nick_find(&C, C.room[1], "alicia", NULL) >= 0);
    CHECK(!strcmp(said(1, 0), "You are now known as alicia"));
    /* CASEMAPPING=ascii after the lists: sorted again, and [ ] no longer { } */
    server(":x[y]!a@h JOIN #forced\r\n:x{y}!a@h JOIN #forced\r\n");
    CHECK(C.room[1]->nnicks == 3);
    server(":irc.test 005 alicia CASEMAPPING=ascii :are supported\r\n");
    CHECK(C.casemap == IRC_CM_ASCII && C.room[1]->nnicks == 3);
    server(":x{y}!a@h JOIN #forced\r\n");
    CHECK(C.room[1]->nnicks == 4);
    server(":irc.test 005 alicia CASEMAPPING=rfc1459 :are supported\r\n");   /* and back: once a connection, ignored */
    CHECK(C.casemap == IRC_CM_ASCII && C.room[1]->nnicks == 4 && consistent());
}

/* 11/09/26, the review's findings: CASEMAPPING once a connection (M2), the server's own text under its name (M3),
   channels' names cleaned where they are shown (L6), NickServ's passwords neither shown nor kept (L8) */
static void review(void) {
    static char line[1 << 16];
    const struct chat_line *l;
    char title[300];
    int r, i, k, o, own = 0, theirs = 0;
    /* M2: a server that flips CASEMAPPING over full lists re-sorts nothing after the first */
    online();
    chat_input(&C, "/join #big");
    out();
    server(":alice!a@h JOIN #big\r\n");
    r = chat_find(&C, ROOM_CHANNEL, "#big");
    for(i = 0; i < CHAT_NICKS;) {
        o = snprintf(line, sizeof(line), ":irc.test 353 alice = #big :");
        for(k = 0; k < 400 && i < CHAT_NICKS; k++, i++) o += snprintf(line + o, sizeof(line) - (size_t)o, "%sn%05d", k ? " " : "", CHAT_NICKS - 1 - i);
        snprintf(line + o, sizeof(line) - (size_t)o, "\r\n");
        server(line);
    }
    CHECK(r > 0 && C.room[r]->nnicks == CHAT_NICKS && consistent());
    server(":irc.test 005 alice CASEMAPPING=ascii :are supported\r\n");
    CHECK(C.casemap == IRC_CM_ASCII && C.casemap_set);
    o = snprintf(line, sizeof(line), ":irc.test 005 alice");
    for(k = 0; k < 13; k++) o += snprintf(line + o, sizeof(line) - (size_t)o, " CASEMAPPING=%s", k % 2 ? "ascii" : "rfc1459");
    snprintf(line + o, sizeof(line) - (size_t)o, " :are supported\r\n");
    for(i = 0; i < 50; i++) server(line);
    CHECK(C.casemap == IRC_CM_ASCII && consistent());
    chat_down(&C, "reset");                         /* a new connection takes it again */
    connect_as("alice", CHAT_PASS_NONE, NULL, 1);
    CHECK(!C.casemap_set && C.casemap == IRC_CM_RFC1459);
    server(":irc.test 005 alice CASEMAPPING=strict-rfc1459 :x\r\n");
    CHECK(C.casemap == IRC_CM_STRICT);
    /* M3: a numeric that says what the Chat says is the server's, under its name; the Chat's own line has none */
    online();
    server(":irc.test 002 alice :Connected with TLSv1.3: the server's certificate is verified\r\n"
           "002 alice :no prefix at all\r\n:\xe2\x89\x8b\x07x 421 alice FOO :Unknown command\r\n");
    for(k = 0; k < C.room[0]->count; k++) {
        l = chat_line_at(C.room[0], k);
        if(strcmp(l->text, "Connected with TLSv1.3: the server's certificate is verified")) continue;
        if(!l->from[0]) own++;
        else if(!strcmp(l->from, "irc.test")) theirs++;
    }
    CHECK(own == 1 && theirs == 1);
    CHECK(!strcmp(newest(0, 0)->from, "x") && !strcmp(newest(0, 0)->text, "FOO: Unknown command"));
    CHECK(!strcmp(newest(0, 1)->from, "irc.test") && !strcmp(newest(0, 1)->text, "no prefix at all"));
    server("NOTICE * :*** Looking up your hostname\r\n");
    CHECK(newest(0, 0)->kind == LN_NOTICE && !strcmp(newest(0, 0)->from, "irc.test"));
    /* L6: a channel's name with a direction override and bytes that are no UTF-8, in a KICK and the title */
    server(":alice!a@h JOIN #a\xe2\x80\xae\xff\xfe\r\n");
    r = chat_find(&C, ROOM_CHANNEL, "#a\xe2\x80\xae\xff\xfe");
    /* no UTF-8: windows-1252 byte by byte, the override's bytes three characters that do nothing */
    CHECK(r > 0 && !strcmp(chat_room_label(&C, r), "#a\xc3\xa2\xe2\x82\xac\xc2\xae\xc3\xbf\xc3\xbe"));
    chat_select(&C, r);
    chat_title(&C, title, sizeof(title));
    CHECK(!strcmp(title, "Chat - #a\xc3\xa2\xe2\x82\xac\xc2\xae\xc3\xbf\xc3\xbe"));
    server(":op!o@h KICK #a\xe2\x80\xae\xff\xfe alice :bye\r\n");
    CHECK(!strcmp(said(r, 0), "You were kicked from #a\xc3\xa2\xe2\x82\xac\xc2\xae\xc3\xbf\xc3\xbe by op (bye)"));
    server(":alice!a@h JOIN #b\xe2\x80\xaexy\r\n");     /* UTF-8: the override goes (no name holds a control) */
    r = chat_find(&C, ROOM_CHANNEL, "#b\xe2\x80\xaexy");
    CHECK(r > 0 && !strcmp(chat_room_label(&C, r), "#bxy") && !strcmp(C.room[r]->name, "#b\xe2\x80\xaexy"));
    /* L8: NickServ's commands shown with their first word, the password nowhere once sent */
    chat_select(&C, 0);
    CHECK(chat_secret(&C, "/msg NickServ IDENTIFY made-up-pw") && chat_secret(&C, "/MSG nickserv\tIDENTIFY x"));
    CHECK(chat_secret(&C, "/query NickServ") && !chat_secret(&C, "/msg bob IDENTIFY x") && !chat_secret(&C, "hello"));
    chat_input(&C, "/msg NickServ IDENTIFY made-up-pw");
    CHECK(!strcmp(said(0, 0), "\xe2\x86\x92 NickServ: IDENTIFY ****") && remembered("made-up-pw"));    /* queued */
    CHECK(!strcmp(out(), "PRIVMSG NickServ :IDENTIFY made-up-pw\r\n") && !remembered("made-up-pw"));
    chat_input(&C, "/query NickServ");
    CHECK(C.room[C.active]->kind == ROOM_QUERY && chat_secret(&C, "GHOST alice made-up-pw2") &&
          chat_secret(&C, "/me waves") && chat_secret(&C, "//msg bob x"));   /* "//": a line for NickServ too */
    chat_input(&C, "GHOST alice made-up-pw2");
    CHECK(!strcmp(said(C.active, 0), "GHOST ****"));
    CHECK(!strcmp(out(), "PRIVMSG NickServ :GHOST alice made-up-pw2\r\n") && !remembered("made-up-pw2"));
    chat_input(&C, "HELP");
    CHECK(!strcmp(said(C.active, 0), "HELP"));
    out();
}

static void messages(void) {
    int r = in_surf(), q;
    server(":bob!b@h PRIVMSG #surf :hi Alice, surf's up\r\n");
    CHECK(!strcmp(said(r, 0), "hi Alice, surf's up") && !strcmp(newest(r, 0)->from, "bob"));
    CHECK(newest(r, 0)->flags == LF_HIGHLIGHT && newest(r, 0)->minute == 630);
    server(":bob!b@h PRIVMSG #surf :malice aforethought\r\n");     /* a word, not inside one */
    CHECK(newest(r, 0)->flags == 0);
    server(":bob!b@h PRIVMSG #surf :\001ACTION waves at alice\001\r\n");
    CHECK(newest(r, 0)->kind == LN_ACTION && !strcmp(said(r, 0), "waves at alice") && newest(r, 0)->flags == LF_HIGHLIGHT);
    server(":bob!b@h PRIVMSG #surf :\001VERSION\001\r\n:bob!b@h PRIVMSG alice :\001DCC SEND x 1 2 3\001\r\n");
    CHECK(!strcmp(out(), "") && strstr(said(0, 1), "CTCP VERSION request, which Chat does not answer"));
    CHECK(strstr(said(0, 0), "DCC connection") && C.rooms == 2);
    server(":bob!b@h NOTICE alice :\001PING 1\001\r\n");
    CHECK(!strcmp(said(0, 0), "CTCP PING reply: 1") && !strcmp(out(), ""));
    server(":bob!b@h PRIVMSG alice :psst\r\n");            /* a query, made for it */
    q = chat_find(&C, ROOM_QUERY, "Bob");
    CHECK(q == 2 && !strcmp(said(q, 0), "psst") && C.room[q]->highlight && C.room[q]->unread == 1 && C.active == r);
    server(":bob!b@h NOTICE alice :a notice\r\n");          /* to the query that is there */
    CHECK(!strcmp(said(q, 0), "a notice") && newest(q, 0)->kind == LN_NOTICE);
    server(":NickServ!s@services. NOTICE alice :This nick is registered\r\n");   /* no query: the room in front */
    CHECK(!strcmp(said(r, 0), "This nick is registered") && chat_find(&C, ROOM_QUERY, "NickServ") < 0);
    server(":irc.test NOTICE * :*** Looking up your hostname\r\n");
    CHECK(!strcmp(said(0, 0), "*** Looking up your hostname") && !strcmp(newest(0, 0)->from, "irc.test"));
    server(":carol!c@h PRIVMSG @#surf :ops only\r\n");     /* STATUSMSG was not said: no channel by that name */
    CHECK(strstr(said(0, 0), "[@#surf] ops only"));
    server(":irc.test 005 alice STATUSMSG=@+ :ok\r\n:carol!c@h PRIVMSG @#surf :ops only\r\n");
    CHECK(!strcmp(said(r, 0), "[@] ops only"));
    server(":bob!b@h PRIVMSG #elsewhere :hmm\r\n");         /* a channel with no room */
    CHECK(!strcmp(said(0, 0), "[#elsewhere] hmm"));
    server(":b\x01" "d!b@h PRIVMSG alice :from a bad nick\r\n");   /* a nick no list may hold: the server's room */
    CHECK(!strcmp(said(0, 0), "from a bad nick") && !strcmp(newest(0, 0)->from, "bd") && C.rooms == 3);
    server(":bob!b@h PRIVMSG #surf :\x1b[2J\x03" "4red \x07\xe9t\xe9\r\n");   /* escapes out, codes in, Latin-1 */
    CHECK(!strcmp(said(r, 0), "[2J\x03" "4red \xc3\xa9t\xc3\xa9"));
    server(":bob!b@h INVITE alice :#secret\r\n");
    CHECK(strstr(said(r, 0), "bob invites you to #secret"));
    chat_select(&C, q);
    CHECK(!C.room[q]->highlight && !C.room[q]->unread && C.active == q);
    server(":bob!b@h PRIVMSG alice :again\r\n");
    CHECK(C.room[q]->unread == 0 && (C.dirty & CHAT_DIRTY_LINES));
    /* the query follows its nick, not onto another query's */
    server(":bob!b@h NICK bobby\r\n");
    CHECK(chat_find(&C, ROOM_QUERY, "bobby") == q);
    server(":carol!c@h PRIVMSG alice :hi\r\n:carol!c@h NICK bobby\r\n");
    CHECK(chat_find(&C, ROOM_QUERY, "carol") == 3 && chat_find(&C, ROOM_QUERY, "bobby") == q);
    server(":bobby!b@h QUIT :gone\r\n");
    CHECK(!strcmp(said(q, 0), "bobby quit (gone)"));
}

static void pings(void) {
    int i;
    online();
    server("PING :irc.test\r\n");
    CHECK(!strcmp(out(), "PONG :irc.test\r\n"));
    server("PING\r\n");
    CHECK(!strcmp(out(), "PONG\r\n"));
    for(i = 0; i < 100; i++) server("PING :flood\r\n");     /* answered twice, the rest dropped */
    CHECK(!strcmp(out(), "PONG :flood\r\nPONG :flood\r\n"));
    /* quiet for 90 s: our PING; 60 s more: lost */
    C.now += CHAT_IDLE_MS - 1;
    chat_tick(&C);
    CHECK(!strcmp(out(), "") && chat_wait(&C) == 1);
    C.now += 1;
    chat_tick(&C);
    CHECK(!strcmp(out(), "PING :wchat\r\n") && C.ping_at == C.now);
    C.now += CHAT_PING_MS - 1;
    chat_tick(&C);
    CHECK(!C.ending);
    server(":irc.test PONG irc.test :wchat\r\n");           /* anything at all from the server answers it */
    CHECK(!C.ping_at);
    C.now += CHAT_IDLE_MS;
    chat_tick(&C);
    out();
    C.now += CHAT_PING_MS;
    chat_tick(&C);
    CHECK(C.ending && strstr(C.why, "150 seconds") && chat_wait(&C) == 0);
    chat_down(&C, C.why);
    CHECK(C.state == CHAT_OFF && !C.ending && !strcmp(said(0, 0), "Disconnected: the server has not answered for 150 seconds"));
}

/* the server's line for one of our PRIVMSGs: at most 512 bytes with our nick, user and host in front */
static int fits(const char *line, const char *userhost) {
    return (int)(1 + strlen(C.nick) + 1 + strlen(userhost) + 1 + strlen(line) + 2) <= IRC_LINE_MAX;
}

/* what goes after a while: the pacing's burst again */
static const char *flush(void) {
    C.now += 60000;
    return out();
}

/* the bytes of the messages sent now, without what PRIVMSG puts around them */
static int sent_bytes(void) {
    char *p, *q, *t;
    int n = 0;
    while(*flush())                                 /* every burst until the queue is empty */
        for(p = sent; *p; p = q + 2) {
            q = strstr(p, "\r\n");
            t = strstr(p, " :");
            if(!q || !t) break;
            n += (int)(q - t - 2);
        }
    return n;
}

static void typing(void) {
    static char big[CHAT_INPUT_MAX + 100], again[CHAT_INPUT_MAX + 100];
    int r = in_surf(), n, i;
    char *p, *q;
    chat_input(&C, "hello  there ");
    CHECK(!strcmp(flush(), "PRIVMSG #surf :hello  there \r\n") && !strcmp(said(r, 0), "hello  there "));
    CHECK(newest(r, 0)->flags == LF_SELF && !strcmp(newest(r, 0)->from, "alice"));
    chat_input(&C, "//slash");
    CHECK(!strcmp(flush(), "PRIVMSG #surf :/slash\r\n"));
    chat_input(&C, "one\r\nQUIT :ha\ttwo\001three");       /* a paste of lines: one message, nothing else */
    CHECK(!strcmp(flush(), "PRIVMSG #surf :one  QUIT :ha twothree\r\n"));
    chat_input(&C, "\x02" "bold\x02 and \x03" "4red");
    CHECK(!strcmp(flush(), "PRIVMSG #surf :\x02" "bold\x02 and \x03" "4red\r\n"));
    chat_input(&C, "/me ducks");
    CHECK(!strcmp(flush(), "PRIVMSG #surf :\001ACTION ducks\001\r\n") && newest(r, 0)->kind == LN_ACTION);
    chat_input(&C, "   ");
    CHECK(!strcmp(flush(), "") && !strcmp(said(r, 0), "ducks"));
    chat_input(&C, "/MSG bob psst there");
    CHECK(!strcmp(flush(), "PRIVMSG bob :psst there\r\n") && strstr(said(r, 0), "\xe2\x86\x92 bob: psst there"));
    chat_input(&C, "/msg #surf to the channel");            /* its room shows it as ours */
    CHECK(!strcmp(flush(), "PRIVMSG #surf :to the channel\r\n") && !strcmp(said(r, 0), "to the channel"));
    chat_input(&C, "/msg b,c x");
    CHECK(!strcmp(flush(), "") && strstr(said(r, 0), "is not a nick"));
    chat_input(&C, "/msg bob");
    CHECK(!strcmp(flush(), "") && strstr(said(r, 0), "/msg nick text"));
    chat_input(&C, "/topic New waves");
    CHECK(!strcmp(flush(), "TOPIC #surf :New waves\r\n"));
    chat_input(&C, "/topic");
    CHECK(!strcmp(flush(), "TOPIC #surf\r\n"));
    chat_input(&C, "/nick ali ce");
    CHECK(!strcmp(flush(), "NICK ali\r\n"));
    chat_input(&C, "/nick a:b,c");
    CHECK(!strcmp(flush(), "") && strstr(said(r, 0), "/nick NAME"));
    chat_input(&C, "/join #a,b,&c key");                    /* & is no channel type here (CHANTYPES=#) */
    CHECK(!strcmp(flush(), "JOIN #a,#b,#&c key\r\n"));
    chat_input(&C, "/join #a,b c d");
    CHECK(!strcmp(flush(), "JOIN #a,#b c\r\n"));
    chat_input(&C, "/join #surf");                          /* in it: its room comes in front */
    CHECK(!strcmp(flush(), "") && C.active == r);
    chat_input(&C, "/join");
    CHECK(strstr(said(r, 0), "which channel"));
    chat_input(&C, "/join #a\x7f" "b");                     /* DEL is taken out of what is typed */
    CHECK(!strcmp(flush(), "JOIN #ab\r\n"));
    chat_input(&C, "/frobnicate now");
    CHECK(!strcmp(flush(), "") && strstr(said(r, 0), "There is no command /frobnicate"));
    chat_input(&C, "/query carol hi there");                /* a query, in front, and the message */
    n = chat_find(&C, ROOM_QUERY, "carol");
    CHECK(n > 0 && C.active == n && !strcmp(flush(), "PRIVMSG carol :hi there\r\n") && !strcmp(said(n, 0), "hi there"));
    chat_input(&C, "/topic x");
    CHECK(!strcmp(flush(), "") && strstr(said(n, 0), "A topic is a channel's"));
    chat_input(&C, "/part");
    CHECK(!strcmp(flush(), "") && strstr(said(n, 0), "which channel"));
    chat_input(&C, "/part #surf gone surfing");
    CHECK(!strcmp(flush(), "PART #surf :gone surfing\r\n") && C.room[r]->parting);
    chat_select(&C, 0);
    chat_input(&C, "hello server");
    CHECK(!strcmp(flush(), "") && strstr(said(0, 0), "This is the server's room"));
    /* a message longer than one PRIVMSG: pieces that fit, cut at spaces, all of it sent */
    chat_select(&C, r);
    for(i = 0, p = big; i < 300; i++) p += snprintf(p, 8, "w%d ", i);
    p[-1] = 0;
    chat_input(&C, big);
    flush();
    again[0] = 0;
    for(p = sent, n = 0; *p; p = q + 2, n++) {
        q = strstr(p, "\r\n");
        *q = 0;
        CHECK(!strncmp(p, "PRIVMSG #surf :", 15) && fits(p, C.user));
        if(n) strcat(again, " ");
        strcat(again, p + 15);
    }
    CHECK(n == 3 && !strcmp(again, big));
    /* and before our JOIN showed our host: room for the longest */
    online();
    C.user[0] = 0;
    memset(big, 'x', 1500);
    big[1500] = 0;
    server(":alice JOIN #q\r\n");                           /* no user or host: nothing to keep */
    CHECK(chat_find(&C, ROOM_CHANNEL, "#q") == 1 && !C.user[0]);
    chat_select(&C, 1);
    chat_input(&C, big);
    flush();
    for(p = sent, n = 0; *p; p = q + 2, n++) {
        q = strstr(p, "\r\n");
        *q = 0;
        CHECK(fits(p, "abcdefghij@abcdefghijabcdefghijabcdefghijabcdefghijabcdefghijabc"));
    }
    CHECK(n == 4);
    /* not connected, or not yet registered: nothing goes */
    fresh();
    chat_input(&C, "/join #x");
    CHECK(strstr(said(0, 0), "Not connected"));
    chat_input(&C, "/quit");
    CHECK(strstr(said(0, 0), "Not connected"));
    connect_as("alice", CHAT_PASS_NONE, NULL, 1);
    out();
    chat_input(&C, "/join #x");
    CHECK(!strcmp(flush(), "") && strstr(said(0, 0), "Not registered yet"));
    /* a line over CHAT_INPUT_MAX is cut, between characters */
    online();
    chat_input(&C, "/query bob");
    for(i = 0; i < CHAT_INPUT_MAX + 50; i += 2) memcpy(big + i, "\xc3\xa9", 2);
    big[CHAT_INPUT_MAX + 50] = 0;
    chat_input(&C, big);
    CHECK(sent_bytes() == CHAT_INPUT_MAX && !strcmp(said(C.active, 0) + strlen(said(C.active, 0)) - 2, "\xc3\xa9"));
    big[CHAT_INPUT_MAX + 2] = 0;                            /* an odd cut: the half character goes */
    chat_input(&C, big + 1);
    CHECK(sent_bytes() == CHAT_INPUT_MAX - 1);
}

static void pacing(void) {
    int i, n;
    in_surf();
    C.now += 60000;
    for(i = 0; i < 10; i++) chat_input(&C, "line");
    CHECK(C.qcount == 10);
    n = (int)strlen(out()) / (int)strlen("PRIVMSG #surf :line\r\n");
    CHECK(n == 4 && chat_wait(&C) == 1);                    /* a burst of four, then one every two seconds */
    C.now += 1;
    CHECK(strlen(out()) == strlen("PRIVMSG #surf :line\r\n") && chat_wait(&C) == CHAT_LINE_MS);
    C.now += CHAT_LINE_MS - 1;
    CHECK(!strcmp(out(), ""));
    server("PING :x\r\n");                                  /* a PONG goes at once, ahead of them */
    CHECK(!strcmp(out(), "PONG :x\r\n"));
    C.now += 60000;
    n = (int)strlen(out()) / (int)strlen("PRIVMSG #surf :line\r\n");
    CHECK(n == 4 && C.qcount == 1);
    chat_quit(&C, "bye");                                   /* what waits is dropped: QUIT goes */
    CHECK(!strcmp(out(), "QUIT :bye\r\n") && C.state == CHAT_QUITTING && strstr(said(C.active, 0), "1 line waiting"));
    chat_input(&C, "more");
    CHECK(!strcmp(out(), "") && strstr(said(C.active, 0), "Leaving"));
    C.now += CHAT_QUIT_MS;
    chat_tick(&C);
    CHECK(C.ending && !strcmp(C.why, "closed after QUIT"));
    chat_down(&C, "closed after QUIT");
    CHECK(!strcmp(said(1, 0), "Disconnected: closed after QUIT") && newest(1, 0)->kind == LN_INFO);
    CHECK(!C.room[1]->joined && !C.room[1]->nnicks && C.nicks_all == 0);
    /* the queue's limit */
    in_surf();
    for(i = 0; i < CHAT_QUEUE + 5; i++) chat_input(&C, "flood");
    CHECK(C.qcount == CHAT_QUEUE && strstr(said(1, 0), "too much is waiting"));
    /* a connection being made ends at once */
    fresh();
    {
        struct chat_params p;
        char err[100];
        params(&p, "alice", CHAT_PASS_NONE, NULL, 1);
        CHECK(!chat_connect(&C, &p, err, sizeof(err)) && chat_connect(&C, &p, err, sizeof(err)) == -1);
        chat_quit(&C, NULL);
        CHECK(C.ending && !strcmp(C.why, "cancelled"));
        chat_down(&C, "cancelled");
        CHECK(!strcmp(said(0, 0), "Not connected: cancelled"));
        strcpy(p.host, "other.test");                       /* another server: the old rooms go */
        CHECK(!chat_connect(&C, &p, err, sizeof(err)));
    }
}

static void windowside(void) {
    char s[160];
    int r = in_surf();
    chat_status(&C, s, sizeof(s));
    CHECK(!strcmp(s, "alice on irc.test, port 6697 (TLSv1.3)"));
    chat_title(&C, s, sizeof(s));
    CHECK(!strcmp(s, "Chat - #surf"));
    CHECK(!strcmp(chat_room_label(&C, 0), "Test Net") && !strcmp(chat_room_label(&C, r), "#surf"));
    CHECK(chat_close_room(&C, 0) == -1);
    CHECK(chat_close_room(&C, r) == 0 && !strcmp(out(), "PART #surf\r\n"));
    server(":alice!a@h PART #surf\r\n");
    CHECK(C.rooms == 1);
    chat_input(&C, "/query bob");
    CHECK(C.rooms == 2 && chat_close_room(&C, 1) == 0 && C.rooms == 1 && C.active == 0);
    chat_down(&C, "x");
    chat_status(&C, s, sizeof(s));
    CHECK(!strcmp(s, "Not connected"));
}

/**** a hostile server ****/

/* every list of the model as it should be: sorted, counted, bytes added up */
static int consistent(void) {
    long bytes = 0;
    int i, j, nicks = 0;
    for(i = 0; i < C.rooms; i++) {
        struct chat_room *r = C.room[i];
        long rb = 0;
        if(r->count > CHAT_LINES || r->nnicks > CHAT_NICKS) return 0;
        for(j = 0; j < r->count; j++) {
            const struct chat_line *l = chat_line_at(r, j);
            rb += (long)(sizeof(*l) + strlen(l->from) + strlen(l->text) + 2);
        }
        if(rb != r->bytes) return 0;
        bytes += rb;
        for(j = 1; j < r->nnicks; j++) if(irc_casecmp(C.casemap, r->nicks[j - 1].name, r->nicks[j].name) >= 0) return 0;
        nicks += r->nnicks;
    }
    return bytes == C.bytes && nicks == C.nicks_all && C.bytes <= CHAT_BYTES && C.nicks_all <= CHAT_NICKS_ALL &&
           C.rooms <= CHAT_ROOMS && C.active >= 0 && C.active < C.rooms;
}

static void limits(void) {
    static char line[1200];
    int i;
    in_surf();
    for(i = 0; i < 200; i++) {                              /* queries from 200 nicks: 64 rooms, the rest the server's */
        snprintf(line, sizeof(line), ":n%d!u@h PRIVMSG alice :hi %d\r\n", i, i);
        server(line);
    }
    CHECK(C.rooms == CHAT_ROOMS && !strcmp(said(0, 0), "(private) hi 199") && consistent());
    in_surf();
    for(i = 0; i < CHAT_NICKS + 100; i += 100) {            /* a list longer than kept, said once */
        int k, o = snprintf(line, sizeof(line), ":irc.test 353 alice = #surf :");
        for(k = 0; k < 100; k++) o += snprintf(line + o, sizeof(line) - (size_t)o, "%sn%d", k ? " " : "", i + k);
        snprintf(line + o, sizeof(line) - (size_t)o, "\r\n");
        server(line);
    }
    CHECK(C.room[1]->nnicks == CHAT_NICKS && C.room[1]->nicks_over == 2 && consistent());
    CHECK(strstr(said(1, 0), "longer than Chat keeps"));
    for(i = 0; i < 3000; i++) server(":bob!b@h PRIVMSG #surf :a line to fill the room with\r\n");
    CHECK(C.room[1]->count == CHAT_LINES && C.room[1]->seq > 3000 && consistent());
    /* long lines in many rooms: every room's together within CHAT_BYTES, each room keeps its newest */
    online();
    memset(line, 'z', sizeof(line));
    for(i = 0; i < 60; i++) {
        snprintf(line, 60, ":alice!a@h JOIN #c%d\r\n", i);
        server(line);
    }
    for(i = 0; i < 9000; i++) {
        int o = snprintf(line, 40, ":bob!b@h PRIVMSG #c%d :", i % 60);
        memset(line + o, 'z', 1000);
        strcpy(line + o + 1000, "\r\n");
        server(line);
    }
    CHECK(C.bytes <= CHAT_BYTES && consistent());
    for(i = 1; i < C.rooms; i++) CHECK(C.room[i]->count > 0);
    /* ISUPPORT that is no such thing */
    server(":irc.test 005 alice PREFIX=(ov)@ PREFIX=(abcdefghijklmnopq)!!!!!!!!!!!!!!!!! CHANTYPES=#\001 CHANMODES=,,,,,,"
           " NICKLEN=-3 NETWORK=\\x0a\\x0d\\x00 CASEMAPPING= = - -FOO :x\r\n");
    CHECK(!strcmp(C.prefix_chars, "@%+") && !strcmp(C.chantypes, "#") && C.nicklen == 0 && !strcmp(C.network, ""));
    server(":irc.test 005 alice -PREFIX CHANMODES=b PREFIX= :x\r\n");
    CHECK(!strcmp(C.prefix_modes, "") && !strcmp(C.chanmodes[0], "b") && !strcmp(C.chanmodes[1], "") && consistent());
    /* numerics short of their parameters */
    for(i = 0; i < 1000; i++) {
        static const char *const cmds[] = { "PRIVMSG", "NOTICE", "JOIN", "PART", "QUIT", "NICK", "KICK", "TOPIC", "MODE",
                                            "CAP", "AUTHENTICATE", "INVITE", "KILL", "ERROR", "PING" };
        int k, argc = (int)rnd(4), o;
        if(i % 2) o = snprintf(line, sizeof(line), ":x!y@z %s", cmds[rnd(15)]);
        else o = snprintf(line, sizeof(line), ":irc.test %03u", rnd(1000));
        for(k = 0; k < argc; k++) o += snprintf(line + o, sizeof(line) - (size_t)o, " %s", rnd(2) ? "#surf" : "alice");
        snprintf(line + o, sizeof(line) - (size_t)o, "\r\n");
        server(line);
    }
    CHECK(consistent());
}

/* every room, its lines and its nicks, as one text */
static const char *digest(void) {
    static char d[1 << 16];
    int i, j, o = 0;
    for(i = 0; i < C.rooms && o < (int)sizeof(d) - 600; i++) {
        struct chat_room *r = C.room[i];
        o += snprintf(d + o, sizeof(d) - (size_t)o, "[%d %s %d %s]", r->kind, r->name, r->joined, r->topic ? r->topic : "-");
        for(j = 0; j < r->count && o < (int)sizeof(d) - 600; j++) {
            const struct chat_line *l = chat_line_at(r, j);
            o += snprintf(d + o, sizeof(d) - (size_t)o, "%d/%d/%s/%s;", l->kind, l->flags, l->from, l->text);
        }
        for(j = 0; j < r->nnicks; j++) o += snprintf(d + o, sizeof(d) - (size_t)o, "%c%s,", chat_nick_mark(&C, &r->nicks[j]), r->nicks[j].name);
    }
    o += snprintf(d + o, sizeof(d) - (size_t)o, "{%s}", out());
    return d;
}

/* a whole session's bytes from the server, given whole, a byte at a time, and cut at random: the same rooms */
static void splits(void) {
    static const char talk[] = ":irc.test NOTICE * :*** hello\r\n:irc.test CAP * LS * :multi-prefix\r\n:irc.test CAP * LS :sasl\r\n"
        ":irc.test CAP * ACK :sasl\r\nAUTHENTICATE +\r\n:irc.test 903 alice :ok\r\n:irc.test 433 * alice :taken\r\n"
        ":irc.test 001 alice_ :Welcome\r\n:irc.test 005 alice_ PREFIX=(ov)@+ CHANTYPES=# NETWORK=T :ok\r\n"
        ":alice_!a@h JOIN #surf\r\n:irc.test 332 alice_ #surf :\002Waves\r\n:irc.test 353 alice_ = #surf :alice_ @bob +carol\r\n"
        ":irc.test 366 alice_ #surf :end\r\n@t=1 :bob!b@h PRIVMSG #surf :hi alice_\r\n:carol!c@h PRIVMSG alice_ :\001ACTION waves\001\r\n"
        "PING :abc\r\n:bob!b@h MODE #surf -o+v bob bob\r\n:carol!c@h NICK caro\r\n:caro!c@h QUIT :bye\r\n"
        ":bob!b@h PRIVMSG #surf :caf\xe9 \xe2\x82\xac\n:bob!b@h KICK #surf alice_ :x\r\nERROR :Closing\r\n";
    static char whole[1 << 16];
    int n = (int)sizeof(talk) - 1, step, at, k;
    for(step = 0; step < 12; step++) {
        fresh();
        connect_as("alice", CHAT_PASS_SASL, "made-up", 1);
        out();
        for(at = 0; at < n; at += k) {
            k = step == 0 ? n : step < 10 ? step : 1 + (int)rnd(40);
            if(k > n - at) k = n - at;
            chat_recv(&C, talk + at, k);
        }
        if(!step) snprintf(whole, sizeof(whole), "%s", digest());
        else CHECK(!strcmp(digest(), whole));
    }
    CHECK(strstr(whole, "caf\xc3\xa9 \xc3\xa2\xe2\x80\x9a\xc2\xac") && strstr(whole, "PONG :abc") &&
          strstr(whole, "You were kicked from #surf by bob") && strstr(whole, "[2 caro 0 -]"));
}

/* random bytes, random lines, random splits: no crash, every list as it should be */
static int rounds = 300;                        /* test_wchat N: N rounds of it (the sanitizer runs take thousands) */

static void hostile(void) {
    static const char *const words[] = { ":a!b@c ", ":irc.test ", "@t=1 ", "PRIVMSG ", "NOTICE ", "JOIN ", "PART ", "QUIT ",
        "NICK ", "KICK ", "MODE ", "TOPIC ", "353 ", "366 ", "332 ", "005 ", "CAP ", "AUTHENTICATE ", "PING ", "001 ",
        "433 ", "#surf ", "alice ", "bob ", ":", "+ov ", "-o ", "=", "@", "\001ACTION ", "\0034,5", "\r\n", "\n", "\r",
        " ", ",", "\xff", "\xc3", "\x00", "PREFIX=(ov)@+ ", "CASEMAPPING=ascii ", "LS ", "ACK ", "sasl ", "* " };
    static char buf[20000];
    int round, n, at;
    for(round = 0; round < rounds; round++) {
        if(round % 50 == 0) in_surf();
        n = 0;
        while(n < (int)sizeof(buf) - 64) {
            if(rnd(8) == 0) buf[n++] = (char)rnd(256);
            else {
                const char *w = words[rnd(sizeof(words) / sizeof(words[0]))];
                size_t k = w[0] ? strlen(w) : 1;
                memcpy(buf + n, w, k);
                n += (int)k;
            }
        }
        for(at = 0; at < n;) {
            int k = 1 + (int)rnd(700);
            if(k > n - at) k = n - at;
            chat_recv(&C, buf + at, k);
            at += k;
            if(rnd(10) == 0) {
                C.now += rnd(70000);
                chat_tick(&C);
            }
            if(rnd(5) == 0) out();
            if(rnd(30) == 0) chat_select(&C, (int)rnd((unsigned)C.rooms));
        }
        CHECK(consistent());
    }
    /* and whole oversized lines */
    in_surf();
    memset(buf, 'A', sizeof(buf));
    memcpy(buf, ":x!y@z PRIVMSG #surf :", 22);
    buf[sizeof(buf) - 2] = '\r';
    buf[sizeof(buf) - 1] = '\n';
    chat_recv(&C, buf, sizeof(buf));
    CHECK(strstr(said(0, 0), "1 left out so far") && consistent());
}

/* a session from connecting to closing, every allocation in it failing in turn: never a crash, nothing left */
static void session(void) {
    int i;
    if(made) chat_free(&C);
    made = 0;
    if(chat_init(&C)) return;                       /* its first allocation failed: nothing made */
    made = 1;
    C.now = 5000;
    C.minute = 630;
    connect_as("alice", CHAT_PASS_SASL, "made-up", 1);
    server(":irc.test CAP * LS :sasl\r\n:irc.test CAP * ACK :sasl\r\nAUTHENTICATE +\r\n:irc.test 903 alice :ok\r\n"
           ":irc.test 001 alice :Welcome\r\n:irc.test 005 alice CASEMAPPING=ascii PREFIX=(ov)@+ :x\r\n");
    chat_input(&C, "/join #surf");
    server(":alice!a@h JOIN #surf\r\n:irc.test 332 alice #surf :topic\r\n:irc.test 353 alice = #surf :@alice bob carol\r\n"
           ":irc.test 366 alice #surf :end\r\n:bob!b@h PRIVMSG #surf :hi alice\r\n:bob!b@h PRIVMSG alice :psst\r\n"
           ":carol!c@h NICK caroline\r\n:bob!b@h MODE #surf +v caroline\r\n:caroline!c@h QUIT :bye\r\n");
    for(i = 0; i < 5; i++) chat_input(&C, "a message");
    chat_input(&C, "/query dave hi");
    server(":bob!b@h PART #surf\r\n:alice!a@h KICK #surf alice :x\r\n");
    out();
    chat_quit(&C, "bye");
    chat_down(&C, "closed");
    chat_close_room(&C, 1);
}

static void allocations(void) {
    long k, total;
    chat_free(&C);
    made = 0;
    CHECK(live == 0);
    calls = 0;
    session();
    total = calls;
    chat_free(&C);
    made = 0;
    CHECK(live == 0 && total > 20);
    for(k = 0; k < total + 5; k++) {
        fail_at = k;
        session();
        fail_at = -1;
        if(made) {
            CHECK(consistent());
            chat_free(&C);
            made = 0;
        }
        if(live) {
            printf("allocation %ld failing leaves %ld blocks\n", k, live);
            failed++;
            live = 0;
        }
    }
}

/**** the link to the helper ****/

static void links(void) {
    static char stream[4 * (LINK_DATA_MAX + 64)], data[LINK_DATA_MAX];
    static struct link_reader r;
    char words[300];
    int n = 0, k, step, used, kind, len, frames;
    n += link_frame(stream + n, (int)sizeof(stream) - n, LINK_UP, "TLSv1.3", 7);
    n += link_frame(stream + n, (int)sizeof(stream) - n, LINK_DATA, "PING :x\r\n", 9);
    memset(data, 'q', sizeof(data));
    k = link_frame(stream + n, (int)sizeof(stream) - n, LINK_DATA, data, LINK_DATA_MAX);
    CHECK(k == LINK_DATA_MAX + (int)sizeof(struct link_head));
    n += k;
    n += link_frame(stream + n, (int)sizeof(stream) - n, LINK_DOWN, "gone", 4);
    for(step = 1; step < n; step = step < 40 ? step + 1 : step * 3) {     /* frames however the bytes come */
        int at = 0;
        memset(&r, 0, sizeof(r));
        frames = 0;
        while(at < n) {
            int piece = n - at < step ? n - at : step, off = 0;
            while(off < piece) {
                k = link_read(&r, stream + at + off, piece - off, &used, &kind, &len);
                CHECK(k >= 0);
                off += used;
                if(k == 1) {
                    if(frames == 0) CHECK(kind == LINK_UP && len == 7 && !strcmp(r.body, "TLSv1.3"));
                    if(frames == 1) CHECK(kind == LINK_DATA && len == 9 && !strcmp(r.body, "PING :x\r\n"));
                    if(frames == 2) CHECK(kind == LINK_DATA && len == LINK_DATA_MAX);
                    if(frames == 3) CHECK(kind == LINK_DOWN && !strcmp(r.body, "gone"));
                    frames++;
                }
            }
            at += piece;
        }
        CHECK(frames == 4);
    }
    /* what is no frame */
    {
        struct link_head h;
        static const unsigned int heads[][3] = {
            { 0, LINK_UP, 0 }, { 1, 9, 0 }, { 1, LINK_UP, LINK_TLS_MAX + 1 }, { 1, LINK_DATA, 0 },
            { 1, LINK_DATA, LINK_DATA_MAX + 1 }, { 1, LINK_DOWN, LINK_TEXT_MAX + 1 }, { 1, LINK_DATA, 1 },
        };
        int i;
        for(i = 0; i < 7; i++) {
            memset(&r, 0, sizeof(r));
            if(i == 6) r.up = 0;                            /* DATA before UP */
            h.magic = heads[i][0] ? LINK_MAGIC : 0x31434857u;
            h.kind = heads[i][1];
            h.len = heads[i][2];
            CHECK(link_read(&r, (const char *)&h, sizeof(h), &used, &kind, &len) == -1);
        }
        memset(&r, 0, sizeof(r));                           /* UP twice; anything after DOWN */
        n = link_frame(stream, sizeof(stream), LINK_UP, "", 0);
        CHECK(link_read(&r, stream, n, &used, &kind, &len) == 1 && link_read(&r, stream, n, &used, &kind, &len) == -1);
        memset(&r, 0, sizeof(r));
        n = link_frame(stream, sizeof(stream), LINK_DOWN, "x", 1);
        n += link_frame(stream + n, (int)sizeof(stream) - n, LINK_DOWN, "y", 1);
        CHECK(link_read(&r, stream, n, &used, &kind, &len) == 1 && used < n);
        CHECK(link_read(&r, stream + used, n - used, &used, &kind, &len) == -1);
        CHECK(link_frame(stream, 10, LINK_DATA, "x", 1) == -1 && link_frame(stream, 100, LINK_UP, stream, 16) == -1);
    }
    link_words(words, sizeof(words), "HTTPS needs strong random numbers and this CPU has no RDRAND instruction; "
               "http:// addresses still work (in QEMU, use -cpu max)");
    CHECK(!strcmp(words, "TLS needs strong random numbers and this CPU has no RDRAND instruction (in QEMU, use -cpu max)"));
    link_words(words, sizeof(words), "HTTPS refused: the certificate of irc.test expired on X UTC (the clock says Y; if "
               "that clock is wrong, set it with clock YYYY-MM-DD HH:MM:SS (UTC))");
    CHECK(!strcmp(words, "TLS refused: the certificate of irc.test expired on X UTC (the clock says Y)"));
    link_words(words, 8, "HTTPS HTTPS HTTPS");
    CHECK(!strcmp(words, "TLS TLS"));
}

int main(int argc, char **argv) {
    if(argc > 1) rounds = atoi(argv[1]);
    if(argc > 2 && !(seed = strtoull(argv[2], NULL, 0))) seed = 1;     /* xorshift stays at 0 */
    reader();
    parser();
    names();
    compose();
    base64();
    ctcp();
    text();
    formatting();
    wrapping();
    registration();
    passwords();
    channels();
    review();
    messages();
    pings();
    typing();
    pacing();
    windowside();
    limits();
    splits();
    hostile();
    allocations();
    links();
    if(made) chat_free(&C);
    CHECK(live == 0);
    printf("test_wchat: %d checks, %d failed\n", checks, failed);
    return failed != 0;
}
