/*
SurfOS Undertow: the text web browser
----------------------
File: main.c  Date: 10/11/26
----------------------
    undertow                 the welcome screen, then the undertow> prompt
    undertow ADDRESS         that page first
    undertow --dump ADDRESS  the page's text, then "Links:" and "[N] address" for
                             each link, on stdout; its status and any error on
                             stderr; exit 0 for a 2xx status, 1 for any other
                             status or a failure, 2 for bad usage
    undertow --help          the usage

The commands are the design's (docs/superpowers/specs/2026-10-10-undertow-design.md,
the command table; help lists them): open (o), a link's NUMBER or go NUMBER, back
(b), forward (f), reload (r), Enter or next, prev, top, links, find, findnext,
history, url, clock, help (?), quit (q). An address typed alone ("example.com",
anything with "://") is opened too. Ctrl+C and Ctrl+D at the prompt quit.

The terminal: the line is read in raw mode by edit.c, which is back in line mode
before anything else happens, so a load, a view and every list are written in
line mode, where Ctrl+C interrupts the program as it does the shell's others
(the session is not kept: the kernel ends the program and closes its socket)
and the shell's pager pages long lists. A view is exactly one screen with the
prompt (view.h), after ttyctl(TTY_PAGE, 1) has started a new screenful, so it is
never stopped at --More--; links, history and help may be, as any long output
is. Before each load "Loading ADDRESS ..." goes on one row, and what is typed
while it loads is dropped. The size and capabilities of the terminal are asked
for before every view (a telnet window may change). Interactive use needs a
terminal on stdin and stdout; elsewhere --dump is the way.

Everything printed that came from a page or a server goes through view.c (or
ut_safe_text() in dump mode), never raw. Everything is freed at the end.
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <errno.h>
#include <poll.h>
#include <surf.h>
#include "undertow.h"
#include "config.h"
#include "transport.h"
#include "http.h"
#include "html.h"
#include "session.h"
#include "view.h"
#include "edit.h"

static struct ut_config *cfg;
static struct ut_session *ses;
static struct ut_editor *ed;
static struct ut_term term;
static char err[UT_ERR_MAX];
static int loaded;              /* a load ran since the last prompt: what was typed meanwhile is dropped */

static void usage(FILE *f) {
    fprintf(f, "usage: undertow [ADDRESS]          read pages: the welcome screen, or ADDRESS first\n"
               "       undertow --dump ADDRESS     print the page's text and its links, then end\n"
               "       undertow --help             this text\n"
               "ADDRESS: http://HOST[:PORT]/PATH, https://..., or a name alone (HTTPS).\n"
               "In Undertow, help lists the commands.\n");
}

/**** the terminal ****/

/* Is this program's terminal a pseudo-terminal, a telnet session's? Its far end draws any
   character; the console draws code page 437's. Asked once, as surfsh's editor does: the process
   list is read by index, and a task collected meanwhile shifts the rest, so a search that does not
   find this task looks again. */
static int remote_terminal(void) {
    static int known = -1;
    struct procinfo pi;
    int i, k, me;
    if(known >= 0) return known;
    me = getpid();
    known = 0;
    for(k = 0; k < 3; k++)
        for(i = 0; procinfo(i, &pi) == 0; i++)
            if(pi.pid == me) return known = pi.tty >= PROC_TTY_PTY;
    return known;
}

static void term_update(void) {
    int size = ttyctl(TTY_GETSIZE, 0), caps = ttyctl(TTY_CAPS, 0);
    term.cols = size > 0 ? (unsigned int)(size >> 16) & 0xffff : 80;
    term.rows = size > 0 ? (unsigned int)size & 0xffff : 24;
    if(caps < 0) caps = 0;
    term.utf8 = (caps & TTY_CAP_UTF8) != 0;
    term.color = (caps & TTY_CAP_COLOR) != 0;
    term.remote = remote_terminal();
}

static long io_read(void *ctx, unsigned char *buf, size_t cap) {
    ssize_t n;
    (void)ctx;
    do n = read(0, buf, cap); while(n < 0 && errno == EINTR);
    return (long)n;
}

static int io_ready(void *ctx) {
    struct pollfd p;
    (void)ctx;
    p.fd = 0;
    p.events = POLLIN;
    p.revents = 0;
    return poll(&p, 1, 0) > 0 && (p.revents & POLLIN);
}

static void io_write(void *ctx, const char *buf, size_t len) {
    (void)ctx;
    while(len) {
        ssize_t n = write(1, buf, len);
        if(n < 0 && errno == EINTR) continue;
        if(n <= 0) return;
        buf += n;
        len -= (size_t)n;
    }
}

/* a new screenful for the shell's pager: what comes now starts at its top */
static void new_screenful(void) {
    fflush(stdout);
    ttyctl(TTY_PAGE, 1);
}

/* a message, each of its lines through ut_safe_text() (an address in it may hold anything a server
   sent, url.c takes raw bytes in paths), then wrapped as a page's text is */
static void say(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void say(const char *fmt, ...) {
    static char buf[UT_ERR_MAX + 128], safe[(UT_ERR_MAX + 128) * 3 + 1];   /* U+FFFD: 3 bytes a byte */
    const char *line = buf, *nl;
    size_t o = 0;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    for(;;) {
        nl = strchr(line, '\n');
        o += ut_safe_text(safe + o, sizeof(safe) - o, line, nl ? (size_t)(nl - line) : strlen(line), 1);
        if(!nl || o + 1 >= sizeof(safe)) break;
        safe[o++] = '\n';
        line = nl + 1;
    }
    ut_view_text(stdout, &term, safe, o, NULL, 0);
}

/**** a growing text with colours, for help and the lists ****/

struct text {
    char *s;
    size_t len, cap;
    struct ut_span *spans;
    size_t nspans, span_cap;
    int failed;                 /* out of memory: shown short */
};

static void text_add(struct text *t, const char *s, size_t n, int kind) {
    if(t->failed) return;
    if(t->len + n + 1 > t->cap) {
        size_t cap = (t->len + n + 1) * 2;
        char *p = realloc(t->s, cap);
        if(!p) {
            t->failed = 1;
            return;
        }
        t->s = p;
        t->cap = cap;
    }
    if(kind && n) {
        if(t->nspans == t->span_cap) {
            size_t cap = t->span_cap ? t->span_cap * 2 : 32;
            struct ut_span *p = realloc(t->spans, cap * sizeof(*p));
            if(!p) {
                t->failed = 1;
                return;
            }
            t->spans = p;
            t->span_cap = cap;
        }
        t->spans[t->nspans].offset = t->len;
        t->spans[t->nspans].len = n;
        t->spans[t->nspans].kind = kind;
        t->nspans++;
    }
    memcpy(t->s + t->len, s, n);
    t->len += n;
    t->s[t->len] = 0;
}

static void text_str(struct text *t, const char *s, int kind) {
    text_add(t, s, strlen(s), kind);
}

/* untrusted text on one line: ut_safe_text() first (line breaks become spaces) */
static void text_safe(struct text *t, const char *s, size_t n, int kind) {
    char *buf;
    if(t->failed) return;
    buf = malloc(n * 3 + 1);                    /* an ill-formed byte becomes U+FFFD: 3 bytes */
    if(!buf) {
        t->failed = 1;
        return;
    }
    text_add(t, buf, ut_safe_text(buf, n * 3 + 1, s, n, 1), kind);
    free(buf);
}

static void text_printf(struct text *t, int kind, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
static void text_printf(struct text *t, int kind, const char *fmt, ...) {
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    text_str(t, buf, kind);
}

/* shown from a new screenful: the pager may stop it, as any long output */
static void text_show(struct text *t) {
    new_screenful();
    if(t->failed) say("undertow: out of memory for this list");
    else ut_view_text(stdout, &term, t->s, t->len, t->spans, t->nspans);
    free(t->s);
    free(t->spans);
    memset(t, 0, sizeof(*t));
}

/**** screens ****/

/* how many PEM certificates the trusted roots file holds (-1: no file) */
static int count_roots(void) {
    char buf[128];
    int n = 0;
    FILE *f = fopen(UT_CA_PATH, "r");
    if(!f) return -1;
    while(fgets(buf, sizeof(buf), f)) if(!strncmp(buf, "-----BEGIN CERTIFICATE-----", 27)) n++;
    fclose(f);
    return n;
}

static void welcome(void) {
    char clock[200], https[220], when[UT_CLOCK_TEXT];
    unsigned char random[16];
    time_t now = ut_clock_now(&cfg->clock);
    int roots = count_roots(), strong = getentropy(random, sizeof(random)) == 0, why = errno;
    memset(random, 0, sizeof(random));
    ut_clock_format(now, when);
    if(!ut_clock_plausible(now))
        snprintf(clock, sizeof(clock), "Clock  %s: too early for HTTPS to check certificates\n"
                 "       set it with clock YYYY-MM-DD HH:MM:SS (in UTC)", when);
    else if(cfg->clock.override_set)
        snprintf(clock, sizeof(clock), "Clock  %s, set for this session", when);
    else if(cfg->clock.rtc_offset_minutes)
        snprintf(clock, sizeof(clock), "Clock  %s, the hardware clock less %d minutes", when,
                 cfg->clock.rtc_offset_minutes);
    else
        snprintf(clock, sizeof(clock), "Clock  %s, from the hardware clock", when);
    if(!strong)
        snprintf(https, sizeof(https), "HTTPS  unavailable: %s\n       http:// pages still work",
                 why == ENOSYS ? "this processor has no strong random numbers (RDRAND)" :
                 "the processor's strong random numbers failed");
    else if(roots <= 0)
        snprintf(https, sizeof(https), "HTTPS  unavailable: no trusted roots in %s\n       http:// pages still work",
                 UT_CA_PATH);
    else
        snprintf(https, sizeof(https), "HTTPS  ready: strong random numbers and %d trusted roots", roots);
    term_update();
    new_screenful();
    ut_view_welcome(stdout, &term, clock, https);
}

/* the current page, one screen from its reading position; message on its position line */
static void show(const char *message) {
    struct ut_page *p = ut_session_current(ses);
    if(!p) {
        welcome();
        return;
    }
    term_update();
    new_screenful();
    ut_view_page(stdout, &term, p, p->position, message);
}

static struct ut_page *need_page(void) {
    struct ut_page *p = ut_session_current(ses);
    if(!p) say("No page is open yet: type open and an address (help lists the commands).");
    return p;
}

/**** loading ****/

static void load(const struct ut_url *u) {
    char message[UT_FIELD_MAX + 80];
    int found = 1;
    term_update();
    ut_view_line(stdout, &term, "Loading ", u->text, " ...");
    fflush(stdout);
    loaded = 1;
    if(ut_session_load(ses, u, cfg, &found, err, sizeof(err))) {
        say("undertow: %s", err);
        return;
    }
    if(found) {
        show(NULL);
        return;
    }
    snprintf(message, sizeof(message), "the page has no part called #%.200s: this is its top", u->fragment);
    show(message);
}

static void open_address(const char *address) {
    struct ut_url *u = malloc(sizeof(*u));
    if(!u) {
        say("undertow: out of memory");
        return;
    }
    if(ut_url_parse(address, u, err, sizeof(err))) say("undertow: cannot open %.300s: %s", address, err);
    else load(u);
    free(u);
}

static void follow(unsigned long n) {
    struct ut_url *u = malloc(sizeof(*u));
    if(!u) {
        say("undertow: out of memory");
        return;
    }
    if(ut_session_link(ses, n, u, err, sizeof(err))) {
        say("undertow: %s", err);
    } else if(ut_session_same_page(ses, u)) {   /* a part of this page: no fetch */
        if(ut_session_jump(ses, u, err, sizeof(err))) say("undertow: %s", err);
        else show(NULL);
    } else {
        load(u);
    }
    free(u);
}

static void reload(void) {
    struct ut_page *p = need_page();
    if(!p) return;
    term_update();
    ut_view_line(stdout, &term, "Loading ", p->url.text, " ...");
    fflush(stdout);
    loaded = 1;
    if(ut_session_reload(ses, cfg, err, sizeof(err))) say("undertow: %s (the page shown before is kept)", err);
    else show(NULL);
}

/**** moving on the page ****/

static void next_screen(void) {
    struct ut_page *p = need_page();
    size_t to;
    if(!p) return;
    term_update();
    to = ut_view_next(&term, p->doc.text, p->doc.text_len, p->position);
    if(to >= p->doc.text_len) {
        say("This is the end of the page (top goes back to its start).");
        return;
    }
    p->position = to;
    show(NULL);
}

static void prev_screen(void) {
    struct ut_page *p = need_page();
    if(!p) return;
    term_update();
    if(ut_view_row_start(&term, p->doc.text, p->doc.text_len, p->position) == 0) {
        say("This is the top of the page.");
        return;
    }
    p->position = ut_view_prev(&term, p->doc.text, p->doc.text_len, p->position);
    show(NULL);
}

static void find(const char *words, int next) {
    char message[UT_SEARCH_MAX + 80];
    int r;
    if(!need_page()) return;
    r = ut_session_find(ses, words, next, err, sizeof(err));
    if(r < 0) {
        say("undertow: %s", err);
        return;
    }
    snprintf(message, sizeof(message), "\"%.200s\" is on the first line%s", ses->search,
             r ? " (found from the top again)" : "");
    show(message);
}

/**** lists ****/

static void links(void) {
    struct ut_page *p = need_page();
    struct text t;
    size_t i;
    int digits;
    if(!p) return;
    if(!p->doc.link_count) {
        say("This page has no links.");
        return;
    }
    memset(&t, 0, sizeof(t));
    text_printf(&t, UT_SPAN_HEADING, "Links on this page: %lu", (unsigned long)p->doc.link_count);
    if(p->doc.truncated & UT_TRUNC_LINKS)
        text_printf(&t, 0, " (the ones after the first %u have no number)", UT_LINKS_MAX);
    text_str(&t, "\n", 0);
    digits = p->doc.link_count >= 100 ? 3 : p->doc.link_count >= 10 ? 2 : 1;
    for(i = 0; i < p->doc.link_count; i++) {
        const struct ut_link *l = &p->doc.links[i];
        text_printf(&t, 0, "%*s", 3 - digits, "");
        text_printf(&t, UT_SPAN_LINK, "%*lu.", digits, (unsigned long)i + 1);
        text_str(&t, " ", 0);
        if(l->label && l->label[0]) text_safe(&t, l->label, strlen(l->label), 0);
        else text_str(&t, "(no text)", 0);
        text_str(&t, "\n     ", 0);                /* under the label, which starts in column 5 */
        text_safe(&t, l->url ? l->url : "", l->url ? strlen(l->url) : 0, 0);
        if(!l->usable) text_str(&t, "   (not an http: or https: address)", 0);
        text_str(&t, "\n", 0);
    }
    text_show(&t);
}

static void history(void) {
    struct text t;
    unsigned int i;
    if(!need_page()) return;
    memset(&t, 0, sizeof(t));
    text_printf(&t, UT_SPAN_HEADING, "Pages kept for back and forward: %u", ses->count);
    text_str(&t, " (> is the one on the screen)\n", 0);
    for(i = 0; i < ses->count; i++) {
        const struct ut_page *p = ses->pages[i];
        const char *title = p->doc.title && p->doc.title[0] ? p->doc.title : "(no title)";
        text_str(&t, i == ses->current ? "  > " : "    ", i == ses->current ? UT_SPAN_LINK : 0);
        text_printf(&t, 0, "%2u. ", i + 1);
        text_safe(&t, title, strlen(title), i == ses->current ? UT_SPAN_HEADING : 0);
        text_str(&t, "\n        ", 0);
        text_safe(&t, p->url.text, strlen(p->url.text), 0);
        text_str(&t, "\n", 0);
    }
    text_show(&t);
}

static void url(void) {
    struct ut_page *p = need_page();
    struct text t;
    if(!p) return;
    memset(&t, 0, sizeof(t));
    text_safe(&t, p->url.text, strlen(p->url.text), 0);
    text_printf(&t, 0, "\nstatus %d", p->status);
    if(p->reason[0]) {
        text_str(&t, " ", 0);
        text_safe(&t, p->reason, strnlen(p->reason, sizeof(p->reason)), 0);
    }
    if(p->url.secure && p->verified) text_str(&t, "; HTTPS verified", 0);
    else if(p->url.secure) text_str(&t, "; HTTPS not verified", 0);
    else text_str(&t, "; HTTP, unencrypted", 0);
    if(p->tls[0]) {
        text_str(&t, ", ", 0);
        text_safe(&t, p->tls, strnlen(p->tls, sizeof(p->tls)), 0);
    }
    if(p->content_type[0]) {
        text_str(&t, "; ", 0);
        text_safe(&t, p->content_type, strnlen(p->content_type, sizeof(p->content_type)), 0);
    }
    if(p->redirects) text_printf(&t, 0, "; %d redirect%s", p->redirects, p->redirects == 1 ? "" : "s");
    if(p->doc.notice[0]) {
        text_str(&t, "\n", 0);
        text_safe(&t, p->doc.notice, strnlen(p->doc.notice, sizeof(p->doc.notice)), 0);
    }
    text_str(&t, "\n", 0);
    text_show(&t);
}

static void clock_cmd(const char *arg) {
    char when[UT_CLOCK_TEXT];
    time_t now;
    if(arg[0]) {
        if(ut_clock_set(&cfg->clock, arg, err, sizeof(err))) {
            say("undertow: %s", err);
            return;
        }
        ut_clock_format(ut_clock_now(&cfg->clock), when);
        say("Clock set: %s, for this session only.", when);
        return;
    }
    now = ut_clock_now(&cfg->clock);
    ut_clock_format(now, when);
    if(cfg->clock.override_set) say("%s, as set for this session.", when);
    else if(cfg->clock.rtc_offset_minutes)
        say("%s: the hardware clock less %d minutes (rtc_offset_minutes in %s).", when,
            cfg->clock.rtc_offset_minutes, UT_CONF_PATH);
    else say("%s, from the hardware clock.", when);
    if(!ut_clock_plausible(now))
        say("That is too early for HTTPS to check certificates: set it with clock YYYY-MM-DD HH:MM:SS (UTC).");
}

static void help(void) {
    static const char *const rows[][2] = {
        { "open ADDRESS, o ADDRESS", "load a page (HTTPS unless it says http://)" },
        { "NUMBER, go NUMBER", "follow the link with that number" },
        { "Enter, next", "the next screen of the page (PgDn too)" },
        { "prev, top", "the screen before (PgUp too); the start of the page" },
        { "back, b", "the page before, where you left it" },
        { "forward, f", "the page after it again" },
        { "reload, r", "load this page again" },
        { "links", "the links of this page and their addresses" },
        { "find WORDS", "find the words from this screen on, in any case" },
        { "findnext", "the next place they are" },
        { "history", "the pages kept for back and forward" },
        { "url", "the whole address of this page, and how it came" },
        { "clock", "the UTC time HTTPS checks certificates against" },
        { "clock YYYY-MM-DD HH:MM:SS", "set it for this session" },
        { "help, ?", "this list" },
        { "quit, q", "leave Undertow (Ctrl+C or Ctrl+D at the prompt too)" }
    };
    struct text t;
    size_t i;
    memset(&t, 0, sizeof(t));
    text_str(&t, "Undertow " UT_VERSION ", the SurfOS web browser: the commands\n", UT_SPAN_HEADING);
    for(i = 0; i < sizeof(rows) / sizeof(rows[0]); i++) {
        size_t n = strlen(rows[i][0]);
        text_str(&t, "  ", 0);
        text_str(&t, rows[i][0], UT_SPAN_TYPED);
        text_printf(&t, 0, "%*s%s\n", n < 26 ? (int)(26 - n) : 1, "", rows[i][1]);
    }
    text_str(&t, "An address typed alone (example.com/news) is opened too.\n"
                 "Keys: Left, Right, Home, End, Backspace, Delete; Ctrl+U clears the line;\n"
                 "Up and Down bring back the lines typed before.\n", 0);
    text_show(&t);
}

/**** the commands ****/

static int is_number(const char *s) {
    if(!*s) return 0;
    for(; *s; s++) if(*s < '0' || *s > '9') return 0;
    return 1;
}

/* a word that names a page rather than a command: "://" in it, or a dot with letters after it */
static int is_address(const char *s) {
    const char *dot = strchr(s, '.');
    if(strstr(s, "://")) return 1;
    if(!dot || dot == s) return 0;
    for(dot++; *dot; dot++) if((*dot >= 'a' && *dot <= 'z') || (*dot >= 'A' && *dot <= 'Z')) return 1;
    return 0;
}

static void number_cmd(const char *digits) {
    struct ut_page *p = need_page();
    unsigned long n = 0;
    size_t len = strlen(digits);
    if(!p) return;
    if(len > 9) {                               /* more links than that there are not */
        say("undertow: there is no link %.20s%s: this page has %lu", digits, len > 20 ? "..." : "",
            (unsigned long)p->doc.link_count);
        return;
    }
    while(*digits) n = n * 10 + (unsigned long)(*digits++ - '0');
    follow(n);
}

/* one line typed at the prompt: 1 when it is the end */
static int run(char *line) {
    char *cmd, *arg, *end;
    size_t i;
    while(*line == ' ' || *line == '\t') line++;
    for(end = line + strlen(line); end > line && (end[-1] == ' ' || end[-1] == '\t'); end--);
    *end = 0;
    if(!*line) {                                /* Enter alone */
        next_screen();
        return 0;
    }
    cmd = line;
    for(arg = line; *arg && *arg != ' ' && *arg != '\t'; arg++);
    if(*arg) {
        *arg++ = 0;
        while(*arg == ' ' || *arg == '\t') arg++;
    }
    if(is_number(cmd)) {
        if(*arg) say("undertow: a link's number alone follows it (%.20s)", cmd);
        else number_cmd(cmd);
        return 0;
    }
    if(!*arg && is_address(cmd)) {
        open_address(cmd);
        return 0;
    }
    for(i = 0; cmd[i]; i++) if(cmd[i] >= 'A' && cmd[i] <= 'Z') cmd[i] = (char)(cmd[i] - 'A' + 'a');

#define IS(a) (!strcmp(cmd, a))
#define NO_ARG(name) if(*arg) { say("usage: %s (it takes nothing after it)", name); return 0; }
    if(IS("open") || IS("o")) {
        if(!*arg) say("usage: open ADDRESS (for example open https://example.com/)");
        else open_address(arg);
    } else if(IS("go")) {
        if(!is_number(arg)) say("usage: go NUMBER (a link's number; links lists them)");
        else number_cmd(arg);
    } else if(IS("back") || IS("b") || IS("forward") || IS("f")) {
        int back = cmd[0] == 'b';
        NO_ARG(back ? "back" : "forward");
        if(!need_page()) return 0;
        if(ut_session_move(ses, back ? -1 : 1))
            say(back ? "There is no page before this one." : "There is no page after this one.");
        else show(NULL);
    } else if(IS("reload") || IS("r")) {
        NO_ARG("reload");
        reload();
    } else if(IS("next")) {
        NO_ARG("next");
        next_screen();
    } else if(IS("prev")) {
        NO_ARG("prev");
        prev_screen();
    } else if(IS("top")) {
        NO_ARG("top");
        if(need_page()) {
            ut_session_current(ses)->position = 0;
            show(NULL);
        }
    } else if(IS("links")) {
        NO_ARG("links");
        links();
    } else if(IS("find")) {
        if(!*arg) say("usage: find WORDS (findnext finds them again)");
        else find(arg, 0);
    } else if(IS("findnext")) {
        NO_ARG("findnext");
        find(NULL, 1);
    } else if(IS("history")) {
        NO_ARG("history");
        history();
    } else if(IS("url")) {
        NO_ARG("url");
        url();
    } else if(IS("clock")) {
        clock_cmd(arg);
    } else if(IS("help") || IS("?")) {
        NO_ARG("help");
        help();
    } else if(IS("quit") || IS("q")) {
        return 1;
    } else {
        say("undertow: there is no command %.40s%s: help lists them", cmd, strlen(cmd) > 40 ? "..." : "");
    }
#undef IS
#undef NO_ARG
    return 0;
}

static int interactive(const char *address) {
    struct ut_edit_io io;
    char *line = malloc(UT_COMMAND_MAX + 1);
    int n;
    io.ctx = NULL;
    io.read = io_read;
    io.ready = io_ready;
    io.write = io_write;
    ses = malloc(sizeof(*ses));
    ed = ut_edit_new(&io);
    if(!line || !ses || !ed) {
        fprintf(stderr, "undertow: out of memory\n");
        free(line);
        free(ses);
        ut_edit_free(ed);
        return 1;
    }
    ut_session_init(ses);
    term_update();
    if(address) open_address(address);
    else welcome();
    for(;;) {
        fflush(stdout);
        term_update();
        n = ut_edit_line(ed, &term, loaded, line);
        loaded = 0;
        if(n == UT_EDIT_EOF || n == UT_EDIT_QUIT) break;
        if(n == UT_EDIT_LONG) say("undertow: the line is longer than %u bytes: it was not run", UT_COMMAND_MAX);
        else if(n == UT_EDIT_NEXT) next_screen();
        else if(n == UT_EDIT_PREV) prev_screen();
        else if(run(line)) break;
    }
    fflush(stdout);
    ut_session_free(ses);
    free(ses);
    ut_edit_free(ed);
    free(line);
    return 0;
}

/**** --dump ****/

/* safe UTF-8 (n bytes) on stdout: as it is into a file or a pipe; on a terminal each character as
   the terminal draws it (view.c: a stand-in for what it cannot draw), the line not wrapped */
static void dump_drawn(const char *s, size_t n, const struct ut_term *t) {
    unsigned int cp;
    const char *alt;
    size_t i, k;
    if(!t) {
        fwrite(s, 1, n, stdout);
        return;
    }
    for(i = 0; i < n; i += k) {
        k = ut_utf8_decode((const unsigned char *)s + i, n - i, &cp);
        alt = ut_view_stand_in(t, cp);
        if(alt) fputs(alt, stdout);
        else fwrite(s + i, 1, k, stdout);
    }
}

/* the page's text line by line, through ut_safe_text() all the same (t: stdout is that terminal) */
static void dump_text(const char *s, size_t len, const struct ut_term *t) {
    char *buf;
    size_t start = 0, end, longest = 0;
    for(end = 0; end <= len; end++)
        if(end == len || s[end] == '\n') {
            if(end - start > longest) longest = end - start;
            start = end + 1;
        }
    buf = malloc(longest * 3 + 1);              /* an ill-formed byte's U+FFFD: 3 bytes */
    if(!buf) {
        fprintf(stderr, "undertow: out of memory\n");
        return;
    }
    for(start = 0; start < len; start = end + 1) {
        for(end = start; end < len && s[end] != '\n'; end++);
        dump_drawn(buf, ut_safe_text(buf, longest * 3 + 1, s + start, end - start, 1), t);
        putchar('\n');
    }
    free(buf);
}

static int dump(const char *address) {
    struct ut_url *u = malloc(sizeof(*u));
    struct ut_response *r = calloc(1, sizeof(*r));
    struct ut_document *d = calloc(1, sizeof(*d));
    char safe[300], reason[64];
    const struct ut_term *t = NULL;
    int rc = 1;
    size_t i;
    if(!u || !r || !d) {
        fprintf(stderr, "undertow: out of memory\n");
        goto out;
    }
    ut_safe_text(safe, sizeof(safe), address, strlen(address), 0);
    if(ut_url_parse(address, u, err, sizeof(err))) {
        fprintf(stderr, "undertow: cannot open %s: %s\n", safe, err);
        goto out;
    }
    ut_safe_text(safe, sizeof(safe), u->text, strlen(u->text), 0);
    if(ut_fetch(u, cfg, r, err, sizeof(err))) {
        fprintf(stderr, "undertow: %s: %s\n", safe, err);
        goto out;
    }
    if(ut_render(r, d, err, sizeof(err))) {
        fprintf(stderr, "undertow: %s: %s\n", safe, err);
        goto out;
    }
    if(isatty(1)) {
        term_update();
        t = &term;
    }
    dump_text(d->text, d->text_len, t);
    if(d->link_count) {
        printf("\nLinks:\n");
        for(i = 0; i < d->link_count; i++) {
            const char *link = d->links[i].url ? d->links[i].url : "";
            size_t n = strlen(link);
            char *buf = malloc(n * 3 + 1);
            if(!buf) break;
            printf("[%lu] ", (unsigned long)i + 1);
            dump_drawn(buf, ut_safe_text(buf, n * 3 + 1, link, n, 1), t);
            putchar('\n');
            free(buf);
        }
    }
    fflush(stdout);
    ut_safe_text(safe, sizeof(safe), r->url.text, strlen(r->url.text), 0);
    ut_safe_text(reason, sizeof(reason), r->reason, strnlen(r->reason, sizeof(r->reason)), 0);
    fprintf(stderr, "undertow: %s: HTTP %d%s%s, %s%s%s\n", safe, r->status, reason[0] ? " " : "", reason,
            r->url.secure ? (r->verified ? "HTTPS verified" : "HTTPS not verified") : "HTTP unencrypted",
            r->tls[0] ? " " : "", r->tls[0] ? r->tls : "");
    if(d->notice[0]) {
        ut_safe_text(safe, sizeof(safe), d->notice, strnlen(d->notice, sizeof(d->notice)), 0);
        fprintf(stderr, "undertow: %s\n", safe);
    }
    rc = r->status >= 200 && r->status < 300 ? 0 : 1;
out:
    if(d) ut_document_free(d);
    if(r) ut_response_free(r);
    free(d);
    free(r);
    free(u);
    return rc;
}

/**** the start ****/

int main(int argc, char **argv) {
    char why[UT_ERR_MAX];
    const char *address = NULL;
    int dumping = 0, rc;
    if(argc == 2 && (!strcmp(argv[1], "--help") || !strcmp(argv[1], "-h"))) {
        usage(stdout);
        return 0;
    }
    if(argc >= 2 && !strcmp(argv[1], "--dump")) {
        if(argc != 3 || !argv[2][0]) {
            fprintf(stderr, "undertow: --dump takes one address\n");
            usage(stderr);
            return 2;
        }
        dumping = 1;
        address = argv[2];
    } else if(argc == 2 && argv[1][0] == '-') {
        fprintf(stderr, "undertow: no option %.40s\n", argv[1]);
        usage(stderr);
        return 2;
    } else if(argc > 2) {
        fprintf(stderr, "undertow: one address at a time (in Undertow, open loads the next)\n");
        usage(stderr);
        return 2;
    } else if(argc == 2) {
        address = argv[1];
    }
    cfg = malloc(sizeof(*cfg));
    if(!cfg) {
        fprintf(stderr, "undertow: out of memory\n");
        return 1;
    }
    ut_config_init(cfg);
    if(ut_config_load(cfg, UT_CONF_PATH, why, sizeof(why)))
        fprintf(stderr, "undertow: warning: %s; the settings are the defaults\n", why);
    if(dumping) {
        rc = dump(address);
    } else if(!isatty(0) || !isatty(1)) {
        fprintf(stderr, "undertow: reading pages needs a terminal; undertow --dump ADDRESS prints one\n");
        rc = 2;
    } else {
        rc = interactive(address);
    }
    ut_transport_cleanup();
    free(cfg);
    return rc;
}
