/*
SurfOS user program: wsettings
--------------------
File: settings.c  Date: 11/05/26 (the desktop applications, track settings)
--------------------
Settings' model: what the wifi and settings commands do, for the desktop, in five pages.

Wi-Fi: the card on and off; the network joined, with its address; the networks in range, a row per
name (the strongest access point's signal, the security a join uses), from a scan the page asks for
when it first shows and again by its Scan button; joining one (an open network at once, but asking
first while a server without a password runs, as the wifi command warns; a WPA2 network by its
password, in a box whose field never shows its text, with Save it); a network by its name (one that
hides its name); leaving; saving the network joined now; the saved networks in the order they are
tried, and forgetting one (asked first); joining at startup on and off (the setting wifi). WEP, WPA3
only, enterprise and other networks are listed but not joined, and say why in the wifi command's
words. Network: Ethernet's DHCP on and off (the setting ethernet), every interface's addresses and
lease, the host name (checked as net/netcfg.c checks it). Desktop: the theme and the pointer's speed,
the desktop's keys in the network manager's table (desktop.theme, pointer.speed), their choices as the
table says them. System: telnetd at startup, turned on only after the warning the settings command
gives, and saying when a change acts as that command says it; the shell's history on the stick; where
the settings are kept. About: the version, the build, the memory, the time since the start and the
processor.

The kernel's answers (SYS_netctl's reads) are read at the start, every WS_READ_MS and when a job ends;
what waits is a job of the kernel's, which this program starts and looks at on each tick (main.c ticks
often while one runs): one at a time, its stage said in the status line and its outcome in words when
it ends. A password is in this model only for the call that hands it to the kernel (ws_join_answer()),
which wipes its copies and the caller's: the kernel makes the key and wipes its own. Nothing here makes
a system call or draws: main.c does both.
*/
#include <stdio.h>
#include <string.h>
#include <surfos/version.h>
#include "settings.h"

/* the kernel's error numbers (include/fs/vfs.h), which SYS_netctl answers: the host's may differ, so the tests' too */
#define E_PERM       1
#define E_NOENT      2
#define E_NOMEM      12
#define E_ACCES      13
#define E_BUSY       16
#define E_NODEV      19
#define E_INVAL      22
#define E_NOSYS      38
#define E_CONNRESET  104
#define E_TIMEDOUT   110
#define E_CONNREFUSED 111
#define E_CANCELED   125

static const char *const pages[WS_PAGES] = { "Wi-Fi", "Network", "Desktop", "System", "About" };

/* not optimised away */
static void wipe(void *p, unsigned n) {
    volatile unsigned char *q = (volatile unsigned char *)p;
    while(n--) *q++ = 0;
}

static void say(struct wsettings *m, const char *fmt, const char *a, const char *b, const char *c) {
    snprintf(m->status, sizeof(m->status), fmt, a ? a : "", b ? b : "", c ? c : "");
    m->said_scan = 0;
}

/**** words ****/

/* the length of a well-formed UTF-8 sequence at p (n bytes there) that is no C1 control, surrogate or code point past
   U+10FFFF (RFC 3629's table), 0 otherwise */
static int utf8_len(const unsigned char *p, int n) {
    unsigned c = p[0], v;
    int len, i;
    if(c < 0xc2 || c > 0xf4) return 0;
    len = c < 0xe0 ? 2 : c < 0xf0 ? 3 : 4;
    if(n < len) return 0;
    v = c & (0x7f >> len);
    for(i = 1; i < len; i++) {
        if((p[i] & 0xc0) != 0x80) return 0;
        v = v << 6 | (p[i] & 0x3f);
    }
    if((len == 3 && v < 0x800) || (len == 4 && (v < 0x10000 || v > 0x10ffff))) return 0;   /* overlong, too big */
    if((v >= 0xd800 && v <= 0xdfff) || (v >= 0x80 && v <= 0x9f)) return 0;   /* surrogates; C1 controls */
    return len;
}

/* an SSID as the window shows it: printable ASCII, a backslash doubled, well-formed UTF-8, and every other byte \xHH,
   so that no byte of a stranger's beacon is a control in the window; as much as fits, whole characters */
void ws_ssid_text(const unsigned char *ssid, int len, char *out, int n) {
    static const char hex[] = "0123456789abcdef";
    int i = 0, o = 0, k, l;
    if(n <= 0) return;
    if(len <= 0) {
        snprintf(out, (size_t)n, "%s", "(no name)");
        return;
    }
    while(i < len) {
        unsigned char c = ssid[i];
        l = c >= 32 && c < 127 ? (c == '\\' ? 2 : 1) : utf8_len(ssid + i, len - i);
        if(o + (l ? l : 4) >= n) break;
        if(c >= 32 && c < 127) {
            if(c == '\\') out[o++] = '\\';
            out[o++] = (char)c;
            i++;
        } else if(l) {
            for(k = 0; k < l; k++) out[o++] = (char)ssid[i + k];
            i += l;
        } else {
            out[o++] = '\\';
            out[o++] = 'x';
            out[o++] = hex[c >> 4];
            out[o++] = hex[c & 15];
            i++;
        }
    }
    out[o] = 0;
}

const char *ws_security_name(int security) {
    switch(security) {
    case NETCTL_SEC_OPEN:       return "open";
    case NETCTL_SEC_WEP:        return "WEP";
    case NETCTL_SEC_WPA2:       return "WPA2-Personal";
    case NETCTL_SEC_WPA2_WPA3:  return "WPA2/WPA3-Personal";
    case NETCTL_SEC_WPA3:       return "WPA3-Personal";
    case NETCTL_SEC_ENTERPRISE: return "Enterprise (802.1X)";
    default:                    return "other (WPA, TKIP, OWE)";
    }
}

const char *ws_security_short(int security) {
    switch(security) {
    case NETCTL_SEC_OPEN:       return "open";
    case NETCTL_SEC_WEP:        return "WEP";
    case NETCTL_SEC_WPA2:       return "WPA2";
    case NETCTL_SEC_WPA2_WPA3:  return "WPA2/WPA3";
    case NETCTL_SEC_WPA3:       return "WPA3";
    case NETCTL_SEC_ENTERPRISE: return "Enterprise";
    default:                    return "other";
    }
}

static int joinable(int security) {
    return security == NETCTL_SEC_OPEN || security == NETCTL_SEC_WPA2 || security == NETCTL_SEC_WPA2_WPA3;
}

/* why SurfOS does not join a network of that security: the wifi command's words */
static const char *refusal(int security) {
    switch(security) {
    case NETCTL_SEC_WEP:        return "uses WEP, which SurfOS does not support (it is broken: the network should use WPA2)";
    case NETCTL_SEC_WPA3:       return "is WPA3-Personal only (SAE): not supported yet";
    case NETCTL_SEC_ENTERPRISE: return "is an enterprise network (802.1X with a login): not supported";
    default:                    return "uses a security SurfOS does not support (WPA, TKIP, OWE or protected management "
                                       "frames)";
    }
}

/* four bars for a signal: from -55 dBm all of them, one fewer each 10 dB down, none below -85 */
int ws_bars(int rssi) {
    if(!rssi) return 0;
    if(rssi >= -55) return 4;
    if(rssi >= -65) return 3;
    if(rssi >= -75) return 2;
    if(rssi >= -85) return 1;
    return 0;
}

void ws_ip(unsigned ip, char *out, int n) {
    unsigned char b[4];
    memcpy(b, &ip, 4);                                  /* network byte order: as the bytes lie */
    snprintf(out, (size_t)n, "%u.%u.%u.%u", b[0], b[1], b[2], b[3]);
}

void ws_duration(unsigned s, char *out, int n) {
    if(s >= 86400) snprintf(out, (size_t)n, "%u day%s %u h", s / 86400, s / 86400 == 1 ? "" : "s", s % 86400 / 3600);
    else if(s >= 3600) snprintf(out, (size_t)n, "%u h %02u min", s / 3600, s % 3600 / 60);
    else if(s >= 60) snprintf(out, (size_t)n, "%u min %u s", s / 60, s % 60);
    else snprintf(out, (size_t)n, "%u s", s);
}

void ws_size_kb(unsigned kb, char *out, int n) {
    if(kb >= 1024 * 1024) snprintf(out, (size_t)n, "%u.%u GB", kb / (1024 * 1024), kb % (1024 * 1024) * 10 / (1024 * 1024));
    else if(kb >= 1024) snprintf(out, (size_t)n, "%u.%u MB", kb / 1024, kb % 1024 * 10 / 1024);
    else snprintf(out, (size_t)n, "%u KB", kb);
}

/**** the kernel's answers ****/

static int same(const unsigned char *a, int al, const unsigned char *b, int bl) {
    return al == bl && al >= 0 && !memcmp(a, b, (size_t)al);
}

static int is_saved(const struct wsettings *m, const unsigned char *ssid, int len) {
    unsigned i;
    for(i = 0; m->have_saved && i < m->saved.count && i < NETCTL_SAVED_MAX; i++)
        if(same(m->saved.net[i].ssid, m->saved.net[i].ssid_len, ssid, len)) return 1;
    return 0;
}

static int joined_to(const struct wsettings *m, const unsigned char *ssid, int len) {
    return m->have_wifi && m->wifi.present && m->wifi.state == NETCTL_STA_UP &&
           same(m->wifi.ssid, m->wifi.ssid_len, ssid, len);
}

/* the access points as rows, a row per name, in the list's order (strongest first) */
static void group(struct wsettings *m) {
    unsigned i;
    int k;
    m->nnets = m->nameless = 0;
    for(i = 0; m->have_bss && i < m->bss.count && i < NETCTL_BSS_MAX; i++) {
        const struct netctl_bss *b = &m->bss.bss[i];
        int len = b->ssid_len <= NETCTL_SSID_MAX ? b->ssid_len : NETCTL_SSID_MAX;
        struct ws_net *r;
        if(!len) {
            m->nameless++;
            continue;
        }
        for(k = 0; k < m->nnets && !same(m->nets[k].ssid, m->nets[k].ssid_len, b->ssid, len); k++) ;
        r = &m->nets[k];
        if(k == m->nnets) {
            memset(r, 0, sizeof(*r));
            memcpy(r->ssid, b->ssid, (size_t)len);
            r->ssid_len = len;
            r->security = b->security;
            r->rssi = b->rssi;
            r->band = b->band;
            m->nnets++;
        } else if(r->security != NETCTL_SEC_WPA2 && r->security != NETCTL_SEC_WPA2_WPA3 &&
                  (b->security == NETCTL_SEC_WPA2 || b->security == NETCTL_SEC_WPA2_WPA3 ||
                   (b->security == NETCTL_SEC_OPEN && r->security != NETCTL_SEC_OPEN))) {
            r->security = b->security;              /* WPA2 first, then open: never the password in the clear */
        }
        if(b->rssi > r->rssi) {                     /* the list comes strongest first; should it not */
            r->rssi = b->rssi;
            r->band = b->band;
        }
        r->count++;
    }
    for(k = 0; k < m->nnets; k++) {
        m->nets[k].saved = is_saved(m, m->nets[k].ssid, m->nets[k].ssid_len);
        m->nets[k].joined = joined_to(m, m->nets[k].ssid, m->nets[k].ssid_len);
    }
}

/* one read into a scratch copy: 1 and the copy kept when it worked and differs, 0 otherwise; *have as it went */
static int read_one(struct wsettings *m, int op, void *into, unsigned size, int *have) {
    static union {
        struct netctl_wifi w;
        struct netctl_bss_list b;
        struct netctl_saved s;
        struct netctl_settings t;
        struct netctl_ifaces f;
    } scratch;
    int r;
    memset(&scratch, 0, sizeof(scratch));
    r = m->netctl(m->ctx, op, &scratch, size);
    if(r == -E_NOSYS) m->no_call = 1;                   /* a kernel without the call */
    if(r < 0) {
        int was = *have;
        *have = 0;
        return was;
    }
    if(*have && !memcmp(into, &scratch, size)) return 0;
    memcpy(into, &scratch, size);
    *have = 1;
    return 1;
}

int ws_refresh(struct wsettings *m) {
    struct ws_net before[NETCTL_BSS_MAX];
    int nbefore = m->nnets, wifi, bss, saved, settings, ifaces, changed;
    memcpy(before, m->nets, sizeof(before));
    wifi = read_one(m, NETCTL_WIFI, &m->wifi, sizeof(m->wifi), &m->have_wifi);
    bss = read_one(m, NETCTL_NETWORKS, &m->bss, sizeof(m->bss), &m->have_bss);
    saved = read_one(m, NETCTL_SAVED, &m->saved, sizeof(m->saved), &m->have_saved);
    settings = read_one(m, NETCTL_SETTINGS, &m->settings, sizeof(m->settings), &m->have_settings);
    ifaces = read_one(m, NETCTL_IFACES, &m->ifaces, sizeof(m->ifaces), &m->have_ifaces);
    m->read_at = m->now;
    if(bss || wifi || saved) group(m);
    bss = nbefore != m->nnets || memcmp(before, m->nets, sizeof(before));  /* the rows, not the ages of what was heard */
    changed = !m->read_once;
    if(!m->job && nbefore && !m->nnets && m->said_scan) {
        m->status[0] = 0;                               /* what the scan counted was not heard for a minute: gone */
        m->said_scan = 0;
        changed = 1;
    }
    m->read_once = 1;
    switch(m->page) {
    case WS_WIFI:     changed |= wifi || bss || saved || settings; break;
    case WS_NETWORK:  changed |= ifaces || settings; break;
    case WS_DESKTOP:
    case WS_SYSTEM:   changed |= settings; break;
    default:          break;
    }
    return changed ? WS_DRAW : 0;
}

/**** jobs ****/

int ws_busy(const struct wsettings *m) {
    return m->job > 0;
}

static const char *stage_words(int stage) {
    switch(stage) {
    case NETCTL_STAGE_STARTING:   return "Starting the Wi-Fi card...";
    case NETCTL_STAGE_STOPPING:   return "Turning the Wi-Fi card off...";
    case NETCTL_STAGE_SCANNING:   return "Looking for networks...";
    case NETCTL_STAGE_JOINING:    return "Joining %s...";
    case NETCTL_STAGE_SAVING:     return "Saving %s on the stick...";
    case NETCTL_STAGE_ADDRESS:    return "Joined %s; asking for an address (DHCP)...";
    case NETCTL_STAGE_CONNECTING: return "Looking for a saved network in range...";
    case NETCTL_STAGE_LEAVING:    return "Leaving %s...";
    default:                      return "Writing the settings on the stick...";
    }
}

static void say_stage(struct wsettings *m) {
    char name[WS_NAME];
    ws_ssid_text(m->job_ssid, m->job_ssid_len, name, sizeof(name));
    if(m->job_op == NETCTL_JOIN && m->job_stage == NETCTL_STAGE_SCANNING)   /* not heard in the last minute: looked for */
        say(m, "Looking for %s...", name, NULL, NULL);
    else say(m, stage_words(m->job_stage), name, NULL, NULL);
}

/* the kernel's refusal to start a job (op), in words */
static void say_refused(struct wsettings *m, int op, int r, const char *name) {
    if(r == -E_BUSY) say(m, "Busy: another program, the network manager or a wifi command is using the Wi-Fi card or "
                            "the settings; try again in a moment", NULL, NULL, NULL);
    else if(r == -E_NODEV) say(m, m->have_wifi && m->wifi.present ? "The Wi-Fi card is off: turn it on first" :
                                  "There is no Wi-Fi card (no driver found one)", NULL, NULL, NULL);
    else if(r == -E_ACCES) say(m, "%s takes a password", name, NULL, NULL);
    else if(r == -E_PERM && op == NETCTL_JOIN) say(m, "%s is open, and a server without a password runs: asked first",
                                                   name, NULL, NULL);
    else if(r == -E_NOSYS && op == NETCTL_JOIN) say(m, "SurfOS cannot join %s: its security is one SurfOS does not "
                                                       "speak, or this SurfOS has no WPA2", name, NULL, NULL);
    else if(r == -E_NOSYS) say(m, "This SurfOS has no network settings for programs (SYS_netctl)", NULL, NULL, NULL);
    else if(r == -E_NOMEM) say(m, "Not enough memory in the kernel; try again", NULL, NULL, NULL);
    else if(r == -E_INVAL && op == NETCTL_JOIN) say(m, "SurfOS did not take that: %s is an open network (it takes no "
                                                       "password), or the password is not one it can use", name, NULL,
                                                    NULL);
    else if(r == -E_INVAL) say(m, "SurfOS did not take that value", NULL, NULL, NULL);
    else say(m, "The request failed", NULL, NULL, NULL);
}

/* the stage a job starts with, as the kernel gives it (kernel/netctl.c's first_stage()) */
static int first_stage(int op, int on) {
    switch(op) {
    case NETCTL_CARD:   return on ? NETCTL_STAGE_STARTING : NETCTL_STAGE_STOPPING;
    case NETCTL_SCAN:   return NETCTL_STAGE_SCANNING;
    case NETCTL_JOIN:   return NETCTL_STAGE_JOINING;
    case NETCTL_LEAVE:  return NETCTL_STAGE_LEAVING;
    case NETCTL_SAVE:   return NETCTL_STAGE_SAVING;
    default:            return NETCTL_STAGE_WRITING;
    }
}

/* a change asked for while this program's last one runs */
static int wait_first(struct wsettings *m) {
    say(m, "Wait: the last change is still under way", NULL, NULL, NULL);
    return WS_DRAW;
}

/* start a job: its number kept, its first stage said; the kernel's refusal said otherwise */
static int start(struct wsettings *m, int op, void *arg, unsigned size, const unsigned char *ssid, int len,
                 const char *key, const char *value, int quiet) {
    char name[WS_NAME];
    int r;
    if(m->job) return quiet ? 0 : wait_first(m);
    ws_ssid_text(ssid, len, name, sizeof(name));
    r = m->netctl(m->ctx, op, arg, size);
    if(r <= 0) {
        m->refused = r ? r : -E_INVAL;
        if(!quiet) say_refused(m, op, m->refused, name);
        return quiet ? 0 : WS_DRAW;
    }
    m->refused = 0;
    m->job = r;
    m->job_op = op;
    m->job_quiet = quiet;
    m->job_ssid_len = ssid && len > 0 && len <= NETCTL_SSID_MAX ? len : 0;
    if(m->job_ssid_len) memcpy(m->job_ssid, ssid, (size_t)len);
    snprintf(m->job_key, sizeof(m->job_key), "%s", key ? key : "");
    snprintf(m->job_value, sizeof(m->job_value), "%s", value ? value : "");
    m->job_stage = first_stage(op, value && value[0] == '1');
    if(op == NETCTL_JOIN) {                             /* a name not in the rows: the kernel looks for it first */
        int i;
        for(i = 0; i < m->nnets && !same(m->nets[i].ssid, m->nets[i].ssid_len, ssid, len); i++) ;
        if(i == m->nnets) m->job_stage = NETCTL_STAGE_SCANNING;
    }
    if(!quiet) say_stage(m);
    return WS_DRAW;
}

/* settings.c's when(): when a change of key acts, after "saved" */
static const char *when(const char *key, const char *value) {
    int on = !strcmp(value, "on") || !strcmp(value, "dhcp");
    if(!strcmp(key, "telnetd")) return on ? "at the next start (telnetd on in a Terminal starts it now)" :
                                           "at the next start (telnetd off in a Terminal stops it now)";
    if(!strcmp(key, "telnetd.port")) return "at the next start";
    if(!strcmp(key, "wifi")) return on ? "at every start a saved network is joined by itself" :
                                        "no saved network is joined by itself, from now on";
    if(!strcmp(key, "ethernet")) return on ? "now: an Ethernet link that comes up gets an address" :
                                            "now: the dhcp command asks for an address by hand";
    if(!strcmp(key, "history")) return on ? "from now on surfsh saves its history there (plain text: a line that "
                                           "starts with a space is never kept), and a surfsh started later recalls it" :
                                           "from now on surfsh saves no history (what is saved stays in "
                                           "surfos-config/history until it is deleted)";
    if(!strcmp(key, "hostname")) return "in the next DHCP request";
    return "now";
}

/* the wifi command's words for a join's failure (and a scan's, a leave's, the card's) */
static const char *join_failure(int r) {
    switch(r) {
    case -E_ACCES:       return "the 4-way handshake failed: the password may be wrong";
    case -E_NOENT:       return "no network of that name is in range";
    case -E_TIMEDOUT:    return "no answer";
    case -E_CONNREFUSED: return "refused";
    case -E_CONNRESET:   return "the access point sent SurfOS away";
    case -E_CANCELED:    return "called off by a wifi leave or wifi down meanwhile";
    case -E_BUSY:        return "the Wi-Fi card was busy (the network manager or a wifi command); try again in a moment";
    case -E_NODEV:       return "the Wi-Fi card is off";
    case -E_NOSYS:       return "WPA2 needs the supplicant, which this SurfOS does not have";
    default:             return "it failed";
    }
}

static int ask_exposed(struct wsettings *m, const unsigned char *ssid, int len, int save);

/* a setting that acts when it is set, under a start that uses the settings: one not saved is in force all the same (the
   network manager keeps telnetd's change only when it is saved: it acts at a start) */
static int in_force_unsaved(const struct wsettings *m, const char *key) {
    return !(m->have_settings && m->settings.off) && strcmp(key, "telnetd") && strcmp(key, "telnetd.port");
}

/* key among the settings in force but not saved (unsaved 1), or no longer (0) */
static void unsaved_mark(struct wsettings *m, const char *key, int unsaved) {
    int i;
    for(i = 0; i < m->nunsaved && strcmp(m->unsaved[i], key); i++) ;
    if(unsaved && i == m->nunsaved && m->nunsaved < NETCTL_KEYS_MAX) {
        snprintf(m->unsaved[m->nunsaved++], NETCTL_KEY_MAX, "%s", key);
    } else if(!unsaved && i < m->nunsaved) {
        memmove(m->unsaved[i], m->unsaved[i + 1], (size_t)(m->nunsaved - i - 1) * NETCTL_KEY_MAX);
        m->nunsaved--;
    }
}

/* the job ended (j): what came of it, in words */
static void say_outcome(struct wsettings *m, const struct netctl_job *j) {
    char name[WS_NAME], ip[16], tail[WS_STATUS];
    size_t at;
    ws_ssid_text(m->job_ssid, m->job_ssid_len, name, sizeof(name));
    ws_ip(m->have_wifi ? m->wifi.ip : 0, ip, sizeof(ip));
    tail[0] = 0;
    m->said_scan = 0;
    switch(m->job_op) {
    case NETCTL_SCAN:
        if(j->result) say(m, "The scan failed: %s", join_failure(j->result), NULL, NULL);
        else if(!m->nnets) say(m, "No networks in range%s", j->flags & NETCTL_DONE_PARTIAL ? " (the scan did not end in "
                               "time: what was heard until then)" : "", NULL, NULL);
        else {
            char n[16];
            snprintf(n, sizeof(n), "%d", m->nnets);
            say(m, "%s network%s in range%s", n, m->nnets == 1 ? "" : "s", j->flags & NETCTL_DONE_PARTIAL ?
                " (the scan did not end in time: what was heard until then)" : "");
            m->said_scan = 1;
        }
        break;
    case NETCTL_JOIN:
        if(j->result == -E_PERM && (j->flags & NETCTL_DONE_EXPOSED)) {     /* a server started while it looked */
            ask_exposed(m, m->job_ssid, m->job_ssid_len, m->job_save);
            break;
        }
        if(j->result) {                                 /* why: what the job's own scan found (kernel/netctl.c) */
            say(m, "Could not join %s: %s", name, j->why[0] ? j->why : join_failure(j->result), NULL);
            break;
        }
        if(j->flags & NETCTL_DONE_SAVED)
            snprintf(tail, sizeof(tail), "; saved: SurfOS joins it by itself at startup");
        else if(j->flags & NETCTL_DONE_NOT_SAVED)
            snprintf(tail, sizeof(tail), "; not saved: %s", j->why);
        at = strlen(tail);
        if(j->flags & NETCTL_DONE_EXPOSED)              /* the person was told: said again with what came of it */
            snprintf(tail + at, sizeof(tail) - at, "%s", "; it is open, and a server without a password runs: anyone "
                     "on it reaches that server");
        if(j->flags & NETCTL_DONE_ADDRESS) say(m, "Joined %s: address %s%s", name, ip, tail);
        else say(m, "Joined %s, but no address came (DHCP)%s", name, tail, NULL);
        break;
    case NETCTL_LEAVE:
        if(j->result) say(m, "Could not leave the network: %s", join_failure(j->result), NULL, NULL);
        else say(m, "Left the network", NULL, NULL, NULL);
        break;
    case NETCTL_CARD:
        if(j->result) say(m, "The Wi-Fi card did not %s: %s", m->job_value[0] == '1' ? "start" : "stop",
                          join_failure(j->result), NULL);
        else if(m->job_value[0] != '1') say(m, "The Wi-Fi card is off", NULL, NULL, NULL);
        else if(j->flags & NETCTL_DONE_JOINED) {
            ws_ssid_text(m->wifi.ssid, m->wifi.ssid_len, name, sizeof(name));
            if(j->flags & NETCTL_DONE_ADDRESS) say(m, "The Wi-Fi card is on: joined %s, address %s", name, ip, NULL);
            else say(m, "The Wi-Fi card is on: joined %s, but no address came (DHCP)", name, NULL, NULL);
        } else if(j->flags & NETCTL_DONE_NONE_NEAR) {
            say(m, "The Wi-Fi card is on; none of the saved networks is in range", NULL, NULL, NULL);
        } else {
            say(m, "The Wi-Fi card is on", NULL, NULL, NULL);
        }
        break;
    case NETCTL_SAVE:
        if(j->result) say(m, "%s not saved: %s", name, j->why[0] ? j->why : join_failure(j->result), NULL);
        else if(m->have_settings && m->settings.off)
            say(m, "Saved %s: SurfOS joins it by itself from the next start (this one does not read the settings)",
                name, NULL, NULL);
        else if(!ws_is_on(m, "wifi"))
            say(m, "Saved %s; SurfOS joins none by itself while joining at startup is off", name, NULL, NULL);
        else say(m, "Saved %s: SurfOS joins it by itself at startup", name, NULL, NULL);
        break;
    case NETCTL_FORGET:
        if(j->result == -E_NOENT) say(m, "%s is not a saved network", name, NULL, NULL);
        else if(j->result) say(m, "%s not forgotten: %s", name, j->why[0] ? j->why : "it failed", NULL);
        else say(m, "Forgot %s: SurfOS no longer joins it by itself", name, NULL, NULL);
        break;
    case NETCTL_SET:
        if(j->result) say(m, "%s = %s not set: %s", m->job_key, m->job_value, j->why[0] ? j->why : "refused");
        else if(j->flags & NETCTL_DONE_NOT_SAVED) {
            if((m->have_settings && m->settings.off) || !strcmp(m->job_key, "telnetd") ||
               !strcmp(m->job_key, "telnetd.port"))
                snprintf(m->status, sizeof(m->status), "%s = %s not saved: %s; nothing changes", m->job_key,
                         m->job_value, j->why);
            else snprintf(m->status, sizeof(m->status), "%s = %s is in force until SurfOS restarts, but not saved: %s "
                          "(System > Save again, once the stick takes it)", m->job_key, m->job_value, j->why);
        } else if(m->have_settings && m->settings.off) {
            say(m, "%s = %s, saved on the stick: from the next start (this one does not read the settings)",
                m->job_key, m->job_value, NULL);
        } else {
            say(m, "%s = %s, saved on the stick: %s", m->job_key, m->job_value, when(m->job_key, m->job_value));
        }
        if(!j->result)
            unsaved_mark(m, m->job_key, (j->flags & NETCTL_DONE_NOT_SAVED) && in_force_unsaved(m, m->job_key));
        break;
    }
}

/**** settings stepped through: in force at once (NETCTL_APPLY), written once the steps stop ****/

/* step i written now, by a NETCTL_SET job; kept for a later try while the kernel is busy with another program's job */
static void write_step(struct wsettings *m, int i, int *draw) {
    struct netctl_req r;
    if(!strcmp(m->step[i].value, m->step[i].from)) {    /* stepped away and back: the value of before, nothing new */
        say(m, "%s = %s, as it was before the steps: nothing written", m->step[i].key, m->step[i].value, NULL);
        memset(&m->step[i], 0, sizeof(m->step[i]));
        *draw |= WS_DRAW;
        return;
    }
    memset(&r, 0, sizeof(r));
    snprintf(r.key, sizeof(r.key), "%s", m->step[i].key);
    snprintf(r.value, sizeof(r.value), "%s", m->step[i].value);
    *draw |= start(m, NETCTL_SET, &r, sizeof(r), NULL, 0, r.key, r.value, 0);
    if(m->job || m->refused != -E_BUSY) memset(&m->step[i], 0, sizeof(m->step[i]));
    else m->step[i].at = m->now;                        /* busy: again once the time has passed again */
}

/**** settings in force but not saved, written again (the review's L4) ****/

/* the first of them, with the value in force now: a job (its outcome takes it off the list); the rest follow it */
static int save_next(struct wsettings *m) {
    const struct netctl_setting *s;
    struct netctl_req r;
    while(m->nunsaved && !(s = ws_setting(m, m->unsaved[0]))) unsaved_mark(m, m->unsaved[0], 0);  /* not in the table */
    if(!m->nunsaved) {
        m->again = 0;
        return 0;
    }
    memset(&r, 0, sizeof(r));
    snprintf(r.key, sizeof(r.key), "%s", s->name);
    snprintf(r.value, sizeof(r.value), "%s", s->value);
    return start(m, NETCTL_SET, &r, sizeof(r), NULL, 0, r.key, r.value, 0);
}

int ws_unsaved(const struct wsettings *m) {
    return m->nunsaved;
}

/* netman_set() writes a value the stick may already hold, so a setting written again with its own value is saved: the
   settings command's way too, which is why steps that end where they began are dropped here and not there */
int ws_save_again(struct wsettings *m) {
    int r;
    if(!m->nunsaved) return 0;
    if(m->job) return wait_first(m);
    m->again = 1;
    r = save_next(m);
    if(!m->job) m->again = 0;                           /* refused at the start: said */
    return r;
}

/* the steps waiting (all: every one, closing; else those whose steps stopped) written as far as jobs allow, one at a
   time: 1 when none waits any more */
static int write_steps(struct wsettings *m, int all, int *draw) {
    int i, waiting = 0;
    for(i = 0; i < WS_STEPS; i++) {
        if(!m->step[i].key[0]) continue;
        if(!m->job && (all || m->now - m->step[i].at >= WS_WRITE_AFTER_MS)) write_step(m, i, draw);
        if(m->step[i].key[0]) waiting = 1;
    }
    return !waiting;
}

int ws_choose(struct wsettings *m, const char *name, const char *value) {
    struct netctl_setting *s = (struct netctl_setting *)ws_setting(m, name);
    struct netctl_req r;
    int i, rc, draw = 0;
    if(!s) {
        say(m, "This SurfOS has no setting called %s", name, NULL, NULL);
        return WS_DRAW;
    }
    for(i = 0; i < WS_STEPS && strcmp(m->step[i].key, name); i++) ;    /* a step of it waiting: the newest value (under
                                                                           settings=off nothing is in force: s's is the
                                                                           value of before, the review's N1) */
    if(!strcmp(i < WS_STEPS ? m->step[i].value : s->value, value)) return 0;
    memset(&r, 0, sizeof(r));
    snprintf(r.key, sizeof(r.key), "%s", name);
    snprintf(r.value, sizeof(r.value), "%s", value);
    rc = m->netctl(m->ctx, NETCTL_APPLY, &r, sizeof(r));
    if(rc < 0) return ws_set(m, name, value);           /* a kernel without the step, or a key it does not step */
    for(i = 0; i < WS_STEPS && m->step[i].key[0] && strcmp(m->step[i].key, name); i++) ;
    if(i == WS_STEPS) {                                 /* (two keys, two places: never, but the oldest goes first) */
        i = m->step[0].at - m->step[1].at < 0x80000000u ? 1 : 0;
        write_step(m, i, &draw);
        if(m->step[i].key[0]) return ws_set(m, name, value);
    }
    if(!m->step[i].key[0]) snprintf(m->step[i].from, sizeof(m->step[i].from), "%s", s->value);  /* the first step's */
    if(rc == 0) snprintf(s->value, sizeof(s->value), "%s", value);    /* in force, as the next read will say */
    snprintf(m->step[i].key, sizeof(m->step[i].key), "%s", name);
    snprintf(m->step[i].value, sizeof(m->step[i].value), "%s", value);
    m->step[i].at = m->now;
    if(rc == 1) say(m, "%s = %s: written on the stick for the next start (this one does not use the settings)", name,
                    value, NULL);
    else say(m, "%s = %s: in force now, and written on the stick once the steps stop", name, value, NULL);
    return WS_DRAW;
}

int ws_tick(struct wsettings *m) {
    struct netctl_job j;
    int draw = 0, r;
    if(m->job) {
        r = m->netctl(m->ctx, NETCTL_JOB, &j, sizeof(j));
        if(r == -E_NOMEM) {
            ;                                           /* not read now: the job runs on; read again next tick (another
                                                           error would come again: the job is given up below) */
        } else if(r < 0 || (int)j.seq != m->job || j.state != NETCTL_JOB_RUNNING) {
            int quiet = m->job_quiet, saved = 0;
            ws_refresh(m);                              /* what it changed */
            if(r == 0 && (int)j.seq == m->job && j.state == NETCTL_JOB_DONE) {
                if(!quiet || j.result == 0) say_outcome(m, &j);
                saved = m->job_op == NETCTL_SET && !j.result && !(j.flags & NETCTL_DONE_NOT_SAVED);
            } else if(!quiet) {
                say(m, "Done (its outcome was not seen: the kernel's record moved on)", NULL, NULL, NULL);
            }
            m->job = 0;
            if(m->job_op == NETCTL_CARD && m->job_value[0] == '1' && !m->nnets) m->scanned = -1;  /* nothing heard: a scan */
            if(m->again) {                              /* Save again: the next one, while the stick takes them */
                if(!saved || m->closing) m->again = 0;
                else if(m->nunsaved) save_next(m);
                else {
                    m->again = 0;
                    say(m, "The settings in force are saved on the stick again", NULL, NULL, NULL);
                }
            }
            draw = WS_DRAW;
        } else if(j.stage != m->job_stage) {
            m->job_stage = j.stage;
            if(!m->job_quiet) say_stage(m);
            draw = WS_DRAW;
        }
    }
    if(!m->read_once || m->now - m->read_at >= WS_READ_MS) draw |= ws_refresh(m);
    if(write_steps(m, m->closing, &draw) && m->closing) return WS_CLOSE;
    /* the first look at the Wi-Fi page scans, once the card is on and nothing else runs, and again after the card
       started with nothing heard, and at a look with nothing heard in the last minute; quietly: a busy card is no
       news */
    if(!m->job && m->scanned <= 0 && m->page == WS_WIFI && m->have_wifi && m->wifi.present && m->wifi.up &&
       !m->no_call && !m->ask.kind) {                  /* (not under a question: its answer may start a job) */
        m->scanned = 1;
        draw |= start(m, NETCTL_SCAN, NULL, 0, NULL, 0, NULL, NULL, 1);
    }
    return draw;
}

void ws_init(struct wsettings *m, ws_netctl_fn netctl, void *ctx) {
    memset(m, 0, sizeof(*m));
    m->netctl = netctl;
    m->ctx = ctx;
    m->net_sel_len = m->saved_sel_len = -1;
    snprintf(m->status, sizeof(m->status), "%s", "");
}

int ws_page(struct wsettings *m, int page) {
    if(page < 0 || page >= WS_PAGES || page == m->page) return 0;
    m->page = page;
    if(page == WS_WIFI && !m->nnets && m->scanned > 0) m->scanned = 0;   /* a look with nothing heard: a scan by itself */
    return WS_DRAW;
}

const char *ws_page_name(int page) {
    return page >= 0 && page < WS_PAGES ? pages[page] : "";
}

void ws_title(const struct wsettings *m, char *out, int n) {
    snprintf(out, (size_t)n, "Settings - %s", ws_page_name(m->page));
}

/* the window closes once the steps waiting are written (their jobs started: a job ends in the kernel whatever comes of
   its program); while a job runs, on a later tick (closing) */
static int close_now(struct wsettings *m) {
    int draw = 0;
    if(write_steps(m, 1, &draw)) return WS_CLOSE;
    m->closing = 1;
    say(m, "Writing the last change on the stick, then closing...", NULL, NULL, NULL);
    return WS_DRAW;
}

int ws_close(struct wsettings *m, const char *typed_host) {
    const struct netctl_setting *h = ws_setting(m, "hostname");
    if(m->ask.kind == WS_ASK_CLOSE || m->closing) return WS_CLOSE;    /* the close button again: as Close */
    if(typed_host && h && strcmp(typed_host, h->value) && typed_host[0]) {
        memset(&m->ask, 0, sizeof(m->ask));
        m->ask.kind = WS_ASK_CLOSE;
        snprintf(m->ask.title, sizeof(m->ask.title), "%s", "Close Settings");
        snprintf(m->ask.text, sizeof(m->ask.text), "The host name %s was typed but not set. Close all the same?",
                 typed_host);
        snprintf(m->ask.yes, sizeof(m->ask.yes), "%s", "Close");
        snprintf(m->ask.no, sizeof(m->ask.no), "%s", "Cancel");
        return WS_DRAW;
    }
    return close_now(m);
}

/**** Wi-Fi ****/

int ws_card(struct wsettings *m, int on) {
    struct netctl_req r;
    memset(&r, 0, sizeof(r));
    r.on = on ? 1 : 0;
    return start(m, NETCTL_CARD, &r, sizeof(r), NULL, 0, "card", on ? "1" : "0", 0);
}

int ws_scan(struct wsettings *m) {
    m->scanned = 1;
    return start(m, NETCTL_SCAN, NULL, 0, NULL, 0, NULL, NULL, 0);
}

int ws_select_net(struct wsettings *m, int i) {
    if(i < 0 || i >= m->nnets) {
        if(m->net_sel_len < 0) return 0;
        m->net_sel_len = -1;
        return WS_DRAW;
    }
    if(same(m->net_sel, m->net_sel_len, m->nets[i].ssid, m->nets[i].ssid_len)) return 0;
    memcpy(m->net_sel, m->nets[i].ssid, (size_t)m->nets[i].ssid_len);
    m->net_sel_len = m->nets[i].ssid_len;
    return WS_DRAW;
}

int ws_net_selected(const struct wsettings *m) {
    int i;
    for(i = 0; i < m->nnets; i++) if(same(m->nets[i].ssid, m->nets[i].ssid_len, m->net_sel, m->net_sel_len)) return i;
    return -1;
}

int ws_select_saved(struct wsettings *m, int i) {
    const struct netctl_saved_net *s;
    if(!m->have_saved || i < 0 || i >= (int)m->saved.count || i >= NETCTL_SAVED_MAX) {
        if(m->saved_sel_len < 0) return 0;
        m->saved_sel_len = -1;
        return WS_DRAW;
    }
    s = &m->saved.net[i];
    if(same(m->saved_sel, m->saved_sel_len, s->ssid, s->ssid_len)) return 0;
    memcpy(m->saved_sel, s->ssid, s->ssid_len);
    m->saved_sel_len = s->ssid_len;
    return WS_DRAW;
}

int ws_saved_selected(const struct wsettings *m) {
    unsigned i;
    for(i = 0; m->have_saved && i < m->saved.count && i < NETCTL_SAVED_MAX; i++)
        if(same(m->saved.net[i].ssid, m->saved.net[i].ssid_len, m->saved_sel, m->saved_sel_len)) return (int)i;
    return -1;
}

static int card_on(const struct wsettings *m) {
    return m->have_wifi && m->wifi.present && m->wifi.up;
}

int ws_can_join(const struct wsettings *m, int i) {
    return i >= 0 && i < m->nnets && card_on(m) && joinable(m->nets[i].security) && !m->nets[i].joined && !m->job;
}

static void ask(struct wsettings *m, int kind, const unsigned char *ssid, int len, const char *title, const char *yes,
                const char *no) {
    memset(&m->ask, 0, sizeof(m->ask));
    m->ask.kind = kind;
    if(ssid && len > 0 && len <= NETCTL_SSID_MAX) {
        memcpy(m->ask.ssid, ssid, (size_t)len);
        m->ask.ssid_len = len;
    }
    m->ask.save = 0;                                    /* unticked: a join saves nothing unless asked (wifi join) */
    snprintf(m->ask.title, sizeof(m->ask.title), "%s", title);
    snprintf(m->ask.yes, sizeof(m->ask.yes), "%s", yes);
    snprintf(m->ask.no, sizeof(m->ask.no), "%s", no);
}

/* what joining an open network means while a server without a password runs (the wifi command's warning) */
static int exposed_text(const struct wsettings *m, char *out, int n) {
    int e = m->have_wifi ? m->wifi.exposed : 0;
    if(!e) return 0;
    snprintf(out, (size_t)n, "%s%s", e & NETCTL_EXPOSED_TELNET ? "A telnet server runs or is to start, and it has no "
             "password: it answers on an open network too (telnetd off in a Terminal stops it). " : "",
             e & NETCTL_EXPOSED_FTP ? "An FTP server runs, and it has no password: anyone on an open network may reach "
             "its files (ftpd off stops it). " : "");
    return 1;
}

/* the kernel refused an open join: a server without a password runs (one may have started since the last read, or
   while the job looked for the network). Asked with what is true now; its Join is the person's word */
static int ask_exposed(struct wsettings *m, const unsigned char *ssid, int len, int save) {
    char why[300], name[WS_NAME];
    ws_refresh(m);
    ws_ssid_text(ssid, len, name, sizeof(name));
    if(m->ask.kind) {                                   /* never in the place of a question that is open (the review's
                                                           N2): its buttons would take a click meant for that one */
        say(m, "%s was not joined: it is open, and a server without a password runs (Join asks first)", name, NULL,
            NULL);
        return WS_DRAW;
    }
    if(!exposed_text(m, why, sizeof(why)))
        snprintf(why, sizeof(why), "%s", "A server without a password runs, and anyone on an open network reaches it. ");
    ask(m, WS_ASK_OPEN, ssid, len, "Join an open network", "Join", "Cancel");
    m->ask.save = save;
    m->ask.raised = 1;                                  /* no click of the person's asked for it: a Yes waits a moment */
    m->ask.at = m->now;
    snprintf(m->ask.text, sizeof(m->ask.text), "%sJoin %s all the same?", why, name);
    say(m, "%s is open, and a server without a password runs: asked first", name, NULL, NULL);
    return WS_DRAW;
}

/* told: the person answered the warning about a server without a password (NETCTL_JOIN_EXPOSED); the only way the
   flag is ever set */
static int join_open(struct wsettings *m, const unsigned char *ssid, int len, int save, int told) {
    struct netctl_join j;
    int r;
    memset(&j, 0, sizeof(j));
    memcpy(j.ssid, ssid, (size_t)len);
    j.ssid_len = (unsigned)len;
    j.flags = (save ? NETCTL_JOIN_SAVE : 0) | (told ? NETCTL_JOIN_EXPOSED : 0);
    m->refused = 0;
    r = start(m, NETCTL_JOIN, &j, sizeof(j), ssid, len, NULL, NULL, 0);
    wipe(&j, sizeof(j));
    if(m->job) m->job_save = save;
    else if(m->refused == -E_PERM) return ask_exposed(m, ssid, len, save);
    return r;
}

int ws_join(struct wsettings *m, int i) {
    char name[WS_NAME], why[300];                        /* exposed_text()'s two sentences */
    const struct ws_net *n;
    if(i < 0 || i >= m->nnets) return 0;
    n = &m->nets[i];
    ws_ssid_text(n->ssid, n->ssid_len, name, sizeof(name));
    if(m->job) return wait_first(m);
    if(!card_on(m)) {
        say(m, "The Wi-Fi card is off: turn it on first", NULL, NULL, NULL);
        return WS_DRAW;
    }
    if(n->joined) {
        say(m, "Joined to %s already", name, NULL, NULL);
        return WS_DRAW;
    }
    if(!joinable(n->security)) {
        say(m, "%s %s", name, refusal(n->security), NULL);
        return WS_DRAW;
    }
    if(n->security == NETCTL_SEC_OPEN) {
        if(exposed_text(m, why, sizeof(why))) {
            ask(m, WS_ASK_OPEN, n->ssid, n->ssid_len, "Join an open network", "Join", "Cancel");
            m->ask.save = 0;                            /* from the list: no Save it was offered */
            snprintf(m->ask.text, sizeof(m->ask.text), "%sJoin %s all the same?", why, name);
            return WS_DRAW;
        }
        return join_open(m, n->ssid, n->ssid_len, 0, 0);
    }
    ask(m, WS_ASK_PASSWORD, n->ssid, n->ssid_len, "Join a network", "Join", "Cancel");
    snprintf(m->ask.text, sizeof(m->ask.text), "Password for %s (%s):", name, ws_security_name(n->security));
    return WS_DRAW;
}

int ws_join_other(struct wsettings *m) {
    if(m->job) return wait_first(m);
    if(!card_on(m)) {
        say(m, "The Wi-Fi card is off: turn it on first", NULL, NULL, NULL);
        return WS_DRAW;
    }
    ask(m, WS_ASK_OTHER, NULL, 0, "Join a network by its name", "Join", "Cancel");
    snprintf(m->ask.text, sizeof(m->ask.text), "%s", "The network's name, and its password (nothing for an open "
             "network). One that hides its name is found by asking for it.");
    return WS_DRAW;
}

/* a WPA2 password SurfOS takes: 8 to 63 printable ASCII characters (IEEE 802.11's passphrase) */
static int password_ok(const char *p) {
    size_t n = strlen(p), i;
    if(n < 8 || n > 63) return 0;
    for(i = 0; i < n; i++) if((unsigned char)p[i] < 32 || (unsigned char)p[i] > 126) return 0;
    return 1;
}

int ws_join_answer(struct wsettings *m, const char *name, int name_len, char *password, int save) {
    struct netctl_join j;
    unsigned char ssid[NETCTL_SSID_MAX];
    char text[WS_NAME];
    int len, need, r, kind = m->ask.kind;
    size_t plen = password ? strlen(password) : 0;
    if(kind != WS_ASK_PASSWORD && kind != WS_ASK_OTHER) {
        if(password) wipe(password, (unsigned)plen);
        return 0;
    }
    if(m->job) {                                        /* the box stays (the window greys its Join meanwhile) */
        if(password) wipe(password, (unsigned)plen);
        return wait_first(m);
    }
    if(kind == WS_ASK_OTHER) {
        if(!name || name_len <= 0 || name_len > NETCTL_SSID_MAX) {
            if(password) wipe(password, (unsigned)plen);
            snprintf(m->ask.text, sizeof(m->ask.text), "%s", "A network's name is 1 to 32 bytes. The network's name, "
                     "and its password (nothing for an open network):");
            return WS_DRAW;
        }
        memcpy(ssid, name, (size_t)name_len);
        len = name_len;
        need = plen > 0;
    } else {
        memcpy(ssid, m->ask.ssid, (size_t)m->ask.ssid_len);
        len = m->ask.ssid_len;
        need = 1;
    }
    ws_ssid_text(ssid, len, text, sizeof(text));
    if(need && (!password || !password_ok(password))) {
        if(password) wipe(password, (unsigned)plen);
        snprintf(m->ask.text, sizeof(m->ask.text), "A WPA2 password is 8 to 63 characters (letters, digits, spaces and "
                 "punctuation). Password for %s:", text);
        return WS_DRAW;
    }
    if(!need) {                                         /* by name, no password: an open network */
        char why[300];
        if(password) wipe(password, (unsigned)plen);
        if(exposed_text(m, why, sizeof(why))) {         /* asked first, as for an open network in the list */
            ask(m, WS_ASK_OPEN, ssid, len, "Join an open network", "Join", "Cancel");
            m->ask.save = save;                         /* what the box said, kept for the join */
            snprintf(m->ask.text, sizeof(m->ask.text), "%sJoin %s all the same?", why, text);
            return WS_DRAW;
        }
    }
    memset(&j, 0, sizeof(j));
    memcpy(j.ssid, ssid, (size_t)len);
    j.ssid_len = (unsigned)len;
    j.flags = save ? NETCTL_JOIN_SAVE : 0;
    if(need) memcpy(j.passphrase, password, plen + 1);
    if(password) wipe(password, (unsigned)plen);        /* the caller's copy goes now; the kernel wipes j's */
    m->ask.kind = WS_ASK_NONE;
    m->refused = 0;
    r = start(m, NETCTL_JOIN, &j, sizeof(j), ssid, len, NULL, NULL, 0);
    wipe(&j, sizeof(j));
    if(m->job) m->job_save = save;
    else if(m->refused == -E_PERM) return ask_exposed(m, ssid, len, save);   /* a server started since the last read */
    return r;
}

int ws_leave(struct wsettings *m) {
    const unsigned char *ssid = m->have_wifi ? m->wifi.ssid : NULL;
    return start(m, NETCTL_LEAVE, NULL, 0, ssid, ssid ? m->wifi.ssid_len : 0, NULL, NULL, 0);
}

int ws_joined_saved(const struct wsettings *m) {
    return m->have_wifi && m->wifi.state == NETCTL_STA_UP && is_saved(m, m->wifi.ssid, m->wifi.ssid_len);
}

int ws_save_joined(struct wsettings *m) {
    if(!m->have_wifi || m->wifi.state != NETCTL_STA_UP) {
        say(m, "The Wi-Fi card is not joined to a network", NULL, NULL, NULL);
        return WS_DRAW;
    }
    return start(m, NETCTL_SAVE, NULL, 0, m->wifi.ssid, m->wifi.ssid_len, NULL, NULL, 0);
}

int ws_forget(struct wsettings *m, int i) {
    char name[WS_NAME];
    const struct netctl_saved_net *s;
    if(!m->have_saved || i < 0 || i >= (int)m->saved.count || i >= NETCTL_SAVED_MAX) return 0;
    s = &m->saved.net[i];
    ws_ssid_text(s->ssid, s->ssid_len, name, sizeof(name));
    ask(m, WS_ASK_FORGET, s->ssid, s->ssid_len, "Forget a network", "Forget", "Cancel");
    snprintf(m->ask.text, sizeof(m->ask.text), "Forget %s? SurfOS will no longer join it by itself, and its key goes "
             "from networks.conf on the stick (the network wlan0 is on stays joined).", name);
    return WS_DRAW;
}

int ws_answer_starts(const struct wsettings *m) {
    int k = m->ask.kind;
    return k == WS_ASK_PASSWORD || k == WS_ASK_OTHER || k == WS_ASK_OPEN || k == WS_ASK_FORGET || k == WS_ASK_TELNETD;
}

/* what saving a network means (the review's M3, N3); the box itself is unticked for every join. A WPA2 network: with
   telnetd at startup on, telnetd starts on it at every start (a server started by hand does not come back by itself).
   An open network: the network manager joins none by itself while a server without a password runs or is to start,
   and telnetd never starts on one */
const char *ws_save_label(const struct wsettings *m, int wpa2) {
    const struct netctl_setting *t = ws_setting(m, "telnetd");
    int at_start = t && !strcmp(t->value, "on");
    int exposed = at_start || (m->have_wifi && m->wifi.exposed);
    if(wpa2) return at_start ? "Save it: joined by itself at every start, and telnetd starts on it" :
                               "Save it: SurfOS joins it by itself at every start";
    return exposed ? "Save it: joined by itself only while no telnetd or ftpd is on" :
                     "Save it: SurfOS joins it by itself at every start";
}

int ws_answer(struct wsettings *m, int yes) {
    struct ws_ask a = m->ask;
    struct netctl_req r;
    if(yes && m->job && ws_answer_starts(m)) return wait_first(m);    /* the question stays until the job has ended */
    if(yes && a.raised && m->now - a.at < WS_ASK_GUARD_MS) return 0;  /* too soon to be an answer to it */
    memset(&m->ask, 0, sizeof(m->ask));
    if(a.kind == WS_ASK_CLOSE) return yes ? close_now(m) : WS_DRAW;
    if(!yes || a.kind == WS_ASK_NONE) return WS_DRAW;
    switch(a.kind) {
    case WS_ASK_OPEN:
        return join_open(m, a.ssid, a.ssid_len, a.save, 1);    /* the person's word: the kernel may join it so */
    case WS_ASK_FORGET:
        memset(&r, 0, sizeof(r));
        memcpy(r.ssid, a.ssid, (size_t)a.ssid_len);
        r.ssid_len = (unsigned)a.ssid_len;
        return start(m, NETCTL_FORGET, &r, sizeof(r), a.ssid, a.ssid_len, NULL, NULL, 0);
    case WS_ASK_TELNETD:
        memset(&r, 0, sizeof(r));
        snprintf(r.key, sizeof(r.key), "%s", "telnetd");
        snprintf(r.value, sizeof(r.value), "%s", "on");
        return start(m, NETCTL_SET, &r, sizeof(r), NULL, 0, "telnetd", "on", 0);
    default:
        return WS_DRAW;                                 /* the password boxes answer through ws_join_answer() */
    }
}

void ws_wifi_line(const struct wsettings *m, char *out, int n) {
    char name[WS_NAME], ip[16];
    const struct netctl_wifi *w = &m->wifi;
    if(m->no_call) {
        snprintf(out, (size_t)n, "%s", "This SurfOS has no network settings for programs (SYS_netctl)");
        return;
    }
    if(!m->have_wifi) {
        snprintf(out, (size_t)n, "%s", "The Wi-Fi card could not be asked");
        return;
    }
    if(!w->present) {
        snprintf(out, (size_t)n, "%s", "No Wi-Fi card (no driver found one)");
        return;
    }
    ws_ssid_text(w->ssid, w->ssid_len, name, sizeof(name));
    ws_ip(w->ip, ip, sizeof(ip));
    if(!w->up) snprintf(out, (size_t)n, "%s", "The Wi-Fi card is off");
    else if(w->state == NETCTL_STA_UP && w->ip)
        snprintf(out, (size_t)n, "Joined %s (%s): address %s", name, ws_security_name(w->security), ip);
    else if(w->state == NETCTL_STA_UP)
        snprintf(out, (size_t)n, "Joined %s (%s): no address yet", name, ws_security_name(w->security));
    else if(w->state == NETCTL_STA_JOINING) snprintf(out, (size_t)n, "Joining %s...", name);
    else if(w->state == NETCTL_STA_RECONNECTING) snprintf(out, (size_t)n, "The link to %s went: joining it again", name);
    else if(w->idle == NETCTL_IDLE_JOIN_FAILED && w->last[0])
        snprintf(out, (size_t)n, "Not joined: joining %s failed: %s", name, w->last);
    else if(w->idle == NETCTL_IDLE_GAVE_UP) snprintf(out, (size_t)n, "Not joined: %s was lost", name);
    else snprintf(out, (size_t)n, "%s", "Not joined to a network");
}

/* four bars for the list: lit blocks and shaded ones (code page 437 has both) */
static void bars(int rssi, char *out, int n) {
    int k = ws_bars(rssi), i, o = 0;
    for(i = 0; i < 4 && o + 4 <= n; i++) {
        memcpy(out + o, i < k ? "\xe2\x96\x88" : "\xe2\x96\x91", 3);      /* U+2588, U+2591 */
        o += 3;
    }
    out[o] = 0;
}

void ws_net_row(const struct wsettings *m, int i, char *out, int n) {
    char name[WS_NAME], b[16];
    const struct ws_net *r;
    if(n <= 0) return;
    out[0] = 0;
    if(i < 0 || i >= m->nnets) return;
    r = &m->nets[i];
    ws_ssid_text(r->ssid, r->ssid_len, name, sizeof(name));
    bars(r->rssi, b, sizeof(b));
    snprintf(out, (size_t)n, "%s\t%s %d dBm\t%s\t%s", name, b, r->rssi, ws_security_short(r->security),
             r->joined ? "joined" : r->saved ? "saved" : joinable(r->security) ? "" : "not supported");
}

void ws_net_note(const struct wsettings *m, int i, char *out, int n) {
    char name[WS_NAME];
    if(n <= 0) return;
    out[0] = 0;
    if(i < 0 || i >= m->nnets || joinable(m->nets[i].security)) return;
    ws_ssid_text(m->nets[i].ssid, m->nets[i].ssid_len, name, sizeof(name));
    snprintf(out, (size_t)n, "%s %s", name, refusal(m->nets[i].security));
}

/* row i of the saved networks: the name, its security, and its state as wifi saved says it */
void ws_saved_row(const struct wsettings *m, int i, char *out, int n) {
    const struct netctl_saved_net *s;
    const char *sec;
    char name[WS_NAME];
    int k;
    if(n <= 0) return;
    out[0] = 0;
    if(!m->have_saved || i < 0 || i >= (int)m->saved.count || i >= NETCTL_SAVED_MAX) return;
    s = &m->saved.net[i];
    sec = s->security == NETCTL_SAVED_WPA2 ? "WPA2" : "open";
    ws_ssid_text(s->ssid, s->ssid_len, name, sizeof(name));
    if(joined_to(m, s->ssid, s->ssid_len)) {
        snprintf(out, (size_t)n, "%s\t%s\tjoined now", name, sec);
        return;
    }
    if(s->refused) {
        snprintf(out, (size_t)n, "%s\t%s\tits key was refused: join it again, then save it", name, sec);
        return;
    }
    for(k = 0; k < m->nnets; k++) {
        if(same(m->nets[k].ssid, m->nets[k].ssid_len, s->ssid, s->ssid_len)) {
            snprintf(out, (size_t)n, "%s\t%s\tin range, %d dBm", name, sec, m->nets[k].rssi);
            return;
        }
    }
    snprintf(out, (size_t)n, "%s\t%s\t%s", name, sec, s->hidden ? "hidden" : "");
}

/* where text's line-th line starts once it is wrapped at spaces to cells columns (each UTF-8 character a cell, as the
   window draws them; a word longer than a line cut where the line ends; a newline ends a line), and its bytes in *len;
   NULL past the last line */
const char *ws_wrap(const char *text, int cells, int line, int *len) {
    const char *p = text, *e, *space;
    int c, k;
    if(cells < 1) cells = 1;
    for(;;) {
        while(*p == ' ') p++;                           /* a line never starts with the spaces it was broken at */
        if(!*p) return NULL;
        for(e = p, c = 0, space = NULL; *e && *e != '\n' && c < cells; c++) {
            if(*e == ' ') space = e;
            for(k = 1; (e[k] & 0xc0) == 0x80 && k < 4; k++) ;   /* the character's continuation bytes */
            e += k;
        }
        if(*e && *e != '\n' && *e != ' ' && space) e = space;  /* the text goes on mid-word: break at the last space */
        if(!line--) {
            while(e > p && e[-1] == ' ') e--;
            *len = (int)(e - p);
            return p;
        }
        p = *e == '\n' ? e + 1 : e;
    }
}

int ws_wrap_lines(const char *text, int cells) {
    int n = 0, len;
    while(ws_wrap(text, cells, n, &len)) n++;
    return n;
}

/**** the settings ****/

const struct netctl_setting *ws_setting(const struct wsettings *m, const char *name) {
    unsigned i;
    for(i = 0; m->have_settings && i < m->settings.count && i < NETCTL_KEYS_MAX; i++) {
        if(!strcmp(m->settings.key[i].name, name)) return &m->settings.key[i];
    }
    return NULL;
}

int ws_is_on(const struct wsettings *m, const char *name) {
    const struct netctl_setting *s = ws_setting(m, name);
    return s && (!strcmp(s->value, "on") || !strcmp(s->value, "dhcp"));
}

int ws_choices(const struct netctl_setting *s, char out[][NETCTL_VALUE_MAX], int max) {
    const char *p = s ? s->values : "", *e;
    int n = 0;
    size_t len;
    while(*p && n < max) {
        while(*p == ' ') p++;
        e = strchr(p, '|');
        len = e ? (size_t)(e - p) : strlen(p);
        while(len && p[len - 1] == ' ') len--;
        if(len && len < NETCTL_VALUE_MAX) {
            memcpy(out[n], p, len);
            out[n][len] = 0;
            n++;
        }
        if(!e) break;
        p = e + 1;
    }
    return n;
}

int ws_set(struct wsettings *m, const char *name, const char *value) {
    struct netctl_req r;
    const struct netctl_setting *s = ws_setting(m, name);
    if(!s) {
        say(m, "This SurfOS has no setting called %s", name, NULL, NULL);
        return WS_DRAW;
    }
    if(!strcmp(s->value, value)) return 0;              /* as it is */
    if(!strcmp(name, "telnetd") && !strcmp(value, "on")) {
        ask(m, WS_ASK_TELNETD, NULL, 0, "The telnet server at startup", "Turn it on", "Cancel");
        snprintf(m->ask.text, sizeof(m->ask.text), "%s", "telnetd has no password and no encryption: whoever reaches "
                 "the laptop over a network SurfOS is on gets a shell. It starts only once there is an address on "
                 "Ethernet or a WPA2 network, and while it runs SurfOS joins no open Wi-Fi network by itself. But "
                 "SurfOS cannot tell whose network it is: a cafe's WPA2 network, whose password others know, counts "
                 "too. Save only your own networks.");
        return WS_DRAW;
    }
    memset(&r, 0, sizeof(r));
    snprintf(r.key, sizeof(r.key), "%s", name);
    snprintf(r.value, sizeof(r.value), "%s", value);
    return start(m, NETCTL_SET, &r, sizeof(r), NULL, 0, name, value, 0);
}

int ws_toggle(struct wsettings *m, const char *name) {
    const struct netctl_setting *s = ws_setting(m, name);
    if(!s) return ws_set(m, name, "on");                /* says there is none */
    if(!strcmp(name, "ethernet")) return ws_set(m, name, strcmp(s->value, "dhcp") ? "dhcp" : "off");
    return ws_set(m, name, strcmp(s->value, "on") ? "on" : "off");
}

/* net/netcfg.c's rule for a host name: letters, digits and -, not first or last, 1 to 32 of them */
int ws_host_ok(const char *name, char *why, int n) {
    size_t len = strlen(name), i;
    if(len < 1 || len > 32 || name[0] == '-' || name[len - 1] == '-') {
        snprintf(why, (size_t)n, "%s", "A host name is 1 to 32 letters, digits and -, but not - first or last");
        return 0;
    }
    for(i = 0; i < len; i++) {
        char c = name[i];
        if(!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-')) {
            snprintf(why, (size_t)n, "%s", "A host name is letters, digits and - only (no spaces, dots or accents)");
            return 0;
        }
    }
    return 1;
}

int ws_set_host(struct wsettings *m, const char *name) {
    char why[WS_LINE];
    if(!ws_host_ok(name, why, sizeof(why))) {
        say(m, "%s", why, NULL, NULL);
        return WS_DRAW;
    }
    return ws_set(m, "hostname", name);
}

/* interface i in three lines: its name, link and MAC; its addresses; its lease */
void ws_iface_lines(const struct wsettings *m, int i, char *a, int an, char *b, int bn, char *c, int cn) {
    const struct netctl_if *f;
    char ip[16], mask[16], gw[16], dns[16], left[32], len[32];
    if(an > 0) a[0] = 0;
    if(bn > 0) b[0] = 0;
    if(cn > 0) c[0] = 0;
    if(!m->have_ifaces || i < 0 || i >= (int)m->ifaces.count || i >= NETCTL_IF_MAX) return;
    f = &m->ifaces.iface[i];
    snprintf(a, (size_t)an, "%s (%s): link %s, MAC %02x:%02x:%02x:%02x:%02x:%02x", f->name, f->wifi ? "Wi-Fi" :
             !strncmp(f->name, "eth", 3) ? "Ethernet" : "network", f->link_up ? "up" : "down", f->mac[0], f->mac[1],
             f->mac[2], f->mac[3], f->mac[4], f->mac[5]);
    if(!f->ip) {
        snprintf(b, (size_t)bn, "%s", "No address");
        return;
    }
    ws_ip(f->ip, ip, sizeof(ip));
    ws_ip(f->netmask, mask, sizeof(mask));
    ws_ip(f->gateway, gw, sizeof(gw));
    ws_ip(f->dns, dns, sizeof(dns));
    snprintf(b, (size_t)bn, "Address %s, netmask %s, gateway %s, DNS %s", ip, mask, f->gateway ? gw : "none",
             f->dns ? dns : "none");
    if(f->lease_s) {
        ws_ip(f->dhcp_server, ip, sizeof(ip));
        ws_duration(f->lease_left, left, sizeof(left));
        ws_duration(f->lease_s, len, sizeof(len));
        snprintf(c, (size_t)cn, "DHCP lease from %s: %s left of %s", ip, left, len);
    } else {
        snprintf(c, (size_t)cn, "%s", "Set by hand (no lease)");
    }
}

void ws_about_line(const struct wsettings *m, int k, char *label, int ln, char *value, int vn) {
    static const char *const labels[] = { "Version", "Build", "Memory", "Programs", "Up for", "Processor" };
    const struct ws_about *ab = &m->about;
    char a[24], b[24];
    if(k < 0 || k > 5) {
        if(ln > 0) label[0] = 0;
        if(vn > 0) value[0] = 0;
        return;
    }
    snprintf(label, (size_t)ln, "%s", labels[k]);
    switch(k) {
    case 0: snprintf(value, (size_t)vn, "%s %s", SURFOS_NAME, SURFOS_VERSION); break;
    case 1: snprintf(value, (size_t)vn, "%s", ab->build[0] ? ab->build : "none recorded (/initrd/etc/build-id)"); break;
    case 2:
        if(!ab->have_mem) { snprintf(value, (size_t)vn, "%s", "not known"); break; }
        ws_size_kb(ab->mem_kb, a, sizeof(a));
        ws_size_kb(ab->free_kb, b, sizeof(b));
        snprintf(value, (size_t)vn, "%s, %s of it free", a, b);
        break;
    case 3:
        if(!ab->have_mem) { snprintf(value, (size_t)vn, "%s", "not known"); break; }
        ws_size_kb(ab->programs_kb, a, sizeof(a));
        ws_size_kb(ab->heap_kb, b, sizeof(b));
        snprintf(value, (size_t)vn, "%s in programs, %s in the kernel's heap", a, b);
        break;
    case 4:
        if(!ab->have_mem) snprintf(value, (size_t)vn, "%s", "not known");
        else if(ab->uptime_s >= 86400)
            snprintf(value, (size_t)vn, "%u day%s %u:%02u:%02u", ab->uptime_s / 86400, ab->uptime_s / 86400 == 1 ? "" :
                     "s", ab->uptime_s % 86400 / 3600, ab->uptime_s / 60 % 60, ab->uptime_s % 60);
        else snprintf(value, (size_t)vn, "%u:%02u:%02u", ab->uptime_s / 3600, ab->uptime_s / 60 % 60, ab->uptime_s % 60);
        break;
    default: snprintf(value, (size_t)vn, "%s", ab->cpu[0] ? ab->cpu : "not known"); break;
    }
}
