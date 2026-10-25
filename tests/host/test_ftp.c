/* Host-side unit tests for user/ftpd/ftp.c, ftpd's FTP protocol: the control connection's lines
   (CR LF and LF, fed whole, a byte at a time and in every two pieces, two lines in one piece,
   Telnet's IAC sequences: IP and Synch before ABOR, IAC IAC, options and sub-negotiations taken
   out, CR NUL, a CR alone, a NUL, lines of 512 bytes and longer); verbs and arguments; the verb
   table (the X forms, the flags, what is known but not done); a session's paths ("." and "..",
   held at the folder served, relative and absolute, controls refused, too long) and the file
   system's; the paths never touched (/dev, /.netman); names, quoted paths and reply text; PORT,
   EPRT, PASV and EPSV texts; REST's offsets; LIST's options; the lines of LIST (recent and old
   dates, the six months, a time ahead, folders, read-only), MLSD, MLST and NLST; text mode's line
   ends both ways, in pieces and at random, round trips; replies for errno values. Built from the
   same file as the program and run by `make unittest`. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include "ftp.h"

static int failures, checks;

static void check(int ok, const char *what) {
    checks++;
    if(!ok) { failures++; printf("FAIL: %s\n", what); }
}

/* feeds s (n bytes) to c in pieces of at most step bytes: the lines that end, each followed by '|'
   (a line too long as "<LONG>|"), into out; the bytes fed always all taken */
static void feed_all(struct ftp_ctl *c, const char *s, int n, int step, char *out, int size) {
    int off = 0;
    out[0] = 0;
    while(off < n) {
        int len = n - off < step ? n - off : step, used = 0;
        while(used < len) {
            int got, k = ftp_ctl_feed(c, s + off + used, len - used, &got);
            used += k;
            if(got == FTP_LINE) {
                strncat(out, c->line, (size_t)size - strlen(out) - 2);
                strcat(out, "|");
            } else if(got == FTP_LONG) strcat(out, "<LONG>|");
        }
        off += len;
    }
}

static int lines_are(const char *s, int n, int step, const char *want) {
    struct ftp_ctl c;
    char out[4096];
    memset(&c, 0, sizeof(c));
    feed_all(&c, s, n, step, out, sizeof(out));
    return !strcmp(out, want);
}

/* the same for a string literal, NUL bytes in it included */
#define LINES(s, step, want) lines_are(s, (int)sizeof(s) - 1, step, want)

static void test_ctl(void) {
    struct ftp_ctl c;
    static char big[2000];
    char want[64], out[4096];
    const char *two = "USER bob\r\nPASS x\r\n";
    int got, k, i, j, ok;

    check(LINES("USER bob\r\n", 100, "USER bob|"), "ctl: a line with CR LF");
    check(LINES("NOOP\n", 100, "NOOP|"), "ctl: a line with LF alone");
    check(LINES("NOOP", 100, ""), "ctl: no line before its end");
    memset(&c, 0, sizeof(c));
    k = ftp_ctl_feed(&c, two, (int)strlen(two), &got);
    check(k == 10 && got == FTP_LINE && !strcmp(c.line, "USER bob"), "ctl: the first of two lines ends the call");
    k = ftp_ctl_feed(&c, two + 10, (int)strlen(two) - 10, &got);
    check(k == 8 && got == FTP_LINE && !strcmp(c.line, "PASS x"), "ctl: then the second");
    k = ftp_ctl_feed(&c, "", 0, &got);
    check(k == 0 && got == FTP_MORE, "ctl: nothing fed, nothing ends");
    ok = 1;
    for(i = 1; i <= (int)strlen(two); i++) if(!lines_are(two, (int)strlen(two), i, "USER bob|PASS x|")) ok = 0;
    check(ok, "ctl: two lines fed in pieces of every size");
    check(LINES("\xff\xf4\xff\xf2" "ABOR\r\n", 1, "ABOR|"), "ctl: IAC IP and IAC DM before ABOR are taken out");
    check(LINES("RETR a\xff\xff" "b\r\n", 100, "RETR a\xff" "b|"), "ctl: IAC IAC is a byte 255");
    check(LINES("\xff\xfb\x01\xff\xfd\x03" "NOOP\r\n", 1, "NOOP|"), "ctl: IAC WILL and DO with their options");
    check(LINES("\xff\xfa\x18\x01xy\r\n\xff\xff\xff\xf0" "NOOP\r\n", 1, "NOOP|"),
          "ctl: a sub-negotiation to IAC SE, with a CR LF and IAC IAC inside");
    check(LINES("NO\xff\xf1" "OP\r\n", 1, "NOOP|"), "ctl: IAC NOP in the middle of a verb");
    check(LINES("A\rB\r\n", 100, "A\rB|"), "ctl: a CR that ends nothing is part of the line");
    check(LINES("A\r\0B\r\n", 1, "A\rB|"), "ctl: CR NUL is a CR");
    check(LINES("A\0B\n", 100, "A\x7f" "B|"), "ctl: a NUL becomes a DEL");
    check(LINES("A\r", 100, "") && LINES("A\r\r\n", 100, "A\r|"), "ctl: CR CR LF keeps the first CR");
    memset(big, 'x', sizeof(big));
    memcpy(big + FTP_LINE_MAX, "\r\nNOOP\r\n", 8);
    snprintf(want, sizeof(want), "|NOOP|");
    memset(&c, 0, sizeof(c));
    feed_all(&c, big, FTP_LINE_MAX + 8, 7, out, sizeof(out));
    check(strlen(out) == FTP_LINE_MAX + 6 && !strcmp(out + FTP_LINE_MAX, want), "ctl: a line of 512 bytes is whole");
    big[FTP_LINE_MAX] = 'y';
    memcpy(big + FTP_LINE_MAX + 1, "\r\nNOOP\r\n", 8);
    check(lines_are(big, FTP_LINE_MAX + 9, 64, "<LONG>|NOOP|"), "ctl: a line of 513 bytes is too long, and the next is read");
    memset(big, 'z', sizeof(big));
    memcpy(big + 1990, "\nQUIT\n", 6);
    check(lines_are(big, 1996, 1, "<LONG>|QUIT|"), "ctl: a long line fed a byte at a time");
    /* 10/25/26 (review): Telnet's sequences inside a line too long are taken out all the same, a sub-negotiation's
       CR LF ends no line there either, and the next line is read as ever */
    memset(big, 'q', sizeof(big));
    {
        static const char tail[] = "\xff\xf4\xff\xf2" "qq\xff\xfa\x18\r\n\xff\xf0" "qq\r\nNOOP\r\n";
        int n = 600 + (int)sizeof(tail) - 1;
        memcpy(big + 600, tail, sizeof(tail) - 1);
        check(lines_are(big, n, 1, "<LONG>|NOOP|") && lines_are(big, n, 13, "<LONG>|NOOP|") &&
              lines_are(big, n, n, "<LONG>|NOOP|"), "ctl: IAC IP, IAC DM and a sub-negotiation inside a line too long");
    }
    ok = 1;
    for(i = 0; i < 200 && ok; i++) {               /* random IAC sequences and lines, in random pieces */
        char s[256], expect[512];
        int n = 0, e = 0, lines = 1 + rand() % 4;
        expect[0] = 0;
        for(j = 0; j < lines; j++) {
            int len = 1 + rand() % 20, m;
            for(m = 0; m < len; m++) {
                if(rand() % 5 == 0) {
                    s[n++] = (char)0xff;
                    s[n++] = (char)(0xf1 + rand() % 4);    /* NOP, DM, BRK, IP */
                }
                s[n++] = (char)('a' + rand() % 26);
                expect[e++] = s[n - 1];
            }
            s[n++] = '\r';
            s[n++] = '\n';
            expect[e++] = '|';
        }
        expect[e] = 0;
        if(!lines_are(s, n, 1 + rand() % 9, expect)) ok = 0;
    }
    check(ok, "ctl: random lines with Telnet commands in them, in random pieces");
}

static void test_commands(void) {
    char verb[8];
    const char *arg;
    const struct ftp_verb *v;
    check(ftp_split("USER bob", verb, &arg) == 0 && !strcmp(verb, "USER") && !strcmp(arg, "bob"), "split: USER bob");
    check(ftp_split("noop", verb, &arg) == 0 && !strcmp(verb, "NOOP") && !*arg, "split: lower case, no argument");
    check(ftp_split("RETR  two", verb, &arg) == 0 && !strcmp(arg, " two"), "split: only the one space goes");
    check(ftp_split("RETR ", verb, &arg) == 0 && !*arg, "split: a space and nothing");
    check(ftp_split("Stor a b ", verb, &arg) == 0 && !strcmp(verb, "STOR") && !strcmp(arg, "a b "), "split: the argument as sent");
    check(ftp_split("", verb, &arg) == -1, "split: an empty line");
    check(ftp_split(" NOOP", verb, &arg) == -1, "split: a space first");
    check(ftp_split("N0OP", verb, &arg) == -1, "split: a digit in the verb");
    check(ftp_split("ABCDEFGH", verb, &arg) == -1 && ftp_split("ABCDEFG x", verb, &arg) == 0, "split: seven letters at most");
    check(ftp_split("XSHA1 x", verb, &arg) == -1, "split: letters only (XSHA1 is no verb here)");
    check(ftp_split("N\xc3\x96OP", verb, &arg) == -1 && ftp_split("\xff" "ABOR", verb, &arg) == -1 &&
          ftp_split("RETR\xa0x", verb, &arg) == -1 && ftp_split("\xc3\x9c", verb, &arg) == -1,
          "split: bytes that are no ASCII letters, in the verb or after it");                  /* 10/25/26 (review) */
    v = ftp_find("USER");
    check(v && v->id == FTP_USER && (v->flags & FTP_F_ARG) && !(v->flags & FTP_F_LOGIN), "find: USER needs no login");
    v = ftp_find("XPWD");
    check(v && v->id == FTP_PWD, "find: XPWD is PWD");
    check(ftp_find("XCWD")->id == FTP_CWD && ftp_find("XCUP")->id == FTP_CDUP && ftp_find("XMKD")->id == FTP_MKD &&
          ftp_find("XRMD")->id == FTP_RMD, "find: the X forms");
    v = ftp_find("STOR");
    check(v && v->flags == (FTP_F_LOGIN | FTP_F_ARG | FTP_F_DATA | FTP_F_WRITE), "find: STOR's flags");
    v = ftp_find("RETR");
    check(v && (v->flags & FTP_F_DATA) && !(v->flags & FTP_F_WRITE), "find: RETR reads");
    check(ftp_find("LIST")->flags == (FTP_F_LOGIN | FTP_F_DATA), "find: LIST's argument is optional");
    check(ftp_find("RNTO")->flags & FTP_F_WRITE && ftp_find("DELE")->flags & FTP_F_WRITE && ftp_find("MKD")->flags & FTP_F_WRITE,
          "find: RNTO, DELE and MKD write");
    check(ftp_find("AUTH")->id == FTP_NOTIMPL && ftp_find("MDTM")->id == FTP_NOTIMPL && ftp_find("MFMT")->id == FTP_NOTIMPL,
          "find: AUTH, MDTM and MFMT are known, not done");
    check(ftp_find("FOO") == NULL && ftp_find("user") == NULL, "find: unknown verbs (and only capitals)");
    check(ftp_find("FEAT")->flags == 0 && ftp_find("SYST")->flags == 0 && ftp_find("QUIT")->flags == 0,
          "find: FEAT, SYST and QUIT before a login");
}

static int path_is(const char *cwd, const char *arg, const char *want) {
    char out[FTP_PATH_MAX];
    return ftp_path(cwd, arg, out, sizeof(out)) == 0 && !strcmp(out, want);
}

static void test_paths(void) {
    char out[FTP_PATH_MAX], longarg[400];
    check(path_is("/", "a", "/a"), "path: relative from /");
    check(path_is("/a", "b/c", "/a/b/c"), "path: relative from a folder");
    check(path_is("/a", "/x/y", "/x/y"), "path: absolute");
    check(path_is("/", "..", "/") && path_is("/a", "../../..", "/"), "path: .. is held at the folder served");
    check(path_is("/a/b", "..", "/a") && path_is("/a/b", "../c", "/a/c"), "path: ..");
    check(path_is("/", "a/../b", "/b") && path_is("/", "./a/./b/", "/a/b"), "path: . and .. inside");
    check(path_is("/a", "", "/a") && path_is("/", "", "/"), "path: no argument is the session's folder");
    check(path_is("/", "a//b///", "/a/b"), "path: repeated slashes");
    check(path_is("/", "/../../etc", "/etc"), "path: an absolute path cannot leave the folder");
    check(path_is("/", "Hello World.txt", "/Hello World.txt") && path_is("/", "caf\xc3\xa9", "/caf\xc3\xa9"),
          "path: spaces and UTF-8 kept");
    check(path_is("/", "...", "/...") && path_is("/", ".x", "/.x"), "path: names of dots that are no . or ..");
    check(ftp_path("/", "a\tb", out, sizeof(out)) == -2 && ftp_path("/", "a\x7f", out, sizeof(out)) == -2 &&
          ftp_path("/", "\x1b[2J", out, sizeof(out)) == -2, "path: control characters refused");
    memset(longarg, 'n', sizeof(longarg) - 1);
    longarg[sizeof(longarg) - 1] = 0;
    check(ftp_path("/", longarg, out, sizeof(out)) == -1, "path: too long");
    longarg[254] = 0;
    check(ftp_path("/", longarg, out, sizeof(out)) == 0 && strlen(out) == 255, "path: 255 bytes fit");
    longarg[254] = 'n';
    longarg[255] = 0;
    check(ftp_path("/", longarg, out, sizeof(out)) == -1, "path: 256 bytes do not");
    {                                               /* 10/25/26 (review): a long folder and a relative argument */
        char cwd[FTP_PATH_MAX], rel[FTP_LINE_MAX + 1];
        int i;
        cwd[0] = '/';
        memset(cwd + 1, 'c', FTP_PATH_MAX - 2);
        cwd[FTP_PATH_MAX - 1] = 0;
        memset(rel, 'a', 300);
        rel[300] = 0;
        check(ftp_path(cwd, rel, out, sizeof(out)) == -1, "path: a long folder and a long argument: too long to join");
        for(i = 0; i < 80; i++) memcpy(rel + 3 * i, "../", 3);
        strcpy(rel + 240, "x");
        check(ftp_path(cwd, rel, out, sizeof(out)) == 0 && !strcmp(out, "/x"),
              "path: from a folder of 255 bytes, 241 bytes that climb out of it");
        memset(rel, 'a', FTP_LINE_MAX);
        rel[FTP_LINE_MAX] = 0;
        check(ftp_path("/", rel, out, sizeof(out)) == -1, "path: an argument as long as a line");
    }
    check(ftp_real("/", "/a/b", out, sizeof(out)) == 0 && !strcmp(out, "/a/b"), "real: under /");
    check(ftp_real("/data", "/", out, sizeof(out)) == 0 && !strcmp(out, "/data"), "real: the folder itself");
    check(ftp_real("/data", "/x/y", out, sizeof(out)) == 0 && !strcmp(out, "/data/x/y"), "real: under a folder");
    check(ftp_real("/data/www", "/x", out, 11) == -1 && ftp_real("/data/www", "/x", out, 12) == 0, "real: too long");
    check(ftp_reserved("/dev") && ftp_reserved("/dev/console") && ftp_reserved("/DEV/kmsg") && ftp_reserved("/.netman") &&
          ftp_reserved("/.netman/surfos-config/settings.conf"), "reserved: /dev and /.netman and below");
    check(!ftp_reserved("/devices") && !ftp_reserved("/data/dev") && !ftp_reserved("/.netmanx") && !ftp_reserved("/") &&
          !ftp_reserved("/initrd"), "reserved: nothing else");
    check(!strcmp(ftp_basename("/a/b"), "b") && !strcmp(ftp_basename("/a"), "a") && !strcmp(ftp_basename("/"), "/"),
          "basename");
    check(ftp_quote(out, sizeof(out), "/a b") == 0 && !strcmp(out, "\"/a b\""), "quote: a path");
    check(ftp_quote(out, sizeof(out), "/say \"hi\"") == 0 && !strcmp(out, "\"/say \"\"hi\"\"\""), "quote: quotes doubled");
    check(ftp_quote(out, 5, "/ab") == -1 && ftp_quote(out, 6, "/ab") == 0, "quote: the room it needs");
    ftp_clean(out, sizeof(out), "a\r\nb\x1b" "c\x7f");
    check(!strcmp(out, "a??b?c?"), "clean: controls become ?");
    ftp_clean(out, 4, "abcdef");
    check(!strcmp(out, "abc"), "clean: cut to fit");
}

static int ip_is(unsigned int ip, int a, int b, int c, int d) {
    const unsigned char *p = (const unsigned char *)&ip;
    return p[0] == a && p[1] == b && p[2] == c && p[3] == d;
}

static void test_addresses(void) {
    unsigned int ip = 0;
    unsigned short port = 0;
    long off = 0;
    char out[128];
    check(ftp_parse_port("10,0,2,2,4,1", &ip, &port) == 0 && ip_is(ip, 10, 0, 2, 2) && port == 1025, "PORT: an address");
    check(ftp_parse_port("192,168,1,20,255,255", &ip, &port) == 0 && ip_is(ip, 192, 168, 1, 20) && port == 65535,
          "PORT: the highest port");
    check(ftp_parse_port("10,0,2,2,4", &ip, &port) == -1 && ftp_parse_port("10,0,2,2,4,1,9", &ip, &port) == -1,
          "PORT: five or seven numbers");
    check(ftp_parse_port("10,0,2,256,4,1", &ip, &port) == -1 && ftp_parse_port("10,0,2,2,4,1000", &ip, &port) == -1,
          "PORT: a number above 255");
    check(ftp_parse_port("10, 0,2,2,4,1", &ip, &port) == -1 && ftp_parse_port("10,0,2,2,4,1 ", &ip, &port) == -1 &&
          ftp_parse_port("", &ip, &port) == -1 && ftp_parse_port("a,b,c,d,e,f", &ip, &port) == -1, "PORT: other text");
    check(ftp_parse_eprt("|1|10.0.2.2|5000|", &ip, &port) == 0 && ip_is(ip, 10, 0, 2, 2) && port == 5000, "EPRT: IPv4");
    check(ftp_parse_eprt("!1!192.168.1.9!65535!", &ip, &port) == 0 && ip_is(ip, 192, 168, 1, 9) && port == 65535,
          "EPRT: another delimiter");
    check(ftp_parse_eprt("|2|::1|5000|", &ip, &port) == -2 && ftp_parse_eprt("|3|x|5000|", &ip, &port) == -2,
          "EPRT: another network protocol");
    check(ftp_parse_eprt("|1|10.0.2|5000|", &ip, &port) == -1 && ftp_parse_eprt("|1|10.0.2.2|0|", &ip, &port) == -1 &&
          ftp_parse_eprt("|1|10.0.2.2|65536|", &ip, &port) == -1 && ftp_parse_eprt("|1|10.0.2.2|5000", &ip, &port) == -1 &&
          ftp_parse_eprt("|1|10.0.2.2|5000|x", &ip, &port) == -1 && ftp_parse_eprt(" 1 10.0.2.2 5000 ", &ip, &port) == -1 &&
          ftp_parse_eprt("||10.0.2.2|5000|", &ip, &port) == -1 && ftp_parse_eprt("", &ip, &port) == -1,
          "EPRT: other text");
    /* 10/25/26 (review): a digit for the delimiter is refused (it would be read as part of a number); a dot is one
       of the characters RFC 2428 allows, and the address's own dots are told apart by their place */
    check(ftp_parse_eprt("1111110.0.2.2150001", &ip, &port) == -1 && ftp_parse_eprt("5150.0.2.25995", &ip, &port) == -1,
          "EPRT: a digit for the delimiter");
    check(ftp_parse_eprt(".1.10.0.2.2.5000.", &ip, &port) == 0 && ip_is(ip, 10, 0, 2, 2) && port == 5000,
          "EPRT: a dot for the delimiter");
    ip = 0;
    ((unsigned char *)&ip)[0] = 10;
    ((unsigned char *)&ip)[3] = 15;
    ((unsigned char *)&ip)[2] = 2;
    ftp_pasv_text(out, sizeof(out), ip, 50001);
    check(!strcmp(out, "Entering Passive Mode (10,0,2,15,195,81)."), "PASV: the text");
    ftp_epsv_text(out, sizeof(out), 50001);
    check(!strcmp(out, "Entering Extended Passive Mode (|||50001|)"), "EPSV: the text");
    check(ftp_parse_offset("0", &off) == 0 && off == 0 && ftp_parse_offset("123456", &off) == 0 && off == 123456,
          "REST: offsets");
    check(ftp_parse_offset("2147483647", &off) == 0 && off == 2147483647L, "REST: the last below 2 GB");
    check(ftp_parse_offset("2147483648", &off) == -1 && ftp_parse_offset("99999999999999999999", &off) == -1 &&
          ftp_parse_offset("-1", &off) == -1 && ftp_parse_offset("", &off) == -1 && ftp_parse_offset("12a", &off) == -1,
          "REST: 2 GB and other text refused");
}

static void test_listings(void) {
    const unsigned int now = 1792836000u;       /* 2026-10-24 10:00 */
    struct ftp_entry f = { "notes.txt", 0, 1234, 1792836000u }, d = { "data", 1, 0, 1741334700u };
    const char *path;
    char out[512];
    int all, n;
    ftp_list_args("-la /x", &all, &path);
    check(all && !strcmp(path, "/x"), "list args: -la and a path");
    ftp_list_args("-l", &all, &path);
    check(!all && !*path, "list args: -l alone");
    ftp_list_args("", &all, &path);
    check(!all && !*path, "list args: none");
    ftp_list_args("  -A  -l   name with space", &all, &path);
    check(all && !strcmp(path, "name with space"), "list args: -A, then a name with spaces");
    ftp_list_args("name", &all, &path);
    check(!all && !strcmp(path, "name"), "list args: a name alone");
    n = ftp_list_line(out, sizeof(out), &f, now, 1);
    check(n == (int)strlen(out) && !strcmp(out, "-rw-r--r--   1 surfos   surfos         1234 Oct 24 10:00 notes.txt\r\n"),
          "LIST: a file of today");
    n = ftp_list_line(out, sizeof(out), &d, now, 0);
    check(n > 0 && !strcmp(out, "dr-xr-xr-x   1 surfos   surfos            0 Mar  7  2025 data\r\n"),
          "LIST: a read-only folder of last year: its year");
    f.mtime = 1777370400u;                      /* 2026-04-28 10:00: within six months */
    ftp_list_line(out, sizeof(out), &f, now, 1);
    check(strstr(out, " Apr 28 10:00 notes.txt") != NULL, "LIST: within 180 days, the time");
    f.mtime = 1777197600u;                      /* 2026-04-26 10:00: beyond */
    ftp_list_line(out, sizeof(out), &f, now, 1);
    check(strstr(out, " Apr 26  2026 notes.txt") != NULL, "LIST: beyond 180 days, the year");
    f.mtime = 1792841400u;                      /* 11:30, an hour and a half ahead */
    ftp_list_line(out, sizeof(out), &f, now, 1);
    check(strstr(out, " Oct 24  2026 notes.txt") != NULL, "LIST: more than an hour ahead, the year");
    f.mtime = 1792835940u;                      /* 9:59 */
    ftp_list_line(out, sizeof(out), &f, now, 1);
    check(strstr(out, " Oct 24 09:59 notes.txt") != NULL, "LIST: a minute ago");
    f.mtime = 951868740u;                       /* 2000-02-29 23:59 */
    ftp_list_line(out, sizeof(out), &f, now, 1);
    check(strstr(out, " Feb 29  2000 notes.txt") != NULL, "LIST: a leap day");
    f.mtime = 0;
    ftp_list_line(out, sizeof(out), &f, now, 1);
    check(strstr(out, " Jan  1  1970 notes.txt") != NULL, "LIST: time 0");
    check(ftp_list_line(out, 20, &f, now, 1) == -1, "LIST: a line that does not fit");
    f.size = 4000000000u;
    ftp_list_line(out, sizeof(out), &f, now, 1);
    check(strstr(out, " 4000000000 Jan") != NULL, "LIST: a size above 2 GB");
    f.size = 1234;
    n = ftp_mlsx_line(out, sizeof(out), &f, 1, 0);
    check(n == (int)strlen(out) && !strcmp(out, "type=file;size=1234;perm=adfrw; notes.txt\r\n"), "MLSD: a file");
    ftp_mlsx_line(out, sizeof(out), &d, 0, 0);
    check(!strcmp(out, "type=dir;perm=el; data\r\n"), "MLSD: a read-only folder");
    ftp_mlsx_line(out, sizeof(out), &d, 1, 0);
    check(!strcmp(out, "type=dir;perm=cdeflmp; data\r\n"), "MLSD: a folder that may be changed");
    f.name = "/initrd/README";
    ftp_mlsx_line(out, sizeof(out), &f, 0, 1);
    check(!strcmp(out, " type=file;size=1234;perm=r; /initrd/README\r\n"), "MLST: a space before, the path");
    check(ftp_mlsx_line(out, 10, &f, 0, 1) == -1, "MLST: a line that does not fit");
    ftp_nlst_line(out, sizeof(out), "", "a.txt");
    check(!strcmp(out, "a.txt\r\n"), "NLST: no prefix");
    ftp_nlst_line(out, sizeof(out), "dir", "a.txt");
    check(!strcmp(out, "dir/a.txt\r\n"), "NLST: a folder's prefix");
    ftp_nlst_line(out, sizeof(out), "/", "a.txt");
    check(!strcmp(out, "/a.txt\r\n"), "NLST: the prefix /");
}

/* text mode both ways on s in pieces of step bytes */
static int to_crlf_is(const char *s, int step, const char *want) {
    char out[256];
    int cr = 0, o = 0, i, n = (int)strlen(s);
    for(i = 0; i < n; i += step) o += ftp_to_crlf(s + i, n - i < step ? n - i : step, out + o, &cr);
    out[o] = 0;
    return !strcmp(out, want);
}

static int from_crlf(const char *s, int n, int step, char *out) {
    int held = 0, o = 0, i;
    for(i = 0; i < n; i += step) o += ftp_from_crlf(s + i, n - i < step ? n - i : step, out + o, &held);
    o += ftp_from_crlf("", 0, out + o, &held);
    return o;
}

static int from_crlf_is(const char *s, int step, const char *want) {
    char out[256];
    int o = from_crlf(s, (int)strlen(s), step, out);
    out[o] = 0;
    return !strcmp(out, want);
}

static void test_text_mode(void) {
    static char data[20000], wire[40000], back[40000];
    int i, ok = 1, step;
    check(to_crlf_is("a\nb\n", 100, "a\r\nb\r\n"), "to CR LF: LF alone");
    check(to_crlf_is("a\r\nb", 100, "a\r\nb"), "to CR LF: CR LF stays");
    check(to_crlf_is("a\r\nb\r\n", 1, "a\r\nb\r\n"), "to CR LF: the CR before an LF in another piece");
    check(to_crlf_is("\n\n", 1, "\r\n\r\n") && to_crlf_is("\r\r\n", 1, "\r\r\n"), "to CR LF: empty lines, CR CR LF");
    check(from_crlf_is("a\r\nb", 100, "a\nb"), "from CR LF: CR LF becomes LF");
    check(from_crlf_is("a\rb", 100, "a\rb") && from_crlf_is("a\rb", 1, "a\rb"), "from CR LF: a CR alone stays");
    check(from_crlf_is("a\r\nb\r\n", 1, "a\nb\n"), "from CR LF: a byte at a time");
    check(from_crlf_is("a\r", 100, "a\r"), "from CR LF: a CR at the end comes back at the end");
    check(from_crlf_is("\r\r\n", 1, "\r\n") && from_crlf_is("\r\r\r\n", 2, "\r\r\n"), "from CR LF: CRs before a CR LF");
    check(from_crlf_is("a\nb", 100, "a\nb"), "from CR LF: an LF alone stays");
    for(i = 0; i < (int)sizeof(data); i++) {   /* text without CRs: there and back, in random pieces */
        int r = rand() % 10;
        data[i] = r == 0 ? '\n' : (char)('a' + r);
    }
    for(step = 1; step < 5000 && ok; step = step * 3 + 1) {
        int cr = 0, w = 0, n;
        for(i = 0; i < (int)sizeof(data); i += step)
            w += ftp_to_crlf(data + i, (int)sizeof(data) - i < step ? (int)sizeof(data) - i : step, wire + w, &cr);
        n = from_crlf(wire, w, step + 2, back);
        if(n != (int)sizeof(data) || memcmp(back, data, sizeof(data))) ok = 0;
    }
    check(ok, "text mode: there and back gives the text again, in pieces of any size");
    for(i = 0; i < (int)sizeof(data); i++) {   /* with CRs: pieces change nothing */
        int r = rand() % 6;
        data[i] = r == 0 ? '\n' : r == 1 ? '\r' : 'x';
    }
    {
        int whole = from_crlf(data, (int)sizeof(data), (int)sizeof(data), wire);
        ok = 1;
        for(step = 1; step < 300 && ok; step += 7) {
            int n = from_crlf(data, (int)sizeof(data), step, back);
            if(n != whole || memcmp(back, wire, (size_t)n)) ok = 0;
        }
        check(ok, "from CR LF: the same whatever the pieces, CRs among the bytes");
    }
}

static void test_errno(void) {
    const char *text;
    check(ftp_errno_reply(ENOENT, &text) == 550 && !strcmp(text, "No such file or directory"), "errno: ENOENT 550");
    check(ftp_errno_reply(EROFS, &text) == 550 && !strcmp(text, "Read-only file system"), "errno: EROFS 550");
    check(ftp_errno_reply(ENOTEMPTY, &text) == 550 && ftp_errno_reply(EEXIST, &text) == 550, "errno: ENOTEMPTY, EEXIST");
    check(ftp_errno_reply(ENOSPC, &text) == 552 && ftp_errno_reply(EFBIG, &text) == 552, "errno: no room is 552");
    check(ftp_errno_reply(ENAMETOOLONG, &text) == 553 && ftp_errno_reply(EINVAL, &text) == 553, "errno: names are 553");
    check(ftp_errno_reply(EBUSY, &text) == 450 && ftp_errno_reply(EMFILE, &text) == 450, "errno: try again later is 450");
    check(ftp_errno_reply(EIO, &text) == 451 && !strcmp(text, "I/O error"), "errno: EIO 451");
    check(ftp_errno_reply(12345, &text) == 451 && text == NULL, "errno: another is 451, its text strerror()'s");
}

int main(void) {
    srand(2410);
    test_ctl();
    test_commands();
    test_paths();
    test_addresses();
    test_listings();
    test_text_mode();
    test_errno();
    printf("test_ftp: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
