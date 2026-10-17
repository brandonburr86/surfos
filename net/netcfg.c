/*
SurfOS settings: the files on the stick
--------------------
File: netcfg.c  Date: 10/17/26 (the network manager)
--------------------
The text of surfos-config/settings.conf and surfos-config/networks.conf (include/net/netcfg.h),
for kernel/netman.c, which reads and writes them on the stick SurfOS was started from. Pure:
no device, no allocation, nothing kept between calls, so the tests run it on the host
(tests/host/test_netcfg.c).

Both files are lines of "key = value"; # starts a comment, except inside double quotes. Blank
lines and comments mean nothing. A line ends with LF or CR LF (a file from Windows); a line
longer than LINE_MAX bytes is reported and skipped.

settings.conf is the owner's as much as SurfOS's: an edit (netcfg_edit_settings()) changes the
one line that sets the key and leaves every other byte as it was, comments and the keys this
SurfOS does not know included. A new file is the template below, every key explained and
commented out with its default.

networks.conf is SurfOS's own: written anew each time (netcfg_write_networks()), its header
saying so. A network is a "network = <SSID>" line, then its security (open or wpa2), its key
(psk = 64 hex digits: the PMK, PBKDF2 of the password and the SSID) and whether it is hidden.
The password itself is never written. One typed in by hand (password = "...") is read, for the
reader to turn into the key (wpa_passphrase_to_pmk()); the next write keeps only the key.

An SSID is up to 32 bytes of anything, so the file quotes it: "..." with \" and \\, and \xHH
for every byte that is not printable ASCII. A bare name typed by hand is taken as it is.
*/

#include <surfos/types.h>
#include <net/netcfg.h>
#include <blibc_common.h>

#define LINE_MAX   255                              /* the longest line taken */
#define VALUE_MAX  200                              /* the longest value: a quoted SSID is 130 at most */

/**** lines ****/

/* the line at pos of text (len): its bytes [pos, *end) without the line end; the next line's position */
static u_int line_at(const char *text, u_int len, u_int pos, u_int *end) {
    u_int e = pos;
    while(e < len && text[e] != '\n') e++;
    *end = (e > pos && text[e - 1] == '\r') ? e - 1 : e;
    return e < len ? e + 1 : e;
}

static bool blank(char c) {
    return c == ' ' || c == '\t';
}

/* where a comment starts in line (n bytes): the first # outside double quotes (a backslash in quotes takes the
   next byte with it), or n */
static u_int comment_at(const char *line, u_int n) {
    bool quoted = false;
    u_int i;
    for(i = 0; i < n; i++) {
        if(quoted && line[i] == '\\' && i + 1 < n) i++;
        else if(line[i] == '"') quoted = !quoted;
        else if(line[i] == '#' && !quoted) return i;
    }
    return n;
}

/* a line as key and value, both trimmed: 1; nothing on it but blanks and a comment: 0; -1 and *why otherwise */
static int split(const char *line, u_int n, char *key, size_t ksize, char *value, size_t vsize, const char **why) {
    u_int c = comment_at(line, n), a = 0, b, eq;
    while(a < c && blank(line[a])) a++;
    while(c > a && blank(line[c - 1])) c--;
    if(a == c) return 0;
    for(eq = a; eq < c && line[eq] != '='; eq++);
    if(eq == c || eq == a) {
        *why = "not key = value";
        return -1;
    }
    for(b = eq; b > a && blank(line[b - 1]); b--);
    if(b - a >= ksize) {
        *why = "no such key";
        return -1;
    }
    memcpy(key, line + a, b - a);
    key[b - a] = 0;
    for(eq++; eq < c && blank(line[eq]); eq++);
    if(c - eq >= vsize) {
        *why = "the value is too long";
        return -1;
    }
    memcpy(value, line + eq, c - eq);
    value[c - eq] = 0;
    return 1;
}

static int lower(int c) {
    return c >= 'A' && c <= 'Z' ? c + 32 : c;
}

static bool same_word(const char *a, const char *b) {
    while(*a && *b && lower((unsigned char)*a) == lower((unsigned char)*b)) a++, b++;
    return !*a && !*b;
}

/* a settings value without the double quotes around it, if it has them (nothing in them needs escaping) */
static const char *bare(char *value) {
    size_t n = strlen(value);
    if(n >= 2 && value[0] == '"' && value[n - 1] == '"') {
        value[n - 1] = 0;
        return value + 1;
    }
    return value;
}

/**** settings.conf ****/

struct key_info {
    const char *name, *values, *help, *dflt;
};

static const struct key_info keys[NETCFG_NKEYS] = {
    { "telnetd", "on | off", "the telnet server at startup (no password: your own network only)", "off" },
    { "telnetd.port", "1-65535", "the port the telnet server listens on", "23" },
    { "wifi", "on | off", "at startup, join the first saved Wi-Fi network in range, and again when it comes back", "on" },
    { "ethernet", "dhcp | off", "dhcp: an address whenever an Ethernet link comes up; off: the dhcp command", "off" },
    { "hostname", "letters, digits, -", "the name DHCP requests give, which the router lists", "surfos" },
};

static const char template_text[] =
    "# SurfOS settings, read at every start from surfos-config/settings.conf on the stick\n"
    "# SurfOS was started from. The settings command lists and changes them and writes this\n"
    "# file; it may be edited on another computer too. Lines are \"key = value\", # starts a\n"
    "# comment, and a key that is not set has the value shown commented out.\n"
    "\n"
    "# telnetd: start the telnet server at startup, on or off. It has no password and no\n"
    "# encryption: anyone who reaches the laptop over the network gets a shell, so use it on\n"
    "# your own network only. (The development build from the stick starts it anyway.)\n"
    "#telnetd = off\n"
    "\n"
    "# telnetd.port: the port it listens on, 1 to 65535.\n"
    "#telnetd.port = 23\n"
    "\n"
    "# wifi: at startup, start the Wi-Fi card, join the first saved network in range\n"
    "# (networks.conf beside this file: wifi save adds one, wifi saved lists them), get an\n"
    "# address (DHCP), and join again when the network comes back. on or off.\n"
    "#wifi = on\n"
    "\n"
    "# ethernet: dhcp asks for an address whenever an Ethernet adapter's link comes up; off\n"
    "# leaves it to the dhcp command.\n"
    "#ethernet = off\n"
    "\n"
    "# hostname: the name SurfOS gives in its DHCP requests, which the router shows in its\n"
    "# list of devices: letters, digits and -, up to 32.\n"
    "#hostname = surfos\n";

void netcfg_defaults(struct netcfg_settings *s) {
    memset(s, 0, sizeof(*s));
    s->telnetd = false;
    s->telnetd_port = 23;
    s->wifi = true;
    s->ethernet_dhcp = false;
    strlcpy(s->hostname, keys[NETCFG_HOSTNAME].dflt, sizeof(s->hostname));
}

int netcfg_key_find(const char *name) {
    int k;
    for(k = 0; k < NETCFG_NKEYS; k++) if(same_word(name, keys[k].name)) return k;
    return -1;
}

const char *netcfg_key_name(int k) {
    return k >= 0 && k < NETCFG_NKEYS ? keys[k].name : "";
}

const char *netcfg_key_values(int k) {
    return k >= 0 && k < NETCFG_NKEYS ? keys[k].values : "";
}

const char *netcfg_key_help(int k) {
    return k >= 0 && k < NETCFG_NKEYS ? keys[k].help : "";
}

const char *netcfg_default_value(int k, char *buf, size_t size) {
    strlcpy(buf, k >= 0 && k < NETCFG_NKEYS ? keys[k].dflt : "", size);
    return buf;
}

const char *netcfg_value(const struct netcfg_settings *s, int k, char *buf, size_t size) {
    switch(k) {
    case NETCFG_TELNETD:      strlcpy(buf, s->telnetd ? "on" : "off", size); break;
    case NETCFG_TELNETD_PORT: snprintf(buf, size, "%u", (u_int)s->telnetd_port); break;
    case NETCFG_WIFI:         strlcpy(buf, s->wifi ? "on" : "off", size); break;
    case NETCFG_ETHERNET:     strlcpy(buf, s->ethernet_dhcp ? "dhcp" : "off", size); break;
    case NETCFG_HOSTNAME:     strlcpy(buf, s->hostname, size); break;
    default:                  strlcpy(buf, "", size); break;
    }
    return buf;
}

static int on_off(const char *v, bool *out) {
    if(same_word(v, "on") || same_word(v, "yes")) *out = true;
    else if(same_word(v, "off") || same_word(v, "no")) *out = false;
    else return -1;
    return 0;
}

static bool host_name(const char *v) {
    size_t i, n = strlen(v);
    if(n < 1 || n > NETCFG_HOSTNAME_MAX || v[0] == '-' || v[n - 1] == '-') return false;
    for(i = 0; i < n; i++) {
        char c = v[i];
        if(!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-')) return false;
    }
    return true;
}

int netcfg_set(struct netcfg_settings *s, int k, const char *value, char *why, size_t size) {
    bool b;
    u_long n = 0;
    const char *p;
    switch(k) {
    case NETCFG_TELNETD:
    case NETCFG_WIFI:
        if(on_off(value, &b)) break;
        if(k == NETCFG_TELNETD) s->telnetd = b;
        else s->wifi = b;
        s->set |= 1u << k;
        return 0;
    case NETCFG_TELNETD_PORT:
        for(p = value; *p >= '0' && *p <= '9' && n <= 65535; p++) n = n * 10 + (u_long)(*p - '0');
        if(p == value || *p || n < 1 || n > 65535) break;
        s->telnetd_port = (u16)n;
        s->set |= 1u << k;
        return 0;
    case NETCFG_ETHERNET:
        if(same_word(value, "dhcp")) s->ethernet_dhcp = true;
        else if(same_word(value, "off")) s->ethernet_dhcp = false;
        else break;
        s->set |= 1u << k;
        return 0;
    case NETCFG_HOSTNAME:
        if(!host_name(value)) {
            snprintf(why, size, "hostname is letters, digits and - (not first or last), 1 to %u of them, not %s",
                     NETCFG_HOSTNAME_MAX, value);
            return -1;
        }
        strlcpy(s->hostname, value, sizeof(s->hostname));
        s->set |= 1u << k;
        return 0;
    default:
        snprintf(why, size, "no setting called %s", netcfg_key_name(k));
        return -1;
    }
    snprintf(why, size, "%s is %s, not %s", keys[k].name, keys[k].values, value);
    return -1;
}

void netcfg_parse_settings(const char *text, u_int len, struct netcfg_settings *s, netcfg_note_fn note, void *arg) {
    char key[40], value[VALUE_MAX], why[120];
    const char *w;
    u_int pos = 0, end, next, line = 0;
    int r, k;
    netcfg_defaults(s);
    while(text && pos < len) {
        next = line_at(text, len, pos, &end);
        line++;
        if(end - pos > LINE_MAX) {
            if(note) note(arg, line, "the line is too long");
        } else if((r = split(text + pos, end - pos, key, sizeof(key), value, sizeof(value), &w)) < 0) {
            if(note) note(arg, line, w);
        } else if(r > 0) {
            if((k = netcfg_key_find(key)) < 0) {
                snprintf(why, sizeof(why), "no setting called %s (it is left as it is)", key);
                if(note) note(arg, line, why);
            } else if(netcfg_set(s, k, bare(value), why, sizeof(why))) {
                if(note) note(arg, line, why);
            }
        }
        pos = next;
    }
}

/* does line (n bytes) set key k: 1 as a setting, 2 commented out ("#key = value" with nothing before the # and a
   value the key takes: "# telnetd = off is the default" is a comment that mentions it), 0 not */
static int sets_key(const char *line, u_int n, int k) {
    struct netcfg_settings scratch;
    char key[40], value[VALUE_MAX], why[120];
    const char *w;
    u_int a = 0;
    if(split(line, n, key, sizeof(key), value, sizeof(value), &w) > 0) return same_word(key, keys[k].name) ? 1 : 0;
    while(a < n && blank(line[a])) a++;
    if(a < n && line[a] == '#') {
        a++;
        while(a < n && blank(line[a])) a++;
        if(split(line + a, n - a, key, sizeof(key), value, sizeof(value), &w) > 0 && same_word(key, keys[k].name) &&
           !netcfg_set(&scratch, k, bare(value), why, sizeof(why)))
            return 2;
    }
    return 0;
}

/* out (size) gets n more bytes at *at; false when they do not fit (with the NUL) */
static bool put(char *out, u_int size, u_int *at, const char *s, u_int n) {
    if(*at + n + 1 > size) return false;
    memcpy(out + *at, s, n);
    *at += n;
    out[*at] = 0;
    return true;
}

/* "key = value", the comment that ended the old line (from its #) after it */
static bool put_setting(char *out, u_int size, u_int *at, int k, const char *value, const char *line, u_int n) {
    u_int c = n ? comment_at(line, n) : 0;
    if(!put(out, size, at, keys[k].name, strlen(keys[k].name)) || !put(out, size, at, " = ", 3) ||
       !put(out, size, at, value, strlen(value)))
        return false;
    if(n && c < n && sets_key(line, n, k) == 1)
        return put(out, size, at, "   ", 3) && put(out, size, at, line + c, n - c);
    return true;
}

int netcfg_edit_settings(const char *text, u_int len, int k, const char *given, char *out, u_int size) {
    struct netcfg_settings check;
    char why[120], value[NETCFG_HOSTNAME_MAX + 8];
    u_int pos, end, next, at = 0, active = 0, commented = 0, found_active = (u_int)-1, found_commented = (u_int)-1;
    int r;
    if(!size) return -1;
    out[0] = 0;
    netcfg_defaults(&check);
    if(k < 0 || k >= NETCFG_NKEYS) {
        strlcpy(out, "no such setting", size);
        return -2;
    }
    if(netcfg_set(&check, k, given, why, sizeof(why))) {
        strlcpy(out, why, size);
        return -2;
    }
    netcfg_value(&check, k, value, sizeof(value));     /* as the file writes it: yes is on, DHCP is dhcp */
    if(!text) {
        text = template_text;
        len = sizeof(template_text) - 1;
    }
    for(pos = 0; pos < len; pos = next) {                 /* where the setting goes */
        next = line_at(text, len, pos, &end);
        if(end - pos > LINE_MAX) continue;
        r = sets_key(text + pos, end - pos, k);
        if(r == 1 && !active++) found_active = pos;
        if(r == 2 && !commented++) found_commented = pos;
    }
    for(pos = 0; pos < len; pos = next) {
        u_int n;
        next = line_at(text, len, pos, &end);
        n = end - pos;
        r = n > LINE_MAX ? 0 : sets_key(text + pos, n, k);
        if(pos == found_active || (!active && pos == found_commented)) {
            if(!put_setting(out, size, &at, k, value, text + pos, n) || !put(out, size, &at, text + end, next - end))
                return -1;
            if(next == end && !put(out, size, &at, "\n", 1)) return -1;     /* the last line had no line end */
            continue;
        }
        if(r == 1) continue;                               /* another line setting it: gone */
        if(!put(out, size, &at, text + pos, next - pos)) return -1;
    }
    if(!active && !commented) {
        if(at && out[at - 1] != '\n' && !put(out, size, &at, "\n", 1)) return -1;
        if(!put_setting(out, size, &at, k, value, NULL, 0) || !put(out, size, &at, "\n", 1)) return -1;
    }
    return (int)at;
}

/**** networks.conf ****/

static const char networks_header[] =
    "# Wi-Fi networks SurfOS joins by itself: surfos-config/networks.conf on the stick SurfOS\n"
    "# was started from. At startup, and when a network is lost for good, SurfOS joins the\n"
    "# first of these in range and gets an address (DHCP). wifi save adds the network wlan0\n"
    "# is on (at the top), wifi forget <name> removes one, wifi saved lists them. SurfOS\n"
    "# writes this file anew whenever it changes it: comments added here are not kept.\n"
    "#\n"
    "# psk is the network's key, made from its password and its name (WPA2-Personal). The\n"
    "# password itself is not stored, but whoever has this file can join the network with\n"
    "# it: keep the stick safe. A network can be added here by hand with its password:\n"
    "#   network = \"Name\"\n"
    "#   security = wpa2\n"
    "#   password = \"its password\"\n"
    "# SurfOS makes the key from it when it reads the file, and keeps only the key the next\n"
    "# time it writes the file. An open network is security = open, with no key.\n";

static int hexval(char c) {
    if(c >= '0' && c <= '9') return c - '0';
    if(c >= 'a' && c <= 'f') return c - 'a' + 10;
    if(c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

int netcfg_quote(const u8 *ssid, u_int len, char *out, size_t size) {
    static const char hex[] = "0123456789abcdef";
    size_t n = 0;
    u_int i;
    if(size < 3) return -1;
    out[n++] = '"';
    for(i = 0; i < len; i++) {
        u8 b = ssid[i];
        if(b == '"' || b == '\\') {
            if(n + 2 + 2 > size) return -1;
            out[n++] = '\\';
            out[n++] = (char)b;
        } else if(b >= 0x20 && b < 0x7f) {
            if(n + 1 + 2 > size) return -1;
            out[n++] = (char)b;
        } else {
            if(n + 4 + 2 > size) return -1;
            out[n++] = '\\';
            out[n++] = 'x';
            out[n++] = hex[b >> 4];
            out[n++] = hex[b & 15];
        }
    }
    out[n++] = '"';
    out[n] = 0;
    return (int)n;
}

int netcfg_unquote(const char *in, u8 *out, u_int max, u_int *len) {
    const char *p;
    u_int n = 0;
    if(in[0] != '"') {                                  /* bare, as typed by hand */
        n = strlen(in);
        if(n > max) return -1;
        memcpy(out, in, n);
        *len = n;
        return 0;
    }
    for(p = in + 1; *p && *p != '"'; p++) {
        int b = (unsigned char)*p;
        if(b == '\\') {
            p++;
            if(*p == 'x' || *p == 'X') {
                int h = hexval(p[1]), l = h >= 0 ? hexval(p[2]) : -1;
                if(l < 0) return -1;
                b = h * 16 + l;
                p += 2;
            } else if(*p == '"' || *p == '\\') {
                b = (unsigned char)*p;
            } else {
                return -1;
            }
        }
        if(n >= max) return -1;
        out[n++] = (u8)b;
    }
    if(*p != '"' || p[1]) return -1;                    /* no closing quote, or something after it */
    *len = n;
    return 0;
}

int netcfg_find_net(const struct netcfg_net *nets, int n, const u8 *ssid, u_int len) {
    int i;
    for(i = 0; i < n; i++) if(nets[i].ssid_len == len && !memcmp(nets[i].ssid, ssid, len)) return i;
    return -1;
}

/* the state of the network being read: its line and what it said */
struct reading {
    struct netcfg_net net;
    u_int line;
    bool open, bad, said_security;
};

static void wipe_net(struct netcfg_net *net) {
    volatile u8 *p = (volatile u8 *)net;
    size_t i;
    for(i = 0; i < sizeof(*net); i++) p[i] = 0;
}

/* the network read so far, checked: into nets[*n] */
static void finish(struct reading *rd, struct netcfg_net *nets, int max, int *n, netcfg_note_fn note, void *arg) {
    struct netcfg_net *net = &rd->net;
    const char *why = NULL;
    if(!rd->open) return;
    rd->open = false;
    if(!rd->said_security) net->security = (net->has_psk || net->has_password) ? NETCFG_SEC_WPA2 : NETCFG_SEC_OPEN;
    if(rd->bad) why = NULL;                             /* said already, line by line */
    else if(!net->ssid_len) why = "a network with no name: left out";
    else if(net->security == NETCFG_SEC_WPA2 && !net->has_psk && !net->has_password)
        why = "a WPA2 network needs its psk (or password): left out";
    else if(net->security == NETCFG_SEC_OPEN && (net->has_psk || net->has_password))
        why = "an open network has no key or password: left out (security = wpa2?)";
    else if(netcfg_find_net(nets, *n, net->ssid, net->ssid_len) >= 0) why = "that network was named before: left out";
    else if(*n >= max) why = "too many networks: left out";
    if(!rd->bad && !why) {
        if(net->has_psk) net->has_password = false;     /* the key wins: the password would only make it again */
        nets[(*n)++] = *net;
    } else if(why && note) {
        note(arg, rd->line, why);
    }
    wipe_net(net);
}

int netcfg_parse_networks(const char *text, u_int len, struct netcfg_net *nets, int max, netcfg_note_fn note, void *arg) {
    struct reading rd;
    char key[40], value[VALUE_MAX];
    const char *w;
    u_int pos = 0, end, next, line = 0, l, i;
    int n = 0, r;
    memset(&rd, 0, sizeof(rd));
    while(text && pos < len) {
        next = line_at(text, len, pos, &end);
        line++;
        if(end - pos > LINE_MAX) {
            if(note) note(arg, line, "the line is too long");
            if(rd.open) rd.bad = true;
            pos = next;
            continue;
        }
        r = split(text + pos, end - pos, key, sizeof(key), value, sizeof(value), &w);
        pos = next;
        if(r == 0) continue;
        if(r < 0) {
            if(note) note(arg, line, w);
            if(rd.open) rd.bad = true;
            continue;
        }
        if(same_word(key, "network")) {
            finish(&rd, nets, max, &n, note, arg);
            memset(&rd, 0, sizeof(rd));
            rd.open = true;
            rd.line = line;
            if(netcfg_unquote(value, rd.net.ssid, NETCFG_SSID_MAX, &l) || !l) {
                if(note) note(arg, line, "the network's name is not 1 to 32 bytes in double quotes: left out");
                rd.bad = true;
            } else {
                rd.net.ssid_len = (u8)l;
            }
            continue;
        }
        if(!rd.open) {
            if(note) note(arg, line, "a setting before the first network = line");
            continue;
        }
        if(same_word(key, "security")) {
            rd.said_security = true;
            if(same_word(value, "wpa2")) rd.net.security = NETCFG_SEC_WPA2;
            else if(same_word(value, "open")) rd.net.security = NETCFG_SEC_OPEN;
            else {
                if(note) note(arg, line, "security is open or wpa2: the network is left out");
                rd.bad = true;
            }
        } else if(same_word(key, "psk")) {
            bool ok = strlen(value) == 2 * NETCFG_KEY_LEN;
            for(i = 0; ok && i < NETCFG_KEY_LEN; i++) {
                int h = hexval(value[2 * i]), lo = hexval(value[2 * i + 1]);
                if(h < 0 || lo < 0) ok = false;
                else rd.net.psk[i] = (u8)(h * 16 + lo);
            }
            if(ok) rd.net.has_psk = true;
            else {
                if(note) note(arg, line, "psk is 64 hexadecimal digits: the network is left out");
                rd.bad = true;
            }
        } else if(same_word(key, "password")) {
            u8 pw[64];
            bool ok = !netcfg_unquote(value, pw, sizeof(pw) - 1, &l) && l >= 8 && l <= 63;
            for(i = 0; ok && i < l; i++) if(pw[i] < 0x20 || pw[i] > 0x7e) ok = false;
            if(ok) {
                memcpy(rd.net.password, pw, l);
                rd.net.password[l] = 0;
                rd.net.has_password = true;
            } else {
                if(note) note(arg, line, "a WPA2 password is 8 to 63 characters (letters, digits, spaces, punctuation), "
                                         "in double quotes: the network is left out");
                rd.bad = true;
            }
            for(i = 0; i < sizeof(pw); i++) ((volatile u8 *)pw)[i] = 0;
        } else if(same_word(key, "hidden")) {
            bool h;
            if(on_off(value, &h) == 0) rd.net.hidden = h;
            else if(note) note(arg, line, "hidden is yes or no");
        } else if(note) {
            note(arg, line, "no such network setting (left out)");
        }
    }
    finish(&rd, nets, max, &n, note, arg);
    for(i = 0; i < sizeof(value); i++) ((volatile char *)value)[i] = 0;   /* it held a key or a password */
    return n;
}

int netcfg_write_networks(const struct netcfg_net *nets, int n, char *out, u_int size) {
    static const char hex[] = "0123456789abcdef";
    char q[4 * NETCFG_SSID_MAX + 4], psk[2 * NETCFG_KEY_LEN + 1];
    u_int at = 0;
    int i, j;
    if(!size) return -1;
    out[0] = 0;
    if(!put(out, size, &at, networks_header, sizeof(networks_header) - 1)) return -1;
    for(i = 0; i < n; i++) {
        const struct netcfg_net *net = &nets[i];
        if(!net->ssid_len || (net->security == NETCFG_SEC_WPA2 && !net->has_psk)) return -1;   /* never unreadable */
        if(netcfg_quote(net->ssid, net->ssid_len, q, sizeof(q)) < 0) return -1;
        if(!put(out, size, &at, "\nnetwork = ", 11) || !put(out, size, &at, q, strlen(q)) || !put(out, size, &at, "\n", 1))
            return -1;
        if(net->security == NETCFG_SEC_WPA2) {
            for(j = 0; j < NETCFG_KEY_LEN; j++) {
                psk[2 * j] = hex[net->psk[j] >> 4];
                psk[2 * j + 1] = hex[net->psk[j] & 15];
            }
            psk[2 * NETCFG_KEY_LEN] = 0;
            if(!put(out, size, &at, "security = wpa2\npsk = ", 22) || !put(out, size, &at, psk, 2 * NETCFG_KEY_LEN) ||
               !put(out, size, &at, "\n", 1))
                return -1;
        } else if(!put(out, size, &at, "security = open\n", 16)) {
            return -1;
        }
        if(net->hidden && !put(out, size, &at, "hidden = yes\n", 13)) return -1;
    }
    for(j = 0; j < (int)sizeof(psk); j++) ((volatile char *)psk)[j] = 0;
    return (int)at;
}

/**** DHCP leases ****/

/* the wait after a try is half the time the try had left before the next stage (T2, or the end), at least
   min(60 s, lease / 8) and 1 s (RFC 2131 4.4.5); reaching T2 starts rebinding at once */
int netcfg_lease_step(u32 lease_s, u_long granted, u_long last_try, u_long now, u_int hz) {
    u32 t1 = lease_s / 2, t2 = lease_s - lease_s / 8, elapsed, tried, stage_end, wait, least;
    if(!lease_s || !hz) return NETCFG_LEASE_WAIT;
    elapsed = (u32)((now - granted) / hz);
    if(elapsed >= lease_s) return NETCFG_LEASE_EXPIRED;
    if(elapsed < t1) return NETCFG_LEASE_WAIT;
    if(last_try && (long)(last_try - granted) >= 0 && (long)(now - last_try) >= 0) {
        tried = (u32)((last_try - granted) / hz);
        if(!(tried < t2 && elapsed >= t2)) {            /* still the stage it was tried in */
            stage_end = tried < t2 ? t2 : lease_s;
            least = lease_s / 8 < 60 ? lease_s / 8 : 60;
            if(least < 1) least = 1;
            wait = stage_end > tried ? (stage_end - tried) / 2 : 0;
            if(wait < least) wait = least;
            if((now - last_try) / hz < wait) return NETCFG_LEASE_WAIT;
        }
    }
    return elapsed < t2 ? NETCFG_LEASE_RENEW : NETCFG_LEASE_REBIND;
}
