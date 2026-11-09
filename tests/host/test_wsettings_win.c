/* Host tests for Settings' window (user/wsettings/window.c; the desktop applications, track settings, 11/05/26) on the
   fake desktop (tests/host/gk_fake.c), with the model (settings.c) as it is and the simulated kernel of
   tests/host/wsettings_fake.h, driven as a person drives it: the window opening (and refused off the desktop), the
   scan the Wi-Fi page asks for by itself, greyed while it runs, and the networks in their columns; a WPA2 network
   joined by its password, typed into a field that shows dots, refused when too short (the box stays, the field
   wiped), handed to the kernel by Enter and wiped from the field at once, nowhere in the window's memory, the panel's
   words wrapped; the stages and the outcome in the band; an open network while a server without a password runs (the
   whole warning shown, Esc cancels); joining by name; the card off and on, its switch showing what was asked while it
   runs; a saved network forgotten after a question; telnetd at startup only after its warning, every word of which
   is on the screen; history; the interfaces and the host name (refused by the rule, then set by Enter); closing with a
   host name typed and not set; the desktop's choices once the table has them; About's figures; the menus and their
   shortcuts; nothing presented when nothing changed; the theme followed. Built and run by `make unittest`
   (unittest-settings). */
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <gk.h>
#include "gk_fake.h"
#include "window.h"
#include "wsettings_fake.h"

static int checks, failed;
#define CHECK(x) do { checks++; if(!(x)) { failed++; printf("FAIL %s line %d: %s\n", __FILE__, __LINE__, #x); } } while(0)

static struct wswin s;
static int abouts;

static void about(struct ws_about *ab, int all) {
    abouts++;
    if(all) {
        snprintf(ab->build, sizeof(ab->build), "%s", "66a598370000 2026-11-05T12:00:00Z");
        snprintf(ab->cpu, sizeof(ab->cpu), "%s", "QEMU Virtual CPU version 2.5+");
    }
    ab->have_mem = 1;
    ab->mem_kb = 262144;
    ab->free_kb = 180000;
    ab->programs_kb = 5120;
    ab->heap_kb = 900;
    ab->uptime_s = 60 + abouts;
}

/* every event the window has, through the program; 0 once it asked to close */
static int run(void) {
    struct gk_event ev;
    while(gk_next(s.ui, &ev, 0) > 0) if(!wsw_event(&s, &ev)) return 0;
    return 1;
}

/* the clock moves on (the model's ticks come), and the window takes what came */
static int tick(unsigned ms) {
    unsigned step;
    int r = 1;
    for(; ms && r; ms -= step) {
        step = ms < WSW_TICK_MS ? ms : WSW_TICK_MS;
        gkf_advance(step);
        r = run();
    }
    return r;
}

static void click_on(struct gk_widget *w) {
    int x, y, width, height;
    gk_bounds(w, &x, &y, &width, &height);
    gkf_advance(600);                                   /* never a double-click of the last one */
    gkf_click(x + width / 2, y + height / 2);
}

static void click_row(struct gk_widget *list, int i) {
    int x, y, width, height;
    gk_bounds(list, &x, &y, &width, &height);
    gkf_advance(600);
    gkf_click(x + 20, y + 1 + i * GK_ROW_H + GK_ROW_H / 2);
}

/* one of the window's own text areas holds text, and shows it on the screen as it wraps it to its width: each line
   where window.c's draw_text() puts it (of a last line cut with the dot, what is left of it) */
static int shows(int t, const char *text) {
    const struct wsw_text *a = &s.text[t];
    char line[sizeof(a->text) + 4];
    const char *p;
    int x, y, w, h, cells, i, len, more, ok = 1;
    if(strcmp(a->text, text)) return 0;
    gk_bounds(a->w, &x, &y, &w, &h);
    cells = (w - (a->band ? 20 : 0)) / 8;
    for(i = 0; i < a->lines && (p = ws_wrap(text, cells, i, &len)) != NULL; i++) {
        more = i == a->lines - 1 && ws_wrap(text, cells, i + 1, &len) != NULL;
        ws_wrap(text, cells, i, &len);
        if(more && len > cells - 1) len = cells - 1;    /* its last cell is the dot */
        memcpy(line, p, (size_t)len);
        line[len] = 0;
        if(!gkf_text(x + (a->band ? 10 : 0), y + (a->band ? 4 : 2) + i * 16, line, a->muted ? GW_MUTED : GW_INK)) ok = 0;
    }
    return ok && i > 0;
}

/* and nothing of it is cut: every line has its room */
static int whole(int t) {
    const struct wsw_text *a = &s.text[t];
    int x, y, w, h;
    gk_bounds(a->w, &x, &y, &w, &h);
    return ws_wrap_lines(a->text, (w - (a->band ? 20 : 0)) / 8) <= a->lines;
}

static int band_says(const char *text) {
    return shows(T_MSG, text);
}

/* is s (n bytes) anywhere in the window's memory, the model's included? */
static int in_window(const char *str) {
    const unsigned char *p = (const unsigned char *)&s;
    size_t n = strlen(str), i;
    for(i = 0; i + n <= sizeof(s); i++) if(!memcmp(p + i, str, n)) return 1;
    return 0;
}

static void opening_and_scan(void) {
    int x, y, w, h;
    gkf_reset(0, 0);
    fake_reset();
    add_saved("SurfSimWPA", NETCTL_SAVED_WPA2, 0);
    gkf_refuse(ENODEV);
    CHECK(wsw_open(&s, fake_netctl, NULL, about) < 0 && errno == ENODEV && k.starts == 0);
    CHECK(wsw_open(&s, fake_netctl, NULL, about) == 0 && gkf_w() == WSW_W && gkf_h() == WSW_H && gkf_resizable());
    CHECK(!strcmp(gkf_title(), "Settings - Wi-Fi") && abouts == 1);
    gkf_focus(1);
    CHECK(run());
    /* the scan the page asked for by itself: what would start another greyed meanwhile */
    CHECK(k.last_op == NETCTL_SCAN && s.m.job && band_says("Looking for networks..."));
    CHECK(!gk_enabled(s.scan) && !gk_enabled(s.card) && !gk_enabled(s.other) && gk_value(s.card) == 1);
    CHECK(shows(T_STATION, "Not joined to a network"));
    job_done(0, 0, NULL);
    CHECK(tick(WSW_TICK_MS));
    CHECK(!s.m.job && band_says("6 networks in range") && gk_list_count(s.nets) == 6 && gk_enabled(s.scan));
    gk_bounds(s.nets, &x, &y, &w, &h);
    CHECK(gkf_text(x + 6, y + 2, "SurfSim", GW_INK) && gkf_text(x + 6 + 200, y + 2, "\xe2\x96\x88\xe2\x96\x88\xe2\x96\x88"
                                                                       "\xe2\x96\x88 -45 dBm", GW_INK));
    CHECK(gkf_text(x + 6 + 320, y + 2, "open", GW_INK) && gkf_text(x + 6 + 410, y + 2 + GK_ROW_H, "saved", GW_INK));
    CHECK(shows(T_NETNOTE, "A network nearby hides its name: By name joins it.") && whole(T_NETNOTE));
    gk_bounds(s.saved, &x, &y, &w, &h);
    CHECK(gk_list_count(s.saved) == 1 && gkf_text(x + 6, y + 2, "SurfSimWPA", GW_INK) &&
          gkf_text(x + 6 + 260, y + 2, "in range, -52 dBm", GW_INK));
    CHECK(gk_value(s.startup) == 1 && !gk_shown(s.leave) && !gk_shown(s.save) && !gk_enabled(s.join));
    /* a row that SurfOS does not join: Join greyed, and why (the list's last row, by End) */
    click_row(s.nets, 0);
    gkf_key(WIN_KEY_END, 0);
    CHECK(run() && ws_net_selected(&s.m) == 5 && !gk_enabled(s.join));
    CHECK(shows(T_NETNOTE, "SurfSimWEP uses WEP, which SurfOS does not support (it is broken: the network should use "
                           "WPA2)") && whole(T_NETNOTE));
}

static void join_wpa2(void) {
    int x, y, w, h;
    static const char zero[64];
    gkf_key(WIN_KEY_HOME, 0);                           /* the list back at its top: the rows where click_row() finds them */
    CHECK(run() && ws_net_selected(&s.m) == 0);
    click_row(s.nets, 1);
    CHECK(run() && ws_net_selected(&s.m) == 1 && gk_enabled(s.join));
    click_on(s.join);
    CHECK(run() && gk_value(s.main) == 1 && s.asked == WS_ASK_PASSWORD && gk_focused(s.ui) == s.ask_pass);
    CHECK(shows(T_ASK, "Password for SurfSimWPA (WPA2-Personal):") && gk_shown(s.ask_pass_row) &&
          !gk_shown(s.ask_name_row) && gk_value(s.ask_save) == 0);     /* unticked: saved only when asked (M3) */
    CHECK(!strcmp(gk_text(s.ask_save), "Save it: SurfOS joins it by itself at every start"));
    click_on(s.ask_save);                               /* ticked here: the save is tested below */
    CHECK(run() && gk_value(s.ask_save) == 1);
    gk_focus(s.ask_pass);
    CHECK(!strcmp(gk_text(s.ask_yes), "Join") && !strcmp(gk_text(s.ask_no), "Cancel"));
    CHECK(!strcmp(gkf_title(), "Settings - Wi-Fi"));
    gkf_type("short\n");                                /* too short: the panel stays, its field wiped, its words new */
    CHECK(run() && gk_value(s.main) == 1 && !gk_text(s.ask_pass)[0] && k.starts == 1);
    CHECK(shows(T_ASK, "A WPA2 password is 8 to 63 characters (letters, digits, spaces and punctuation). Password for "
                       "SurfSimWPA:"));
    gkf_type(PASSPHRASE);
    CHECK(run() && strlen(gk_text(s.ask_pass)) == strlen(PASSPHRASE));
    gk_bounds(s.ask_pass, &x, &y, &w, &h);
    CHECK(gkf_text(x + 4, y + 3, "\xe2\x80\xa2\xe2\x80\xa2\xe2\x80\xa2\xe2\x80\xa2", GW_INK));    /* dots */
    CHECK(!gkf_find("surfsim", GW_INK, NULL, NULL) && !in_window("surfsim-wpa2"));   /* never shown, never kept here */
    gkf_key('\r', 0);
    CHECK(run() && !strcmp(k.last_join.passphrase, PASSPHRASE) && k.last_join.flags == NETCTL_JOIN_SAVE);
    CHECK(k.last_join.ssid_len == 10 && !memcmp(k.last_join.ssid, "SurfSimWPA", 10));
    CHECK(!memcmp(gk_text(s.ask_pass), zero, NETCTL_PASS_MAX) && gk_value(s.main) == 0 &&   /* every byte (max + 1) */
          s.asked == WS_ASK_NONE);                      /* wiped at once, gone */
    CHECK(!in_window("surfsim-wpa2") && !in_window(PASSPHRASE));
    CHECK(band_says("Joining SurfSimWPA...") && !gk_enabled(s.scan) && !gk_enabled(s.join));
    k.job.stage = NETCTL_STAGE_ADDRESS;
    CHECK(tick(WSW_TICK_MS) && band_says("Joined SurfSimWPA; asking for an address (DHCP)..."));
    k.wifi.state = NETCTL_STA_UP;
    memcpy(k.wifi.ssid, "SurfSimWPA", 10);
    k.wifi.ssid_len = 10;
    k.wifi.security = NETCTL_SEC_WPA2;
    k.wifi.ip = ip4(10, 0, 2, 15);
    k.wifi.link_up = 1;
    job_done(0, NETCTL_DONE_ADDRESS | NETCTL_DONE_SAVED, NULL);
    CHECK(tick(WSW_TICK_MS));
    CHECK(band_says("Joined SurfSimWPA: address 10.0.2.15; saved: SurfOS joins it by itself at startup"));
    CHECK(shows(T_STATION, "Joined SurfSimWPA (WPA2-Personal): address 10.0.2.15"));
    CHECK(gk_shown(s.leave) && gk_enabled(s.leave) && !gk_shown(s.save));   /* saved already */
    gk_bounds(s.nets, &x, &y, &w, &h);
    CHECK(gkf_text(x + 6 + 410, y + 2 + GK_ROW_H, "joined", GW_INK));
    (void)zero;
}

static void open_and_by_name(void) {
    k.wifi.exposed = NETCTL_EXPOSED_TELNET;
    k.wifi.state = NETCTL_STA_IDLE;
    CHECK(tick(WS_READ_MS));
    click_row(s.nets, 0);
    click_on(s.join);
    CHECK(run() && s.asked == WS_ASK_OPEN && gk_value(s.main) == 1 && !s.m.job);
    CHECK(shows(T_ASK, "A telnet server runs or is to start, and it has no password: it answers on an open network too "
                       "(telnetd off in a Terminal stops it). Join SurfSim all the same?") && whole(T_ASK));
    CHECK(gk_focused(s.ui) == s.ask_no);
    CHECK(!gk_shown(s.ask_pass_row) && !gk_shown(s.ask_save));
    gkf_key(27, 0);                                     /* Esc: Cancel */
    CHECK(run() && gk_value(s.main) == 0 && !s.m.job && k.starts == 2);
    click_on(s.join);
    CHECK(run() && s.asked == WS_ASK_OPEN);
    click_on(s.ask_yes);
    CHECK(run() && s.m.job && k.last_op == NETCTL_JOIN && !k.last_join.passphrase[0] && k.last_join.ssid_len == 7);
    job_done(0, 0, NULL);
    CHECK(tick(WSW_TICK_MS) && band_says("Joined SurfSim, but no address came (DHCP)"));
    /* by its name: the name field first, then the password; no password: an open network */
    k.wifi.exposed = 0;
    CHECK(tick(WS_READ_MS));                            /* read again: no server any more, so no question */
    click_on(s.other);
    CHECK(run() && s.asked == WS_ASK_OTHER && gk_shown(s.ask_name_row) && gk_focused(s.ui) == s.ask_name);
    gkf_type("SurfSimHidden\n");
    CHECK(run() && gk_focused(s.ui) == s.ask_pass);
    click_on(s.ask_yes);
    CHECK(run() && s.m.job && k.last_join.ssid_len == 13 && !memcmp(k.last_join.ssid, "SurfSimHidden", 13));
    CHECK(!k.last_join.passphrase[0] && gk_value(s.main) == 0 && !gk_text(s.ask_name)[0]);
    job_done(-2, 0, NULL);
    CHECK(tick(WSW_TICK_MS) && band_says("Could not join SurfSimHidden: no network of that name is in range"));
    gkf_key(10, WIN_MOD_CTRL);                          /* Ctrl+J: the same, by the Wi-Fi menu's shortcut */
    CHECK(run() && s.asked == WS_ASK_OTHER);
    click_on(s.ask_no);
    CHECK(run() && s.asked == WS_ASK_NONE && gk_value(s.main) == 0);
}

/* what was heard, not heard for a minute (the kernel's list forgets it): the rows go, and the note says how to look
   again; Scan brings them back */
static void networks_age(void) {
    struct netctl_bss_list heard = k.bss;
    k.bss.count = 0;
    CHECK(tick(WS_READ_MS) && gk_list_count(s.nets) == 0 && !s.m.job && !gk_enabled(s.join));
    CHECK(shows(T_NETNOTE, "Nothing heard in the last minute: Scan looks again.") && whole(T_NETNOTE));
    k.bss = heard;
    click_on(s.scan);
    CHECK(run() && s.m.job && k.last_op == NETCTL_SCAN && band_says("Looking for networks..."));
    job_done(0, 0, NULL);
    CHECK(tick(WSW_TICK_MS) && gk_list_count(s.nets) == 6 && band_says("6 networks in range"));
    CHECK(shows(T_NETNOTE, "A network nearby hides its name: By name joins it."));
}

static void card_and_forget(void) {
    click_on(s.card);                                   /* off: the switch shows it while the card stops */
    CHECK(run() && s.m.job && k.last_op == NETCTL_CARD && k.last_req.on == 0 && gk_value(s.card) == 0);
    CHECK(band_says("Turning the Wi-Fi card off..."));
    CHECK(tick(WSW_TICK_MS) && gk_value(s.card) == 0);  /* the kernel still says on: the switch keeps what was asked */
    k.wifi.up = 0;
    k.wifi.state = NETCTL_STA_DOWN;
    k.bss.count = 0;
    job_done(0, 0, NULL);
    CHECK(tick(WSW_TICK_MS) && band_says("The Wi-Fi card is off") && gk_value(s.card) == 0);
    CHECK(shows(T_STATION, "The Wi-Fi card is off") && !gk_enabled(s.scan) && gk_list_count(s.nets) == 0);
    click_on(s.card);
    CHECK(run() && k.last_req.on == 1 && gk_value(s.card) == 1 && band_says("Starting the Wi-Fi card..."));
    k.wifi.up = 1;
    k.wifi.state = NETCTL_STA_IDLE;
    job_done(0, NETCTL_DONE_NONE_NEAR, NULL);
    CHECK(tick(WSW_TICK_MS) && s.m.job && k.last_op == NETCTL_SCAN);   /* nothing heard: a scan by itself */
    fake_reset();
    add_saved("SurfSimWPA", NETCTL_SAVED_WPA2, 0);
    add_saved("Home", NETCTL_SAVED_WPA2, 1);
    k.seq = 50;
    k.job.seq = (unsigned)s.m.job;
    k.job.op = NETCTL_SCAN;
    job_done(0, 0, NULL);
    CHECK(tick(WSW_TICK_MS) && gk_list_count(s.saved) == 2);
    /* forget: asked first */
    click_row(s.saved, 1);
    CHECK(run() && ws_saved_selected(&s.m) == 1 && gk_enabled(s.forget));
    click_on(s.forget);
    CHECK(run() && s.asked == WS_ASK_FORGET && shows(T_ASK, "Forget Home? SurfOS will no longer join it by itself, and its "
                                                     "key goes from networks.conf on the stick (the network wlan0 is "
                                                     "on stays joined)."));
    click_on(s.ask_yes);
    CHECK(run() && k.last_op == NETCTL_FORGET && k.last_req.ssid_len == 4 && !memcmp(k.last_req.ssid, "Home", 4));
    k.saved.count = 1;
    job_done(0, 0, NULL);
    CHECK(tick(WSW_TICK_MS) && band_says("Forgot Home: SurfOS no longer joins it by itself") &&
          gk_list_count(s.saved) == 1 && !gk_enabled(s.forget));
}

static void system_page(void) {
    int tx, ty;
    gkf_key(WIN_KEY_F8, 0);                             /* View > System */
    CHECK(run() && s.m.page == WS_SYSTEM && gk_value(s.tabs) == WS_SYSTEM && !strcmp(gkf_title(), "Settings - System"));
    CHECK(shows(T_TELNETD, "It has no password and no encryption, and starts only for an address on Ethernet or a WPA2 "
                           "network; it listens on port 23 (settings telnetd.port in a Terminal changes it).") &&
          whole(T_TELNETD));
    CHECK(gkf_find("surfos-config/settings.conf on usb1", GW_INK, NULL, NULL));
    click_on(s.telnetd);                                /* on: only after the warning, in full */
    CHECK(run() && s.asked == WS_ASK_TELNETD && !s.m.job && gk_value(s.telnetd) == 0);
    CHECK(!strncmp(s.text[T_ASK].text, "telnetd has no password and no encryption: whoever reaches the laptop", 69));
    CHECK(shows(T_ASK, s.text[T_ASK].text) && whole(T_ASK));   /* every word of it */
    CHECK(gkf_find("Save only your own networks.", GW_INK, &tx, &ty));
    CHECK(!strcmp(gk_text(s.ask_yes), "Turn it on") && gk_focused(s.ui) == s.ask_no);
    click_on(s.ask_yes);
    CHECK(run() && k.last_op == NETCTL_SET && !strcmp(k.last_req.key, "telnetd") && !strcmp(k.last_req.value, "on"));
    CHECK(gk_value(s.main) == 0 && gk_value(s.telnetd) == 1 && !gk_enabled(s.history));     /* as asked, while it runs */
    strcpy(setting("telnetd")->value, "on");
    job_done(0, 0, NULL);
    CHECK(tick(WSW_TICK_MS) && gk_value(s.telnetd) == 1 && gk_enabled(s.history));
    CHECK(band_says("telnetd = on, saved on the stick: at the next start (telnetd on in a Terminal starts it now)"));
    click_on(s.history);
    CHECK(run() && !strcmp(k.last_req.key, "history") && !strcmp(k.last_req.value, "on") && s.asked == WS_ASK_NONE);
    job_done(0, NETCTL_DONE_NOT_SAVED, "usb1 is write-protected (its switch)");
    CHECK(tick(WSW_TICK_MS) && gk_value(s.history) == 0);  /* not in force: the kernel's off again */
    CHECK(band_says("history = on is in force until SurfOS restarts, but not saved: usb1 is write-protected (its "
                    "switch) (System > Save again, once the stick takes it)"));
    /* L4: what is in force here but not saved, written again from the System page once the stick takes it */
    strcpy(setting("history")->value, "on");            /* the network manager keeps it in force */
    CHECK(tick(WS_READ_MS) && gk_shown(s.save_again) && gk_enabled(s.save_again) && gk_value(s.history) == 1);
    click_on(s.save_again);
    CHECK(run() && s.m.job && !strcmp(k.last_req.key, "history") && !strcmp(k.last_req.value, "on"));
    CHECK(!gk_enabled(s.save_again));                   /* greyed while it runs */
    job_done(0, 0, NULL);
    CHECK(tick(WSW_TICK_MS) && !gk_shown(s.save_again) &&
          band_says("The settings in force are saved on the stick again"));
    strcpy(setting("history")->value, "off");
    CHECK(tick(WS_READ_MS));
}

static void network_page(void) {
    int x, y, w, h;
    gkf_key(WIN_KEY_F6, 0);
    CHECK(run() && s.m.page == WS_NETWORK);
    gk_bounds(s.ifaces, &x, &y, &w, &h);
    CHECK(gk_list_count(s.ifaces) == 8 && gkf_text(x + 6, y + 2, "eth0 (Ethernet): link up, MAC 52:54:00:00:00:00", GW_INK));
    CHECK(!strcmp(gk_list_text(s.ifaces, 2), "    DHCP lease from 10.0.2.2: 23 h 58 min left of 1 day 0 h"));
    CHECK(!strcmp(gk_list_text(s.ifaces, 4), "wlan0 (Wi-Fi): link down, MAC 00:00:00:00:00:00"));
    CHECK(!strcmp(gk_text(s.host), "surfos") && !gk_enabled(s.host_set));
    click_on(s.host);
    gkf_key(1, WIN_MOD_CTRL);                           /* Ctrl+A, and a name the rule refuses */
    gkf_type("-bad");
    CHECK(run() && gk_enabled(s.host_set));
    click_on(s.host_set);
    CHECK(run() && !s.m.job && band_says("A host name is 1 to 32 letters, digits and -, but not - first or last"));
    click_on(s.host);
    gkf_key(1, WIN_MOD_CTRL);
    gkf_type("laptop\n");
    CHECK(run() && s.m.job && !strcmp(k.last_req.key, "hostname") && !strcmp(k.last_req.value, "laptop"));
    job_done(-22, 0, "hostname is letters, digits and - (not first or last), 1 to 32 of them, not laptop!");
    CHECK(tick(WSW_TICK_MS) && !strcmp(gk_text(s.host), "laptop"));    /* what was typed stays */
    /* closing with it typed and not set asks first; the close button again closes */
    gkf_close();
    CHECK(run() && s.asked == WS_ASK_CLOSE && shows(T_ASK, "The host name laptop was typed but not set. Close all the "
                                                    "same?"));
    gkf_key(27, 0);
    CHECK(run() && s.asked == WS_ASK_NONE);
    click_on(s.ethernet);
    CHECK(run() && !strcmp(k.last_req.key, "ethernet") && !strcmp(k.last_req.value, "dhcp") && gk_value(s.ethernet));
    strcpy(setting("ethernet")->value, "dhcp");
    job_done(0, 0, NULL);
    CHECK(tick(WSW_TICK_MS) && band_says("ethernet = dhcp, saved on the stick: now: an Ethernet link that comes up gets "
                                         "an address"));
    strcpy(setting("hostname")->value, "harbour");      /* the setting moves while the field holds what was typed */
    CHECK(tick(WS_READ_MS) && !strcmp(gk_text(s.host), "laptop"));
}

static void desktop_and_about(void) {
    int x, y, w, h, starts;
    gkf_key(WIN_KEY_F7, 0);
    CHECK(run() && s.m.page == WS_DESKTOP && !gk_shown(s.theme_row) && !gk_shown(s.pointer_row));
    CHECK(shows(T_DESK, "This SurfOS keeps no desktop settings: its network manager's table has no desktop.theme or "
                        "pointer.speed."));
    add_setting("desktop.theme", "ocean", "ocean | dusk");
    add_setting("pointer.speed", "normal", "slow | normal | fast");
    CHECK(tick(WS_READ_MS) && gk_shown(s.theme_row) && gk_value(s.theme) == 0 && gk_value(s.pointer) == 1);
    CHECK(gkf_find("Dusk", GW_INK, &x, &y) && gkf_find("Fast", GW_INK, NULL, NULL));
    gkf_advance(600);
    starts = k.starts;
    gkf_click(x + 8, y + 8);                            /* Dusk: in force at once, nothing written yet */
    CHECK(run() && k.applies == 1 && !strcmp(k.last_apply.value, "dusk") && gk_value(s.theme) == 1 && !s.m.job);
    CHECK(band_says("desktop.theme = dusk: in force now, and written on the stick once the steps stop"));
    gkf_key(WIN_KEY_LEFT, 0);                           /* the arrows step, a held one repeating: each in force */
    gkf_key(WIN_KEY_RIGHT, 0);
    gkf_key(WIN_KEY_LEFT, WIN_MOD_REPEAT);
    gkf_key(WIN_KEY_RIGHT, WIN_MOD_REPEAT);
    CHECK(run() && k.applies == 5 && gk_value(s.theme) == 1 && k.starts == starts);
    CHECK(tick(WS_WRITE_AFTER_MS - WSW_TICK_MS) && k.starts == starts);  /* not yet: the steps just stopped */
    CHECK(tick(2 * WSW_TICK_MS) && k.starts == starts + 1 && s.m.job && !strcmp(k.last_req.key, "desktop.theme"));
    CHECK(!strcmp(k.last_req.value, "dusk"));         /* the stick written once, with the last step */
    job_done(0, 0, NULL);
    CHECK(tick(WSW_TICK_MS) && band_says("desktop.theme = dusk, saved on the stick: now") && gk_value(s.theme) == 1);
    CHECK(tick(3000) && k.starts == starts + 1);
    gkf_set_theme(WIN_THEME_DUSK, NULL);                /* the desktop follows; the window is painted again */
    CHECK(run() && band_says("desktop.theme = dusk, saved on the stick: now"));
    /* About, its figures read as they go */
    gkf_key(WIN_KEY_F9, 0);
    CHECK(run() && s.m.page == WS_ABOUT);
    gk_bounds(s.about_value[0], &x, &y, &w, &h);
    CHECK(gkf_text(x, y + 3, "SurfOS 1.0", GW_INK) && !strcmp(gk_text(s.about_value[5]), "QEMU Virtual CPU version 2.5+"));
    CHECK(!strcmp(gk_text(s.about_value[1]), "66a598370000 2026-11-05T12:00:00Z"));
    CHECK(!strcmp(gk_text(s.about_value[2]), "256.0 MB, 175.7 MB of it free"));
    {
        int before = abouts;
        CHECK(tick(2000) && abouts > before);
    }
    /* the menus, in the desktop's bar */
    CHECK(gkf_menu_find("Scan for networks") >= 0 && !strcmp(gkf_menu_entry(gkf_menu_find("Scan for networks"))->keys,
                                                              "Ctrl+R"));
    CHECK(gkf_menu_find("About Settings") >= 0 && gkf_menu_choose(MENU_PAGE + WS_WIFI));
    CHECK(run() && s.m.page == WS_WIFI);
    gkf_key(18, WIN_MOD_CTRL);                          /* Ctrl+R: a scan */
    CHECK(run() && s.m.job && k.last_op == NETCTL_SCAN && band_says("Looking for networks..."));
    job_done(0, 0, NULL);
    CHECK(tick(WSW_TICK_MS) && !s.m.job);
}

static void quiet_when_nothing_changes(void) {
    int x, y, w, h, n;
    CHECK(tick(WS_READ_MS));                            /* everything read again once */
    gkf_presented(&x, &y, &w, &h);
    n = gkf_presented(&x, &y, &w, &h);
    CHECK(n == 0);
    CHECK(tick(WS_READ_MS + WSW_TICK_MS));              /* ticks and a read that changed nothing: nothing presented */
    n = gkf_presented(&x, &y, &w, &h);
    CHECK(n == 0);
    k.wifi.ip = ip4(10, 0, 2, 99);                      /* a new address: the station's line, and nothing far from it */
    k.wifi.state = NETCTL_STA_UP;
    CHECK(tick(WS_READ_MS));
    n = gkf_presented(&x, &y, &w, &h);
    CHECK(n >= 1 && w * h < WSW_W * WSW_H / 2);
}

static int row_of(const char *ssid) {
    int i;
    for(i = 0; i < s.m.nnets; i++)
        if(s.m.nets[i].ssid_len == (int)strlen(ssid) && !memcmp(s.m.nets[i].ssid, ssid, strlen(ssid))) return i;
    return -1;
}

/* the review's fixes in the window (11/05/26): M2 the question when the kernel refuses an open join, M3 Save it, L2 the
   panel's Join greyed while a job runs (Enter too: the password stays typed), L3 the newest step shown while a write
   runs */
static void review_window(void) {
    static const char zero[NETCTL_PASS_MAX];
    int starts;
    gkf_key(WIN_KEY_F5, 0);
    k.wifi.state = NETCTL_STA_IDLE;
    CHECK(run() && s.m.page == WS_WIFI && tick(WS_READ_MS));
    /* M2: a server started since the last read: the kernel refuses, and the panel asks with what is true now */
    k.wifi.exposed = NETCTL_EXPOSED_TELNET;
    CHECK(ws_select_net(&s.m, row_of("SurfSim")) >= 0 && tick(WSW_TICK_MS) && gk_enabled(s.join));
    click_on(s.join);
    CHECK(run() && s.asked == WS_ASK_OPEN && gk_value(s.main) == 1 && !s.m.job && !k.last_join.flags);
    CHECK(!strncmp(s.text[T_ASK].text, "A telnet server runs or is to start", 35) && whole(T_ASK));
    click_on(s.ask_yes);
    CHECK(run() && s.m.job && k.last_join.flags == NETCTL_JOIN_EXPOSED);
    job_done(0, NETCTL_DONE_EXPOSED, NULL);
    CHECK(tick(WSW_TICK_MS) && strstr(s.text[T_MSG].text, "it is open, and a server without a password runs") != NULL);
    /* M3, L2: Save it unticked, saying what saving means while telnetd is on; a scan started meanwhile (Ctrl+R) greys
       the panel's Join, and Enter waits with the password still typed */
    k.wifi.exposed = 0;
    strcpy(setting("telnetd")->value, "on");
    CHECK(tick(WS_READ_MS) && ws_select_net(&s.m, row_of("SurfSimWPA")) >= 0 && tick(WSW_TICK_MS));
    click_on(s.join);
    CHECK(run() && s.asked == WS_ASK_PASSWORD && gk_value(s.ask_save) == 0 && gk_enabled(s.ask_yes));
    CHECK(!strcmp(gk_text(s.ask_save), "Save it: joined by itself at every start, and telnetd starts on it"));
    gkf_type(PASSPHRASE);
    gkf_key(18, WIN_MOD_CTRL);
    CHECK(run() && s.m.job && k.last_op == NETCTL_SCAN && !gk_enabled(s.ask_yes) && s.asked == WS_ASK_PASSWORD);
    starts = k.starts;
    gkf_key('\r', 0);
    CHECK(run() && k.starts == starts && s.asked == WS_ASK_PASSWORD && strlen(gk_text(s.ask_pass)) == strlen(PASSPHRASE));
    job_done(0, 0, NULL);
    CHECK(tick(WSW_TICK_MS) && gk_enabled(s.ask_yes) && s.asked == WS_ASK_PASSWORD);
    gkf_key('\r', 0);
    CHECK(run() && k.last_op == NETCTL_JOIN && !strcmp(k.last_join.passphrase, PASSPHRASE) && !k.last_join.flags);
    CHECK(!memcmp(gk_text(s.ask_pass), zero, NETCTL_PASS_MAX) && !in_window(PASSPHRASE));
    job_done(0, 0, NULL);
    strcpy(setting("telnetd")->value, "off");
    CHECK(tick(WS_READ_MS));
    /* L3: a step's write runs, and a newer click comes: the radio shows the newer one */
    gkf_key(WIN_KEY_F7, 0);
    CHECK(run() && s.m.page == WS_DESKTOP && gk_value(s.theme) == 1);    /* dusk, as desktop_and_about left it */
    gkf_advance(600);
    gkf_click(156 + 8, 43 + 6);                         /* Ocean */
    CHECK(run() && gk_value(s.theme) == 0);
    CHECK(tick(WS_WRITE_AFTER_MS + WSW_TICK_MS) && s.m.job && !strcmp(k.last_req.value, "ocean"));
    gkf_advance(600);
    gkf_click(232 + 8, 43 + 6);                         /* Dusk, while ocean is written */
    CHECK(run() && gk_value(s.theme) == 1 && s.m.job && !strcmp(s.m.job_value, "ocean"));
    job_done(0, 0, NULL);
    CHECK(tick(WSW_TICK_MS) && gk_value(s.theme) == 1);
    CHECK(tick(WS_WRITE_AFTER_MS) && s.m.job && !strcmp(k.last_req.value, "dusk"));    /* and then the newer written */
    job_done(0, 0, NULL);
    CHECK(tick(WSW_TICK_MS) && gk_value(s.theme) == 1 && !s.m.job);
}

static void closing(void) {
    gkf_close();                                        /* the host name typed: asked; Close ends it */
    CHECK(run() && s.asked == WS_ASK_CLOSE);
    click_on(s.ask_yes);
    CHECK(!run());
    wsw_close(&s);
    CHECK(!gkf_open() && k.bad == 0);
    /* a window with nothing typed closes at once, by Ctrl+W too */
    fake_reset();
    CHECK(wsw_open(&s, fake_netctl, NULL, NULL) == 0);
    gkf_focus(1);
    CHECK(run());
    gkf_key(23, WIN_MOD_CTRL);
    CHECK(!run());
    wsw_close(&s);
    CHECK(!gkf_open());
    /* a kernel without the call: every page says so, nothing breaks */
    fake_reset();
    k.no_call = 1;
    CHECK(wsw_open(&s, fake_netctl, NULL, NULL) == 0);
    CHECK(run() && shows(T_STATION, "This SurfOS has no network settings for programs (SYS_netctl)") &&
          !gk_enabled(s.card));
    gkf_close();
    CHECK(!run());
    wsw_close(&s);
    gkf_reset(0, 0);
}

int main(void) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    opening_and_scan();
    join_wpa2();
    open_and_by_name();
    networks_age();
    card_and_forget();
    system_page();
    network_page();
    desktop_and_about();
    quiet_when_nothing_changes();
    review_window();
    closing();
    printf("test_wsettings_win: %d checks, %d failed\n", checks, failed);
    return failed ? 1 : 0;
}
