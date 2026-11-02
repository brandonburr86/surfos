/* Host-side unit tests for user/ftp/ftpclient.c, the ftp client's protocol (11/01/26): replies of one line and of
   several, with numbers inside and the wrong code at the end; replies malformed, too long and without end; commands,
   a control character refused and a byte 255 doubled, at every size of buffer; EPSV and PASV replies whole, malformed
   and out of range; 257 and 213; what may be shown on a terminal, valid and invalid UTF-8, cut at every size; a
   password scrubbed from text; a typed line's words; names from a server; wildcards, against the host's fnmatch(3) on
   random patterns; a listing's lines at every split; a password as it is typed. Built from the same files as the
   program and run by `make unittest`, and with clang's sanitizers on the Mac (the buffers here are exact, on the heap,
   so that a byte too far is caught). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fnmatch.h>
#include "ftpclient.h"

static int failures, checks;

static void check(int ok, const char *what) {
    checks++;
    if(!ok) {
        failures++;
        printf("FAIL: %s\n", what);
    }
}

/* a reply from these lines, one per entry: what the last one returned */
static int reply_of(struct ftpc_reply *r, const char *const *lines, int n) {
    int i, v = FTPC_R_MORE;
    ftpc_reply_init(r);
    for(i = 0; i < n; i++) v = ftpc_reply_line(r, lines[i], 0);
    return v;
}

#define REPLY(r, ...) reply_of(r, (const char *const[]){ __VA_ARGS__ }, (int)(sizeof((const char *const[]){ __VA_ARGS__ }) / sizeof(char *)))

static void test_replies(void) {
    struct ftpc_reply r;
    int i, v;
    check(REPLY(&r, "220 hello") == FTPC_R_DONE && r.code == 220 && !strcmp(r.text, "hello"), "one line");
    check(REPLY(&r, "220") == FTPC_R_DONE && r.code == 220 && !r.text[0], "a code alone");
    check(REPLY(&r, "220 ") == FTPC_R_DONE && !r.text[0], "a code and a blank");
    check(REPLY(&r, "123-First line", "Second line", "  234 A line beginning with numbers", "123 The last line") == FTPC_R_DONE &&
          r.code == 123 && !strcmp(r.text, "The last line"), "RFC 959's example of several lines");
    check(REPLY(&r, "123-First", "456 another code") == FTPC_R_MORE, "another code does not end a reply");
    check(REPLY(&r, "123-First", "123-still going") == FTPC_R_MORE, "nor the same code with a hyphen");
    check(REPLY(&r, "123-First", "1234 no") == FTPC_R_MORE, "nor four digits");
    check(REPLY(&r, "123-First", "123") == FTPC_R_DONE, "a code alone ends it");
    check(REPLY(&r, "") == FTPC_R_BAD, "an empty first line");
    check(REPLY(&r, "hello") == FTPC_R_BAD, "no code");
    check(REPLY(&r, "22 x") == FTPC_R_BAD, "two digits");
    check(REPLY(&r, "2200 x") == FTPC_R_BAD, "four digits");
    check(REPLY(&r, "22a x") == FTPC_R_BAD, "a letter in the code");
    check(REPLY(&r, "099 x") == FTPC_R_BAD && REPLY(&r, "600 x") == FTPC_R_BAD && REPLY(&r, "999 x") == FTPC_R_BAD, "codes from 100 to 599 only");
    check(REPLY(&r, "220\tx") == FTPC_R_BAD, "a code and a tab");
    ftpc_reply_init(&r);
    check(ftpc_reply_line(&r, "", 1) == FTPC_R_BAD, "a first line too long");
    ftpc_reply_init(&r);
    ftpc_reply_line(&r, "220-x", 0);
    check(ftpc_reply_line(&r, "220 not this one", 1) == FTPC_R_MORE, "a line too long inside a reply is passed over");
    ftpc_reply_init(&r);
    ftpc_reply_line(&r, "220-x", 0);
    for(i = 0, v = FTPC_R_MORE; i < FTPC_REPLY_LINES + 10 && v == FTPC_R_MORE; i++) v = ftpc_reply_line(&r, "x", 0);
    check(v == FTPC_R_BAD && r.lines == FTPC_REPLY_LINES + 1, "a reply without end stops at its limit in lines");
    ftpc_reply_init(&r);
    ftpc_reply_line(&r, "220-x", 0);
    for(i = 0, v = FTPC_R_MORE; i < 1000 && v == FTPC_R_MORE; i++) v = ftpc_reply_line(&r, "", 1);
    check(v == FTPC_R_BAD && r.bytes <= FTPC_REPLY_BYTES + FTP_LINE_MAX, "and in bytes");
}

static void test_commands(void) {
    static const char want[] = "STOR a\xff\xff" "b\r\n";
    char plain[32];
    int n, size;
    n = ftpc_command(plain, sizeof(plain), "NOOP", NULL);
    check(n == 6 && !memcmp(plain, "NOOP\r\n", 6), "no argument");
    n = ftpc_command(plain, sizeof(plain), "CWD", "my dir");
    check(n == 12 && !memcmp(plain, "CWD my dir\r\n", 12), "an argument with a blank");
    n = ftpc_command(plain, sizeof(plain), "CWD", "");
    check(n == 6 && !memcmp(plain, "CWD \r\n", 6), "an empty argument");
    n = ftpc_command(plain, sizeof(plain), "STOR", "a\xff" "b");
    check(n == (int)sizeof(want) - 1 && !memcmp(plain, want, sizeof(want) - 1), "a byte 255 is doubled");
    check(ftpc_command(plain, sizeof(plain), "CWD", "a\r\nDELE b") < 0, "a CR LF in an argument cannot make a second command");
    check(ftpc_command(plain, sizeof(plain), "CWD", "a\nb") < 0 && ftpc_command(plain, sizeof(plain), "CWD", "a\rb") < 0, "nor one of them alone");
    check(ftpc_command(plain, sizeof(plain), "CWD", "a\x7f") < 0 && ftpc_command(plain, sizeof(plain), "CWD", "a\x01") < 0, "nor another control");
    for(size = 0; size < 20; size++) {          /* every size of buffer, exact on the heap */
        char *b = malloc(size ? (size_t)size : 1);
        int m = ftpc_command(b, size, "STOR", "a\xff" "b");
        check(size >= (int)sizeof(want) - 1 ? m == (int)sizeof(want) - 1 : m < 0, "a command fits, or says so");
        free(b);
    }
}

static void test_passive(void) {
    unsigned short port = 0;
    check(!ftpc_parse_epsv("Entering Extended Passive Mode (|||6446|)", &port) && port == 6446, "RFC 2428's example");
    check(!ftpc_parse_epsv("(!!!21!)", &port) && port == 21, "another delimiter (a port's own check is apart)");
    check(!ftpc_parse_epsv("x (junk) y (|||7|)", &port) && port == 7, "brackets before the real ones");
    check(!ftpc_parse_epsv("(|||65535|)", &port) && port == 65535, "the highest port");
    check(ftpc_parse_epsv("(|||65536|)", &port) < 0, "one past it");
    check(ftpc_parse_epsv("(|||0|)", &port) < 0, "port 0");
    check(ftpc_parse_epsv("(|||123456|)", &port) < 0, "six digits");
    check(ftpc_parse_epsv("(|||99999999999999999999|)", &port) < 0, "a very long number");
    check(ftpc_parse_epsv("(|||6446)", &port) < 0 && ftpc_parse_epsv("(|||6446|", &port) < 0, "cut short");
    check(ftpc_parse_epsv("(||6446|)", &port) < 0, "two delimiters");
    check(ftpc_parse_epsv("(|||6446#)", &port) < 0, "two kinds of delimiter");
    check(ftpc_parse_epsv("(111222|)", &port) < 0, "a digit for a delimiter");
    check(ftpc_parse_epsv("(   1 )", &port) < 0, "a blank for a delimiter");
    check(ftpc_parse_epsv("", &port) < 0 && ftpc_parse_epsv("(", &port) < 0 && ftpc_parse_epsv("(|", &port) < 0 && ftpc_parse_epsv("(||", &port) < 0 &&
          ftpc_parse_epsv("(|||", &port) < 0, "ends everywhere");

    check(!ftpc_parse_pasv("Entering Passive Mode (10,0,2,2,19,136).", &port) && port == 5000, "PASV");
    check(!ftpc_parse_pasv("=10,0,2,2,19,136", &port) && port == 5000, "without brackets");
    check(!ftpc_parse_pasv("at 12:30 on 1,2 (10,0,2,2,19,137)", &port) && port == 5001, "numbers before the real ones");
    check(!ftpc_parse_pasv("(255,255,255,255,255,255)", &port) && port == 65535, "the highest of each");
    check(ftpc_parse_pasv("(256,0,0,0,1,1)", &port) < 0 && ftpc_parse_pasv("(1,0,0,0,256,1)", &port) < 0, "a number past 255");
    check(ftpc_parse_pasv("(1,2,3,4,5)", &port) < 0, "five numbers");
    check(ftpc_parse_pasv("(1,2,3,4,5,6,7)", &port) < 0, "seven");
    check(ftpc_parse_pasv("(1,,3,4,5,6)", &port) < 0, "one missing");
    check(ftpc_parse_pasv("(1000,2,3,4,5,6)", &port) < 0, "four digits");
    check(ftpc_parse_pasv("99999999999999999999999999999999999999999999999999", &port) < 0, "a long number");
    check(ftpc_parse_pasv("", &port) < 0 && ftpc_parse_pasv("no numbers", &port) < 0, "none at all");

    check(ftpc_port_ok(1024) && ftpc_port_ok(65535) && !ftpc_port_ok(1023) && !ftpc_port_ok(21) && !ftpc_port_ok(0), "a passive port is from 1024 on");
}

static void test_values(void) {
    char out[32];
    unsigned long long v = 0;
    check(!ftpc_parse_path("\"/a/b\" is current", out, sizeof(out)) && !strcmp(out, "/a/b"), "257");
    check(!ftpc_parse_path("\"/a\"\"b\"", out, sizeof(out)) && !strcmp(out, "/a\"b"), "a quote doubled");
    check(!ftpc_parse_path("text \"/x\" more", out, sizeof(out)) && !strcmp(out, "/x"), "text before it");
    check(!ftpc_parse_path("\"\"", out, sizeof(out)) && !out[0], "empty");
    check(ftpc_parse_path("\"unclosed", out, sizeof(out)) < 0, "not closed");
    check(ftpc_parse_path("no quotes", out, sizeof(out)) < 0, "no quotes");
    check(ftpc_parse_path("\"abcdefghij\"", out, 5) < 0, "too long");
    check(!ftpc_parse_path("\"abcd\"", out, 5) && !strcmp(out, "abcd"), "just fits");
    check(ftpc_parse_path("x", out, 0) < 0, "no room at all");
    check(!ftpc_parse_size("1234", &v) && v == 1234, "213");
    check(!ftpc_parse_size("  7 bytes", &v) && v == 7, "blanks, and a word after a blank");
    check(!ftpc_parse_size("18446744073709551615", &v) && v == 18446744073709551615ULL, "the highest 64 bits hold");
    check(ftpc_parse_size("18446744073709551616", &v) < 0, "one past it");
    check(!ftpc_parse_size("18446744073709551610", &v) && v == 18446744073709551610ULL, "the last ten below it");
    check(ftpc_parse_size("18446744073709551620", &v) < 0, "ten past it");
    check(!ftpc_parse_size("1844674407370955161", &v) && v == 1844674407370955161ULL, "one digit fewer");
    check(ftpc_parse_size("184467440737095516150", &v) < 0, "one digit more");
    check(!ftpc_parse_size("00000000000000000000000000000000012", &v) && v == 12, "leading zeros are no overflow");
    check(ftpc_parse_size("99999999999999999999999999", &v) < 0, "far past it");
    check(ftpc_parse_size("", &v) < 0 && ftpc_parse_size("-1", &v) < 0 && ftpc_parse_size("12x", &v) < 0 && ftpc_parse_size("x", &v) < 0, "no number");
}

static void test_text(void) {
    static const struct { const char *in, *out; } cases[] = {
        { "plain", "plain" }, { "", "" }, { "a\tb", "a b" }, { "\x1b[2J", "?[2J" }, { "\x07", "?" }, { "a\rb\nc", "a?b?c" }, { "\x7f", "?" },
        { "caf\xc3\xa9", "caf\xc3\xa9" }, { "\xe2\x89\x8b", "\xe2\x89\x8b" }, { "\xf0\x9f\x8f\x84", "\xf0\x9f\x8f\x84" },
        { "\xc2\x80", "?" }, { "\xc2\x9b", "?" }, { "\xc2\x9f", "?" }, { "\xc2\xa0", "\xc2\xa0" },      /* C1 controls, then the first that is none */
        { "\x9b", "?" }, { "\xff", "?" }, { "\xfe", "?" }, { "\xc0\x80", "??" }, { "\xc1\xbf", "??" },  /* lone bytes, overlong forms */
        { "\xe0\x80\x80", "???" }, { "\xed\xa0\x80", "???" }, { "\xf0\x80\x80\x80", "????" }, { "\xf4\x90\x80\x80", "????" }, { "\xf5\x80\x80\x80", "????" },
        { "\xc3", "?" }, { "\xe2\x89", "??" }, { "\xf0\x9f\x8f", "???" }, { "a\xc3", "a?" },            /* cut short */
    };
    unsigned int i;
    int size;
    char out[64];
    for(i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        ftpc_text(out, sizeof(out), cases[i].in);
        if(strcmp(out, cases[i].out)) printf("  case %u: [%s]\n", i, out);
        check(!strcmp(out, cases[i].out), "text as a terminal may show it");
    }
    for(size = 1; size < 12; size++) {          /* cut at a whole character, whatever the room */
        char *b = malloc((size_t)size);
        ftpc_text(b, size, "ab\xe2\x89\x8b" "cd");
        check(strlen(b) < (size_t)size && (size_t)size - 1 >= strlen(b) && (strlen(b) <= 2 || strlen(b) >= 5), "cut, but never inside a character");
        free(b);
    }
    {
        char t[] = "pw is hunter2 and hunter2hunter2 x";
        ftpc_scrub(t, "hunter2");
        check(!strcmp(t, "pw is ******* and ************** x"), "every copy of a password becomes stars");
        ftpc_scrub(t, "");
        check(!strcmp(t, "pw is ******* and ************** x"), "an empty one changes nothing");
    }
    {
        char t[] = "aaaa";
        ftpc_scrub(t, "aa");
        check(!strcmp(t, "****"), "copies side by side");
    }
}

static void test_words(void) {
    char line[128], *w[8];
    int n;
    strcpy(line, "get a b");
    n = ftpc_words(line, w, 8);
    check(n == 3 && !strcmp(w[0], "get") && !strcmp(w[1], "a") && !strcmp(w[2], "b"), "words");
    strcpy(line, "   spaced \t out   ");
    n = ftpc_words(line, w, 8);
    check(n == 2 && !strcmp(w[0], "spaced") && !strcmp(w[1], "out"), "blanks around them");
    strcpy(line, "");
    check(!ftpc_words(line, w, 8), "nothing");
    strcpy(line, "   ");
    check(!ftpc_words(line, w, 8), "only blanks");
    strcpy(line, "get \"my file\" \"\" x\"y z\"w");
    n = ftpc_words(line, w, 8);
    check(n == 4 && !strcmp(w[1], "my file") && !w[2][0] && !strcmp(w[3], "xy zw"), "quotes around and inside a word, an empty word");
    strcpy(line, "put \"a\\\"b\\\\c\\d\"");
    n = ftpc_words(line, w, 8);
    check(n == 2 && !strcmp(w[1], "a\"b\\c\\d"), "\\\" and \\\\ inside quotes, another backslash as it is");
    strcpy(line, "back\\slash");
    n = ftpc_words(line, w, 8);
    check(n == 1 && !strcmp(w[0], "back\\slash"), "a backslash outside quotes is itself");
    strcpy(line, "get \"open");
    check(ftpc_words(line, w, 8) == -2, "a quote not closed");
    strcpy(line, "a b c d e f g h i");
    check(ftpc_words(line, w, 8) == -1, "more than fit");
    strcpy(line, "a b c d e f g h");
    check(ftpc_words(line, w, 8) == 8, "just as many as fit");

    check(!strcmp(ftpc_last_name("/a/b"), "b") && !strcmp(ftpc_last_name("b"), "b") && !strcmp(ftpc_last_name("a/"), "") && !strcmp(ftpc_last_name(""), ""), "last names");
    check(!ftpc_join(line, 32, "a", "b") && !strcmp(line, "a/b"), "a join");
    check(!ftpc_join(line, 32, "a/", "b") && !strcmp(line, "a/b"), "a folder that ends in /");
    check(!ftpc_join(line, 32, "", "b") && !strcmp(line, "b"), "no folder");
    check(!ftpc_join(line, 4, "a", "b") && ftpc_join(line, 3, "a", "b") < 0, "just fits; does not");
    {
        char dir[8];
        const char *name = NULL;
        check(!ftpc_split("a/b/c", dir, sizeof(dir), &name) && !strcmp(dir, "a/b") && !strcmp(name, "c"), "a split");
        check(!ftpc_split("c", dir, sizeof(dir), &name) && !dir[0] && !strcmp(name, "c"), "no folder");
        check(!ftpc_split("/c", dir, sizeof(dir), &name) && !strcmp(dir, "/") && !strcmp(name, "c"), "the root");
        check(!ftpc_split("a/", dir, sizeof(dir), &name) && !strcmp(dir, "a") && !*name, "no name");
        check(ftpc_split("abcdefghijkl/c", dir, sizeof(dir), &name) < 0, "a folder too long");
    }
}

static void test_names(void) {
    static const char *const good[] = { "a.txt", "README", "my file.txt", "caf\xc3\xa9.txt", "a..b", ".hidden", "x-y", "~backup", "1", "\xe2\x89\x8b" };
    static const char *const bad[] = { "", ".", "..", "/", "/etc/passwd", "../x", "a/b", "a\\b", "..\\x", "c:x", "a*", "a?", "a\"b", "a<b", "a>b", "a|b",
                                       "a\rb", "a\nb", "a\tb", "\x1b[2J", "a\x7f", "-rf", "-", "x.", "x ", "..  ", "... ", "\xff", "a\xc3", "\xc2\x9b" };
    char big[300];
    unsigned int i;
    for(i = 0; i < sizeof(good) / sizeof(good[0]); i++) check(ftpc_safe_name(good[i]), "a name that may become a file");
    for(i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) check(!ftpc_safe_name(bad[i]), "a name that may not");
    memset(big, 'a', sizeof(big));
    big[255] = 0;
    check(ftpc_safe_name(big), "255 bytes, FAT's longest");
    big[255] = 'a';
    big[256] = 0;
    check(!ftpc_safe_name(big), "256 bytes");
}

static void test_glob(void) {
    static const struct { const char *p, *n; int want; } cases[] = {
        { "*", "a", 1 }, { "*", "", 1 }, { "", "", 1 }, { "", "a", 0 }, { "a", "a", 1 }, { "a", "b", 0 }, { "?", "a", 1 }, { "?", "", 0 }, { "??", "a", 0 },
        { "*.txt", "a.txt", 1 }, { "*.txt", "a.TXT", 0 }, { "a*b*c", "aXbYc", 1 }, { "a*b*c", "aXbY", 0 }, { "**a", "a", 1 }, { "*a*", "bab", 1 },
        { "[abc]", "b", 1 }, { "[abc]", "d", 0 }, { "[a-c]", "b", 1 }, { "[!a-c]", "b", 0 }, { "[!a-c]", "d", 1 }, { "[^a]", "b", 1 }, { "[]a]", "]", 1 },
        { "[a", "[a", 1 }, { "[", "[", 1 }, { "[!", "[!", 1 }, { "a[", "a[", 1 }, { "\\*", "*", 1 }, { "\\*", "a", 0 }, { "\\[", "[", 1 }, { "a\\", "a\\", 1 },
        { "*", ".hidden", 0 }, { "?hidden", ".hidden", 0 }, { "[.]hidden", ".hidden", 0 }, { ".*", ".hidden", 1 }, { "\\.h", ".h", 1 }, { "*.txt", ".a.txt", 0 },
        { "a*", "a.b", 1 }, { "?", "\xc3\xa9", 1 }, { "??", "\xc3\xa9", 0 }, { "a?c", "a\xe2\x89\x8b" "c", 1 }, { "\xc3\xa9", "\xc3\xa9", 1 },
        { "[\xc3\xa9]", "\xc3\xa9", 1 }, { "[\xc3\xa0-\xc3\xbf]", "\xc3\xa9", 1 }, { "[a-z]", "\xc3\xa9", 0 }, { "?", "\xff", 1 }, { "??", "\xff\xff", 1 },
        /* a backslash in brackets quotes, as in surfsh's patterns and glibc (macOS's fnmatch takes it as itself): so
           the first ] here closes nothing, and the [ is itself; there are no [.x.] symbols, as in surfsh */
        { "[a\\]", "a", 0 }, { "[a\\]", "[a]", 1 }, { "[a\\]]", "]", 1 }, { "[a\\]]", "a", 1 }, { "[.]", ".", 0 }, { "[.]a", "ba", 0 }, { "a[.]", "a.", 1 },
    };
    unsigned int i, seed = 1;
    int k, diffs = 0;
    static char longp[2001], longn[256];
    for(i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        int got = ftpc_glob(cases[i].p, cases[i].n, 0);
        if(got != cases[i].want) printf("  [%s] [%s]: %d\n", cases[i].p, cases[i].n, got);
        check(got == cases[i].want, "a wildcard");
    }
    check(ftpc_glob("*.TXT", "a.txt", 1) && ftpc_glob("[A-C].txt", "b.TXT", 1) && ftpc_glob("[a-c]", "B", 1), "ignoring case, as FAT does");

    /* the host's fnmatch(3), on random patterns of ASCII */
    for(k = 0; k < 400000; k++) {
        static const char pa[] = "ab.*?[]!-\\", na[] = "ab.]-!";
        char p[10], n[8];
        int a, b, lp, ln, j;
        seed = seed * 1103515245u + 12345u;
        lp = (int)(seed >> 16) % 9;
        for(j = 0; j < lp; j++) {
            seed = seed * 1103515245u + 12345u;
            p[j] = pa[(seed >> 16) % (sizeof(pa) - 1)];
        }
        p[lp] = 0;
        seed = seed * 1103515245u + 12345u;
        ln = (int)(seed >> 16) % 7;
        for(j = 0; j < ln; j++) {
            seed = seed * 1103515245u + 12345u;
            n[j] = na[(seed >> 16) % (sizeof(na) - 1)];
        }
        n[ln] = 0;
        if(strchr(p, '[') && (strchr(p, '\\') || strstr(p, "[."))) continue;       /* the two corners above, where hosts differ */
        a = ftpc_glob(p, n, 0);
        b = fnmatch(p, n, FNM_PERIOD) == 0;
        if(a != b) {
            if(diffs < 10) printf("  fnmatch differs: pattern [%s] name [%s]: here %d, there %d\n", p, n, a, b);
            diffs++;
        }
    }
    check(!diffs, "the same answers as fnmatch(3) on random pairs");

    /* no pattern takes long: the last star is all that is tried again */
    memset(longn, 'a', sizeof(longn) - 1);
    for(k = 0; k < 1998; k += 2) {
        longp[k] = 'a';
        longp[k + 1] = '*';
    }
    longp[1998] = 'b';
    check(!ftpc_glob(longp, longn, 0), "a pattern made to take for ever ends");
}

static void test_lines(void) {
    static const char text[] = "one\r\ntwo\nthree\r\r\n\nlast";
    int step;
    for(step = 1; step < (int)sizeof(text); step++) {
        struct ftpc_lines l;
        char out[64];
        int off = 0, n = (int)sizeof(text) - 1, got;
        memset(&l, 0, sizeof(l));
        out[0] = 0;
        while(off < n) {
            int k = n - off < step ? n - off : step, used = 0;
            while(used < k) {
                used += ftpc_lines_feed(&l, text + off + used, k - used, &got);
                if(got == FTP_LINE) {
                    strcat(out, l.line);
                    strcat(out, "|");
                }
            }
            off += k;
        }
        if(ftpc_lines_end(&l) == FTP_LINE) strcat(out, l.line);
        check(!strcmp(out, "one|two|three\r||last"), "a listing's lines, at every split");
    }
    {
        static char big[3000];
        struct ftpc_lines l;
        int got, used;
        memset(&l, 0, sizeof(l));
        memset(big, 'x', sizeof(big));
        big[sizeof(big) - 1] = '\n';
        used = ftpc_lines_feed(&l, big, sizeof(big), &got);
        check(used == (int)sizeof(big) && got == FTP_LONG, "a line too long is said at its end");
        used = ftpc_lines_feed(&l, "ok\n", 3, &got);
        check(used == 3 && got == FTP_LINE && !strcmp(l.line, "ok"), "and the next is whole");
        memset(&l, 0, sizeof(l));
        ftpc_lines_feed(&l, big, FTPC_LIST_LINE, &got);
        check(got == FTP_MORE, "just as long as may be");
        ftpc_lines_feed(&l, "\n", 1, &got);
        check(got == FTP_LINE && strlen(l.line) == FTPC_LIST_LINE, "fits");
        memset(&l, 0, sizeof(l));
        ftpc_lines_feed(&l, big, sizeof(big) - 1, &got);
        check(got == FTP_MORE && ftpc_lines_end(&l) == FTP_MORE, "a last line too long, with no end, is left out");
        memset(&l, 0, sizeof(l));
        ftpc_lines_feed(&l, "a\0b\n", 4, &got);
        check(got == FTP_LINE && !strcmp(l.line, "a\x7f" "b") && !ftpc_safe_name(l.line), "a NUL becomes a DEL, which no name may hold");
    }
}

static void type(struct ftpc_secret *s, const char *keys, int *last) {
    memset(s, 0, sizeof(*s));
    for(; *keys; keys++) *last = ftpc_secret_key(s, (unsigned char)*keys);
}

static void test_secret(void) {
    struct ftpc_secret s;
    unsigned char b[8];
    int r = 0, i;
    type(&s, "abc\r", &r);
    check(r == FTPC_S_DONE && !strcmp(s.text, "abc"), "a password and Return");
    type(&s, "abc\n", &r);
    check(r == FTPC_S_DONE && !strcmp(s.text, "abc"), "an LF is a Return too");
    type(&s, "\r", &r);
    check(r == FTPC_S_DONE && !s.text[0], "an empty one");
    type(&s, "abc\x03", &r);
    check(r == FTPC_S_CANCEL, "Ctrl+C");
    type(&s, "abc\x04", &r);
    check(r == FTPC_S_CANCEL, "Ctrl+D");
    type(&s, "ab\x7f" "c\r", &r);
    check(!strcmp(s.text, "ac"), "Backspace");
    type(&s, "ab\x08" "c\r", &r);
    check(!strcmp(s.text, "ac"), "BS");
    type(&s, "\x7f\x7f" "a\r", &r);
    check(!strcmp(s.text, "a"), "Backspace with nothing there");
    type(&s, "ab\xc3\xa9\x7f\r", &r);
    check(!strcmp(s.text, "ab"), "Backspace takes a whole character");
    type(&s, "ab\x15" "c\r", &r);
    check(!strcmp(s.text, "c"), "Ctrl+U");
    type(&s, "a\x1b[Ab\x1b[1;5Cc\x1b[3~d\r", &r);
    check(!strcmp(s.text, "abcd"), "special keys' sequences are dropped");
    type(&s, "a\x1bxb\r", &r);
    check(!strcmp(s.text, "ab"), "Alt and a key");
    type(&s, "a\x01\x02" "b\t\r", &r);
    check(!strcmp(s.text, "ab"), "other controls are dropped");
    memset(&s, 0, sizeof(s));
    for(i = 0; i < FTPC_SECRET_MAX; i++) ftpc_secret_key(&s, 'k');
    check(!s.over && s.len == FTPC_SECRET_MAX, "as long as may be");
    ftpc_secret_key(&s, 'k');
    check(s.over && s.len == FTPC_SECRET_MAX, "one more says so, and the buffer is safe");
    check(ftpc_secret_key(&s, '\r') == FTPC_S_DONE && strlen(s.text) == FTPC_SECRET_MAX, "and it ends with its NUL inside");
    memset(b, 'x', sizeof(b));
    ftpc_wipe(b, 4);
    check(!b[0] && !b[3] && b[4] == 'x', "a wipe, and no further");
}

int main(void) {
    test_replies();
    test_commands();
    test_passive();
    test_values();
    test_text();
    test_words();
    test_names();
    test_glob();
    test_lines();
    test_secret();
    printf("test_ftpc: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
