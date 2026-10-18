/* Host-side unit tests for net/netcfg.c, the text of the settings files on the stick
   (surfos-config/settings.conf and networks.conf): the keys and their values, reading a
   file (comments, CR LF and CR alone, a byte order mark, NUL bytes, straight and curly
   quotes, bad lines told and skipped), editing one in place (the owner's lines, comments
   and indentation kept byte for byte, a line never grown past what is read back, the new
   file's commented-out defaults), quoting SSIDs (every byte value there and back), the
   saved networks written and read back, a password written by hand and never written
   back, every kind of bad network told, left out and kept as it was, and the DHCP lease
   timers. Built like the kernel objects and run by `make unittest`. */
#include <stdio.h>
#include <string.h>
#include <net/netcfg.h>

void exit(int status);

static int failures, checks;

static void check(int ok, const char *what) {
    checks++;
    if(!ok) { failures++; printf("FAIL: %s\n", what); }
}

/* the notes a reader gives, one per line: "line: why" */
static char notes[4096];
static int nnotes;

static void note(void *arg, u_int line, const char *why) {
    size_t n = strlen(notes);
    (void)arg;
    nnotes++;
    snprintf(notes + n, sizeof(notes) - n, "%u: %s\n", line, why);
}

static void clear_notes(void) {
    notes[0] = 0;
    nnotes = 0;
}

/* a note with text in it; text starting with a line number matches only at the start of a note ("6: ..." is not
   "16: ...") */
static bool has_note(const char *text) {
    const char *p;
    for(p = strstr(notes, text); p; p = strstr(p + 1, text))
        if(text[0] < '0' || text[0] > '9' || p == notes || p[-1] == '\n') return true;
    return false;
}

static struct netcfg_settings parse_len(const char *text, u_int len) {
    struct netcfg_settings s;
    clear_notes();
    netcfg_parse_settings(text, len, &s, note, NULL);
    return s;
}

static struct netcfg_settings parse(const char *text) {
    return parse_len(text, text ? strlen(text) : 0);
}

/**** keys and values ****/

static void test_keys(void) {
    struct netcfg_settings s;
    char buf[64], why[160];
    netcfg_defaults(&s);
    check(!s.telnetd && s.telnetd_port == 23 && s.wifi && !s.ethernet_dhcp && !strcmp(s.hostname, "surfos") && !s.set,
          "the defaults: telnetd off on 23, wifi on, ethernet off, hostname surfos, nothing set");
    check(netcfg_key_find("telnetd") == NETCFG_TELNETD && netcfg_key_find("TELNETD.Port") == NETCFG_TELNETD_PORT &&
          netcfg_key_find("wifi") == NETCFG_WIFI && netcfg_key_find("ethernet") == NETCFG_ETHERNET &&
          netcfg_key_find("hostname") == NETCFG_HOSTNAME, "every key is found, whatever its case");
    check(netcfg_key_find("telnet") < 0 && netcfg_key_find("") < 0 && netcfg_key_find("wifi.x") < 0, "no other key");
    check(!strcmp(netcfg_key_name(NETCFG_TELNETD_PORT), "telnetd.port") && !strcmp(netcfg_key_name(99), ""),
          "key names");
    check(!strcmp(netcfg_value(&s, NETCFG_TELNETD, buf, sizeof(buf)), "off") &&
          !strcmp(netcfg_value(&s, NETCFG_TELNETD_PORT, buf, sizeof(buf)), "23") &&
          !strcmp(netcfg_value(&s, NETCFG_WIFI, buf, sizeof(buf)), "on") &&
          !strcmp(netcfg_value(&s, NETCFG_ETHERNET, buf, sizeof(buf)), "off") &&
          !strcmp(netcfg_value(&s, NETCFG_HOSTNAME, buf, sizeof(buf)), "surfos"), "values as text");
    check(!strcmp(netcfg_default_value(NETCFG_ETHERNET, buf, sizeof(buf)), "off") &&
          !strcmp(netcfg_default_value(NETCFG_WIFI, buf, sizeof(buf)), "on"), "default values as text");

    check(netcfg_set(&s, NETCFG_TELNETD, "on", why, sizeof(why)) == 0 && s.telnetd && (s.set & (1u << NETCFG_TELNETD)),
          "telnetd on");
    check(netcfg_set(&s, NETCFG_TELNETD, "No", why, sizeof(why)) == 0 && !s.telnetd, "telnetd No (yes and no too)");
    check(netcfg_set(&s, NETCFG_TELNETD, "maybe", why, sizeof(why)) < 0 && !s.telnetd &&
          strstr(why, "telnetd is on | off, not maybe"), "telnetd maybe: refused, saying what it takes");
    check(netcfg_set(&s, NETCFG_TELNETD_PORT, "2323", why, sizeof(why)) == 0 && s.telnetd_port == 2323, "port 2323");
    check(netcfg_set(&s, NETCFG_TELNETD_PORT, "65535", why, sizeof(why)) == 0 && s.telnetd_port == 65535, "port 65535");
    check(netcfg_set(&s, NETCFG_TELNETD_PORT, "0", why, sizeof(why)) < 0 &&
          netcfg_set(&s, NETCFG_TELNETD_PORT, "65536", why, sizeof(why)) < 0 &&
          netcfg_set(&s, NETCFG_TELNETD_PORT, "99999999999", why, sizeof(why)) < 0 &&
          netcfg_set(&s, NETCFG_TELNETD_PORT, "-1", why, sizeof(why)) < 0 &&
          netcfg_set(&s, NETCFG_TELNETD_PORT, "23 ", why, sizeof(why)) < 0 &&
          netcfg_set(&s, NETCFG_TELNETD_PORT, "", why, sizeof(why)) < 0 && s.telnetd_port == 65535,
          "ports out of range, signs, blanks and nothing are refused");
    check(netcfg_set(&s, NETCFG_ETHERNET, "DHCP", why, sizeof(why)) == 0 && s.ethernet_dhcp &&
          netcfg_set(&s, NETCFG_ETHERNET, "off", why, sizeof(why)) == 0 && !s.ethernet_dhcp &&
          netcfg_set(&s, NETCFG_ETHERNET, "on", why, sizeof(why)) < 0, "ethernet dhcp or off");
    check(netcfg_set(&s, NETCFG_WIFI, "off", why, sizeof(why)) == 0 && !s.wifi, "wifi off");
    check(netcfg_set(&s, NETCFG_HOSTNAME, "ideapad-3", why, sizeof(why)) == 0 && !strcmp(s.hostname, "ideapad-3"),
          "a host name with a -");
    check(netcfg_set(&s, NETCFG_HOSTNAME, "abcdefghijklmnopqrstuvwxyz012345", why, sizeof(why)) == 0,
          "a 32-character host name");
    check(netcfg_set(&s, NETCFG_HOSTNAME, "abcdefghijklmnopqrstuvwxyz0123456", why, sizeof(why)) < 0 &&
          netcfg_set(&s, NETCFG_HOSTNAME, "-surf", why, sizeof(why)) < 0 &&
          netcfg_set(&s, NETCFG_HOSTNAME, "surf-", why, sizeof(why)) < 0 &&
          netcfg_set(&s, NETCFG_HOSTNAME, "surf os", why, sizeof(why)) < 0 &&
          netcfg_set(&s, NETCFG_HOSTNAME, "surf.os", why, sizeof(why)) < 0 &&
          netcfg_set(&s, NETCFG_HOSTNAME, "", why, sizeof(why)) < 0 &&
          !strcmp(s.hostname, "abcdefghijklmnopqrstuvwxyz012345"),
          "host names too long, with a - at an end, a blank, a dot, or empty are refused");
    check(netcfg_set(&s, 99, "on", why, sizeof(why)) < 0, "no key 99");
}

/**** reading settings.conf ****/

static void test_parse(void) {
    struct netcfg_settings s = parse(NULL);
    check(!s.set && s.wifi && !nnotes, "no file: the defaults, nothing said");
    s = parse("");
    check(!s.set && !nnotes, "an empty file: the defaults");
    s = parse("# only a comment\n\n   \t\n#telnetd = on\n");
    check(!s.set && !s.telnetd && !nnotes, "comments, blank lines and a commented-out setting set nothing");
    s = parse("telnetd = on\ntelnetd.port=2323\n  wifi   =   off   # not at home\r\nethernet = dhcp\r\nhostname = \"laptop\"");
    check(s.telnetd && s.telnetd_port == 2323 && !s.wifi && s.ethernet_dhcp && !strcmp(s.hostname, "laptop") &&
          s.set == 0x1f && !nnotes,
          "every key: no spaces, many spaces and a comment, CR LF, quotes, no line end at the end");
    s = parse("telnetd = on\ntelnetd = off\n");
    check(!s.telnetd && !nnotes, "the last line of a key wins");
    s = parse("TelnetD = ON\n");
    check(s.telnetd, "keys and words in any case");
    s = parse("telnetd on\n= on\nfrobnicate = 3\nwifi = sometimes\ntelnetd.port = 70000\nhostname = no_underscores\n"
              "telnetd = on\n");
    check(s.telnetd && s.wifi && s.telnetd_port == 23 && !strcmp(s.hostname, "surfos") && nnotes == 6,
          "bad lines are each told and skipped, the good one after them taken");
    check(has_note("1: not key = value") && has_note("2: not key = value") &&
          has_note("3: no setting called frobnicate (it is left as it is)") &&
          has_note("4: wifi is on | off, not sometimes") && has_note("5: telnetd.port is 1-65535, not 70000") &&
          has_note("6: hostname is letters"), "the notes give the line and why");
    {
        char text[400];
        memset(text, 'x', 300);
        memcpy(text, "hostname = ", 11);
        strcpy(text + 300, "\ntelnetd = on\n");
        s = parse(text);
        check(s.telnetd && !strcmp(s.hostname, "surfos") && nnotes == 1 && has_note("1: the line is too long"),
              "a line of 300 bytes is told and skipped, the next one read");
    }
    s = parse("hostname = \"#notacomment\"\n");
    check(nnotes == 1 && has_note("not #notacomment"), "a # inside quotes is no comment (and no host name either)");
    s = parse("telnetd = on\rwifi = off\rhostname = mac\r");
    check(s.telnetd && !s.wifi && !strcmp(s.hostname, "mac") && !nnotes, "CR alone ends a line (an old Mac's files)");
    s = parse("\xEF\xBB\xBFtelnetd = on\n");
    check(s.telnetd && !nnotes, "a byte order mark first is skipped");
    s = parse_len("telnetd\0xyz = on\nwifi = off\n", 28);
    check(!s.telnetd && !s.wifi && nnotes == 1 && has_note("1: the line has a NUL byte in it"),
          "a line with a NUL byte in it is told and skipped");
    s = parse("hostname = \xE2\x80\x9Claptop\xE2\x80\x9D   # smart quotes\n");
    check(!strcmp(s.hostname, "laptop") && !nnotes, "TextEdit's curly quotes are quotes");
    s = parse("telnetd = on # \"quoted\" comment\n");
    check(s.telnetd && !nnotes, "a comment may hold quotes");
}

/**** editing settings.conf ****/

static char out[8192];

static int edit(const char *text, int k, const char *value) {
    return netcfg_edit_settings(text, text ? strlen(text) : 0, k, value, out, sizeof(out));
}

static void test_edit(void) {
    struct netcfg_settings s;
    char small[16];
    int n;

    n = edit(NULL, NETCFG_TELNETD, "on");
    check(n > 0 && n == (int)strlen(out), "a new file");
    check(strstr(out, "\ntelnetd = on\n") && !strstr(out, "#telnetd = off") && strstr(out, "#telnetd.port = 23\n") &&
          strstr(out, "#wifi = on\n") && strstr(out, "#ethernet = off\n") && strstr(out, "#hostname = surfos\n"),
          "the new file: the template, telnetd's commented-out default made the setting, the others left commented out");
    check(strstr(out, "# SurfOS settings") && strstr(out, "no password"), "the new file explains itself");
    s = parse(out);
    check(s.telnetd && s.set == (1u << NETCFG_TELNETD) && !nnotes, "it reads back: telnetd on, the rest defaults, no note");
    {
        char first[8192];
        strcpy(first, out);
        n = edit(first, NETCFG_HOSTNAME, "atlas");
        s = parse(out);
        check(n > 0 && s.telnetd && !strcmp(s.hostname, "atlas") && s.set == ((1u << NETCFG_TELNETD) | (1u << NETCFG_HOSTNAME)),
              "a second edit of the new file: both set");
        check(strlen(out) == strlen(first) + strlen("hostname = atlas") - strlen("#hostname = surfos"),
              "only that line changed length");
    }

    n = edit("  telnetd = off\t# was on before\n", NETCFG_TELNETD, "on");
    check(n > 0 && !strcmp(out, "  telnetd = on\t# was on before\n"), "its indentation and the blanks before its comment stay");
    n = edit("#telnetd = off   # the default\n", NETCFG_TELNETD, "on");
    check(n > 0 && !strcmp(out, "telnetd = on   # the default\n"), "a commented-out setting keeps its own comment");
    n = edit("wifi = on\rtelnetd = off\r", NETCFG_TELNETD, "on");
    check(n > 0 && !strcmp(out, "wifi = on\rtelnetd = on\r"), "CR alone stays CR alone");
    n = edit("wifi = on\r\n", NETCFG_TELNETD, "on");
    check(n > 0 && !strcmp(out, "wifi = on\r\ntelnetd = on\r\n"), "a line added to a Windows file ends with CR LF");
    n = edit("wifi = on\r", NETCFG_TELNETD, "on");
    check(n > 0 && !strcmp(out, "wifi = on\rtelnetd = on\r"), "and to an old Mac's with CR");
    n = edit("wifi = on\r\ntelnetd = off", NETCFG_TELNETD, "on");
    check(n > 0 && !strcmp(out, "wifi = on\r\ntelnetd = on\r\n"), "a last line without its end gets the file's");
    n = edit("\xEF\xBB\xBFtelnetd = off\n", NETCFG_TELNETD, "on");
    check(n > 0 && !strcmp(out, "\xEF\xBB\xBFtelnetd = on\n"), "a byte order mark first stays first");
    n = netcfg_edit_settings("x\0y = 1\ntelnetd = off\n", 22, NETCFG_TELNETD, "on", out, sizeof(out));
    check(n == 21 && !memcmp(out, "x\0y = 1\ntelnetd = on\n", 21), "a line with a NUL byte in it stays as it was");
    {
        char text[400], want[400];
        memset(text, 0, sizeof(text));
        strcpy(text, "hostname = a   #");
        memset(text + 16, 'c', 236);                /* a line of 252 bytes: readable */
        strcpy(text + 252, "\nwifi = on\n");
        n = edit(text, NETCFG_HOSTNAME, "abcdefghijklmnopqrstuvwxyz012345");
        memset(want, 0, sizeof(want));
        want[0] = '#';
        memset(want + 1, 'c', 236);
        strcpy(want + 237, "\nhostname = abcdefghijklmnopqrstuvwxyz012345\nwifi = on\n");
        s = parse(out);
        check(n > 0 && !strcmp(out, want) && !strcmp(s.hostname, "abcdefghijklmnopqrstuvwxyz012345") && !nnotes,
              "a line that would grow past what is read: its comment goes on a line of its own above it");
        memset(text, 0, sizeof(text));
        strcpy(text, "telnetd = off   #");
        memset(text + 17, 'c', 300);                /* too long to read: it sets nothing */
        strcpy(text + 317, "\n");
        n = edit(text, NETCFG_TELNETD, "on");
        s = parse(out);
        check(n > 0 && !strncmp(out, text, 318) && !strcmp(out + 318, "telnetd = on\n") && s.telnetd,
              "a setting on a line too long to read is kept as it was, and the setting added");
    }
    n = edit("# mine\ntelnetd = off   # was on before\nwifi = on\n", NETCFG_TELNETD, "yes");
    check(n > 0 && !strcmp(out, "# mine\ntelnetd = on   # was on before\nwifi = on\n"),
          "an owner's file: the line changed in place, its comment kept, the value written as on");
    n = edit("telnetd = off\nwifi = on\ntelnetd = on\n", NETCFG_TELNETD, "off");
    check(n > 0 && !strcmp(out, "telnetd = off\nwifi = on\n"), "two lines setting it: the first changed, the second gone");
    n = edit("wifi = on\n", NETCFG_ETHERNET, "dhcp");
    check(n > 0 && !strcmp(out, "wifi = on\nethernet = dhcp\n"), "a key not in the file: added at the end");
    n = edit("wifi = on", NETCFG_ETHERNET, "DHCP");
    check(n > 0 && !strcmp(out, "wifi = on\nethernet = dhcp\n"), "the last line had no line end: one is added first");
    n = edit("wifi = on\r\ntelnetd = off\r\n", NETCFG_TELNETD, "on");
    check(n > 0 && !strcmp(out, "wifi = on\r\ntelnetd = on\r\n"), "CR LF lines keep their CR LF");
    n = edit("telnetd = off", NETCFG_TELNETD, "on");
    check(n > 0 && !strcmp(out, "telnetd = on\n"), "a setting on a last line without its line end");
    n = edit("# telnetd = off is the default\n#  telnetd   = off\nfrob = 1\n", NETCFG_TELNETD, "on");
    check(n > 0 && !strcmp(out, "# telnetd = off is the default\ntelnetd = on\nfrob = 1\n"),
          "a commented-out setting becomes the setting; a comment that only mentions it does not; unknown keys stay");
    n = edit("#telnetd = off\ntelnetd = off\n", NETCFG_TELNETD, "on");
    check(n > 0 && !strcmp(out, "#telnetd = off\ntelnetd = on\n"), "a real setting wins over a commented-out one");
    n = edit("", NETCFG_WIFI, "off");
    check(n > 0 && !strcmp(out, "wifi = off\n"), "an empty file gets the line alone");
    n = edit("wifi = on\n", NETCFG_WIFI, "sometimes");
    check(n == -2 && strstr(out, "wifi is on | off, not sometimes"), "a bad value: -2 and why, nothing written");
    n = edit("wifi = on\n", 42, "on");
    check(n == -2, "no such key: -2");
    n = netcfg_edit_settings("wifi = on\n", 10, NETCFG_TELNETD, "on", small, sizeof(small));
    check(n == -1, "a buffer too small: -1");
    n = netcfg_edit_settings(NULL, 0, NETCFG_TELNETD, "on", small, sizeof(small));
    check(n == -1, "the new file does not fit either: -1");
    {
        char text[400];
        memset(text, 'y', 300);
        text[0] = '#';
        strcpy(text + 300, "\ntelnetd = off\n");
        n = edit(text, NETCFG_TELNETD, "on");
        check(n > 0 && !strncmp(out, text, 301) && !strcmp(out + 301, "telnetd = on\n"),
              "a line too long to read is kept as it was");
    }
}

/**** SSIDs ****/

static void test_quote(void) {
    u8 ssid[32], back[32];
    char q[140];
    u_int len, i, b;
    int n;
    n = netcfg_quote((const u8 *)"HouseOfBurr", 11, q, sizeof(q));
    check(n == 13 && !strcmp(q, "\"HouseOfBurr\""), "a plain name, quoted");
    n = netcfg_quote((const u8 *)"a\"b\\c d#e", 9, q, sizeof(q));
    check(n > 0 && !strcmp(q, "\"a\\\"b\\\\c d#e\""), "a quote and a backslash get a backslash; space and # stay");
    n = netcfg_quote((const u8 *)"caf\xc3\xa9\x01\x7f", 7, q, sizeof(q));
    check(n > 0 && !strcmp(q, "\"caf\\xc3\\xa9\\x01\\x7f\""), "bytes beyond printable ASCII as \\xHH");
    n = netcfg_quote((const u8 *)"", 0, q, sizeof(q));
    check(n == 2 && !strcmp(q, "\"\""), "an empty name");
    for(b = 0; b < 256; b += 8) {                     /* every byte value there and back, 8 to a name */
        for(i = 0; i < 8; i++) ssid[i] = (u8)(b + i);
        n = netcfg_quote(ssid, 8, q, sizeof(q));
        if(n < 0 || netcfg_unquote(q, back, sizeof(back), &len) || len != 8 || memcmp(back, ssid, 8)) break;
    }
    check(b == 256, "every byte value is quoted and read back");
    memset(ssid, 0xff, 32);
    n = netcfg_quote(ssid, 32, q, sizeof(q));
    check(n == 130 && !netcfg_unquote(q, back, sizeof(back), &len) && len == 32 && !memcmp(back, ssid, 32),
          "32 bytes that all need \\xHH: 130 characters, and back");
    check(netcfg_quote(ssid, 32, q, 129) < 0 && netcfg_quote((const u8 *)"ab", 2, q, 4) < 0 &&
          netcfg_quote((const u8 *)"ab", 2, q, 5) == 4, "a buffer too small, by one");
    check(!netcfg_unquote("HouseOfBurr", back, sizeof(back), &len) && len == 11 && !memcmp(back, "HouseOfBurr", 11),
          "a bare name typed by hand");
    check(!netcfg_unquote("My Home Wifi", back, sizeof(back), &len) && len == 12, "a bare name with spaces");
    check(!netcfg_unquote("\"\\X41\\x62\"", back, sizeof(back), &len) && len == 2 && back[0] == 'A' && back[1] == 'b',
          "\\X and \\x, either case of hex");
    check(netcfg_unquote("\"open", back, sizeof(back), &len) < 0 && netcfg_unquote("\"a\" b", back, sizeof(back), &len) < 0 &&
          netcfg_unquote("\"a\\q\"", back, sizeof(back), &len) < 0 && netcfg_unquote("\"\\x4\"", back, sizeof(back), &len) < 0 &&
          netcfg_unquote("\"\\x4g\"", back, sizeof(back), &len) < 0 && netcfg_unquote("\"a\\\"", back, sizeof(back), &len) < 0,
          "no closing quote, text after it, an unknown escape, short or bad hex: refused");
    check(netcfg_unquote("\"abcdefghijklmnopqrstuvwxyz0123456\"", back, 32, &len) < 0 &&
          netcfg_unquote("abcdefghijklmnopqrstuvwxyz0123456", back, 32, &len) < 0, "33 bytes do not fit 32");
}

/**** networks.conf ****/

static struct netcfg_net nets[NETCFG_NETS_MAX + 2], back[NETCFG_NETS_MAX + 2];
static char text[16384];

static void set_net(struct netcfg_net *n, const char *ssid, int security, int key_byte, bool hidden) {
    int i;
    memset(n, 0, sizeof(*n));
    n->ssid_len = (u8)strlen(ssid);
    memcpy(n->ssid, ssid, n->ssid_len);
    n->security = (u8)security;
    n->hidden = hidden;
    if(security == NETCFG_SEC_WPA2) {
        n->has_psk = true;
        for(i = 0; i < NETCFG_KEY_LEN; i++) n->psk[i] = (u8)(key_byte + i);
    }
}

static struct netcfg_span kept[NETCFG_SPANS_MAX];
static int nkept;

static int read_nets(const char *t) {
    clear_notes();
    return netcfg_parse_networks(t, strlen(t), back, NETCFG_NETS_MAX, kept, &nkept, note, NULL);
}

/* the kept span i of t, as a string */
static const char *span_text(const char *t, int i) {
    static char buf[1024];
    u_int n = kept[i].end - kept[i].start;
    if(n >= sizeof(buf)) n = sizeof(buf) - 1;
    memcpy(buf, t + kept[i].start, n);
    buf[n] = 0;
    return buf;
}

static void test_networks(void) {
    int n, i;
    u8 odd[5] = { 'a', '"', 0x00, 0xe9, '#' };
    set_net(&nets[0], "HouseOfBurr", NETCFG_SEC_WPA2, 0x10, false);
    set_net(&nets[1], "xfinitywifi", NETCFG_SEC_OPEN, 0, false);
    set_net(&nets[2], "Hidden One", NETCFG_SEC_WPA2, 0xf0, true);
    memset(&nets[3], 0, sizeof(nets[3]));
    memcpy(nets[3].ssid, odd, 5);
    nets[3].ssid_len = 5;
    nets[3].security = NETCFG_SEC_OPEN;
    n = netcfg_write_networks(nets, 4, NULL, NULL, 0, text, sizeof(text));
    check(n > 0 && n == (int)strlen(text), "four networks written");
    check(strstr(text, "# Wi-Fi networks SurfOS joins by itself") && strstr(text, "keep the stick safe"),
          "the header explains the file and its keys");
    check(strstr(text, "\nnetwork = \"HouseOfBurr\"\nsecurity = wpa2\npsk = 101112131415161718191a1b1c1d1e1f"
                       "202122232425262728292a2b2c2d2e2f\n") != NULL, "a WPA2 network: its name, security, its key in hex");
    check(strstr(text, "\nnetwork = \"xfinitywifi\"\nsecurity = open\n") && strstr(text, "hidden = yes\n") &&
          strstr(text, "network = \"a\\\"\\x00\\xe9#\"\n"), "an open one, a hidden one, an odd name quoted");
    n = read_nets(text);
    check(n == 4 && !nnotes && !nkept, "they read back, with no note and nothing kept");
    for(i = 0; i < 4 && i < n; i++) {
        check(back[i].ssid_len == nets[i].ssid_len && !memcmp(back[i].ssid, nets[i].ssid, nets[i].ssid_len) &&
              back[i].security == nets[i].security && back[i].hidden == nets[i].hidden &&
              back[i].has_psk == nets[i].has_psk && !memcmp(back[i].psk, nets[i].psk, NETCFG_KEY_LEN) &&
              !back[i].has_password, "each network as it was, in order");
    }
    check(netcfg_find_net(back, n, (const u8 *)"Hidden One", 10) == 2 && netcfg_find_net(back, n, odd, 5) == 3 &&
          netcfg_find_net(back, n, (const u8 *)"hidden one", 10) < 0, "found by name, exactly");
    check(netcfg_write_networks(nets, 0, NULL, NULL, 0, text, sizeof(text)) > 0 && read_nets(text) == 0 && !nnotes,
          "no networks: the header alone, read as none");
    nets[0].has_password = true;                    /* a key, and the password it was made from */
    strcpy(nets[0].password, "leaked-password");
    n = netcfg_write_networks(nets, 1, NULL, NULL, 0, text, sizeof(text));
    check(n > 0 && !strstr(text, "leaked-password") && !strstr(text, "\npassword"), "no password is ever written");
    nets[0].has_password = false;
    nets[0].ssid_len = 33;
    check(netcfg_write_networks(nets, 1, NULL, NULL, 0, text, sizeof(text)) < 0, "nor a name longer than 32 bytes");
    nets[0].ssid_len = 11;

    n = read_nets("network = My Home\nsecurity = wpa2\npassword = \"correct horse #1\"   # by hand\n"
                  "network = \"Cafe\"\n");
    check(n == 2 && back[0].ssid_len == 7 && !memcmp(back[0].ssid, "My Home", 7) && back[0].security == NETCFG_SEC_WPA2 &&
          back[0].has_password && !back[0].has_psk && !strcmp(back[0].password, "correct horse #1") &&
          back[1].security == NETCFG_SEC_OPEN && !nnotes,
          "by hand: a bare name, a password with a # inside its quotes, a comment after; an open one with no security");
    check(netcfg_write_networks(back, 1, NULL, NULL, 0, text, sizeof(text)) < 0,
          "a WPA2 network with only a password is never written");
    n = read_nets("network = \"A\"\npassword = \"12345678\"\n");
    check(n == 1 && back[0].security == NETCFG_SEC_WPA2, "a password alone means WPA2");
    n = read_nets("network = \"A\"\nsecurity = wpa2\npsk = 000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f\n"
                  "password = \"12345678\"\n");
    check(n == 1 && back[0].has_psk && !back[0].has_password, "with both, the key wins and the password is dropped");

    n = read_nets("security = open\n"
                  "network = \"\"\n"
                  "network = \"NoKey\"\nsecurity = wpa2\n"
                  "network = \"ShortKey\"\npsk = 0011\n"
                  "network = \"OpenWithKey\"\nsecurity = open\npsk = 000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f\n"
                  "network = \"ShortPass\"\npassword = \"1234567\"\n"
                  "network = \"Weird\"\nsecurity = wep\n"
                  "network = \"Good\"\nsecurity = open\nfrequency = 5\nhidden = perhaps\n"
                  "network = \"Good\"\nsecurity = open\n"
                  "network = \"Unclosed\n"
                  "network = \"Fine\"\n");
    check(n == 2 && back[0].ssid_len == 4 && !memcmp(back[0].ssid, "Good", 4) && back[1].ssid_len == 4 &&
          !memcmp(back[1].ssid, "Fine", 4), "of the bad networks none is taken; the good ones are, in order");
    check(has_note("1: a setting before the first network = line (kept as it is)") &&
          has_note("2: its name is not 1 to 32 bytes in double quotes: the network of line 2 is left out (and kept") &&
          has_note("3: a WPA2 network needs its psk (or password): the network \"NoKey\" is left out (and kept") &&
          has_note("6: psk is 64 hexadecimal digits: the network \"ShortKey\" is left out") &&
          has_note("7: an open network has no key or password (security = wpa2?): the network \"OpenWithKey\"") &&
          has_note("11: a WPA2 password is 8 to 63 characters") && has_note("13: security is open or wpa2") &&
          has_note("16: no such network setting (the line is left out)") && has_note("17: hidden is yes or no") &&
          has_note("18: that network was named before: the network \"Good\" is left out") &&
          has_note("20: its name is not 1 to 32 bytes"), "each problem told at its line, with the network's name");
    {
        static char bad[1024], again[16384];
        int i, n2, nk = nkept;
        strcpy(bad, "security = open\n"
                    "network = \"\"\n"
                    "network = \"NoKey\"\nsecurity = wpa2\n"
                    "network = \"ShortKey\"\npsk = 0011\n"
                    "network = \"OpenWithKey\"\nsecurity = open\npsk = 000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f\n"
                    "network = \"ShortPass\"\npassword = \"1234567\"\n"
                    "network = \"Weird\"\nsecurity = wep\n"
                    "network = \"Good\"\nsecurity = open\nfrequency = 5\nhidden = perhaps\n"
                    "network = \"Good\"\nsecurity = open\n"
                    "network = \"Unclosed\n"
                    "network = \"Fine\"\n");
        check(nk == 3 && !strcmp(span_text(bad, 0), "security = open\n") && kept[0].loose && !kept[1].loose &&
              !strncmp(span_text(bad, 1), "network = \"\"\nnetwork = \"NoKey\"\nsecurity = wpa2\n", 44) &&
              strstr(span_text(bad, 1), "network = \"Weird\"\nsecurity = wep\n") &&
              !strcmp(span_text(bad, 2), "network = \"Good\"\nsecurity = open\nnetwork = \"Unclosed\n"),
              "what was not taken is handed back, the blocks that follow each other as one");
        n = read_nets(bad);
        n2 = netcfg_write_networks(back, n, bad, kept, nkept, again, sizeof(again));
        check(n2 > 0 && strstr(again, "kept as they were") &&
              strstr(again, "network = \"OpenWithKey\"\nsecurity = open\npsk = 0001") &&
              strstr(again, "network = \"ShortPass\"\npassword = \"1234567\"\n"),
              "a write keeps them as they were, under a comment");
        check(strstr(again, "\nsecurity = open\n") < strstr(again, "network = \"Good\"") &&
              strstr(again, "network = \"Good\"\nsecurity = open\n") < strstr(again, "network = \"Weird\""),
              "the loose line before the networks, the blocks after them");
        i = read_nets(again);
        check(i == 2 && nkept == 2 && kept[0].loose && !memcmp(back[0].ssid, "Good", 4) &&
              !strncmp(again + kept[1].start, "network = \"\"\n", 13) && again[kept[1].end - 1] == '\n' &&
              kept[1].end == strlen(again), "and the next read takes the same networks and keeps the same bytes");
    }
    n = read_nets("network = \"Home\"\npasword = \"12345678\"\n");
    check(n == 0 && nkept == 1 && has_note("a line SurfOS does not understand and no security line (it is not taken as an "
                                           "open network): the network \"Home\" is left out"),
          "a typo where the password was, with no security line: left out, never taken as an open network");
    n = read_nets("network = \"Home\"\nsecurity = \"open\"\nhidden = \"yes\"\nmood = good\n");
    check(n == 1 && back[0].security == NETCFG_SEC_OPEN && back[0].hidden && nkept == 0,
          "with security said, an unknown line is only told; quoted values for security and hidden");
    n = read_nets("network = \xE2\x80\x9C" "Caf\xC3\xA9\xE2\x80\x9D\npassword = \xE2\x80\x9C" "abc#1234\xE2\x80\x9D\n");
    check(n == 1 && back[0].ssid_len == 5 && !memcmp(back[0].ssid, "Caf\xC3\xA9", 5) &&
          !strcmp(back[0].password, "abc#1234") && !nnotes, "TextEdit's curly quotes: a name and a password with # in it");
    n = read_nets("network = \"A\"\rsecurity = wpa2\rpassword = \"12345678\"\r");
    check(n == 1 && back[0].has_password, "CR alone ends a line here too");
    {
        char many[4096] = "";
        for(i = 0; i < NETCFG_NETS_MAX + 2; i++) {
            size_t l = strlen(many);
            snprintf(many + l, sizeof(many) - l, "network = \"net%d\"\n", i);
        }
        n = read_nets(many);
        check(n == NETCFG_NETS_MAX && nnotes == 2 && has_note("too many networks") && nkept == 1,
              "16 networks at most, the rest told and kept");
    }
    {
        char many[4096] = "";
        for(i = 0; i < NETCFG_SPANS_MAX + 1; i++) {     /* 17 bad ones, a good one between each two */
            size_t l = strlen(many);
            snprintf(many + l, sizeof(many) - l, "network = \"bad%d\"\npsk = 12\n", i);
            l = strlen(many);
            if(i < NETCFG_SPANS_MAX) snprintf(many + l, sizeof(many) - l, "network = \"ok%d\"\n", i);
        }
        n = read_nets(many);
        check(n == NETCFG_SPANS_MAX && nkept == NETCFG_SPANS_MAX && has_note("49: too many networks SurfOS cannot read"),
              "more parts not understood than can be kept: the rest is told");
    }
    {
        char many[4096] = "";
        for(i = 0; i < NETCFG_SPANS_MAX + 1; i++) {     /* 17 lines not understood, a comment between each two */
            size_t l = strlen(many);
            snprintf(many + l, sizeof(many) - l, "junk%d\n# between\n", i);
        }
        n = read_nets(many);
        check(nkept == NETCFG_SPANS_MAX && has_note("33: not key = value (not kept: the file has too many parts"),
              "lines before the networks past what can be kept: said so, not called kept");
    }
    n = read_nets("network = \"Home\"\nsecurity = wpa\npassword = \"my real password\"\n");
    check(n == 0 && nkept == 1 && kept[0].password && kept[0].line == 1 && !kept[0].loose,
          "a password in a network left out: its part is marked (SurfOS never writes a password)");
    n = read_nets("network = \"X\"\nsecurity = wep\n# password = \"secret12\"\n");
    check(n == 0 && nkept == 1 && kept[0].password, "a password commented out in it counts too");
    n = read_nets("password = \"loose-pass\"\nnetwork = \"A\"\n");
    check(n == 1 && nkept == 1 && kept[0].loose && kept[0].password && kept[0].line == 1, "so does one before the networks");
    n = read_nets("network = \"X\"\nsecurity = wep\nnetwork = \"A\"\npassword = \"12345678\"\n");
    check(n == 1 && nkept == 1 && !kept[0].password, "a password in a network taken does not mark the part before it");
    n = read_nets("network = \"E\"\npassword = \"a\\\"b\\\\c1234\"\n");
    check(n == 1 && !strcmp(back[0].password, "a\"b\\c1234"), "a password with \\\" and \\\\ in it");
    n = read_nets("network = \"H\"\nsecurity = wpa2\npassword = abcdefgh#ij\n");
    check(n == 0 && has_note("3: a password with a # in it goes in double quotes"), "a bare password with a #: refused");
    n = read_nets("network = \"H\"\nsecurity = wpa2\npassword = abcdefghij\n");
    check(n == 1 && !strcmp(back[0].password, "abcdefghij") && !nnotes, "a bare password without one: taken");
    {
        static char cr[256], w1[16384], w2[16384];
        int n1, n2;
        strcpy(cr, "security = open\rnetwork = \"A\"\rsecurity = open\r");
        n = read_nets(cr);
        n1 = netcfg_write_networks(back, n, cr, kept, nkept, w1, sizeof(w1));
        n = read_nets(w1);
        n2 = netcfg_write_networks(back, n, w1, kept, nkept, w2, sizeof(w2));
        check(n1 > 0 && n1 == n2 && !strcmp(w1, w2) && strstr(w1, "up to the first network = line"),
              "a file with CR alone: written once, the same written again; the loose part's own header");
    }
    {
        static char dup[512], w[16384];
        struct netcfg_net scratch[NETCFG_NETS_MAX], keep;
        int nk, k;
        snprintf(dup, sizeof(dup), "network = \"Cafe\"\nsecurity = open\nnetwork = \"Cafe\"\nsecurity = wpa2\npsk = %s\n",
                 "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f");
        n = read_nets(dup);
        nk = nkept;
        check(n == 1 && nk == 1 && has_note("3: that network was named before"), "a name written twice: the second kept");
        k = netcfg_write_networks(back, n, dup, kept, nk, w, sizeof(w));
        check(k > 0 && netcfg_reads_back(w, (u_int)k, back, n, scratch, NULL), "written again as it was: it reads back");
        keep = back[0];                                 /* forget Cafe: the one written second would come back */
        k = netcfg_write_networks(back, 0, dup, kept, nk, w, sizeof(w));
        check(k > 0 && !netcfg_reads_back(w, (u_int)k, back, 0, scratch, NULL),
              "forgetting it would bring the second back: the check says no");
        set_net(&back[0], "Cafe", NETCFG_SEC_WPA2, 0x40, false);   /* saving Cafe anew: first, the second still second */
        k = netcfg_write_networks(back, 1, dup, kept, nk, w, sizeof(w));
        check(k > 0 && netcfg_reads_back(w, (u_int)k, back, 1, scratch, &i) && i == 1, "saving it again: it reads back");
        back[0] = keep;
        n = read_nets("network = \"Home\"\nsecurity = wpa\npassword = \"my real password\"\n");
        k = netcfg_write_networks(back, 0, "network = \"Home\"\nsecurity = wpa\npassword = \"my real password\"\n",
                                  kept, nkept, w, sizeof(w));
        check(k > 0 && !netcfg_reads_back(w, (u_int)k, back, 0, scratch, NULL),
              "a file that would keep a password does not pass the check");
    }
    check(netcfg_write_networks(nets, 4, NULL, NULL, 0, text, 200) < 0, "a buffer too small: -1");
}

/**** leases ****/

static void test_leases(void) {
    const u_int hz = 100;
    const u_long t0 = 1000;
    check(netcfg_lease_step(0, t0, 0, t0 + 1000000, hz) == NETCFG_LEASE_WAIT, "no lease: nothing to do");
    check(netcfg_lease_step(86400, t0, 0, t0 + 43199 * hz, hz) == NETCFG_LEASE_WAIT, "before half the time: wait");
    check(netcfg_lease_step(86400, t0, 0, t0 + 43200 * hz, hz) == NETCFG_LEASE_RENEW, "at half the time: renew");
    check(netcfg_lease_step(86400, t0, t0 + 43200 * hz, t0 + 43260 * hz, hz) == NETCFG_LEASE_WAIT,
          "a minute after a try with 9 hours to the next stage: wait");
    check(netcfg_lease_step(86400, t0, t0 + 43200 * hz, t0 + (43200 + 16200) * hz, hz) == NETCFG_LEASE_RENEW,
          "half the time left to the next stage after the try: renew again");
    check(netcfg_lease_step(86400, t0, t0 + 43200 * hz, t0 + 75600 * hz, hz) == NETCFG_LEASE_REBIND,
          "at seven eighths: rebind");
    check(netcfg_lease_step(86400, t0, t0 + 86000 * hz, t0 + 86199 * hz, hz) == NETCFG_LEASE_WAIT &&
          netcfg_lease_step(86400, t0, t0 + 86000 * hz, t0 + 86200 * hz, hz) == NETCFG_LEASE_REBIND,
          "a try 400 s before the end: the next one half of that later");
    check(netcfg_lease_step(86400, t0, t0 + 86300 * hz, t0 + 86359 * hz, hz) == NETCFG_LEASE_WAIT &&
          netcfg_lease_step(86400, t0, t0 + 86300 * hz, t0 + 86360 * hz, hz) == NETCFG_LEASE_REBIND,
          "near the end: a minute between tries at least");
    check(netcfg_lease_step(86400, t0, 0, t0 + 86400 * hz, hz) == NETCFG_LEASE_EXPIRED, "at the end: expired");
    check(netcfg_lease_step(20, t0, 0, t0 + 10 * hz, hz) == NETCFG_LEASE_RENEW &&
          netcfg_lease_step(20, t0, t0 + 10 * hz, t0 + 12 * hz, hz) == NETCFG_LEASE_WAIT &&
          netcfg_lease_step(20, t0, t0 + 10 * hz, t0 + 13 * hz, hz) == NETCFG_LEASE_WAIT &&
          netcfg_lease_step(20, t0, t0 + 10 * hz, t0 + 14 * hz, hz) == NETCFG_LEASE_RENEW &&
          netcfg_lease_step(20, t0, t0 + 14 * hz, t0 + 18 * hz, hz) == NETCFG_LEASE_REBIND &&
          netcfg_lease_step(20, t0, t0 + 18 * hz, t0 + 19 * hz, hz) == NETCFG_LEASE_WAIT &&
          netcfg_lease_step(20, t0, 0, t0 + 20 * hz, hz) == NETCFG_LEASE_EXPIRED,
          "a 20-second lease (dhcplease=20): renew at 10 s, again 4 s later, rebind at 18 s at once, gone at 20");
    check(netcfg_lease_step(86400, t0, t0 - 5, t0 + 43200 * hz, hz) == NETCFG_LEASE_RENEW,
          "a try from before the lease was granted does not count");
    if(sizeof(u_long) == 4) {                       /* the kernel's ticks: 32 bits, wrapping after 497 days */
        check(netcfg_lease_step(86400, 0xFFFFFF00UL, 0, 0xFFFFFF00UL + 43200UL * hz, hz) == NETCFG_LEASE_RENEW,
              "the tick count wrapping around");
        check(netcfg_lease_step(86400, 0xFFFFFF00UL, 0xFFFFFF00UL + 43200UL * hz, 0xFFFFFF00UL + 43260UL * hz, hz) ==
              NETCFG_LEASE_WAIT, "a try across the wrap counts");
    } else {
        printf("  (the tick wrap is not tried: u_long is %u bytes on this host, the kernel's is 4)\n",
               (u_int)sizeof(u_long));
    }
    check(netcfg_lease_step(86400, t0, 0, t0 + 50000 * hz, 0) == NETCFG_LEASE_WAIT, "no clock rate: nothing");
}

int main(void) {
    test_keys();
    test_parse();
    test_edit();
    test_quote();
    test_networks();
    test_leases();
    printf("netcfg: %d checks, %d failures\n", checks, failures);
    if(failures) exit(1);
    return 0;
}
