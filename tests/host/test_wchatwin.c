/* Host tests for the Chat's window (user/wchat/window.c) on the fake desktop (tests/host/gk_fake.c), with the model as
   it is and the helper played here, at the other ends of the window's pipes (the test's chat_spawn(), chat_kill(),
   chat_reaped() and chat_minute() stand in for user/wchat/proc.c): the connect form and its checks; a connection with
   SASL, the password gone from the field, the window and the model once it was sent; the lines going out only as the
   pipe takes them; a channel's tab, topic, messages, nick list; a highlight, a colour drawn and left out; scrollback
   following the newest lines, and staying where it was when scrolled back, also while old lines go; a query from the
   nick list; a tab's x leaving the channel; Alt+1; Page Up; a helper that says nonsense, one that dies, one that does
   not start; the close button asking first, QUIT, the window going with the connection. Built and run by `make
   unittest`; on the Mac with AddressSanitizer and UndefinedBehaviorSanitizer by hand. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <gk.h>
#include "gk_fake.h"
#include "window.h"
#include "chatint.h"

static int checks, failed;
#define CHECK(x) do { checks++; if(!(x)) { failed++; printf("FAIL %s line %d: %s\n", __FILE__, __LINE__, #x); } } while(0)

static struct chat c;
static struct chatwin w;

/**** the helper, played here ****/

static int h_in = -1, h_out = -1;               /* what the window sends comes out of h_in; what it reads goes into h_out */
static int alive, spawns, kills, refuse_spawn;
static char spawn_host[256];
static unsigned spawn_port;
static int spawn_tls;

int chat_spawn(const char *host, unsigned port, int tls, int *to_fd, int *from_fd) {
    int down[2], up[2];
    if(refuse_spawn) {
        errno = EAGAIN;
        return -1;
    }
    if(pipe(down) < 0 || pipe(up) < 0) return -1;
    fcntl(down[0], F_SETFL, O_NONBLOCK);
    *to_fd = down[1];
    h_in = down[0];
    *from_fd = up[0];
    h_out = up[1];
    snprintf(spawn_host, sizeof(spawn_host), "%s", host);
    spawn_port = port;
    spawn_tls = tls;
    spawns++;
    alive = 1;
    return 4242;
}

static void helper_gone(void) {                 /* its descriptors go as a collected process's do */
    if(h_in >= 0) close(h_in);
    if(h_out >= 0) close(h_out);
    h_in = h_out = -1;
}

void chat_kill(int pid) {
    (void)pid;
    kills++;
    alive = 0;
}

int chat_reaped(int pid) {
    (void)pid;
    if(!alive) helper_gone();
    return !alive;
}

int chat_minute(void) {
    return 10 * 60 + 15;
}

/* everything the window has to do now, as main.c's loop does it: each event and a tick after it, until nothing more
   comes; 0 once it closes. More than 10000 events is a window that never rests */
static int run(void) {
    struct gk_event ev;
    int events = 0;
    while(gk_next(w.ui, &ev, 0) > 0) {
        if(!chatwin_event(&w, &ev) || !chatwin_tick(&w)) return 0;
        if(++events > 10000) {
            CHECK(!"a window that never rests");
            return 1;
        }
    }
    if(!chatwin_tick(&w)) return 0;
    gk_flush(w.ui);                                 /* painted, as gk_next() would before it waits */
    return 1;
}

static char took_buf[1 << 16];

/* what the window wrote to the helper since the last call */
static const char *took(void) {
    int n, o = 0;
    run();
    while(h_in >= 0 && o < (int)sizeof(took_buf) - 1 && (n = (int)read(h_in, took_buf + o, sizeof(took_buf) - 1 - (size_t)o)) > 0)
        o += n;
    took_buf[o] = 0;
    return took_buf;
}

static void frame(int kind, const char *data, int len) {
    char buf[LINK_DATA_MAX + 64];
    int n = link_frame(buf, sizeof(buf), kind, data, len);
    CHECK(n > 0 && write(h_out, buf, (size_t)n) == n);
}

static void server(const char *s) {
    int n = (int)strlen(s), at, k;
    for(at = 0; at < n; at += k) {
        k = n - at < LINK_DATA_MAX ? n - at : LINK_DATA_MAX;
        frame(LINK_DATA, s + at, k);
        run();
    }
}

/**** looking at the window ****/

static const struct gk_theme *theme(void) {
    return gk_theme(w.ui);
}

static int shows(const char *s, unsigned fg) {
    int x, y;
    return gkf_find(s, fg, &x, &y);
}

/* the area's row of a line's start: where its time is, -1 when it is not in view */
static int area_row(const char *text, unsigned fg) {
    int x, y, ax, ay, aw, ah;
    if(!gkf_find(text, fg, &x, &y)) return -1;
    gk_bounds(w.area, &ax, &ay, &aw, &ah);
    if(y < ay || y >= ay + ah) return -1;
    return (y - ay) / CW_ROW_H;
}

static void click_on(struct gk_widget *wd) {
    int x, y, width, height;
    gk_bounds(wd, &x, &y, &width, &height);
    gkf_click(x + width / 2, y + height / 2);
    gkf_advance(600);                           /* the next click is no double-click */
}

static void type_line(const char *s) {
    gkf_type(s);
    gkf_key('\r', 0);
    run();
}

static const char *said_last(int room) {
    const struct chat_room *r = c.room[room];
    const struct chat_line *l = chat_line_at(r, r->count - 1);
    return l ? l->text : "(none)";
}

/* the password's bytes anywhere in the window's or the model's memory */
static int remembered(const char *pass) {
    size_t n = strlen(pass), i;
    const unsigned char *b = (const unsigned char *)&w, *m = (const unsigned char *)&c;
    for(i = 0; i + n <= sizeof(w); i++) if(!memcmp(b + i, pass, n)) return 1;
    for(i = 0; i + n <= sizeof(c); i++) if(!memcmp(m + i, pass, n)) return 1;
    return 0;
}

/* a fresh window and model, the window drawn */
static void opened(void) {
    if(w.ui) chatwin_close(&w);
    if(c.rooms) chat_free(&c);
    gkf_reset(0, 0);
    CHECK(chat_init(&c) == 0);
    CHECK(chatwin_open(&w, &c) == 0);
    gkf_focus(1);
    run();
}

/* connected to irc.test with TLS as surfer, registered, without a password */
static void online(void) {
    opened();
    gk_set_text(w.host, "irc.test");
    click_on(gk_focused(w.ui));
    gk_focus(w.host);
    gkf_key('\r', 0);
    run();
    CHECK(spawns && w.pid == 4242 && gk_value(w.pages) == PAGE_CHAT);
    frame(LINK_UP, "TLSv1.3", 7);
    CHECK(!strcmp(took(), "CAP LS 302\r\nNICK surfer\r\nUSER surfer 0 * :surfer\r\n"));
    server(":irc.test CAP * LS :sasl\r\n");
    CHECK(!strcmp(took(), "CAP END\r\n"));
    server(":irc.test 001 surfer :Welcome to the test network\r\n:irc.test 005 surfer CHANTYPES=# PREFIX=(ov)@+ :ok\r\n");
    CHECK(c.state == CHAT_ONLINE);
}

static void in_surf(void) {
    online();
    type_line("/join #surf");
    CHECK(!strcmp(took(), "JOIN #surf\r\n"));
    server(":surfer!s@host.test JOIN #surf\r\n:irc.test 332 surfer #surf :Waves \002today\002\r\n"
           ":irc.test 353 surfer = #surf :surfer @bob +carol dave\r\n:irc.test 366 surfer #surf :End\r\n");
}

/**** the tests ****/

static void form(void) {
    static const char pass[] = "made-up-pass";
    int i;
    opened();
    CHECK(gkf_w() == CW_W && gkf_h() == CW_H && !strcmp(gkf_title(), "Chat") && gkf_resizable());
    CHECK(shows("Connect to an IRC server", theme()->ink) && shows("Not connected", theme()->ink));
    CHECK(gk_focused(w.ui) == w.host && !strcmp(gk_text(w.port), "6697") && gk_value(w.tls) == 1);
    click_on(w.tls);                                /* TLS off: the usual port follows */
    run();
    CHECK(gk_value(w.tls) == 0 && !strcmp(gk_text(w.port), "6667"));
    click_on(w.tls);
    run();
    CHECK(!strcmp(gk_text(w.port), "6697"));
    gk_focus(w.host);
    gkf_key('\r', 0);                               /* no server: said in the form, nothing started */
    run();
    CHECK(!spawns && shows("Type the server's name", theme()->coral));
    gkf_type("irc test");
    gkf_key('\r', 0);
    run();
    CHECK(!spawns && shows("A server's name has only letters, digits, dots and dashes", theme()->coral));
    gk_set_text(w.host, "irc.test");
    gk_focus(w.passkind);                           /* SASL: the password field opens */
    CHECK(!gk_enabled(w.pass));
    gkf_key(WIN_KEY_RIGHT, 0);
    gkf_key(WIN_KEY_RIGHT, 0);
    run();
    CHECK(gk_value(w.passkind) == CHAT_PASS_SASL && gk_enabled(w.pass));
    gk_focus(w.pass);
    gkf_type(pass);
    run();
    CHECK(!strcmp(gk_text(w.pass), pass) && !shows(pass, theme()->ink));     /* dots, never the text */
    gkf_key('\r', 0);
    run();
    CHECK(spawns == 1 && !strcmp(spawn_host, "irc.test") && spawn_port == 6697 && spawn_tls == 1);
    CHECK(!strcmp(gk_text(w.pass), "") && gk_value(w.pages) == PAGE_CHAT && gk_focused(w.ui) == w.input);
    CHECK(shows("Connecting to irc.test, port 6697", theme()->muted));
    frame(LINK_UP, "TLSv1.3", 7);
    CHECK(!strcmp(took(), "CAP LS 302\r\nNICK surfer\r\nUSER surfer 0 * :surfer\r\n"));
    CHECK(shows("Connected with TLSv1.3: the server's certificate is verified", theme()->muted));
    server(":irc.test CAP * LS :sasl=PLAIN\r\n:irc.test CAP * ACK :sasl\r\nAUTHENTICATE +\r\n");
    CHECK(!strcmp(took(), "CAP REQ :sasl\r\nAUTHENTICATE PLAIN\r\nAUTHENTICATE AHN1cmZlcgBtYWRlLXVwLXBhc3M=\r\n"));
    CHECK(!remembered(pass));                       /* sent: gone from the field, the window and the model */
    server(":irc.test 903 surfer :SASL authentication successful\r\n");
    CHECK(!strcmp(took(), "CAP END\r\n"));
    server(":irc.test 001 surfer :Welcome\r\n");
    CHECK(shows("surfer on irc.test, port 6697 (TLSv1.3)", theme()->ink) && !strcmp(gkf_title(), "Chat - irc.test"));
    for(i = 0; i < gkf_menu_count(); i++)           /* connected: Connect greyed, Disconnect not */
        if(gkf_menu_entry(i)->id == MENU_CONNECT) CHECK(gkf_menu_entry(i)->flags & WIN_MENU_DISABLED);
        else if(gkf_menu_entry(i)->id == MENU_DISCONNECT) CHECK(!(gkf_menu_entry(i)->flags & WIN_MENU_DISABLED));
}

static void channel(void) {
    int x, y, row, nx, ny, nw, nh;
    in_surf();
    CHECK(gk_tab_count(w.tabs) == 2 && gk_value(w.tabs) == 1 && !strcmp(gk_text(w.tabs), "#surf"));
    CHECK(shows("Waves today", theme()->ink) && shows("You joined #surf", theme()->muted));
    CHECK(gk_list_count(w.nicks) == 4 && !strcmp(gk_list_text(w.nicks, 0), "@bob") &&
          !strcmp(gk_list_text(w.nicks, 1), "+carol") && !strcmp(gk_list_text(w.nicks, 3), " surfer"));
    CHECK(shows("4 nicks", theme()->muted) && !strcmp(gkf_title(), "Chat - #surf"));
    server(":bob!b@h PRIVMSG #surf :hi surfer, \0034red\003 waves\r\n");
    row = area_row("<bob>", theme()->coral);        /* it names us: the nick in coral, the row in the band colour */
    CHECK(row >= 0 && shows("10:15", theme()->muted) && shows("hi surfer, ", theme()->ink));
    CHECK(gkf_find("red", irc_rgb(4), &x, &y) && gkf_pixel(x - 2, y + 2) == theme()->band);
    gkf_menu_choose(MENU_CODES);                    /* View > Formatting off: the colour goes */
    run();
    CHECK(gkf_find("red", theme()->ink, &x, &y) && !gkf_find("red", irc_rgb(4), &x, &y));
    gkf_menu_choose(MENU_CODES);
    run();
    CHECK(gkf_find("red", irc_rgb(4), &x, &y));
    server(":erin!e@h JOIN #surf\r\n:dave!d@h NICK dan\r\n:bob!b@h MODE #surf +v dan\r\n");
    CHECK(gk_list_count(w.nicks) == 5 && !strcmp(gk_list_text(w.nicks, 1), "+carol") &&
          !strcmp(gk_list_text(w.nicks, 2), "+dan") && !strcmp(gk_list_text(w.nicks, 3), " erin"));
    gk_set_value(w.nicks, 4);                       /* the list's choice stays on its nick as others come */
    server(":aaron!a@h JOIN #surf\r\n");
    CHECK(!strcmp(gk_text(w.nicks), " surfer"));
    /* a double-click on a nick: a query with it, in front */
    gk_bounds(w.nicks, &nx, &ny, &nw, &nh);
    gkf_click(nx + 20, ny + 1 + 0 * GK_ROW_H + 8);
    gkf_click(nx + 20, ny + 1 + 0 * GK_ROW_H + 8);
    run();
    CHECK(chat_find(&c, ROOM_QUERY, "bob") == 2 && c.active == 2 && gk_tab_count(w.tabs) == 3);
    CHECK(shows("Talking with bob", theme()->muted) && gk_list_count(w.nicks) == 0);
    type_line("hello bob");
    CHECK(!strcmp(took(), "PRIVMSG bob :hello bob\r\n") && shows("<surfer>", theme()->muted));
    /* Alt+1: the server's room; Ctrl+Page Down: the next one */
    gkf_key('1', WIN_MOD_ALT);
    run();
    CHECK(c.active == 0 && gk_value(w.tabs) == 0 && gk_list_count(w.nicks) == 0);
    gkf_key(WIN_KEY_PGDN, WIN_MOD_CTRL);
    run();
    CHECK(c.active == 1 && gk_list_count(w.nicks) == 6);
    /* unread lines in another room: its tab says how many, and ! for one that names us */
    server(":bob!b@h PRIVMSG surfer :are you there, surfer?\r\n:bob!b@h PRIVMSG surfer :hello?\r\n");
    gk_set_value(w.tabs, 2);
    CHECK(!strcmp(gk_text(w.tabs), "bob ! 2"));
    gk_set_value(w.tabs, 1);
    /* the tab's x leaves the channel: PART, and the tab goes with the server's answer */
    {
        int tx, ty, tw, th;
        gk_bounds(w.tabs, &tx, &ty, &tw, &th);
        CHECK(shows("#surf", theme()->on_teal));
        gkf_find("#surf", theme()->on_teal, &x, &y);
        gkf_click(x + 7 * 8 + 4, y + 4);            /* "#surf" and the x after it */
        gkf_advance(600);
        run();
    }
    CHECK(!strcmp(took(), "PART #surf\r\n") && gk_tab_count(w.tabs) == 3);
    server(":surfer!s@host.test PART #surf\r\n");
    CHECK(gk_tab_count(w.tabs) == 2 && chat_find(&c, ROOM_CHANNEL, "#surf") < 0);
}

static void scrollback(void) {
    char line[160];
    struct gk_view v;
    int i, page, row;
    in_surf();
    gk_area_view(w.area, &v);
    page = v.h / CW_ROW_H;
    for(i = 0; i < 100; i++) {
        snprintf(line, sizeof(line), ":bob!b@h PRIVMSG #surf :line %d\r\n", i);
        server(line);
    }
    gk_area_view(w.area, &v);                       /* at the newest: it follows them */
    CHECK(w.total > page && v.sy == w.total - page && area_row("line 99", theme()->ink) == page - 1);
    gkf_key(WIN_KEY_PGUP, 0);                       /* the input has the keys: Page Up scrolls the messages */
    run();
    gk_area_view(w.area, &v);
    CHECK(v.sy == w.total - page - (page - 1));
    row = v.sy;
    server(":bob!b@h PRIVMSG #surf :line 100\r\n");     /* scrolled back: it stays */
    gk_area_view(w.area, &v);
    CHECK(v.sy == row && area_row("line 100", theme()->ink) < 0);
    /* a long line wraps under its text; the rows are counted again at another width */
    snprintf(line, sizeof(line), ":bob!b@h PRIVMSG #surf :%s\r\n",
             "a very long line that goes on and on past the edge of the area to wrap at its spaces, more than once");
    gkf_key(WIN_KEY_END, WIN_MOD_CTRL);
    gk_scroll_to(w.area, 0, w.total);
    run();
    server(line);
    i = w.total;
    gkf_resize(CW_MIN_W, CW_H);
    run();
    CHECK(w.total > i && w.cols < 60);
    gk_area_view(w.area, &v);
    CHECK(v.sy == w.total - v.h / CW_ROW_H);
    /* thousands of lines: the oldest go, and a view scrolled back keeps its lines */
    gk_scroll_to(w.area, 0, w.total - v.h / CW_ROW_H - 50);
    run();
    gk_area_view(w.area, &v);
    row = v.sy;
    for(i = 0; i < CHAT_LINES; i++) {
        snprintf(line, sizeof(line), ":bob!b@h PRIVMSG #surf :more %d\r\n", i);
        chat_recv(&c, line, (int)strlen(line));
    }
    run();
    gk_area_view(w.area, &v);
    CHECK(v.sy == 0 && c.room[1]->count == CHAT_LINES);  /* its lines went: the oldest kept at the top */
}

static void helpers(void) {
    char junk[64];
    /* a helper that says what no helper says */
    online();
    kills = 0;
    memset(junk, 'x', sizeof(junk));
    CHECK(write(h_out, junk, sizeof(junk)) == (int)sizeof(junk));
    run();
    CHECK(c.state == CHAT_OFF);
    CHECK(kills == 1 && w.pid == 0);
    CHECK(shows("sent what no helper sends", theme()->coral));
    /* one killed by a fault: its pipe stays open until it is collected; what it sent before is read */
    online();
    kills = 0;
    server(":irc.test NOTICE surfer :before the end\r\n");
    frame(LINK_DATA, ":irc.test NOTICE surfer :its last words\r\n", 41);
    alive = 0;
    gkf_advance(CW_REAP_MS);
    run();
    CHECK(c.state == CHAT_OFF);
    CHECK(shows("its last words", theme()->ink));
    CHECK(shows("the connection's helper stopped", theme()->coral));
    /* the end it says itself */
    online();
    {
        static const char why[] = "HTTPS refused: the certificate of irc.test is not from a trusted issuer";
        frame(LINK_DOWN, why, (int)strlen(why));
    }
    run();
    CHECK(c.state == CHAT_OFF && shows("Disconnected: TLS refused: the certificate", theme()->coral));
    CHECK(!strcmp(said_last(0), "Disconnected: TLS refused: the certificate of irc.test is not from a trusted issuer"));
    /* one that cannot start */
    opened();
    refuse_spawn = 1;
    gk_set_text(w.host, "irc.test");
    gk_focus(w.host);
    gkf_key('\r', 0);
    run();
    refuse_spawn = 0;
    CHECK(c.state == CHAT_OFF && shows("could not be started", theme()->coral));
    /* the connection lost by the model (no answer to its PING): the helper killed */
    online();
    kills = 0;
    gkf_advance(CHAT_IDLE_MS);
    run();
    CHECK(strstr(took(), "PING :wchat"));
    gkf_advance(CHAT_PING_MS);
    run();
    CHECK(c.state == CHAT_OFF && kills == 1 && shows("has not answered for 150 seconds", theme()->coral));
}

static void closing(void) {
    /* not connected: the close button closes */
    opened();
    gkf_close();
    CHECK(!run());
    chatwin_close(&w);
    /* connected: it asks; Cancel keeps it; Close says QUIT and goes with the connection */
    online();
    gkf_close();
    run();
    CHECK(gk_box_open(w.ui) && gk_asking(w.ui) == BOX_CLOSE);
    gkf_key(27, 0);
    CHECK(run() && !gk_box_open(w.ui) && c.state == CHAT_ONLINE);
    gkf_close();
    run();
    gkf_key('\r', 0);
    CHECK(run());
    CHECK(!strcmp(took(), "QUIT\r\n") && c.state == CHAT_QUITTING);
    server("ERROR :Closing Link: surfer (Client Quit)\r\n");
    close(h_out);                                   /* the helper's end: the server closed the connection */
    h_out = -1;
    CHECK(!run());
    chatwin_close(&w);
    alive = 0;
    helper_gone();
    /* a server that never closes: the window goes after CHAT_QUIT_MS all the same */
    online();
    gkf_menu_choose(MENU_QUIT);
    run();
    gkf_key('\r', 0);
    run();
    gkf_advance(CHAT_QUIT_MS);
    CHECK(!run());
    chatwin_close(&w);
}

int main(void) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    gkf_reset(0, 0);
    gkf_refuse(ENODEV);
    CHECK(chat_init(&c) == 0 && chatwin_open(&w, &c) < 0 && errno == ENODEV);
    chat_free(&c);
    memset(&c, 0, sizeof(c));
    form();
    channel();
    scrollback();
    helpers();
    closing();
    if(w.ui) chatwin_close(&w);
    if(c.rooms) chat_free(&c);
    helper_gone();
    gkf_reset(0, 0);
    printf("test_wchatwin: %d checks, %d failed\n", checks, failed);
    return failed ? 1 : 0;
}
