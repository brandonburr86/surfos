/* Host tests for Settings' model (user/wsettings/settings.c; the desktop applications, track settings, 11/05/26),
   built as it is with the host's C library, against a simulated kernel: SYS_netctl's reads answered from structures
   the test sets, and its jobs started, refused (busy, a security SurfOS does not join, no such call) and ended as the
   test says, with what the kernel was handed kept for the checks. The scenarios: the first reads and the scan the
   Wi-Fi page asks for by itself; the networks in range a row per name (WPA2 first, the strongest's signal, the
   nameless access points apart, saved and joined marked); SSIDs as text (controls, bad UTF-8, C1, surrogates,
   overlong forms as \xHH; whole characters cut); joining a WPA2 network by its password (refused when too short or
   too long, always wiped from the caller's buffer, never in the model's memory or words), an open one (asking first
   while a server without a password runs), one by its name, and refusing WEP, WPA3 and the like in the wifi command's
   words; one change at a time, another program's making the kernel busy; the card off and on, a saved network
   joined or none in range; leave, save, forget (asked first); the settings: on and off, telnetd only after its
   warning, when a change acts as the settings command says it, not saved or under settings=off; the host name's
   rule; the desktop's choices; the interfaces' lines and leases; the About page; a kernel without the call; closing
   with a host name typed and not set; reading again only shows what changed; selections that follow a network as
   the list moves. Built and run by `make unittest` (unittest-settings). */
#include <stdio.h>
#include <string.h>
#include "settings.h"

static int checks, failed;
#define CHECK(x) do { checks++; if(!(x)) { failed++; printf("FAIL %s line %d: %s\n", __FILE__, __LINE__, #x); } } while(0)

#define FULL "\xe2\x96\x88"                             /* a signal's bar lit, and not */
#define SHADE "\xe2\x96\x91"

#include "wsettings_fake.h"

static struct wsettings m;

/* the simulated kernel as it starts (wsettings_fake.h), and a model on it */
static void start(void) {
    fake_reset();
    ws_init(&m, fake_netctl, NULL);
    m.now = 1000;
}

/* the first look: everything read, and the scan the Wi-Fi page asks for by itself, ended */
static void started(void) {
    CHECK(ws_tick(&m) & WS_DRAW);
    CHECK(m.have_wifi && m.have_bss && m.have_saved && m.have_settings && m.have_ifaces && m.job > 0);
    CHECK(k.last_op == NETCTL_SCAN && m.job_quiet && !m.status[0]);    /* a scan by itself says nothing yet */
    job_done(0, 0, NULL);
    m.now += 200;
    CHECK(ws_tick(&m) & WS_DRAW);
    CHECK(!m.job && !strcmp(m.status, "6 networks in range"));
}

static int row(const char *ssid) {
    int i;
    for(i = 0; i < m.nnets; i++) if(m.nets[i].ssid_len == (int)strlen(ssid) && !memcmp(m.nets[i].ssid, ssid, strlen(ssid)))
        return i;
    return -1;
}

/* is s (n bytes) anywhere in the model's memory? */
static int in_model(const char *s) {
    const unsigned char *p = (const unsigned char *)&m;
    size_t n = strlen(s), i;
    for(i = 0; i + n <= sizeof(m); i++) if(!memcmp(p + i, s, n)) return 1;
    return 0;
}

/* line k of text wrapped to cells, as a string ("" past the end) */
static const char *wrapped(const char *text, int cells, int k) {
    static char out[256];
    int len;
    const char *p = ws_wrap(text, cells, k, &len);
    if(!p) return "";
    memcpy(out, p, (size_t)len);
    out[len] = 0;
    return out;
}

static void wrapping(void) {
    const char *t = "Joined SurfSimWPA (WPA2-Personal): address 10.0.2.15";
    int len;
    CHECK(ws_wrap_lines(t, 80) == 1 && !strcmp(wrapped(t, 80, 0), t));
    CHECK(ws_wrap_lines(t, 30) == 3 && !strcmp(wrapped(t, 30, 0), "Joined SurfSimWPA"));
    CHECK(!strcmp(wrapped(t, 30, 1), "(WPA2-Personal): address"));
    CHECK(!strcmp(wrapped(t, 30, 2), "10.0.2.15") && ws_wrap_lines(t, 24) == 3 && !strcmp(wrapped(t, 24, 2), "10.0.2.15"));
    CHECK(!strcmp(wrapped(t, 17, 0), "Joined SurfSimWPA") && !strcmp(wrapped(t, 17, 1), "(WPA2-Personal):"));
    CHECK(!strcmp(wrapped("abcdefghij", 4, 0), "abcd") && !strcmp(wrapped("abcdefghij", 4, 2), "ij"));   /* a long word */
    CHECK(ws_wrap_lines("abcdefghij", 4) == 3 && ws_wrap_lines("", 10) == 0 && ws_wrap_lines("   ", 10) == 0);
    CHECK(!strcmp(wrapped("one\n\nthree", 20, 1), "") && !strcmp(wrapped("one\n\nthree", 20, 2), "three"));
    CHECK(ws_wrap_lines("one\n\nthree", 20) == 3);
    CHECK(!strcmp(wrapped("caf\xc3\xa9 au lait", 4, 0), "caf\xc3\xa9"));   /* characters, not bytes */
    CHECK(!strcmp(wrapped("a b", 1, 1), "b") && !ws_wrap("a b", 1, 2, &len));
    CHECK(!strcmp(wrapped("word  word", 4, 1), "word"));          /* the spaces it broke at go */
    CHECK(!strcmp(wrapped("x", 0, 0), "x"));                         /* no width: a character a line */
}

static void words(void) {
    static const struct { const char *in; int len; const char *out; } cases[] = {
        { "SurfSim", 7, "SurfSim" },
        { "a\\b", 3, "a\\\\b" },                                       /* a backslash doubled */
        { "tab\there", 8, "tab\\x09here" },
        { "\x1b[2J", 4, "\\x1b[2J" },                                  /* an escape is no escape here */
        { "caf\xc3\xa9", 5, "caf\xc3\xa9" },                           /* UTF-8 as it is */
        { "\xc2\x9b" "x", 3, "\\xc2\\x9bx" },                          /* C1 control (CSI) in UTF-8 */
        { "\xc0\xaf", 2, "\\xc0\\xaf" },                               /* overlong */
        { "\xed\xa0\x80", 3, "\\xed\\xa0\\x80" },                      /* a surrogate */
        { "\xf4\x90\x80\x80", 4, "\\xf4\\x90\\x80\\x80" },             /* past U+10FFFF */
        { "\xe2\x82", 2, "\\xe2\\x82" },                               /* cut short */
        { "\xf0\x9f\x8c\x8a", 4, "\xf0\x9f\x8c\x8a" },                 /* a wave, as the desktop draws what it has */
        { "", 0, "(no name)" },
    };
    char out[WS_NAME], small[8];
    int i;
    for(i = 0; i < (int)(sizeof(cases) / sizeof(cases[0])); i++) {
        ws_ssid_text((const unsigned char *)cases[i].in, cases[i].len, out, sizeof(out));
        CHECK(!strcmp(out, cases[i].out));
    }
    ws_ssid_text((const unsigned char *)"\x01\x02\x03\x04\x05\x06\x07\x08\x09\x0a\x0b\x0c\x0d\x0e\x0f\x10"
                 "\x11\x12\x13\x14\x15\x16\x17\x18\x19\x1a\x1b\x1c\x1d\x1e\x1f\x7f", 32, out, sizeof(out));
    CHECK(strlen(out) == 128 && !strncmp(out, "\\x01\\x02", 8));   /* the worst: 4 a byte, and it fits */
    ws_ssid_text((const unsigned char *)"abc\xc3\xa9", 5, small, 5);
    CHECK(!strcmp(small, "abc"));                                    /* never half a character */
    ws_ssid_text((const unsigned char *)"ab\x01", 3, small, 6);
    CHECK(!strcmp(small, "ab"));
    CHECK(ws_bars(-40) == 4 && ws_bars(-55) == 4 && ws_bars(-56) == 3 && ws_bars(-70) == 2 && ws_bars(-85) == 1);
    CHECK(ws_bars(-86) == 0 && ws_bars(0) == 0);
    ws_ip(ip4(192, 168, 1, 20), out, sizeof(out));
    CHECK(!strcmp(out, "192.168.1.20"));
    ws_duration(45, out, sizeof(out));
    CHECK(!strcmp(out, "45 s"));
    ws_duration(200, out, sizeof(out));
    CHECK(!strcmp(out, "3 min 20 s"));
    ws_duration(7500, out, sizeof(out));
    CHECK(!strcmp(out, "2 h 05 min"));
    ws_duration(86400 + 4 * 3600 + 5, out, sizeof(out));
    CHECK(!strcmp(out, "1 day 4 h"));
    ws_duration(3 * 86400, out, sizeof(out));
    CHECK(!strcmp(out, "3 days 0 h"));
    ws_size_kb(640, out, sizeof(out));
    CHECK(!strcmp(out, "640 KB"));
    ws_size_kb(32256, out, sizeof(out));
    CHECK(!strcmp(out, "31.5 MB"));
    ws_size_kb(2 * 1024 * 1024 + 1024 * 103, out, sizeof(out));
    CHECK(!strcmp(out, "2.1 GB"));
    CHECK(!strcmp(ws_security_name(NETCTL_SEC_WPA2), "WPA2-Personal") && !strcmp(ws_security_short(NETCTL_SEC_WEP), "WEP"));
    CHECK(!strcmp(ws_page_name(WS_SYSTEM), "System") && !strcmp(ws_page_name(9), ""));
    wrapping();
}

static void networks_in_range(void) {
    char name[WS_NAME], right[WS_LINE], note[WS_LINE];
    int t;
    start();
    add_saved("SurfSimWPA", NETCTL_SAVED_WPA2, 0);
    started();
    CHECK(m.nnets == 6 && m.nameless == 1);                        /* Twins twice: one row; the nameless apart */
    t = row("Twins");
    CHECK(t == 2 && m.nets[t].count == 2 && m.nets[t].security == NETCTL_SEC_WPA2 && m.nets[t].rssi == -54);
    CHECK(m.nets[row("SurfSimWPA")].saved && !m.nets[row("SurfSim")].saved);
    CHECK(row("SurfSim") == 0 && row("SurfSimWEP") == 5);           /* strongest first, as the kernel lists them */
    ws_net_row(&m, row("SurfSimWPA"), right, sizeof(right));
    CHECK(!strcmp(right, "SurfSimWPA\t" FULL FULL FULL FULL " -52 dBm\tWPA2\tsaved"));
    ws_net_row(&m, row("SurfSimWEP"), right, sizeof(right));
    CHECK(!strcmp(right, "SurfSimWEP\t" FULL FULL SHADE SHADE " -71 dBm\tWEP\tnot supported"));
    ws_net_row(&m, row("SurfSimSAE"), right, sizeof(right));
    CHECK(!strcmp(right, "SurfSimSAE\t" FULL FULL FULL SHADE " -60 dBm\tWPA3\tnot supported"));
    ws_net_row(&m, row("SurfSim"), right, sizeof(right));
    CHECK(!strcmp(right, "SurfSim\t" FULL FULL FULL FULL " -45 dBm\topen\t"));
    ws_net_row(&m, 99, right, sizeof(right));
    CHECK(!right[0]);
    (void)name;
    CHECK(ws_can_join(&m, row("SurfSim")) && ws_can_join(&m, row("Twins")) && !ws_can_join(&m, row("SurfSimWEP")));
    CHECK(!ws_can_join(&m, row("SurfSimSAE")) && !ws_can_join(&m, 99) && !ws_can_join(&m, -1));
    ws_net_note(&m, row("SurfSimSAE"), note, sizeof(note));
    CHECK(!strcmp(note, "SurfSimSAE is WPA3-Personal only (SAE): not supported yet"));
    ws_net_note(&m, row("SurfSim"), note, sizeof(note));
    CHECK(!note[0]);
    CHECK(ws_join(&m, row("SurfSimWEP")) == WS_DRAW && !m.job && m.ask.kind == WS_ASK_NONE);
    CHECK(!strcmp(m.status, "SurfSimWEP uses WEP, which SurfOS does not support (it is broken: the network should use "
                            "WPA2)"));
    /* the selection follows the network when the list moves */
    CHECK(ws_select_net(&m, row("SurfSimFlaky")) == WS_DRAW && ws_net_selected(&m) == 3);
    CHECK(ws_select_net(&m, 3) == 0);
    k.bss.bss[3].rssi = -30;                                        /* SurfSimFlaky first now */
    {
        struct netctl_bss b = k.bss.bss[3];
        memmove(&k.bss.bss[1], &k.bss.bss[0], 3 * sizeof(b));
        k.bss.bss[0] = b;
    }
    m.now += WS_READ_MS;
    CHECK(ws_tick(&m) == WS_DRAW && ws_net_selected(&m) == 0 && row("SurfSimFlaky") == 0);
    m.now += WS_READ_MS;
    CHECK(ws_tick(&m) == 0);                                        /* nothing changed: nothing to draw */
    k.bss.bss[1].age_s = 9;                                          /* what was heard ages: the rows are the same */
    m.now += WS_READ_MS;
    CHECK(ws_tick(&m) == 0);
    k.bss.count = 0;                                                /* gone: no row, no selection */
    m.now += WS_READ_MS;
    CHECK(ws_tick(&m) == WS_DRAW && m.nnets == 0 && ws_net_selected(&m) == -1);
}

/* what a scan heard leaves the kernel's list after a minute unheard: its count goes from the band, and a look at the
   page again scans by itself; other words stay */
static void networks_age(void) {
    start();
    started();
    CHECK(m.said_scan && m.scanned == 1 && m.nnets == 6);
    k.bss.count = 0;
    m.now += WS_READ_MS;
    CHECK(ws_tick(&m) == WS_DRAW && m.nnets == 0 && !m.status[0] && !m.said_scan && !m.job);   /* no scan by itself */
    m.now += WS_READ_MS;
    CHECK(ws_tick(&m) == 0 && !m.job);
    CHECK(ws_page(&m, WS_NETWORK) == WS_DRAW && m.scanned == 1);
    ws_tick(&m);
    CHECK(!m.job && ws_page(&m, WS_WIFI) == WS_DRAW && m.scanned == 0);
    ws_tick(&m);
    CHECK(m.job && k.last_op == NETCTL_SCAN && m.job_quiet && m.scanned == 1);    /* a look, nothing heard: a scan */
    job_done(0, 0, NULL);
    m.now += 200;
    ws_tick(&m);
    CHECK(!strcmp(m.status, "No networks in range") && !m.said_scan);
    /* a join's words stay when what was heard goes */
    fake_reset();
    m.now += WS_READ_MS;
    ws_tick(&m);
    CHECK(m.nnets == 6 && ws_page(&m, WS_NETWORK) == WS_DRAW && ws_page(&m, WS_WIFI) == WS_DRAW && m.scanned == 1);
    CHECK(ws_join(&m, row("SurfSim")) == WS_DRAW && m.job && !strcmp(m.status, "Joining SurfSim..."));
    job_done(0, 0, NULL);
    m.now += 200;
    ws_tick(&m);
    CHECK(!strcmp(m.status, "Joined SurfSim, but no address came (DHCP)"));
    k.bss.count = 0;
    m.now += WS_READ_MS;
    CHECK(ws_tick(&m) == WS_DRAW && m.nnets == 0 && !strcmp(m.status, "Joined SurfSim, but no address came (DHCP)"));
}

static void join_wpa2(void) {
    char pass[64], line[WS_LINE];
    int t;
    start();
    started();
    t = row("SurfSimWPA");
    CHECK(ws_join(&m, t) == WS_DRAW && m.ask.kind == WS_ASK_PASSWORD && !m.job && k.starts == 1);
    CHECK(!strcmp(m.ask.text, "Password for SurfSimWPA (WPA2-Personal):") && m.ask.save == 0);   /* unticked (M3) */
    CHECK(!strcmp(ws_save_label(&m), "Save it: SurfOS joins it by itself at every start"));
    CHECK(!strcmp(m.ask.yes, "Join") && !strcmp(m.ask.no, "Cancel"));
    strcpy(pass, "short");                                          /* 8 to 63 characters: the box stays */
    CHECK(ws_join_answer(&m, NULL, 0, pass, 1) == WS_DRAW && m.ask.kind == WS_ASK_PASSWORD && k.starts == 1);
    CHECK(!memcmp(pass, "\0\0\0\0\0", 5));                          /* wiped all the same */
    CHECK(!strncmp(m.ask.text, "A WPA2 password is 8 to 63 characters", 37));
    memset(pass, 'x', 63);
    pass[63] = 0;
    strcpy(pass + 10, "\x7f");                                       /* a control in it */
    CHECK(ws_join_answer(&m, NULL, 0, pass, 1) == WS_DRAW && m.ask.kind == WS_ASK_PASSWORD && !pass[0]);
    strcpy(pass, PASSPHRASE);
    CHECK(ws_join_answer(&m, NULL, 0, pass, 1) == WS_DRAW);
    CHECK(m.ask.kind == WS_ASK_NONE && m.job > 0 && k.last_op == NETCTL_JOIN && !pass[0] && !pass[5]);
    CHECK(!strcmp(k.last_join.passphrase, PASSPHRASE) && k.last_join.flags == NETCTL_JOIN_SAVE);
    CHECK(k.last_join.ssid_len == 10 && !memcmp(k.last_join.ssid, "SurfSimWPA", 10));
    CHECK(!in_model(PASSPHRASE) && !in_model("surfsim-wpa2"));      /* nowhere in the model, its words or its stack */
    CHECK(!strcmp(m.status, "Joining SurfSimWPA..."));
    CHECK(ws_join(&m, row("SurfSim")) == WS_DRAW && !strcmp(m.status, "Wait: the last change is still under way"));
    CHECK(ws_scan(&m) == WS_DRAW && k.starts == 2);
    k.job.stage = NETCTL_STAGE_SAVING;
    m.now += 200;
    CHECK(ws_tick(&m) == WS_DRAW && !strcmp(m.status, "Saving SurfSimWPA on the stick..."));
    m.now += 200;
    CHECK(ws_tick(&m) == 0);
    k.job.stage = NETCTL_STAGE_ADDRESS;
    m.now += 200;
    CHECK(ws_tick(&m) == WS_DRAW && !strcmp(m.status, "Joined SurfSimWPA; asking for an address (DHCP)..."));
    k.wifi.state = NETCTL_STA_UP;                                   /* the kernel joined it */
    memcpy(k.wifi.ssid, "SurfSimWPA", 10);
    k.wifi.ssid_len = 10;
    k.wifi.security = NETCTL_SEC_WPA2;
    k.wifi.ip = ip4(10, 0, 2, 15);
    add_saved("SurfSimWPA", NETCTL_SAVED_WPA2, 0);
    job_done(0, NETCTL_DONE_ADDRESS | NETCTL_DONE_SAVED, NULL);
    m.now += 200;
    CHECK(ws_tick(&m) == WS_DRAW && !m.job);
    CHECK(!strcmp(m.status, "Joined SurfSimWPA: address 10.0.2.15; saved: SurfOS joins it by itself at startup"));
    CHECK(m.nets[row("SurfSimWPA")].joined && m.nets[row("SurfSimWPA")].saved && ws_joined_saved(&m));
    ws_wifi_line(&m, line, sizeof(line));
    CHECK(!strcmp(line, "Joined SurfSimWPA (WPA2-Personal): address 10.0.2.15"));
    CHECK(!ws_can_join(&m, row("SurfSimWPA")));
    CHECK(ws_join(&m, row("SurfSimWPA")) == WS_DRAW && !strcmp(m.status, "Joined to SurfSimWPA already"));
    /* the password was wrong: the kernel says so */
    k.wifi.state = NETCTL_STA_IDLE;
    m.now += WS_READ_MS;
    ws_tick(&m);
    CHECK(ws_join(&m, row("SurfSimWPA")) == WS_DRAW && m.ask.kind == WS_ASK_PASSWORD);
    strcpy(pass, "not-the-surfsim-passphrase");
    CHECK(ws_join_answer(&m, NULL, 0, pass, 0) == WS_DRAW && m.job && k.last_join.flags == 0);
    job_done(-13, 0, NULL);
    m.now += 200;
    CHECK(ws_tick(&m) == WS_DRAW);
    CHECK(!strcmp(m.status, "Could not join SurfSimWPA: the 4-way handshake failed: the password may be wrong"));
    /* joined, but the stick may not be written, and no address came */
    CHECK(ws_join(&m, row("SurfSimWPA")) == WS_DRAW);
    strcpy(pass, PASSPHRASE);
    CHECK(ws_join_answer(&m, NULL, 0, pass, 1) == WS_DRAW);
    job_done(0, NETCTL_DONE_NOT_SAVED, "SurfOS was started from the flashed SURFOSUSB stick (usb0), which it only reads");
    m.now += 200;
    ws_tick(&m);
    CHECK(!strcmp(m.status, "Joined SurfSimWPA, but no address came (DHCP); not saved: SurfOS was started from the "
                            "flashed SURFOSUSB stick (usb0), which it only reads"));
    CHECK(!in_model(PASSPHRASE));
}

static void join_open_and_other(void) {
    char pass[64];
    start();
    k.wifi.exposed = NETCTL_EXPOSED_TELNET | NETCTL_EXPOSED_FTP;
    started();
    CHECK(ws_join(&m, row("SurfSim")) == WS_DRAW && m.ask.kind == WS_ASK_OPEN && !m.job);
    CHECK(!strncmp(m.ask.text, "A telnet server runs or is to start, and it has no password", 59));
    CHECK(strstr(m.ask.text, "An FTP server runs") && strstr(m.ask.text, "Join SurfSim all the same?"));
    CHECK(ws_answer(&m, 0) == WS_DRAW && m.ask.kind == WS_ASK_NONE && !m.job);     /* Cancel: nothing */
    CHECK(ws_join(&m, row("SurfSim")) == WS_DRAW && ws_answer(&m, 1) == WS_DRAW && m.job);
    CHECK(k.last_op == NETCTL_JOIN && !k.last_join.passphrase[0] && k.last_join.ssid_len == 7);
    job_done(0, NETCTL_DONE_ADDRESS, NULL);
    k.wifi.state = NETCTL_STA_UP;
    k.wifi.ip = ip4(10, 0, 2, 15);
    m.now += 200;
    ws_tick(&m);
    CHECK(!strcmp(m.status, "Joined SurfSim: address 10.0.2.15"));
    k.wifi.exposed = 0;                                             /* no server: at once */
    k.wifi.state = NETCTL_STA_IDLE;
    m.now += WS_READ_MS;
    ws_tick(&m);
    CHECK(ws_join(&m, row("SurfSimFlaky")) == WS_DRAW && m.ask.kind == WS_ASK_NONE && m.job);
    job_done(0, 0, NULL);
    m.now += 200;
    ws_tick(&m);
    /* by its name: a hidden one */
    CHECK(ws_join_other(&m) == WS_DRAW && m.ask.kind == WS_ASK_OTHER && !m.ask.ssid_len);
    pass[0] = 0;
    CHECK(ws_join_answer(&m, "", 0, pass, 0) == WS_DRAW && m.ask.kind == WS_ASK_OTHER);
    CHECK(!strncmp(m.ask.text, "A network's name is 1 to 32 bytes", 33));
    CHECK(ws_join_answer(&m, "0123456789abcdef0123456789abcdefX", 33, pass, 0) == WS_DRAW && m.ask.kind == WS_ASK_OTHER);
    strcpy(pass, "short");
    CHECK(ws_join_answer(&m, "SurfSimHidden", 13, pass, 0) == WS_DRAW && m.ask.kind == WS_ASK_OTHER && !pass[0]);
    CHECK(ws_join_answer(&m, "SurfSimHidden", 13, pass, 1) == WS_DRAW && m.ask.kind == WS_ASK_NONE && m.job);
    CHECK(k.last_join.ssid_len == 13 && !memcmp(k.last_join.ssid, "SurfSimHidden", 13) && !k.last_join.passphrase[0]);
    CHECK(k.last_join.flags == NETCTL_JOIN_SAVE && !strcmp(m.status, "Looking for SurfSimHidden..."));  /* no row */
    k.job.stage = NETCTL_STAGE_JOINING;                             /* the kernel's scan did not hear it: hidden */
    m.now += 200;
    CHECK(ws_tick(&m) == WS_DRAW && !strcmp(m.status, "Joining SurfSimHidden..."));
    job_done(-2, 0, NULL);
    m.now += 200;
    ws_tick(&m);
    CHECK(!strcmp(m.status, "Could not join SurfSimHidden: no network of that name is in range"));
    /* by name, no password, while a server without one runs: asked first, and Save it kept for the join */
    k.wifi.exposed = NETCTL_EXPOSED_FTP;
    m.now += WS_READ_MS;
    ws_tick(&m);
    CHECK(ws_join_other(&m) == WS_DRAW);
    pass[0] = 0;
    CHECK(ws_join_answer(&m, "Cafe", 4, pass, 1) == WS_DRAW && m.ask.kind == WS_ASK_OPEN && !m.job);
    CHECK(!strncmp(m.ask.text, "An FTP server runs, and it has no password", 42) && strstr(m.ask.text, "Join Cafe all"));
    CHECK(ws_answer(&m, 1) == WS_DRAW && m.job && k.last_join.ssid_len == 4 &&
          k.last_join.flags == (NETCTL_JOIN_SAVE | NETCTL_JOIN_EXPOSED));     /* the person's word, from that answer */
    job_done(0, NETCTL_DONE_SAVED, NULL);
    m.now += 200;
    ws_tick(&m);
    k.wifi.exposed = 0;
    m.now += WS_READ_MS;
    ws_tick(&m);
    /* what the job's own scan found, in the kernel's words */
    CHECK(ws_join_other(&m) == WS_DRAW);
    pass[0] = 0;
    CHECK(ws_join_answer(&m, "Cafe2", 5, pass, 0) == WS_DRAW && m.job && !strcmp(m.status, "Looking for Cafe2..."));
    job_done(-13, 0, "it is a WPA2 network: join it with its password");
    m.now += 200;
    ws_tick(&m);
    CHECK(!strcmp(m.status, "Could not join Cafe2: it is a WPA2 network: join it with its password"));
    /* the kernel's refusals: a password for an open network; busy */
    CHECK(ws_join_other(&m) == WS_DRAW);
    strcpy(pass, PASSPHRASE);
    k.refuse = -22;
    CHECK(ws_join_answer(&m, "SurfSim", 7, pass, 0) == WS_DRAW && !m.job && !pass[0]);
    CHECK(!strncmp(m.status, "SurfOS did not take that: SurfSim is an open network (it takes no password)", 75));
    k.busy_other = 1;                                               /* another program's job */
    CHECK(ws_join(&m, row("SurfSimFlaky")) == WS_DRAW && !m.job);
    CHECK(!strncmp(m.status, "Busy: another program", 21));
    k.busy_other = 0;
    k.wifi.up = 0;                                                  /* the card off */
    m.now += WS_READ_MS;
    ws_tick(&m);
    CHECK(!ws_can_join(&m, row("SurfSim")) && ws_join(&m, row("SurfSim")) == WS_DRAW && !m.job);
    CHECK(!strcmp(m.status, "The Wi-Fi card is off: turn it on first"));
    CHECK(ws_join_other(&m) == WS_DRAW && m.ask.kind == WS_ASK_NONE);
    CHECK(!in_model(PASSPHRASE));
}

static void card_leave_save_forget(void) {
    char line[WS_LINE], name[WS_NAME], state[WS_LINE];
    start();
    add_saved("SurfSim", NETCTL_SAVED_OPEN, 0);
    add_saved("Home", NETCTL_SAVED_WPA2, 1);
    started();
    CHECK(ws_card(&m, 0) == WS_DRAW && k.last_req.on == 0 && !strcmp(m.status, "Turning the Wi-Fi card off..."));
    k.wifi.up = 0;
    k.wifi.state = NETCTL_STA_DOWN;
    k.bss.count = 0;
    job_done(0, 0, NULL);
    m.now += 200;
    ws_tick(&m);
    CHECK(!strcmp(m.status, "The Wi-Fi card is off") && m.nnets == 0);
    ws_wifi_line(&m, line, sizeof(line));
    CHECK(!strcmp(line, "The Wi-Fi card is off"));
    CHECK(ws_card(&m, 1) == WS_DRAW && k.last_req.on == 1 && !strcmp(m.status, "Starting the Wi-Fi card..."));
    k.job.stage = NETCTL_STAGE_CONNECTING;
    m.now += 200;
    CHECK(ws_tick(&m) == WS_DRAW && !strcmp(m.status, "Looking for a saved network in range..."));
    k.wifi.up = 1;
    k.wifi.state = NETCTL_STA_UP;
    memcpy(k.wifi.ssid, "SurfSim", 7);
    k.wifi.ssid_len = 7;
    k.wifi.ip = ip4(10, 0, 2, 16);
    job_done(0, NETCTL_DONE_JOINED | NETCTL_DONE_ADDRESS, NULL);
    m.now += 200;
    ws_tick(&m);
    CHECK(!strcmp(m.status, "The Wi-Fi card is on: joined SurfSim, address 10.0.2.16"));
    CHECK(m.job && k.last_op == NETCTL_SCAN && m.job_quiet);       /* nothing heard: a scan by itself */
    add_bss("SurfSim", NETCTL_SEC_OPEN, -45, 6);
    job_done(0, 0, NULL);
    m.now += 200;
    ws_tick(&m);
    CHECK(!m.job && m.nnets == 1 && !strcmp(m.status, "1 network in range"));
    ws_saved_row(&m, 0, state, sizeof(state));
    CHECK(!strcmp(state, "SurfSim\topen\tjoined now"));
    ws_saved_row(&m, 1, state, sizeof(state));
    CHECK(!strcmp(state, "Home\tWPA2\thidden"));
    (void)name;
    k.saved.net[1].refused = 1;
    m.now += WS_READ_MS;
    ws_tick(&m);
    ws_saved_row(&m, 1, state, sizeof(state));
    CHECK(!strcmp(state, "Home\tWPA2\tits key was refused: join it again, then save it"));
    /* the card on, none in range */
    k.wifi.up = 0;
    m.now += WS_READ_MS;
    ws_tick(&m);
    CHECK(ws_card(&m, 1) == WS_DRAW);
    k.wifi.up = 1;
    job_done(0, NETCTL_DONE_NONE_NEAR, NULL);
    m.now += 200;
    ws_tick(&m);
    CHECK(!strcmp(m.status, "The Wi-Fi card is on; none of the saved networks is in range"));
    CHECK(!m.job);                                                  /* SurfSim is heard: no scan by itself */
    /* leave, save */
    CHECK(ws_leave(&m) == WS_DRAW && !strcmp(m.status, "Leaving SurfSim..."));
    k.wifi.state = NETCTL_STA_IDLE;
    k.wifi.idle = NETCTL_IDLE_LEFT;
    job_done(0, 0, NULL);
    m.now += 200;
    ws_tick(&m);
    CHECK(!strcmp(m.status, "Left the network"));
    CHECK(ws_save_joined(&m) == WS_DRAW && !m.job && !strcmp(m.status, "The Wi-Fi card is not joined to a network"));
    k.wifi.state = NETCTL_STA_UP;
    memcpy(k.wifi.ssid, "Cafe", 4);
    k.wifi.ssid_len = 4;
    m.now += WS_READ_MS;
    ws_tick(&m);
    CHECK(!ws_joined_saved(&m) && ws_save_joined(&m) == WS_DRAW && k.last_op == NETCTL_SAVE);
    CHECK(!strcmp(m.status, "Saving Cafe on the stick..."));
    job_done(0, 0, NULL);
    m.now += 200;
    ws_tick(&m);
    CHECK(!strcmp(m.status, "Saved Cafe: SurfOS joins it by itself at startup"));
    strcpy(setting("wifi")->value, "off");
    m.now += WS_READ_MS;
    ws_tick(&m);
    CHECK(ws_save_joined(&m) == WS_DRAW);
    job_done(-30, 0, "SurfOS was started from the flashed SURFOSUSB stick (usb0), which it only reads");
    m.now += 200;
    ws_tick(&m);
    CHECK(!strcmp(m.status, "Cafe not saved: SurfOS was started from the flashed SURFOSUSB stick (usb0), which it only "
                            "reads"));
    CHECK(ws_save_joined(&m) == WS_DRAW);
    job_done(0, 0, NULL);
    m.now += 200;
    ws_tick(&m);
    CHECK(!strcmp(m.status, "Saved Cafe; SurfOS joins none by itself while joining at startup is off"));
    /* forget: asked first */
    CHECK(ws_select_saved(&m, 1) == WS_DRAW && ws_saved_selected(&m) == 1);
    CHECK(ws_forget(&m, 1) == WS_DRAW && m.ask.kind == WS_ASK_FORGET && !m.job);
    CHECK(!strncmp(m.ask.text, "Forget Home? SurfOS will no longer join it by itself", 52));
    CHECK(ws_answer(&m, 1) == WS_DRAW && m.job && k.last_op == NETCTL_FORGET && k.last_req.ssid_len == 4);
    CHECK(!memcmp(k.last_req.ssid, "Home", 4) && !strcmp(m.status, "Writing the settings on the stick..."));
    k.saved.count = 1;
    job_done(0, 0, NULL);
    m.now += 200;
    ws_tick(&m);
    CHECK(!strcmp(m.status, "Forgot Home: SurfOS no longer joins it by itself") && ws_saved_selected(&m) == -1);
    CHECK(ws_forget(&m, 0) == WS_DRAW && ws_answer(&m, 1) == WS_DRAW);
    job_done(-2, 0, NULL);
    m.now += 200;
    ws_tick(&m);
    CHECK(!strcmp(m.status, "SurfSim is not a saved network"));
    CHECK(ws_forget(&m, 5) == 0 && ws_forget(&m, -1) == 0);
}

static void stepping(void);

static void settings_pages(void) {
    const struct netctl_setting *s;
    char choice[WS_CHOICES][NETCTL_VALUE_MAX], why[WS_LINE];
    start();
    started();
    CHECK(ws_page(&m, WS_SYSTEM) == WS_DRAW && ws_page(&m, WS_SYSTEM) == 0 && ws_page(&m, 7) == 0);
    CHECK(!ws_setting(&m, "desktop.theme") && !ws_setting(&m, "nosuch") && ws_is_on(&m, "wifi") && !ws_is_on(&m, "history"));
    /* telnetd only after its warning */
    CHECK(ws_toggle(&m, "telnetd") == WS_DRAW && m.ask.kind == WS_ASK_TELNETD && !m.job);
    CHECK(!strncmp(m.ask.text, "telnetd has no password and no encryption: whoever reaches the laptop", 69));
    CHECK(strstr(m.ask.text, "Save only your own networks.") != NULL);
    CHECK(ws_answer(&m, 0) == WS_DRAW && !m.job);
    CHECK(ws_toggle(&m, "telnetd") == WS_DRAW && ws_answer(&m, 1) == WS_DRAW && m.job && k.last_op == NETCTL_SET);
    CHECK(!strcmp(k.last_req.key, "telnetd") && !strcmp(k.last_req.value, "on"));
    strcpy(setting("telnetd")->value, "on");
    job_done(0, 0, NULL);
    m.now += 200;
    ws_tick(&m);
    CHECK(!strcmp(m.status, "telnetd = on, saved on the stick: at the next start (telnetd on in a Terminal starts it "
                            "now)"));
    CHECK(ws_toggle(&m, "telnetd") == WS_DRAW && m.ask.kind == WS_ASK_NONE && !strcmp(k.last_req.value, "off"));
    job_done(0, NETCTL_DONE_NOT_SAVED, "usb1 is write-protected (its switch)");
    m.now += 200;
    ws_tick(&m);
    CHECK(!strcmp(m.status, "telnetd = off not saved: usb1 is write-protected (its switch); nothing changes"));
    CHECK(ws_toggle(&m, "history") == WS_DRAW && !strcmp(k.last_req.value, "on"));
    job_done(0, 0, NULL);
    m.now += 200;
    ws_tick(&m);
    CHECK(!strncmp(m.status, "history = on, saved on the stick: from now on surfsh saves its history there", 76));
    CHECK(ws_toggle(&m, "ethernet") == WS_DRAW && !strcmp(k.last_req.key, "ethernet") && !strcmp(k.last_req.value, "dhcp"));
    job_done(0, NETCTL_DONE_NOT_SAVED, "usb1 is write-protected (its switch)");
    m.now += 200;
    ws_tick(&m);
    CHECK(!strcmp(m.status, "ethernet = dhcp is in force until SurfOS restarts, but not saved: usb1 is write-protected "
                            "(its switch) (System > Save again, once the stick takes it)"));
    CHECK(ws_toggle(&m, "wifi") == WS_DRAW && !strcmp(k.last_req.value, "off"));
    job_done(-22, 0, "wifi is on | off, not maybe");
    m.now += 200;
    ws_tick(&m);
    CHECK(!strcmp(m.status, "wifi = off not set: wifi is on | off, not maybe"));
    CHECK(ws_set(&m, "wifi", "on") == 0 && !m.job);                /* as it is: nothing asked */
    CHECK(ws_set(&m, "nosuch", "on") == WS_DRAW && !strcmp(m.status, "This SurfOS has no setting called nosuch"));
    /* under settings=off: for the next start */
    k.settings.off = 1;
    m.now += WS_READ_MS;
    ws_tick(&m);
    CHECK(ws_toggle(&m, "wifi") == WS_DRAW);
    job_done(0, 0, NULL);
    m.now += 200;
    ws_tick(&m);
    CHECK(!strcmp(m.status, "wifi = off, saved on the stick: from the next start (this one does not read the settings)"));
    /* the host name's rule */
    CHECK(!ws_host_ok("", why, sizeof(why)) && !ws_host_ok("-a", why, sizeof(why)) && !ws_host_ok("a-", why, sizeof(why)));
    CHECK(!ws_host_ok("my laptop", why, sizeof(why)) && !strncmp(why, "A host name is letters, digits and - only", 41));
    CHECK(!ws_host_ok("caf\xc3\xa9", why, sizeof(why)) && !ws_host_ok("a.b", why, sizeof(why)));
    CHECK(!ws_host_ok("0123456789012345678901234567890123", why, sizeof(why)));
    CHECK(ws_host_ok("01234567890123456789012345678901", why, sizeof(why)) && ws_host_ok("a", why, sizeof(why)));
    CHECK(ws_host_ok("surf-laptop2", why, sizeof(why)));
    CHECK(ws_set_host(&m, "my laptop") == WS_DRAW && !m.job && !strncmp(m.status, "A host name is letters", 22));
    CHECK(ws_set_host(&m, "surf-laptop") == WS_DRAW && m.job && !strcmp(k.last_req.key, "hostname"));
    CHECK(!strcmp(k.last_req.value, "surf-laptop"));
    k.settings.off = 0;
    strcpy(setting("hostname")->value, "surf-laptop");
    job_done(0, 0, NULL);
    m.now += 200;
    ws_tick(&m);
    CHECK(!strcmp(m.status, "hostname = surf-laptop, saved on the stick: in the next DHCP request"));
    /* the desktop's keys, once the table has them */
    add_setting("desktop.theme", "ocean", "ocean | dusk");
    add_setting("pointer.speed", "normal", "slow | normal | fast");
    m.now += WS_READ_MS;
    CHECK(ws_page(&m, WS_DESKTOP) == WS_DRAW);
    CHECK(ws_tick(&m) == WS_DRAW);
    s = ws_setting(&m, "desktop.theme");
    CHECK(s && ws_choices(s, choice, WS_CHOICES) == 2 && !strcmp(choice[0], "ocean") && !strcmp(choice[1], "dusk"));
    s = ws_setting(&m, "pointer.speed");
    CHECK(s && ws_choices(s, choice, WS_CHOICES) == 3 && !strcmp(choice[2], "fast") && !strcmp(choice[1], "normal"));
    CHECK(ws_choices(ws_setting(&m, "telnetd.port"), choice, WS_CHOICES) == 1 && !strcmp(choice[0], "1-65535"));
    CHECK(ws_choices(NULL, choice, WS_CHOICES) == 0 && ws_choices(s, choice, 1) == 1);
    stepping();
}

/* the desktop's theme and pointer speed, stepped through: each step in force at once, nothing written, and the last one
   written by one job once the steps stop; before the window closes too */
static void stepping(void) {
    int starts = k.starts, r;
    CHECK(ws_choose(&m, "desktop.theme", "dusk") == WS_DRAW && k.applies == 1 && !m.job && k.starts == starts);
    CHECK(!strcmp(k.last_apply.key, "desktop.theme") && !strcmp(k.last_apply.value, "dusk"));
    CHECK(!strcmp(ws_setting(&m, "desktop.theme")->value, "dusk"));
    CHECK(!strcmp(m.status, "desktop.theme = dusk: in force now, and written on the stick once the steps stop"));
    m.now += 300;
    CHECK(ws_choose(&m, "desktop.theme", "ocean") == WS_DRAW && ws_choose(&m, "desktop.theme", "dusk") == WS_DRAW);
    CHECK(ws_choose(&m, "desktop.theme", "dusk") == 0 && k.applies == 3);   /* as it is: nothing */
    m.now += WS_WRITE_AFTER_MS - 1;
    ws_tick(&m);
    CHECK(!m.job && k.starts == starts);                /* the steps have not stopped long enough */
    m.now += 1;
    CHECK(ws_tick(&m) == WS_DRAW && m.job && k.starts == starts + 1 && k.last_op == NETCTL_SET);
    CHECK(!strcmp(k.last_req.key, "desktop.theme") && !strcmp(k.last_req.value, "dusk"));   /* the last step, once */
    job_done(0, 0, NULL);
    m.now += 200;
    ws_tick(&m);
    CHECK(!m.job && !strcmp(m.status, "desktop.theme = dusk, saved on the stick: now"));
    m.now += 5000;
    ws_tick(&m);
    CHECK(k.starts == starts + 1);                      /* written once: nothing more */
    /* two keys stepped: each written once its own steps stop, one job after the other */
    CHECK(ws_choose(&m, "pointer.speed", "fast") == WS_DRAW);
    m.now += 100;
    CHECK(ws_choose(&m, "desktop.theme", "ocean") == WS_DRAW && k.applies == 5);
    m.now += WS_WRITE_AFTER_MS;
    ws_tick(&m);
    CHECK(m.job && !strcmp(k.last_req.key, "pointer.speed") && !strcmp(k.last_req.value, "fast"));
    m.now += 200;
    ws_tick(&m);
    CHECK(k.starts == starts + 2);                      /* the theme's waits for that job */
    job_done(0, 0, NULL);
    m.now += 200;
    ws_tick(&m);
    CHECK(m.job && !strcmp(k.last_req.key, "desktop.theme") && !strcmp(k.last_req.value, "ocean") && k.starts == starts + 3);
    job_done(0, 0, NULL);
    m.now += 200;
    ws_tick(&m);
    /* another program's job when the time comes: the step waits, and is written later */
    CHECK(ws_choose(&m, "desktop.theme", "dusk") == WS_DRAW);
    k.busy_other = 1;
    m.now += WS_WRITE_AFTER_MS;
    ws_tick(&m);
    CHECK(!m.job && m.step[0].key[0] + m.step[1].key[0] != 0);
    k.busy_other = 0;
    m.now += WS_WRITE_AFTER_MS;
    ws_tick(&m);
    CHECK(m.job && !strcmp(k.last_req.value, "dusk"));
    job_done(0, 0, NULL);
    m.now += 200;
    ws_tick(&m);
    /* closing with a step not yet written: its job starts, and the window closes */
    CHECK(ws_choose(&m, "desktop.theme", "ocean") == WS_DRAW);
    r = ws_close(&m, NULL);
    CHECK(r == WS_CLOSE && m.job && !strcmp(k.last_req.value, "ocean"));
    job_done(0, 0, NULL);
    m.now += 200;
    ws_tick(&m);
    /* closing while a job of this program's runs: the step written after it, then closed */
    CHECK(ws_choose(&m, "pointer.speed", "slow") == WS_DRAW);
    CHECK(ws_scan(&m) == WS_DRAW && m.job);
    CHECK(ws_close(&m, NULL) == WS_DRAW && m.closing && !strcmp(m.status, "Writing the last change on the stick, then "
                                                                          "closing..."));
    m.now += 200;
    CHECK(ws_tick(&m) != WS_CLOSE);                     /* the scan still runs */
    job_done(0, 0, NULL);
    m.now += 200;
    CHECK(ws_tick(&m) == WS_CLOSE && m.job && !strcmp(k.last_req.key, "pointer.speed"));
    job_done(0, 0, NULL);
    m.closing = 0;
    m.now += 200;
    ws_tick(&m);
    /* a kernel without the step: written at once, as before */
    k.apply_r = -38;
    CHECK(ws_choose(&m, "desktop.theme", "dusk") == WS_DRAW && m.job && k.last_op == NETCTL_SET);
    job_done(0, 0, NULL);
    m.now += 200;
    ws_tick(&m);
    /* settings=off: not in force, written for the next start */
    k.apply_r = 1;
    CHECK(ws_choose(&m, "pointer.speed", "normal") == WS_DRAW && !m.job);
    CHECK(!strcmp(m.status, "pointer.speed = normal: written on the stick for the next start (this one does not use the "
                            "settings)"));
    m.now += WS_WRITE_AFTER_MS;
    ws_tick(&m);
    CHECK(m.job && !strcmp(k.last_req.value, "normal"));
    job_done(0, 0, NULL);
    k.apply_r = 0;
    m.now += 200;
    ws_tick(&m);
}

static void network_and_about(void) {
    char a[WS_LINE], b[WS_LINE], c[WS_LINE], label[32], value[WS_LINE];
    start();
    started();
    CHECK(ws_page(&m, WS_NETWORK) == WS_DRAW);
    ws_iface_lines(&m, 0, a, sizeof(a), b, sizeof(b), c, sizeof(c));
    CHECK(!strcmp(a, "eth0 (Ethernet): link up, MAC 52:54:00:00:00:00"));
    CHECK(!strcmp(b, "Address 10.0.2.15, netmask 255.255.255.0, gateway 10.0.2.2, DNS 10.0.2.3"));
    CHECK(!strcmp(c, "DHCP lease from 10.0.2.2: 23 h 58 min left of 1 day 0 h"));
    ws_iface_lines(&m, 1, a, sizeof(a), b, sizeof(b), c, sizeof(c));
    CHECK(!strcmp(a, "wlan0 (Wi-Fi): link down, MAC 00:00:00:00:00:00") && !strcmp(b, "No address") && !c[0]);
    ws_iface_lines(&m, 2, a, sizeof(a), b, sizeof(b), c, sizeof(c));
    CHECK(!a[0] && !b[0] && !c[0]);
    k.ifaces.iface[0].lease_left -= 1;                              /* the lease counts down: the page shows it */
    m.now += WS_READ_MS;
    CHECK(ws_tick(&m) == WS_DRAW);
    k.ifaces.iface[1].ip = ip4(10, 0, 2, 16);                       /* set by hand */
    k.ifaces.iface[1].netmask = ip4(255, 0, 0, 0);
    m.now += WS_READ_MS;
    ws_tick(&m);
    ws_iface_lines(&m, 1, a, sizeof(a), b, sizeof(b), c, sizeof(c));
    CHECK(!strcmp(b, "Address 10.0.2.16, netmask 255.0.0.0, gateway none, DNS none"));
    CHECK(!strcmp(c, "Set by hand (no lease)"));
    k.bss.bss[0].rssi = -20;                                        /* the Wi-Fi page's news: not this page's */
    m.now += WS_READ_MS;
    CHECK(ws_tick(&m) == 0);
    /* About */
    CHECK(ws_page(&m, WS_ABOUT) == WS_DRAW);
    ws_about_line(&m, 0, label, sizeof(label), value, sizeof(value));
    CHECK(!strcmp(label, "Version") && !strcmp(value, "SurfOS 1.0"));
    ws_about_line(&m, 1, label, sizeof(label), value, sizeof(value));
    CHECK(!strcmp(value, "none recorded (/initrd/etc/build-id)"));
    ws_about_line(&m, 2, label, sizeof(label), value, sizeof(value));
    CHECK(!strcmp(value, "not known"));
    strcpy(m.about.build, "66a598370000-dirty 2026-11-05T12:00:00Z");
    strcpy(m.about.cpu, "QEMU Virtual CPU version 2.5+");
    m.about.have_mem = 1;
    m.about.mem_kb = 262144 - 1024;
    m.about.free_kb = 180000;
    m.about.programs_kb = 5120;
    m.about.heap_kb = 900;
    m.about.uptime_s = 3725;
    ws_about_line(&m, 1, label, sizeof(label), value, sizeof(value));
    CHECK(!strcmp(label, "Build") && !strcmp(value, "66a598370000-dirty 2026-11-05T12:00:00Z"));
    ws_about_line(&m, 2, label, sizeof(label), value, sizeof(value));
    CHECK(!strcmp(label, "Memory") && !strcmp(value, "255.0 MB, 175.7 MB of it free"));
    ws_about_line(&m, 3, label, sizeof(label), value, sizeof(value));
    CHECK(!strcmp(value, "5.0 MB in programs, 900 KB in the kernel's heap"));
    ws_about_line(&m, 4, label, sizeof(label), value, sizeof(value));
    CHECK(!strcmp(label, "Up for") && !strcmp(value, "1:02:05"));
    m.about.uptime_s = 2 * 86400 + 59;
    ws_about_line(&m, 4, label, sizeof(label), value, sizeof(value));
    CHECK(!strcmp(value, "2 days 0:00:59"));
    ws_about_line(&m, 5, label, sizeof(label), value, sizeof(value));
    CHECK(!strcmp(label, "Processor") && !strcmp(value, "QEMU Virtual CPU version 2.5+"));
    ws_about_line(&m, 6, label, sizeof(label), value, sizeof(value));
    CHECK(!label[0] && !value[0]);
}

static void without_the_call(void) {
    char line[WS_LINE];
    start();
    k.no_call = 1;
    CHECK(ws_tick(&m) == WS_DRAW && m.no_call && !m.have_wifi && !m.job);
    ws_wifi_line(&m, line, sizeof(line));
    CHECK(!strcmp(line, "This SurfOS has no network settings for programs (SYS_netctl)"));
    CHECK(ws_card(&m, 1) == WS_DRAW && !strcmp(m.status, "This SurfOS has no network settings for programs (SYS_netctl)"));
    CHECK(ws_toggle(&m, "wifi") == WS_DRAW && !strcmp(m.status, "This SurfOS has no setting called wifi"));
    /* no Wi-Fi card at all */
    start();
    k.wifi.present = 0;
    k.bss.count = 0;
    CHECK(ws_tick(&m) == WS_DRAW && !m.job);                        /* no scan without a card */
    ws_wifi_line(&m, line, sizeof(line));
    CHECK(!strcmp(line, "No Wi-Fi card (no driver found one)"));
}

static void closing_and_lost_jobs(void) {
    start();
    started();
    CHECK(ws_close(&m, "surfos") == WS_CLOSE && ws_close(&m, NULL) == WS_CLOSE && ws_close(&m, "") == WS_CLOSE);
    CHECK(ws_close(&m, "laptop") == WS_DRAW && m.ask.kind == WS_ASK_CLOSE);
    CHECK(!strcmp(m.ask.text, "The host name laptop was typed but not set. Close all the same?"));
    CHECK(ws_close(&m, "laptop") == WS_CLOSE);                      /* the close button again: as Close */
    CHECK(ws_answer(&m, 0) == WS_DRAW && m.ask.kind == WS_ASK_NONE);
    CHECK(ws_close(&m, "laptop") == WS_DRAW && ws_answer(&m, 1) == WS_CLOSE);
    /* the kernel's record moved on before this program looked */
    CHECK(ws_scan(&m) == WS_DRAW && m.job);
    k.job.seq += 5;
    m.now += 200;
    CHECK(ws_tick(&m) == WS_DRAW && !m.job);
    CHECK(!strcmp(m.status, "Done (its outcome was not seen: the kernel's record moved on)"));
    /* a scan that did not end in time */
    k.job.state = NETCTL_JOB_DONE;
    CHECK(ws_scan(&m) == WS_DRAW && m.job && !strcmp(m.status, "Looking for networks..."));
    job_done(0, NETCTL_DONE_PARTIAL, NULL);
    m.now += 200;
    ws_tick(&m);
    CHECK(!strcmp(m.status, "6 networks in range (the scan did not end in time: what was heard until then)"));
    CHECK(ws_scan(&m) == WS_DRAW);
    job_done(-16, 0, NULL);
    m.now += 200;
    ws_tick(&m);
    CHECK(!strcmp(m.status, "The scan failed: the Wi-Fi card was busy (the network manager or a wifi command); try "
                            "again in a moment"));
    {
        char t[64];
        ws_title(&m, t, sizeof(t));
        CHECK(!strcmp(t, "Settings - Wi-Fi"));
    }
}

/* the review's fixes (11/05/26): M2 an open join while a server without a password runs, M3 Save it unticked, L1 steps
   back where they began, L2 a question kept while a job runs, L4 settings saved again, L6 a job read that failed */
static void review_fixes(void) {
    char pass[64];
    int applies, starts;
    start();
    add_saved("Home", NETCTL_SAVED_WPA2, 0);
    add_setting("desktop.theme", "ocean", "ocean | dusk");
    started();
    /* M2: a server started since the last read: the kernel refuses (EPERM), and the model asks with what is true now */
    k.wifi.exposed = NETCTL_EXPOSED_TELNET;                         /* the model's copy still says none */
    CHECK(ws_join(&m, row("SurfSim")) == WS_DRAW && !m.job && m.ask.kind == WS_ASK_OPEN && k.starts == 2);
    CHECK(!k.last_join.flags);                                      /* the call refused had no word of the person's */
    CHECK(!strncmp(m.ask.text, "A telnet server runs or is to start, and it has no password", 59) &&
          strstr(m.ask.text, "Join SurfSim all the same?") != NULL);
    CHECK(!strcmp(m.status, "SurfSim is open, and a server without a password runs: asked first"));
    CHECK(ws_answer(&m, 1) == WS_DRAW && m.job && k.last_join.flags == NETCTL_JOIN_EXPOSED);
    job_done(0, NETCTL_DONE_EXPOSED, NULL);
    m.now += 200;
    ws_tick(&m);
    CHECK(!strcmp(m.status, "Joined SurfSim, but no address came (DHCP); it is open, and a server without a password "
                            "runs: anyone on it reaches that server"));
    /* the job's own refusal: a server started while it looked for a network not heard: asked again, Save it kept */
    k.wifi.exposed = 0;
    m.now += WS_READ_MS;
    ws_tick(&m);
    CHECK(ws_join_other(&m) == WS_DRAW);
    pass[0] = 0;
    CHECK(ws_join_answer(&m, "Cafe", 4, pass, 1) == WS_DRAW && m.job && k.last_join.flags == NETCTL_JOIN_SAVE);
    k.wifi.exposed = NETCTL_EXPOSED_FTP;
    job_done(-1, NETCTL_DONE_EXPOSED, "a server without a password runs (telnetd or ftpd), and anyone on an open "
             "network reaches it: not joined without the person's word");
    m.now += 200;
    ws_tick(&m);
    CHECK(!m.job && m.ask.kind == WS_ASK_OPEN && m.ask.save == 1 && strstr(m.ask.text, "An FTP server runs") != NULL);
    CHECK(ws_answer(&m, 1) == WS_DRAW && m.job && k.last_join.flags == (NETCTL_JOIN_SAVE | NETCTL_JOIN_EXPOSED));
    job_done(0, NETCTL_DONE_SAVED | NETCTL_DONE_EXPOSED, NULL);
    m.now += 200;
    ws_tick(&m);
    CHECK(strstr(m.status, "; saved: SurfOS joins it by itself at startup; it is open") != NULL);
    k.wifi.exposed = 0;
    m.now += WS_READ_MS;
    ws_tick(&m);
    /* M3: every join's Save it unticked; with telnetd on, or running, what saving means */
    CHECK(ws_join(&m, row("SurfSimWPA")) == WS_DRAW && m.ask.kind == WS_ASK_PASSWORD && m.ask.save == 0);
    snprintf(setting("telnetd")->value, sizeof(setting("telnetd")->value), "%s", "on");
    m.now += WS_READ_MS;
    ws_tick(&m);
    CHECK(!strcmp(ws_save_label(&m), "Save it: joined by itself at every start, and telnetd starts on it"));
    CHECK(ws_answer(&m, 0) == WS_DRAW && m.ask.kind == WS_ASK_NONE);
    snprintf(setting("telnetd")->value, sizeof(setting("telnetd")->value), "%s", "off");
    k.wifi.exposed = NETCTL_EXPOSED_TELNET;                         /* telnetd on in a Terminal: running */
    m.now += WS_READ_MS;
    ws_tick(&m);
    CHECK(ws_join_other(&m) == WS_DRAW && m.ask.save == 0);
    CHECK(!strcmp(ws_save_label(&m), "Save it: joined by itself at every start, and telnetd starts on it"));
    CHECK(ws_answer(&m, 0) == WS_DRAW);
    k.wifi.exposed = 0;
    m.now += WS_READ_MS;
    ws_tick(&m);
    CHECK(!strcmp(ws_save_label(&m), "Save it: SurfOS joins it by itself at every start"));
    /* L2: a job of this window's runs while the password box is open: the answer waits and the box stays */
    CHECK(ws_join(&m, row("SurfSimWPA")) == WS_DRAW && m.ask.kind == WS_ASK_PASSWORD && ws_answer_starts(&m));
    CHECK(ws_scan(&m) == WS_DRAW && m.job);
    strcpy(pass, PASSPHRASE);
    CHECK(ws_join_answer(&m, NULL, 0, pass, 0) == WS_DRAW && m.ask.kind == WS_ASK_PASSWORD && !pass[0]);
    CHECK(!strcmp(m.status, "Wait: the last change is still under way") && k.last_op == NETCTL_SCAN);
    job_done(0, 0, NULL);
    m.now += 200;
    ws_tick(&m);
    CHECK(!m.job && m.ask.kind == WS_ASK_PASSWORD);                 /* still asked, nothing started by itself */
    strcpy(pass, PASSPHRASE);
    CHECK(ws_join_answer(&m, NULL, 0, pass, 0) == WS_DRAW && m.job && k.last_op == NETCTL_JOIN && !k.last_join.flags);
    job_done(0, 0, NULL);
    m.now += 200;
    ws_tick(&m);
    CHECK(ws_forget(&m, 0) == WS_DRAW && m.ask.kind == WS_ASK_FORGET);
    CHECK(ws_scan(&m) == WS_DRAW && m.job);
    CHECK(ws_answer(&m, 1) == WS_DRAW && m.ask.kind == WS_ASK_FORGET && k.last_op == NETCTL_SCAN);   /* it stays */
    CHECK(ws_answer(&m, 0) == WS_DRAW && m.ask.kind == WS_ASK_NONE);    /* No needs no job */
    job_done(0, 0, NULL);
    m.now += 200;
    ws_tick(&m);
    /* L1: stepped away and back to the value of before: nothing written */
    applies = k.applies;
    starts = k.starts;
    CHECK(ws_choose(&m, "desktop.theme", "dusk") == WS_DRAW && k.applies == applies + 1);
    m.now += 300;
    CHECK(ws_choose(&m, "desktop.theme", "ocean") == WS_DRAW && k.applies == applies + 2);
    m.now += WS_WRITE_AFTER_MS;
    ws_tick(&m);
    CHECK(k.starts == starts && !m.job && !m.step[0].key[0] && !m.step[1].key[0]);
    CHECK(!strcmp(m.status, "desktop.theme = ocean, as it was before the steps: nothing written"));
    CHECK(ws_choose(&m, "desktop.theme", "dusk") == WS_DRAW);       /* a step that stays away is written */
    m.now += WS_WRITE_AFTER_MS;
    ws_tick(&m);
    CHECK(m.job && k.starts == starts + 1 && !strcmp(k.last_req.value, "dusk"));
    job_done(0, 0, NULL);
    m.now += 200;
    ws_tick(&m);
    /* L4: settings in force but not saved (the stick write-protected): Save again writes them one after the other */
    CHECK(ws_unsaved(&m) == 0 && ws_toggle(&m, "history") == WS_DRAW);
    snprintf(setting("history")->value, sizeof(setting("history")->value), "%s", "on");     /* in force all the same */
    job_done(0, NETCTL_DONE_NOT_SAVED, "usb1 is write-protected (its switch)");
    m.now += 200;
    ws_tick(&m);
    CHECK(ws_unsaved(&m) == 1 && ws_toggle(&m, "telnetd") == WS_DRAW && m.ask.kind == WS_ASK_TELNETD);
    CHECK(ws_answer(&m, 1) == WS_DRAW && m.job);
    job_done(0, NETCTL_DONE_NOT_SAVED, "usb1 is write-protected (its switch)");
    m.now += 200;
    ws_tick(&m);
    CHECK(ws_unsaved(&m) == 1);                                     /* telnetd not saved is not in force: not counted */
    CHECK(ws_toggle(&m, "ethernet") == WS_DRAW);
    snprintf(setting("ethernet")->value, sizeof(setting("ethernet")->value), "%s", "dhcp");
    job_done(0, NETCTL_DONE_NOT_SAVED, "usb1 is write-protected (its switch)");
    m.now += 200;
    ws_tick(&m);
    CHECK(ws_unsaved(&m) == 2 && !strcmp(m.unsaved[0], "history") && !strcmp(m.unsaved[1], "ethernet"));
    starts = k.starts;
    CHECK(ws_save_again(&m) == WS_DRAW && m.job && k.starts == starts + 1);
    CHECK(!strcmp(k.last_req.key, "history") && !strcmp(k.last_req.value, "on"));     /* the value in force */
    job_done(0, NETCTL_DONE_NOT_SAVED, "usb1 is write-protected (its switch)");      /* still: it stops there */
    m.now += 200;
    ws_tick(&m);
    CHECK(!m.job && !m.again && ws_unsaved(&m) == 2 && k.starts == starts + 1);
    CHECK(ws_save_again(&m) == WS_DRAW && m.job && !strcmp(k.last_req.key, "history"));
    job_done(0, 0, NULL);                                           /* the switch moved: saved */
    m.now += 200;
    ws_tick(&m);
    CHECK(m.job && ws_unsaved(&m) == 1 && !strcmp(k.last_req.key, "ethernet") && !strcmp(k.last_req.value, "dhcp"));
    job_done(0, 0, NULL);
    m.now += 200;
    ws_tick(&m);
    CHECK(!m.job && !m.again && ws_unsaved(&m) == 0 &&
          !strcmp(m.status, "The settings in force are saved on the stick again"));
    CHECK(ws_save_again(&m) == 0);                                  /* nothing to save */
    /* L6: the job's record could not be read (no memory just then): the job is kept and read again */
    CHECK(ws_scan(&m) == WS_DRAW && m.job);
    k.job_read_r = -12;
    m.now += 200;
    ws_tick(&m);
    CHECK(m.job > 0);
    job_done(0, 0, NULL);
    m.now += 200;
    ws_tick(&m);
    CHECK(!m.job && !strcmp(m.status, "6 networks in range"));
}

int main(void) {
    words();
    networks_in_range();
    networks_age();
    join_wpa2();
    join_open_and_other();
    card_leave_save_forget();
    settings_pages();
    network_and_about();
    without_the_call();
    closing_and_lost_jobs();
    review_fixes();
    CHECK(k.bad == 0);                                              /* every call's size and argument as they should be */
    printf("test_wsettings: %d checks, %d failed\n", checks, failed);
    return failed ? 1 : 0;
}
