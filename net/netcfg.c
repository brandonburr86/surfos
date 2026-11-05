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
lines and comments mean nothing. A line ends with LF, CR LF (a file from Windows) or a CR alone
(an old Mac's); a UTF-8 byte order mark at the start is skipped. A line longer than LINE_MAX
bytes, or one with a NUL byte in it, is reported: in settings.conf it is skipped, in
networks.conf it leaves out the network it is in. TextEdit's smart quotes (the curly “ and ”
it makes of ") count as double quotes, so a file edited there still reads.

settings.conf is the owner's as much as SurfOS's: an edit (netcfg_edit_settings()) changes the
one line that sets the key, its indentation and its comment kept, and leaves every other byte
as it was, comments and the keys this SurfOS does not know included; a line that would grow
past LINE_MAX gets its comment on a line of its own above it. A new file is the template
below, every key explained and commented out with its default.

networks.conf is SurfOS's own: written anew each time (netcfg_write_networks()), its header
saying so. A network is a "network = <SSID>" line, then its security (open or wpa2), its key
(psk = 64 hex digits: the PMK, PBKDF2 of the password and the SSID) and whether it is hidden.
The password itself is never written. One typed in by hand (password = "...") is read, for the
reader to turn into the key (wpa_passphrase_to_pmk()); the next write keeps only the key. A
block the reader cannot take (a bad key, a security SurfOS does not speak, a name seen before,
a line it does not understand where no security line says it is open) is told and left out of
the networks, but kept: its bytes are handed back (struct netcfg_span) and the writer copies
them, as they were, under a comment after the networks (lines before the first network go
before them), so a save never loses a network written by hand, even one SurfOS cannot read. A
key it does not know, or a hidden value it cannot read, in a network it takes is only told:
that network is written anew without it. A kept part with a password line in it is marked,
since SurfOS never writes a password (kernel/netman.c then refuses to change the file), and
netcfg_reads_back() checks that a file about to be written reads as the networks meant.

10/25/26 (track A, A10): settings.conf's history key, surfsh's history kept on the stick; surfsh's own files
beside these two (surfshrc, history) are only read and written as they are (kernel/netman.c).

An SSID is up to 32 bytes of anything, so the file quotes it: "..." with \" and \\, and \xHH
for every byte that is not printable ASCII. A bare name typed by hand is taken as it is.
*/

#include <surfos/types.h>
#include <net/netcfg.h>
#include <blibc_common.h>

#define LINE_MAX   255                              /* the longest line taken */
#define VALUE_MAX  200                              /* the longest value: a quoted SSID is 130 at most */
#define WHY_MAX    200

/**** lines ****/

/* a UTF-8 byte order mark, which some editors put first: skipped by the readers, kept by an edit */
static u_int bom_len(const char *text, u_int len) {
    return text && len >= 3 && (u8)text[0] == 0xEF && (u8)text[1] == 0xBB && (u8)text[2] == 0xBF ? 3 : 0;
}

/* the line at pos of text (len): its bytes [pos, *end) without the line end (LF, CR LF, or a CR alone); the next
   line's position */
static u_int line_at(const char *text, u_int len, u_int pos, u_int *end) {
    u_int e = pos;
    while(e < len && text[e] != '\n' && text[e] != '\r') e++;
    *end = e;
    if(e + 1 < len && text[e] == '\r' && text[e + 1] == '\n') return e + 2;
    return e < len ? e + 1 : e;
}

static bool blank(char c) {
    return c == ' ' || c == '\t';
}

/* a curly double quote at s (n bytes left): the opening U+201C or the closing U+201D, in UTF-8 */
static bool curly(const char *s, u_int n, bool open) {
    return n >= 3 && (u8)s[0] == 0xE2 && (u8)s[1] == 0x80 && (u8)s[2] == (open ? 0x9C : 0x9D);
}

/* where a comment starts in line (n bytes): the first # outside quotes ("...", or the curly “...”; a backslash in
   quotes takes the next byte with it), or n */
static u_int comment_at(const char *line, u_int n) {
    int q = 0;                                      /* 1: in "...", 2: in “...” */
    u_int i;
    for(i = 0; i < n; i++) {
        if(q && line[i] == '\\' && i + 1 < n) i++;
        else if(q == 0 && line[i] == '"') q = 1;
        else if(q == 1 && line[i] == '"') q = 0;
        else if(q == 0 && curly(line + i, n - i, true)) { q = 2; i += 2; }
        else if(q == 2 && curly(line + i, n - i, false)) { q = 0; i += 2; }
        else if(q == 0 && line[i] == '#') return i;
    }
    return n;
}

/* a line as key and value, both trimmed: 1; nothing on it but blanks and a comment: 0; -1 and *why otherwise */
static int split(const char *line, u_int n, char *key, size_t ksize, char *value, size_t vsize, const char **why) {
    u_int c, a = 0, b, eq;
    if(memchr(line, 0, n)) {
        *why = "the line has a NUL byte in it";
        return -1;
    }
    c = comment_at(line, n);
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

/* a value without the double quotes (straight or curly) around it, if it has them; nothing in them needs escaping */
static const char *bare(char *value) {
    size_t n = strlen(value);
    if(n >= 2 && value[0] == '"' && value[n - 1] == '"') {
        value[n - 1] = 0;
        return value + 1;
    }
    if(n >= 6 && curly(value, (u_int)n, true) && curly(value + n - 3, 3, false)) {
        value[n - 3] = 0;
        return value + 3;
    }
    return value;
}

/**** settings.conf ****/

struct key_info {
    const char *name, *values, *help, *dflt;
};

static const struct key_info keys[NETCFG_NKEYS] = {
    { "telnetd", "on | off", "the telnet server at startup, on Ethernet or a WPA2 network (no password: your own networks only)",
      "off" },
    { "telnetd.port", "1-65535", "the port the telnet server listens on", "23" },
    { "wifi", "on | off", "at startup, join the first saved Wi-Fi network in range, and again when it comes back", "on" },
    { "ethernet", "dhcp | off", "dhcp: an address whenever an Ethernet link comes up; off: the dhcp command", "off" },
    { "hostname", "letters, digits, -", "the name DHCP requests give, which the router lists", "surfos" },
    { "history", "on | off", "surfsh keeps the lines typed at its prompts in surfos-config/history (plain text) and "
      "recalls them at its next start", "off" },
    /* 11/05/26 (the desktop applications, track desk): the desktop's preferences, which Pointer & desktop and Settings
       change */
    { "desktop.theme", "ocean | dusk", "the desktop's colours", "ocean" },
    { "pointer.speed", "slow | normal | fast", "how far the desktop's pointer goes for a move of the mouse", "slow" },
};

static const char template_text[] =
    "# SurfOS settings, read at every start from surfos-config/settings.conf on the stick\n"
    "# SurfOS was started from. The settings command lists and changes them and writes this\n"
    "# file; it may be edited on another computer too. Lines are \"key = value\", # starts a\n"
    "# comment, and a key that is not set has the value shown commented out.\n"
    "\n"
    "# telnetd: start the telnet server at startup, on or off. It has no password and no\n"
    "# encryption: whoever reaches the laptop over a network it is on gets a shell. It starts\n"
    "# once SurfOS has an address on Ethernet or on a WPA2 Wi-Fi network (never because of an\n"
    "# open one), and then answers on every network SurfOS is on; meanwhile SurfOS joins no\n"
    "# open network by itself. The development build's autonet starts it anyway, for an\n"
    "# Ethernet link.\n"
    "#telnetd = off\n"
    "\n"
    "# telnetd.port: the port it listens on, 1 to 65535 (the development build's autonet\n"
    "# starts its own on 23).\n"
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
    "# list of devices: letters, digits and - (not first or last), up to 32.\n"
    "#hostname = surfos\n"
    "\n"
    "# history: surfsh, the SurfOS shell, keeps the last 1,000 lines typed at its prompts in\n"
    "# surfos-config/history beside this file and recalls them at its next start (Up, Ctrl+R).\n"
    "# It is plain text: a password typed on a command line would be in it (a line that starts\n"
    "# with a space is never kept). on or off.\n"
    "#history = off\n"
    "\n"
    "# desktop.theme: the desktop's colours, ocean or dusk (gui: Pointer & desktop, or Settings).\n"
    "#desktop.theme = ocean\n"
    "\n"
    "# pointer.speed: how far the desktop's pointer goes for a move of the mouse: slow, normal\n"
    "# or fast.\n"
    "#pointer.speed = slow\n";

void netcfg_defaults(struct netcfg_settings *s) {
    memset(s, 0, sizeof(*s));
    s->telnetd = false;
    s->telnetd_port = 23;
    s->wifi = true;
    s->ethernet_dhcp = false;
    strlcpy(s->hostname, keys[NETCFG_HOSTNAME].dflt, sizeof(s->hostname));
    s->history = false;
    s->theme = NETCFG_THEME_OCEAN;                  /* 11/05/26 */
    s->pointer = 1;
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
    case NETCFG_HISTORY:      strlcpy(buf, s->history ? "on" : "off", size); break;
    case NETCFG_THEME:        strlcpy(buf, s->theme == NETCFG_THEME_DUSK ? "dusk" : "ocean", size); break;   /* 11/05/26 */
    case NETCFG_POINTER:      strlcpy(buf, s->pointer == 3 ? "fast" : s->pointer == 2 ? "normal" : "slow", size); break;
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
    case NETCFG_HISTORY:
        if(on_off(value, &b)) break;
        if(k == NETCFG_TELNETD) s->telnetd = b;
        else if(k == NETCFG_WIFI) s->wifi = b;
        else s->history = b;
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
    case NETCFG_THEME:                                  /* 11/05/26 */
        if(same_word(value, "ocean")) s->theme = NETCFG_THEME_OCEAN;
        else if(same_word(value, "dusk")) s->theme = NETCFG_THEME_DUSK;
        else break;
        s->set |= 1u << k;
        return 0;
    case NETCFG_POINTER:
        if(same_word(value, "slow")) s->pointer = 1;
        else if(same_word(value, "normal")) s->pointer = 2;
        else if(same_word(value, "fast")) s->pointer = 3;
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
    char key[40], value[VALUE_MAX], why[WHY_MAX];
    const char *w;
    u_int pos = bom_len(text, len), end, next, line = 0;
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

/* where a line setting key k has its parts: the blanks it is indented by, and its comment (from the blanks before the
   #) to the end of the line, n when it has none */
struct found {
    u_int indent, tail, n;
};

/* does line (n bytes) set key k: 1 as a setting, 2 commented out ("#key = value" with nothing before the # and a
   value the key takes: "# telnetd = off is the default" is a comment that mentions it), 0 not; where its parts are */
static int sets_key(const char *line, u_int n, int k, struct found *f) {
    struct netcfg_settings scratch;
    char key[40], value[VALUE_MAX], why[WHY_MAX];
    const char *w;
    u_int a = 0, start = 0, c, e;
    int kind;
    if(n > LINE_MAX) return 0;
    while(a < n && blank(line[a])) a++;
    if(split(line, n, key, sizeof(key), value, sizeof(value), &w) > 0) {
        if(!same_word(key, keys[k].name)) return 0;
        kind = 1;
    } else {
        if(a >= n || line[a] != '#') return 0;
        for(start = a + 1; start < n && blank(line[start]); start++);
        if(split(line + start, n - start, key, sizeof(key), value, sizeof(value), &w) <= 0 ||
           !same_word(key, keys[k].name) || netcfg_set(&scratch, k, bare(value), why, sizeof(why)))
            return 0;
        kind = 2;
    }
    f->indent = a;
    f->n = n;
    c = start + comment_at(line + start, n - start);   /* the comment of the setting itself */
    for(e = c; e > start && blank(line[e - 1]); e--);
    f->tail = c < n ? e : n;
    return kind;
}

/* out (size) gets n more bytes at *at; false when they do not fit (with the NUL) */
static bool put(char *out, u_int size, u_int *at, const char *s, u_int n) {
    if(*at + n + 1 > size) return false;
    memcpy(out + *at, s, n);
    *at += n;
    out[*at] = 0;
    return true;
}

/* indent, "key = value", then the comment that ended the old line (line, f: none when line is NULL); when that would
   pass LINE_MAX, the comment goes on a line of its own above (eol after it) and the setting has neither */
static bool put_setting(char *out, u_int size, u_int *at, int k, const char *value, const char *line,
                        const struct found *f, const char *eol, u_int eol_len) {
    u_int ind = line ? f->indent : 0, tail = line ? f->n - f->tail : 0;
    u_int base = (u_int)(strlen(keys[k].name) + 3 + strlen(value));
    if(ind + base + tail > LINE_MAX) {
        if(tail) {
            u_int c = f->tail;
            while(c < f->n && blank(line[c])) c++;      /* from its # */
            if(!put(out, size, at, line + c, f->n - c) || !put(out, size, at, eol, eol_len)) return false;
        }
        ind = tail = 0;
    }
    return put(out, size, at, line, ind) && put(out, size, at, keys[k].name, (u_int)strlen(keys[k].name)) &&
           put(out, size, at, " = ", 3) && put(out, size, at, value, (u_int)strlen(value)) &&
           (!tail || put(out, size, at, line + f->tail, tail));
}

/* the line end text uses (its first one): LF, CR LF or CR; LF when it has none */
static const char *eol_of(const char *text, u_int len, u_int *n) {
    u_int i;
    for(i = 0; i < len; i++) {
        if(text[i] == '\n') break;
        if(text[i] == '\r') {
            *n = i + 1 < len && text[i + 1] == '\n' ? 2 : 1;
            return *n == 2 ? "\r\n" : "\r";
        }
    }
    *n = 1;
    return "\n";
}

int netcfg_edit_settings(const char *text, u_int len, int k, const char *given, char *out, u_int size) {
    struct netcfg_settings check;
    struct found f;
    char why[WHY_MAX], value[NETCFG_HOSTNAME_MAX + 8];
    u_int pos, end, next, at = 0, active = 0, commented = 0, found_active = (u_int)-1, found_commented = (u_int)-1, first;
    u_int eol_len;
    const char *eol;
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
    first = bom_len(text, len);
    eol = eol_of(text, len, &eol_len);                  /* a line added or ended here ends as the file's lines do */
    if(!put(out, size, &at, text, first)) return -1;
    for(pos = first; pos < len; pos = next) {           /* where the setting goes */
        next = line_at(text, len, pos, &end);
        r = sets_key(text + pos, end - pos, k, &f);
        if(r == 1 && !active++) found_active = pos;
        if(r == 2 && !commented++) found_commented = pos;
    }
    for(pos = first; pos < len; pos = next) {
        next = line_at(text, len, pos, &end);
        r = sets_key(text + pos, end - pos, k, &f);
        if(pos == found_active || (!active && pos == found_commented)) {
            if(!put_setting(out, size, &at, k, value, text + pos, &f, next > end ? text + end : eol,
                            next > end ? next - end : eol_len) || !put(out, size, &at, text + end, next - end))
                return -1;
            if(next == end && !put(out, size, &at, eol, eol_len)) return -1;     /* the last line had no line end */
            continue;
        }
        if(r == 1) continue;                               /* another line setting it: gone */
        if(!put(out, size, &at, text + pos, next - pos)) return -1;
    }
    if(!active && !commented) {
        if(at > first && out[at - 1] != '\n' && out[at - 1] != '\r' && !put(out, size, &at, eol, eol_len)) return -1;
        if(!put_setting(out, size, &at, k, value, NULL, NULL, eol, eol_len) || !put(out, size, &at, eol, eol_len))
            return -1;
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
    "# time it writes the file. An open network is security = open, with no key; one whose\n"
    "# beacons do not carry its name has hidden = yes.\n";

static const char kept_header[] =
    "\n# SurfOS did not understand the lines below (it says why at startup): they are\n"
    "# kept as they were. Correct them, or delete them.\n";

static const char loose_header[] =
    "\n# SurfOS did not understand the lines below, up to the first network = line (it\n"
    "# says why at startup): they are kept as they were. Correct them, or delete them.\n";

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
    u_int n = 0, inlen = (u_int)strlen(in);
    bool smart = curly(in, inlen, true);            /* “...”, as TextEdit's smart quotes write "..." */
    if(in[0] != '"' && !smart) {                    /* bare, as typed by hand */
        if(inlen > max) return -1;
        memcpy(out, in, inlen);
        *len = inlen;
        return 0;
    }
    for(p = in + (smart ? 3 : 1); *p; p++) {
        int b = (unsigned char)*p;
        if(smart ? curly(p, (u_int)strlen(p), false) : b == '"') break;
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
    if(!*p || p[smart ? 3 : 1]) return -1;          /* no closing quote, or something after it */
    *len = n;
    return 0;
}

int netcfg_find_net(const struct netcfg_net *nets, int n, const u8 *ssid, u_int len) {
    int i;
    for(i = 0; i < n; i++) if(nets[i].ssid_len == len && !memcmp(nets[i].ssid, ssid, len)) return i;
    return -1;
}

static void wipe(void *p, size_t n) {
    volatile u8 *q = (volatile u8 *)p;
    while(n--) *q++ = 0;
}

/* the state of the network being read: its line and what it said */
struct reading {
    struct netcfg_net net;
    u_int line, start;                  /* its network = line: the number, the byte it starts at */
    bool open, bad, said_security, unknown;
    u_int pw_line;                      /* the first line of the block that may hold a password; 0 */
    char name[4 * NETCFG_SSID_MAX + 4]; /* its name quoted, for the notes */
};

/* a value of hex digits only (a key, or one mistyped: never a password) */
static bool all_hex(const char *v) {
    if(!*v) return false;
    for(; *v; v++) if(hexval(*v) < 0) return false;
    return true;
}

/* may line (n bytes) hold a password: a setting, commented out or not ("# password = ..."), whose key (the first word,
   after blanks and #s) names one (password, passphrase, ...), or a psk that is a passphrase (wpa_supplicant's psk =
   "..." in quotes, or other characters than hex digits: psk = 1234 is a key mistyped)? A line that is no setting counts
   too unless it is a comment (prose: "# the key to the office"). SurfOS never writes such a line back */
static bool secret_line(const char *line, u_int n) {
    static const char *const words[] = { "password", "passphrase", "passwd", "pass", "pwd", "pw", "secret", "key",
                                         "wpa_passphrase", "psk", NULL };
    char word[24], key[40], value[VALUE_MAX];
    const char *w, *v;
    u_int i = 0, k = 0, j;
    bool commented = false, setting, secret;
    int r, m;
    while(i < n && (blank(line[i]) || line[i] == '#')) {
        if(line[i] == '#') commented = true;
        i++;
    }
    while(i + k < n && k + 1 < sizeof(word) && (line[i + k] == '_' || (line[i + k] >= 'a' && line[i + k] <= 'z') ||
          (line[i + k] >= 'A' && line[i + k] <= 'Z'))) {
        word[k] = line[i + k];
        k++;
    }
    word[k] = 0;
    for(m = 0; words[m] && !same_word(word, words[m]); m++);
    if(!words[m]) return false;
    for(j = i + k; j < n && blank(line[j]); j++);
    setting = j < n && line[j] == '=';
    if(!setting) return !commented;                     /* "password hunter22" counts; prose in a comment does not */
    if(!same_word(word, "psk")) return true;
    r = split(line + i, n - i, key, sizeof(key), value, sizeof(value), &w);
    v = value;
    secret = r <= 0 || v[0] == '"' || curly(v, (u_int)strlen(v), true) || !all_hex(v);
    wipe(value, sizeof(value));
    return secret;
}

/* a note about the network being read: why, and that it is left out (and kept as it was, when kept is asked for) */
static void net_note(const struct reading *rd, u_int line, const char *why, bool keeping, netcfg_note_fn note, void *arg) {
    char msg[WHY_MAX + 4 * NETCFG_SSID_MAX + 40], where[24];
    if(!note) return;
    snprintf(where, sizeof(where), "of line %u", rd->line);
    snprintf(msg, sizeof(msg), "%s: the network %s is left out%s", why, rd->name[0] ? rd->name : where,
             keeping ? " (and kept in the file as it is)" : "");
    note(arg, line, msg);
}

/* bytes [start, end) of the text, from line on, go into kept[] (a block, or a loose line before the first network, the
   reader could not take; pw_line: its first line that may hold a password, 0), one span with the last when they follow
   it; false when full */
static bool keep_span(struct netcfg_span *kept, int *nkept, u_int start, u_int end, bool loose, u_int line,
                      u_int pw_line) {
    struct netcfg_span *last = kept && *nkept > 0 ? &kept[*nkept - 1] : NULL;
    if(!kept || start >= end) return true;
    if(last && last->loose == loose && last->end == start) {
        last->end = end;
        if(pw_line && !last->pw_line) last->pw_line = pw_line;
        last->password = last->pw_line != 0;
        return true;
    }
    if(*nkept >= NETCFG_SPANS_MAX) return false;
    kept[*nkept].start = start;
    kept[*nkept].end = end;
    kept[*nkept].line = line;
    kept[*nkept].loose = loose;
    kept[*nkept].pw_line = pw_line;
    kept[*nkept].password = pw_line != 0;
    (*nkept)++;
    return true;
}

/* a loose line before the first network the reader could not take: told (why) and kept, when there is room */
static void keep_loose(struct netcfg_span *kept, int *nkept, u_int start, u_int end, u_int line, bool password,
                       const char *why, netcfg_note_fn note, void *arg) {
    char msg[WHY_MAX + 80];
    bool room = keep_span(kept, nkept, start, end, true, line, password ? line : 0);
    snprintf(msg, sizeof(msg), "%s (%s)", why, !kept ? "left out" : room ? "kept as it is" :
             "not kept: the file has too many parts SurfOS does not understand");
    if(note) note(arg, line, msg);
}

/* the network read so far, which ends at byte end, checked: into nets[*n], or its bytes into kept[] */
static void finish(struct reading *rd, u_int end, struct netcfg_net *nets, int max, int *n, struct netcfg_span *kept,
                   int *nkept, netcfg_note_fn note, void *arg) {
    struct netcfg_net *net = &rd->net;
    const char *why = NULL;
    if(!rd->open) return;
    rd->open = false;
    if(!rd->said_security) {
        if(rd->unknown && !net->has_psk && !net->has_password)  /* a typo where the key was: never a guess of open */
            why = "a line SurfOS does not understand and no security line (it is not taken as an open network)";
        net->security = (net->has_psk || net->has_password) ? NETCFG_SEC_WPA2 : NETCFG_SEC_OPEN;
    }
    if(!rd->bad && !why) {                          /* (a bad block was told about line by line already) */
        if(!net->ssid_len) why = "it has no name";
        else if(net->security == NETCFG_SEC_WPA2 && !net->has_psk && !net->has_password)
            why = "a WPA2 network needs its psk (or password)";
        else if(net->security == NETCFG_SEC_OPEN && (net->has_psk || net->has_password))
            why = "an open network has no key or password (security = wpa2?)";
        else if(netcfg_find_net(nets, *n, net->ssid, net->ssid_len) >= 0) why = "that network was named before";
        else if(*n >= max) why = "there are too many networks";
    }
    if(!rd->bad && !why) {
        if(net->has_psk) net->has_password = false;     /* the key wins: the password would only make it again */
        nets[(*n)++] = *net;
    } else {
        bool keeping = kept != NULL;
        if(keeping && !keep_span(kept, nkept, rd->start, end, false, rd->line, rd->pw_line)) {
            keeping = false;
            if(note) note(arg, rd->line, "too many networks SurfOS cannot read: this one is not kept in the file");
        }
        if(why) net_note(rd, rd->line, why, keeping, note, arg);
    }
    wipe(net, sizeof(*net));
}

int netcfg_parse_networks(const char *text, u_int len, struct netcfg_net *nets, int max, struct netcfg_span *kept,
                          int *nkept, netcfg_note_fn note, void *arg) {
    struct reading rd;
    char key[40], value[VALUE_MAX], *v;
    const char *w;
    u_int pos = bom_len(text, len), end, next, line = 0, l, i;
    int n = 0, r, dummy = 0;
    if(!nkept) nkept = &dummy;
    *nkept = 0;
    memset(&rd, 0, sizeof(rd));
    while(text && pos < len) {
        u_int at = pos;
        bool pw;
        next = line_at(text, len, pos, &end);
        line++;
        pos = next;
        pw = secret_line(text + at, end - at);
        if(pw && rd.open && !rd.pw_line) rd.pw_line = line;   /* a line of the block: a comment, a bad line too */
        if(end - at > LINE_MAX) {
            if(rd.open) {
                rd.bad = true;
                net_note(&rd, line, "the line is too long", kept != NULL, note, arg);
            } else {
                keep_loose(kept, nkept, at, next, line, pw, "the line is too long", note, arg);
            }
            continue;
        }
        r = split(text + at, end - at, key, sizeof(key), value, sizeof(value), &w);
        if(r == 0) continue;
        if(r < 0) {
            if(rd.open) {
                rd.bad = true;
                net_note(&rd, line, w, kept != NULL, note, arg);
            } else {
                keep_loose(kept, nkept, at, next, line, pw, w, note, arg);
            }
            continue;
        }
        if(same_word(key, "network")) {
            finish(&rd, at, nets, max, &n, kept, nkept, note, arg);
            memset(&rd, 0, sizeof(rd));
            rd.open = true;
            rd.line = line;
            rd.start = at;
            if(netcfg_unquote(value, rd.net.ssid, NETCFG_SSID_MAX, &l) || !l) {
                strlcpy(rd.name, "", sizeof(rd.name));
                net_note(&rd, line, "its name is not 1 to 32 bytes in double quotes", kept != NULL, note, arg);
                rd.bad = true;
            } else {
                rd.net.ssid_len = (u8)l;
                netcfg_quote(rd.net.ssid, l, rd.name, sizeof(rd.name));
            }
            continue;
        }
        if(!rd.open) {
            keep_loose(kept, nkept, at, next, line, pw, "a setting before the first network = line", note, arg);
            continue;
        }
        v = same_word(key, "password") ? value : (char *)bare(value);  /* a password is unquoted below, escapes and all */
        if(same_word(key, "security")) {
            rd.said_security = true;
            if(same_word(v, "wpa2")) rd.net.security = NETCFG_SEC_WPA2;
            else if(same_word(v, "open")) rd.net.security = NETCFG_SEC_OPEN;
            else {
                net_note(&rd, line, "security is open or wpa2", kept != NULL, note, arg);
                rd.bad = true;
            }
        } else if(same_word(key, "psk")) {
            bool ok = strlen(v) == 2 * NETCFG_KEY_LEN;
            for(i = 0; ok && i < NETCFG_KEY_LEN; i++) {
                int h = hexval(v[2 * i]), lo = hexval(v[2 * i + 1]);
                if(h < 0 || lo < 0) ok = false;
                else rd.net.psk[i] = (u8)(h * 16 + lo);
            }
            if(ok) rd.net.has_psk = true;
            else {
                net_note(&rd, line, "psk is 64 hexadecimal digits", kept != NULL, note, arg);
                rd.bad = true;
            }
        } else if(same_word(key, "password")) {
            u8 pass[64];
            bool quoted = v[0] == '"' || curly(v, (u_int)strlen(v), true);
            bool ok = !netcfg_unquote(v, pass, sizeof(pass) - 1, &l) && l >= 8 && l <= 63;
            for(i = 0; ok && i < l; i++) if(pass[i] < 0x20 || pass[i] > 0x7e) ok = false;
            u_int c = comment_at(text + at, end - at);
            if(!quoted && c < end - at && c > 0 && !blank(text[at + c - 1])) {     /* a # in a bare one: not a comment */
                net_note(&rd, line, "a password with a # in it goes in double quotes (password = \"...\")",
                         kept != NULL, note, arg);
                rd.bad = true;
            } else if(ok) {
                memcpy(rd.net.password, pass, l);
                rd.net.password[l] = 0;
                rd.net.has_password = true;
            } else {
                net_note(&rd, line, "a WPA2 password is 8 to 63 characters (letters, digits, spaces, punctuation; "
                                    "\\\" for a \" in it, \\\\ for a \\), in double quotes", kept != NULL, note, arg);
                rd.bad = true;
            }
            wipe(pass, sizeof(pass));
        } else if(same_word(key, "hidden")) {
            bool h;
            if(on_off(v, &h) == 0) rd.net.hidden = h;
            else if(note) note(arg, line, "hidden is yes or no (taken as no)");
        } else {
            rd.unknown = true;
            if(note) note(arg, line, "no such network setting (the line is left out)");
        }
    }
    finish(&rd, len, nets, max, &n, kept, nkept, note, arg);
    wipe(value, sizeof(value));                    /* it held a key or a password */
    return n;
}

/* the kept spans of old that are loose (or blocks), as they were, under kept_header; false when they do not fit */
static bool put_kept(char *out, u_int size, u_int *at, const char *old, const struct netcfg_span *kept, int nkept,
                     bool loose) {
    bool any = false;
    int i;
    for(i = 0; old && i < nkept; i++) {
        u_int a = kept[i].start, b = kept[i].end;
        if(kept[i].loose != loose || b <= a) continue;
        if(!any && !(loose ? put(out, size, at, loose_header, sizeof(loose_header) - 1) :
                     put(out, size, at, kept_header, sizeof(kept_header) - 1)))
            return false;
        any = true;
        if(!put(out, size, at, old + a, b - a)) return false;
        if(old[b - 1] != '\n' && !put(out, size, at, "\n", 1)) return false;  /* a CR alone too: one line end */
    }
    return true;
}

int netcfg_write_networks(const struct netcfg_net *nets, int n, const char *old, const struct netcfg_span *kept,
                          int nkept, char *out, u_int size) {
    static const char hex[] = "0123456789abcdef";
    char q[4 * NETCFG_SSID_MAX + 4], psk[2 * NETCFG_KEY_LEN + 1];
    u_int at = 0;
    int i, j;
    if(!size) return -1;
    out[0] = 0;
    if(!put(out, size, &at, networks_header, sizeof(networks_header) - 1) ||
       !put_kept(out, size, &at, old, kept, nkept, true))
        return -1;
    for(i = 0; i < n; i++) {
        const struct netcfg_net *net = &nets[i];
        if(!net->ssid_len || net->ssid_len > NETCFG_SSID_MAX || (net->security == NETCFG_SEC_WPA2 && !net->has_psk))
            return -1;                              /* never one the reader would not take back */
        if(netcfg_quote(net->ssid, net->ssid_len, q, sizeof(q)) < 0) return -1;
        if(!put(out, size, &at, "\nnetwork = ", 11) || !put(out, size, &at, q, (u_int)strlen(q)) ||
           !put(out, size, &at, "\n", 1))
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
    if(!put_kept(out, size, &at, old, kept, nkept, false)) return -1;
    return (int)at;
}

bool netcfg_reads_back(const char *text, u_int len, const struct netcfg_net *nets, int n, struct netcfg_net *scratch,
                       int *kept) {
    struct netcfg_span spans[NETCFG_SPANS_MAX];
    int got, nk = 0, i;
    bool same;
    got = netcfg_parse_networks(text, len, scratch, NETCFG_NETS_MAX, spans, &nk, NULL, NULL);
    same = got == n;
    for(i = 0; same && i < n; i++) {
        const struct netcfg_net *a = &nets[i], *b = &scratch[i];
        same = a->ssid_len == b->ssid_len && !memcmp(a->ssid, b->ssid, a->ssid_len) && a->security == b->security &&
               a->hidden == b->hidden && a->has_psk == b->has_psk &&
               (!a->has_psk || !memcmp(a->psk, b->psk, NETCFG_KEY_LEN)) && !b->has_password;
    }
    for(i = 0; i < nk && same; i++) if(spans[i].password) same = false;
    wipe(scratch, NETCFG_NETS_MAX * sizeof(*scratch));
    if(kept) *kept = nk;
    return same;
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
