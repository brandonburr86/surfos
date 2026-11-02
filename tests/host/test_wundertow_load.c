/*
SurfOS Undertow's window: tests of the loader
----------------------
File: test_wundertow_load.c  Date: 11/1/26
----------------------
user/wundertow/loader.c and pump.c on the host, over real pipes and real processes: this file is loadproc.c, and each
"helper" is a forked child that writes what the test scripted and ends, or hangs. Answers that arrive whole and a byte
at a time; a helper that dies at every byte of its answer; one that faults, whose descriptors stay open until it is
collected, as SurfOS has it; a helper that says anything at all (the window reads its pipe as it reads the network):
wrong magic, kinds and lengths, sizes that overflow, strings without their ends, a NUL in an address, more than a
page may have; a helper that never answers, killed at its deadline, by Stop, by a new load; allocation failures;
nothing held, no child left and no descriptor open at the end of each. And the window's loop itself, with a browser
and a pipe for its events.
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <time.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <poll.h>
#include <sys/wait.h>
#include "undertow.h"
#include "alloc.h"
#include "url.h"
#include "config.h"
#include "http.h"
#include "doc.h"
#include "browser.h"
#include "loader.h"

static int failures, checks;
static unsigned int now_ms;

static void check(int ok, const char *what) {
    checks++;
    if(ok) return;
    failures++;
    printf("FAIL %s\n", what);
}

unsigned int uptime(void) { return now_ms; }
time_t ut_sim_time(time_t *t) { if(t) *t = 0; return 0; }

/**** the helper: a child writing the script ****/

static const unsigned char *script, *picture_script;
static size_t script_len, picture_len, script_cut = (size_t)-1;
static int script_bytewise, script_hang, script_faults, spawn_fails;
static const unsigned char *text_script;    /* 11/02/26: what a helper for a text (a sheet) says, when not the page's */
static size_t text_len;
static int text_spawn_fails, picture_hang;  /* a text's helper cannot start; a picture's says nothing and hangs */
static int held_fd = -1;                /* a faulted helper's write end: it stays open until the helper is collected */
static char asked_kind[16], asked_url[WB_ADDR_MAX];
const char *ld_self = LD_SELF;

int ld_spawn(const char *kind, const char *url, int fd) {
    pid_t pid;
    snprintf(asked_kind, sizeof(asked_kind), "%s", kind);
    snprintf(asked_url, sizeof(asked_url), "%s", url);
    if(spawn_fails || (text_spawn_fails && !strcmp(kind, "text"))) {
        errno = ENOMEM;
        return -1;
    }
    fflush(stdout);
    if(script_faults) held_fd = dup(fd);
    pid = fork();
    if(pid == 0) {
        size_t i, n = script_cut < script_len ? script_cut : script_len;
        signal(SIGPIPE, SIG_DFL);
        if(picture_script && !strcmp(kind, "image")) {
            script = picture_script;
            n = picture_len;
        }
        if(picture_hang && !strcmp(kind, "image")) for(;;) pause();
        if(text_script && !strcmp(kind, "text")) {
            script = text_script;
            n = text_len;
        }
        if(script_bytewise) {
            for(i = 0; i < n; i++) {
                (void)!write(fd, script + i, 1);
                usleep(50);             /* so that the window sees them apart */
            }
        } else {
            size_t done = 0;
            while(done < n) {
                ssize_t k = write(fd, script + done, n - done);
                if(k <= 0) break;
                done += (size_t)k;
            }
        }
        if(script_hang) for(;;) pause();
        _exit(0);
    }
    return (int)pid;
}

void ld_kill(int pid) { kill(pid, SIGKILL); }

int ld_collect(void) {
    int st, r = waitpid(-1, &st, WNOHANG);
    if(r > 0 && held_fd >= 0) {         /* SurfOS: a killed task's descriptors close at its reap */
        close(held_fd);
        held_fd = -1;
    }
    return r < 0 ? -1 : r > 0;
}

/**** driving it ****/

static struct loader L;
static struct ld_result R;
static struct wb_request Q;

static void ask(int kind, const char *url) {
    memset(&Q, 0, sizeof(Q));
    Q.kind = kind;
    snprintf(Q.url, sizeof(Q.url), "%s", url);
    Q.page = 7;
    Q.item = 3;
    Q.budget = 1u << 20;
}

/* poll and read until the load in slot ends (1) or nothing has come for a second (0) */
static int until_end(int slot) {
    for(;;) {
        struct pollfd f;
        int r;
        f.fd = L.job[slot].fd;
        f.events = POLLIN;
        f.revents = 0;
        r = poll(&f, 1, 3000);
        if(r <= 0) return 0;
        if(ld_read(&L, slot, &R)) return 1;
    }
}

static int open_fds(void) {
    int fd, n = 0;
    for(fd = 0; fd < 128; fd++) if(fcntl(fd, F_GETFD) != -1) n++;
    return n;
}

static int base_fds;

static void all_gone(const char *what) {
    char s[200];
    int tries, st;
    for(tries = 0; tries < 2000 && ld_reap(&L); tries++) usleep(1000);
    snprintf(s, sizeof(s), "%s: every helper collected", what);
    check(!L.zombies && waitpid(-1, &st, WNOHANG) == -1 && errno == ECHILD, s);
    snprintf(s, sizeof(s), "%s: no descriptor left open", what);
    check(open_fds() == base_fds, s);
    snprintf(s, sizeof(s), "%s: nothing held", what);
    check(ut_alloc_live == 0, s);
}

/**** answers ****/

static unsigned char msg[1 << 17];
static size_t msg_len;

static void add(const void *p, size_t n) {
    memcpy(msg + msg_len, p, n);
    msg_len += n;
}

static struct ld_head head(unsigned int what, unsigned int url_len, unsigned int len) {
    struct ld_head h;
    memset(&h, 0, sizeof(h));
    h.magic = LD_MAGIC;
    h.what = what;
    h.status = 200;
    h.url_len = url_len;
    h.len = len;
    snprintf(h.reason, sizeof(h.reason), "OK");
    snprintf(h.type, sizeof(h.type), "text/html");
    return h;
}

static void body_answer(const char *url, const char *body) {
    struct ld_head h = head(LD_BODY, (unsigned int)strlen(url), (unsigned int)strlen(body));
    msg_len = 0;
    add(&h, sizeof(h));
    add(url, strlen(url));
    add(body, strlen(body));
}

static void go(const unsigned char *s, size_t n) {
    script = s;
    script_len = n;
    script_cut = (size_t)-1;
    script_bytewise = 0;
    script_hang = 0;
}

static void good_page(int bytewise) {
    struct ut_response r;
    char err[200];
    const char *what = bytewise ? "a page a byte at a time" : "a page";
    body_answer("https://a.test/final", "<title>T</title>Hello");
    go(msg, msg_len);
    script_bytewise = bytewise;
    ask(WB_LOAD_PAGE, "https://a.test/asked");
    Q.reload = 1;
    check(!ld_start(&L, &Q, err, sizeof(err)) && ld_busy(&L), "a helper starts");
    check(!strcmp(asked_kind, "page") && !strcmp(asked_url, "https://a.test/asked"), "it is told the kind and the address");
    ut_alloc_fill = 0x5a;               /* what it reads into is not zeroed for it */
    check(until_end(0), what);
    ut_alloc_fill = -1;
    check(!R.failed && R.kind == WB_LOAD_PAGE && R.reload == 1 && R.page == 7 && !strcmp(R.asked, "https://a.test/asked"), "the answer says what was asked");
    check(!strcmp(R.url, "https://a.test/final") && R.len == 21 && !memcmp(R.body, "<title>T</title>Hello", 22), "the address it ended at, and the body with its NUL");
    check(R.status == 200 && !R.verified && !strcmp(R.reason, "OK") && !strcmp(R.type, "text/html"), "its head");
    check(!ld_response(&R, &r, err, sizeof(err)) && !strcmp(r.url.host, "a.test") && r.body == R.body && r.len == 21 && r.status == 200, "a response from it");
    check(!ld_busy(&L), "its slot is free");
    ut_test_free(R.body);
    all_gone(what);
}

/* A helper's strings come through as safe text, whatever they hold */
static void unsafe_strings(void) {
    char err[200];
    struct ld_head h = head(LD_BODY, 12, 4);
    snprintf(h.tls, sizeof(h.tls), "T\x1b[31m");
    snprintf(h.reason, sizeof(h.reason), "R\x1b[2J\n");
    snprintf(h.type, sizeof(h.type), "t/\x07");
    snprintf(h.charset, sizeof(h.charset), "c\x1b");
    h.redirects = 77;
    msg_len = 0;
    add(&h, sizeof(h));
    add("https://a.t/", 12);
    add("body", 4);
    go(msg, msg_len);
    ask(WB_LOAD_PAGE, "https://a.t/");
    ld_start(&L, &Q, err, sizeof(err));
    check(until_end(0) && !R.failed, "an answer with escapes in its strings");
    check(!strchr(R.tls, 0x1b) && !strchr(R.reason, 0x1b) && !strchr(R.reason, '\n') && !strchr(R.type, 7) && !strchr(R.charset, 0x1b),
          "they arrive safe");
    check(!R.redirects, "a number of redirects that cannot be is none");
    ut_test_free(R.body);
    all_gone("escapes");
}

/* "Verified" is a word about https: a helper's for anything else is no word */
static void verified(void) {
    char err[200];
    static const char *const urls[2] = { "http://a.t/", "https://a.t/" };
    int i;
    for(i = 0; i < 2; i++) {
        struct ld_head h = head(LD_BODY, (unsigned int)strlen(urls[i]), 4);
        h.verified = 1;
        msg_len = 0;
        add(&h, sizeof(h));
        add(urls[i], strlen(urls[i]));
        add("body", 4);
        go(msg, msg_len);
        ask(WB_LOAD_PAGE, urls[i]);
        ld_start(&L, &Q, err, sizeof(err));
        check(until_end(0) && !R.failed && R.verified == i, i ? "verified, over https" : "not over http, whatever a helper says");
        ut_test_free(R.body);
        all_gone("verified");
    }
}

static void good_picture(void) {
    unsigned int px[6] = { 1, 2, 3, 4, 5, 6 };
    struct ld_head h = head(LD_PIXELS, 14, sizeof(px));
    char err[200];
    h.w = 6;
    h.h = 4;
    h.pw = 3;
    h.ph = 2;
    msg_len = 0;
    add(&h, sizeof(h));
    add("https://a.t/p", 13);
    msg_len--;                          /* the head says 14 */
    h.url_len = 13;
    msg_len = 0;
    add(&h, sizeof(h));
    add("https://a.t/p", 13);
    add(px, sizeof(px));
    go(msg, msg_len);
    ask(WB_LOAD_IMAGE, "https://a.t/p");
    check(!ld_start(&L, &Q, err, sizeof(err)) && !strcmp(asked_kind, "image"), "a helper for a picture");
    check(L.job[0].state == LJ_FREE && L.job[1].state != LJ_FREE, "not in the page's slot");
    check(until_end(1), "a picture");
    check(!R.failed && R.w == 6 && R.h == 4 && R.pw == 3 && R.ph == 2 && R.len == sizeof(px) && !memcmp(R.body, px, sizeof(px)), "its size and pixels");
    check(R.item == 3, "which picture");
    ut_test_free(R.body);
    /* the page has no room for it: the size still comes back */
    go(msg, msg_len);
    ask(WB_LOAD_IMAGE, "https://a.t/p");
    Q.budget = 10;
    ld_start(&L, &Q, err, sizeof(err));
    check(until_end(1), "a picture the page has no room for");
    check(R.failed && strstr(R.why, "as many pictures") && R.w == 6 && R.h == 4 && !R.body, "no pixels; its size");
    all_gone("pictures");
}

static void failure_answer(void) {
    struct ld_head h = head(LD_FAILED, 0, 12);
    char err[200];
    msg_len = 0;
    add(&h, sizeof(h));
    add("no \x1b[31mway\n", 12);
    go(msg, msg_len);
    ask(WB_LOAD_PAGE, "https://a.t/");
    ld_start(&L, &Q, err, sizeof(err));
    check(until_end(0), "a failure");
    check(R.failed && !strstr(R.why, "\x1b") && !strchr(R.why, '\n') && strstr(R.why, "way"), "its sentence, safe");
    /* no sentence at all */
    h.len = 0;
    msg_len = 0;
    add(&h, sizeof(h));
    go(msg, msg_len);
    ask(WB_LOAD_PAGE, "https://a.t/");
    ld_start(&L, &Q, err, sizeof(err));
    check(until_end(0) && R.failed && R.why[0], "a failure with no sentence");
    all_gone("failures");
}

/* A helper that dies at every byte of its answer */
static void dies(void) {
    size_t cut;
    char err[200];
    body_answer("https://a.test/x", "<p>body");
    for(cut = 0; cut < msg_len; cut++) {
        go(msg, msg_len);
        script_cut = cut;
        ask(WB_LOAD_PAGE, "https://a.test/x");
        check(!ld_start(&L, &Q, err, sizeof(err)), "a helper starts");
        check(until_end(0) && R.failed && strstr(R.why, "helper stopped") && !R.body, "a helper that dies is a failed load");
        ld_reap(&L);
    }
    all_gone("helpers that die");
}

/* What a helper gone bad may say */
struct lie { const char *what; struct ld_head h; const char *url; const void *rest; size_t restlen; int kind; };

static void lies(void) {
    static const unsigned int two[2] = { 1, 2 };
    struct ld_head h, base = head(LD_BODY, 12, 4);
    static struct lie t[48];
    int n = 0, i;
    char err[200];
#define LIE(name, field, value, k) do { h = base; h.field; t[n].what = name; t[n].h = h; t[n].url = "https://a.t/"; t[n].rest = "body"; t[n].restlen = 4; t[n].kind = k; n++; (void)(value); } while(0)
    LIE("a wrong magic", magic = 0x12345678, 0, WB_LOAD_PAGE);
    LIE("no kind of answer", what = 0, 0, WB_LOAD_PAGE);
    LIE("an answer of kind 9", what = 9, 0, WB_LOAD_PAGE);
    LIE("a body for a picture", status = 200, 0, WB_LOAD_IMAGE);
    LIE("a status of 0", status = 0, 0, WB_LOAD_PAGE);
    LIE("a status of 1000", status = 1000, 0, WB_LOAD_PAGE);
    LIE("a negative status", status = -5, 0, WB_LOAD_PAGE);
    LIE("an address of 2049 bytes", url_len = UT_URL_MAX + 1, 0, WB_LOAD_PAGE);
    LIE("an address of 4 GB", url_len = 0xffffffffu, 0, WB_LOAD_PAGE);
    LIE("no address", url_len = 0, 0, WB_LOAD_PAGE);
    LIE("a body over 1 MiB", len = UT_BODY_MAX + 1, 0, WB_LOAD_PAGE);
    LIE("a body of 4 GB", len = 0xffffffffu, 0, WB_LOAD_PAGE);
    base = head(LD_PIXELS, 12, 8);
    base.w = base.pw = 2;
    base.h = base.ph = 1;
    LIE("pixels for a page", status = 200, 0, WB_LOAD_PAGE);
    LIE("wider pixels than the picture", pw = 3, 0, WB_LOAD_IMAGE);
    LIE("taller pixels than the picture", ph = 2, 0, WB_LOAD_IMAGE);
    LIE("a picture of 5000 pixels", w = 5000, 0, WB_LOAD_IMAGE);
    LIE("a picture 5000 high", h = 5000, 0, WB_LOAD_IMAGE);
    LIE("the wrong length for its pixels", len = 12, 0, WB_LOAD_IMAGE);
    LIE("pixels with no address", url_len = 0, 0, WB_LOAD_IMAGE);
    LIE("pixels of a status of 0", status = 0, 0, WB_LOAD_IMAGE);
    /* no size: the lengths agree, so only the sizes themselves can say no */
    base.len = 0;
    LIE("a picture of no pixels across", pw = 0, 0, WB_LOAD_IMAGE);
    LIE("a picture of no pixels down", ph = 0, 0, WB_LOAD_IMAGE);
    {
        h = base;
        h.w = h.pw = 0;
        t[n].what = "a picture of no width";
        t[n].h = h;
        t[n].url = "https://a.t/";
        t[n].rest = two;
        t[n].restlen = 0;
        t[n++].kind = WB_LOAD_IMAGE;
        h = base;
        h.h = h.ph = 0;
        t[n].what = "a picture of no height";
        t[n].h = h;
        t[n].url = "https://a.t/";
        t[n].rest = two;
        t[n].restlen = 0;
        t[n++].kind = WB_LOAD_IMAGE;
    }
    base.len = 8;
    {                                   /* 2^16 x 2^16 x 4 is 0 in 32 bits */
        h = base;
        h.w = h.pw = h.h = h.ph = 0x10000;
        h.len = 0;
        t[n].what = "pixels whose size overflows 32 bits";
        t[n].h = h;
        t[n].url = "https://a.t/";
        t[n].rest = two;
        t[n].restlen = 8;
        t[n++].kind = WB_LOAD_IMAGE;
        h = base;
        h.w = h.pw = h.h = h.ph = 4096;
        h.len = 0x4000000;              /* right, and more than the budget */
        t[n].what = "pixels over the budget";
        t[n].h = h;
        t[n].url = "https://a.t/";
        t[n].rest = two;
        t[n].restlen = 8;
        t[n++].kind = WB_LOAD_IMAGE;
    }
    {                                   /* the lengths agree: only the picture's own size can say no */
        struct ld_head p = head(LD_PIXELS, 12, 12);
        p.w = 2;
        p.h = 1;
        p.pw = 3;
        p.ph = 1;
        t[n].what = "wider pixels than the picture, the length right";
        t[n].h = p;
        t[n].url = "https://a.t/";
        t[n].rest = two;
        t[n].restlen = 8;
        t[n++].kind = WB_LOAD_IMAGE;
        p = head(LD_PIXELS, 12, 8);
        p.w = 2;
        p.h = 1;
        p.pw = 1;
        p.ph = 2;
        t[n].what = "taller pixels than the picture, the length right";
        t[n].h = p;
        t[n].url = "https://a.t/";
        t[n].rest = two;
        t[n].restlen = 8;
        t[n++].kind = WB_LOAD_IMAGE;
        p = head(LD_PIXELS, 12, 4);
        p.w = 0;
        p.h = 1;
        p.pw = 1;
        p.ph = 1;
        t[n].what = "a picture of no width, with a pixel";
        t[n].h = p;
        t[n].url = "https://a.t/";
        t[n].rest = two;
        t[n].restlen = 4;
        t[n++].kind = WB_LOAD_IMAGE;
    }
    base = head(LD_FAILED, 0, 4);
    LIE("a failure of 4 GB", len = 0xffffffffu, 0, WB_LOAD_PAGE);
    {                                   /* only the address is wrong: the bytes that follow would be a good failure */
        h = base;
        h.url_len = 3;
        t[n].what = "a failure with an address";
        t[n].h = h;
        t[n].url = "abc";
        t[n].rest = "body";
        t[n].restlen = 4;
        t[n++].kind = WB_LOAD_PAGE;
    }
    for(i = 0; i < n; i++) {
        msg_len = 0;
        add(&t[i].h, sizeof(t[i].h));
        add(t[i].url, strlen(t[i].url));
        add(t[i].rest, t[i].restlen);
        go(msg, msg_len);
        script_hang = 1;                /* it does not go away by itself */
        ask(t[i].kind, "https://a.t/");
        ut_alloc_fail_after = -1;
        check(!ld_start(&L, &Q, err, sizeof(err)), "a helper starts");
        check(until_end(t[i].kind == WB_LOAD_PAGE ? 0 : 1), t[i].what);
        check(R.failed && !R.body && (strstr(R.why, "no answer") || strstr(R.why, "as many pictures")), t[i].what);
        if(!R.failed) ut_test_free(R.body);
        all_gone(t[i].what);
    }
    /* a failure's size is believed only within the limits; a picture too large for any frame gives none */
    h = head(LD_FAILED, 0, 2);
    h.w = 5000;
    h.h = 4000;
    msg_len = 0;
    add(&h, sizeof(h));
    add("no", 2);
    go(msg, msg_len);
    ask(WB_LOAD_IMAGE, "https://a.t/");
    ld_start(&L, &Q, err, sizeof(err));
    check(until_end(1) && R.failed && !R.w && !R.h, "a failure with a size past the limits");
    h = head(LD_PIXELS, 12, 8);
    h.w = 5000;
    h.h = 1;
    h.pw = 2;
    h.ph = 1;
    msg_len = 0;
    add(&h, sizeof(h));
    go(msg, msg_len);
    script_hang = 1;
    ask(WB_LOAD_IMAGE, "https://a.t/");
    ld_start(&L, &Q, err, sizeof(err));
    check(until_end(1) && R.failed && !R.w, "a picture past the limits has no frame's size to give");
    all_gone("sizes past the limits");
    /* an address with a NUL in it */
    h = head(LD_BODY, 12, 4);
    msg_len = 0;
    add(&h, sizeof(h));
    add("https://a\0t/", 12);
    add("body", 4);
    go(msg, msg_len);
    ask(WB_LOAD_PAGE, "https://a.t/");
    ld_start(&L, &Q, err, sizeof(err));
    script_hang = 1;
    check(until_end(0) && R.failed, "a NUL inside an address");
    all_gone("a NUL");
    /* strings that do not end */
    h = head(LD_BODY, 12, 4);
    memset(h.tls, 'A', sizeof(h.tls));
    memset(h.reason, 'B', sizeof(h.reason));
    memset(h.type, 'C', sizeof(h.type));
    memset(h.charset, 'D', sizeof(h.charset));
    msg_len = 0;
    add(&h, sizeof(h));
    add("https://a.t/", 12);
    add("body", 4);
    go(msg, msg_len);
    ask(WB_LOAD_PAGE, "https://a.t/");
    ld_start(&L, &Q, err, sizeof(err));
    check(until_end(0) && !R.failed, "strings without their ends: kept");
    check(strlen(R.tls) < sizeof(R.tls) && strlen(R.reason) < sizeof(R.reason) && strlen(R.type) < sizeof(R.type) && strlen(R.charset) < sizeof(R.charset), "and cut");
    ut_test_free(R.body);
    /* an address that is none */
    body_answer("gopher://a.t/", "x");
    go(msg, msg_len);
    ask(WB_LOAD_PAGE, "https://a.t/");
    ld_start(&L, &Q, err, sizeof(err));
    check(until_end(0) && !R.failed, "a helper's odd address comes through");
    {
        struct ut_response r;
        check(ld_response(&R, &r, err, sizeof(err)) == -1 && err[0], "but makes no response");
    }
    ut_test_free(R.body);
    all_gone("odd answers");
}

static void stuck(void) {
    char err[200];
    int wait = -1;
    /* a helper that never answers: the deadline */
    go(msg, 0);
    script_hang = 1;
    ask(WB_LOAD_PAGE, "https://never.test/");
    now_ms = 1000;
    ld_start(&L, &Q, err, sizeof(err));
    check(!ld_overdue(&L, now_ms + 1000, &R, &wait) && wait > 0 && wait <= (int)LD_PAGE_MS, "not overdue yet: when to look");
    check(ld_overdue(&L, now_ms + LD_PAGE_MS + 1, &R, &wait) && R.failed && strstr(R.why, "too long"), "past its deadline it is stopped");
    check(!ld_busy(&L), "its slot is free");
    all_gone("a helper past its deadline");
    /* Stop, and a new load in its place */
    ask(WB_LOAD_PAGE, "https://never.test/");
    ld_start(&L, &Q, err, sizeof(err));
    ask(WB_LOAD_IMAGE, "https://never.test/1");
    ld_start(&L, &Q, err, sizeof(err));
    ask(WB_LOAD_IMAGE, "https://never.test/2");
    ld_start(&L, &Q, err, sizeof(err));
    ask(WB_LOAD_IMAGE, "https://never.test/3");
    ld_start(&L, &Q, err, sizeof(err));
    check(L.job[1].state && L.job[2].state && L.job[3].state, "three at a time");
    ask(WB_LOAD_IMAGE, "https://never.test/4");
    check(ld_start(&L, &Q, err, sizeof(err)) == -1, "a fourth waits");
    ask(WB_LOAD_PAGE, "https://never.test/2");
    check(ld_start(&L, &Q, err, sizeof(err)) == -1, "a second page waits for the first to go");
    ld_cancel(&L, WB_CANCEL_RESOURCES);
    check(L.job[0].state && !L.job[1].state && !L.job[2].state && !L.job[3].state, "the pictures stopped, the page not");
    ld_cancel(&L, WB_CANCEL_PAGE);
    check(!ld_busy(&L), "the page stopped");
    all_gone("helpers stopped");
    {
        struct pollfd f[LD_HELPERS];
        int s[LD_HELPERS], n;
        ask(WB_LOAD_IMAGE, "https://never.test/1");
        ld_start(&L, &Q, err, sizeof(err));
        ask(WB_LOAD_PAGE, "https://never.test/2");
        ld_start(&L, &Q, err, sizeof(err));
        n = ld_fds(&L, f, s);
        check(n == 2 && s[0] == 0 && s[1] == 1 && f[0].fd == L.job[0].fd && f[1].events == POLLIN, "what to poll");
        ld_stop_all(&L);
        check(!ld_busy(&L), "all of them stopped");
        all_gone("everything stopped");
    }
    /* no process, no pipe */
    spawn_fails = 1;
    ask(WB_LOAD_PAGE, "https://x.test/");
    check(ld_start(&L, &Q, err, sizeof(err)) == -1 && strstr(err, "no helper") && !ld_busy(&L), "no process");
    spawn_fails = 0;
    all_gone("no process");
}

/**** the loop ****/

static struct wb B;

/* ld_start_loads() and ld_wait() as the window goes round, with fd for its events, until the browser is done or
   seconds have passed: the passes it took */
static int loop(int fd, int seconds) {
    time_t end = (time)(NULL) + seconds;
    int win, n = 0;
    while((time)(NULL) < end) {
        ld_start_loads(&L, &B);
        if(!ld_busy(&L) && !wb_busy(&B)) break;
        ld_wait(&L, &B, fd, &win);
        n++;
    }
    return n;
}


/* 11/02/26 (milestones 2 and 3 together): the last sheet of a page has no helper to be had, so the page is built again with
   the sheet that came inside ld_start_loads(), and its pictures' loads start again there (browser.c's wb_restyled()):
   what that ends is ended before anything more starts, so the loads that start again are not the ones it ends */
static void restyled_in_the_loop(int fd) {
    static unsigned char css[1024];
    static const char sheet[] = "@import \"b.css\"; .h{display:none}";
    struct ld_head h = head(LD_BODY, 20, sizeof(sheet) - 1);
    struct wb_page *p;
    int i, n, alive = 0, win;
    snprintf(h.type, sizeof(h.type), "text/css");
    memcpy(css, &h, sizeof(h));
    memcpy(css + sizeof(h), "https://a.test/a.css", 20);
    memcpy(css + sizeof(h) + 20, sheet, sizeof(sheet) - 1);
    text_script = css;
    text_len = sizeof(h) + 20 + sizeof(sheet) - 1;
    picture_hang = 1;
    body_answer("https://a.test/restyle", "<title>R</title><link rel=stylesheet href=a.css><p class=h><img src=1.png alt=a></p>"
                "<p><img src=2.png alt=b>");
    go(msg, msg_len);
    wb_go(&B, "https://a.test/restyle");
    for(n = 0; n < 40 && !(B.npages && (p = wb_current(&B))->ntexts == 2); n++) {
        ld_start_loads(&L, &B);
        ld_wait(&L, &B, fd, &win);
    }
    p = wb_current(&B);
    check(B.npages && !strcmp(p->doc.title, "R") && p->ntexts == 2 && p->texts[1].state == WP_WAIT && p->doc.image_count == 2 &&
          p->doc.pics[0].state == WP_RUNNING && p->doc.pics[1].state == WP_RUNNING,
          "a page whose sheet came and imports another, with two pictures on their way");
    text_spawn_fails = 1;
    ld_start_loads(&L, &B);
    ld_start_loads(&L, &B);
    text_spawn_fails = 0;
    for(i = 1; i < LD_HELPERS; i++) if(L.job[i].state != LJ_FREE && L.job[i].kind == WB_LOAD_IMAGE && L.job[i].item == 0) alive++;
    check(p->doc.image_count == 1 && p->doc.pics[0].state == WP_RUNNING && !strcmp(p->doc.images[0], "https://a.test/2.png"),
          "the import has no helper: the page is built again, its first picture hidden, the other's load started again");
    check(alive == 1 && B.inflight == 1, "and that load is on its way: the loads ended were the old ones");
    picture_hang = 0;
    text_script = NULL;
    wb_stop(&B);
    ld_start_loads(&L, &B);
    check(!ld_busy(&L) && !wb_busy(&B), "Stop ends it");
}

static void the_loop(void) {
    int ev[2], win = 1;
    char c = 'k';
    check(!pipe(ev), "a pipe for the window's events");
    check(!wb_init(&B, 640, 400), "a browser");
    /* a page */
    body_answer("https://a.test/", "<title>Loop</title><p>Hello");
    go(msg, msg_len);
    wb_go(&B, "https://a.test/");
    loop(ev[0], 5);
    check(B.npages == 1 && !strcmp(wb_current(&B)->doc.title, "Loop") && !wb_busy(&B), "a page through the loop");
    /* a page's pictures: the loop gives them to the browser, whose they are from then on */
    {
        static unsigned char pic[512];
        static const unsigned int two[2] = { 0x112233, 0x445566 };
        struct ld_head h = head(LD_PIXELS, 13, sizeof(two));
        struct wb_page *p;
        size_t n = 0;
        h.w = 2;
        h.h = 1;
        h.pw = 2;
        h.ph = 1;
        memcpy(pic, &h, sizeof(h));
        n = sizeof(h);
        memcpy(pic + n, "https://a.t/p", 13);
        n += 13;
        memcpy(pic + n, two, sizeof(two));
        n += sizeof(two);
        picture_script = pic;
        picture_len = n;
        body_answer("https://a.test/pics", "<title>Pics</title><img src=1.png alt=a><img src=2.png alt=b>");
        go(msg, msg_len);
        wb_go(&B, "https://a.test/pics");
        loop(ev[0], 5);
        p = wb_current(&B);
        check(!strcmp(p->doc.title, "Pics") && p->doc.pics[0].px && p->doc.pics[1].px && p->doc.pics[1].px[1] == 0x445566, "a page and its pictures through the loop");
        check(p->doc.pics[0].w == 2 && p->pic_bytes == 16, "their sizes");
        /* a picture that fails, and one with more than the page has room for */
        msg_len = 0;
        h = head(LD_FAILED, 0, 2);
        memcpy(pic, &h, sizeof(h));
        memcpy(pic + sizeof(h), "no", 2);
        picture_len = sizeof(h) + 2;
        body_answer("https://a.test/more", "<img src=3.png alt=c>");
        go(msg, msg_len);
        wb_go(&B, "https://a.test/more");
        loop(ev[0], 5);
        p = wb_current(&B);
        check(p->doc.pics[0].state == WP_FAILED && !p->doc.pics[0].px && !wb_busy(&B), "a picture that fails: the frame stays, the page too");
        picture_script = NULL;
    }
    while(ld_reap(&L)) usleep(1000);

    /* a helper that faults: nothing on the pipe, its write end open until it is collected, and nobody known to be owed
       a collecting: the page's helper has gone by now */
    while(ld_reap(&L)) usleep(1000);
    check(!L.zombies, "nobody is owed a collecting");
    go(msg, 0);
    script_faults = 1;
    wb_go(&B, "https://faults.test/");
    now_ms = 500000;
    loop(ev[0], 5);
    script_faults = 0;
    check(!wb_busy(&B) && strstr(B.status, "helper stopped") != NULL, "a helper that faults: said at once, not at its deadline");
    check(B.npages == 3, "and the page stays");
    /* the window's events and a deadline: ld_wait() says the window has none when it comes by the deadline */
    go(msg, 0);
    script_hang = 1;
    wb_go(&B, "https://never.test/");
    ld_start_loads(&L, &B);
    check(write(ev[1], &c, 1) == 1, "an event");
    ld_wait(&L, &B, ev[0], &win);
    check(win == 1 && wb_busy(&B), "the window has events");
    now_ms += LD_PAGE_MS + 1;
    check(ld_wait(&L, &B, ev[0], &win) == 1 && !win && !wb_busy(&B) && strstr(B.status, "too long") != NULL,
          "past the deadline: a failed load, and no word of events");
    (void)!read(ev[0], &c, 1);
    now_ms += 10;
    /* Stop: the helper goes */
    wb_go(&B, "https://never.test/");
    ld_start_loads(&L, &B);
    check(ld_busy(&L), "a helper at work");
    wb_stop(&B);
    ld_start_loads(&L, &B);
    check(!ld_busy(&L), "Stop: gone");
    /* a new load takes the place of the one on its way */
    go(msg, 0);
    script_hang = 1;
    wb_go(&B, "https://old.test/");
    check(ld_start_loads(&L, &B) == 1, "a load that starts: something new to show");
    wb_go(&B, "https://new.test/");
    ld_start_loads(&L, &B);
    check(L.job[0].state != LJ_FREE && !strcmp(L.job[0].asked, "https://new.test/") && wb_busy(&B), "a new load takes the place of the old");
    wb_stop(&B);
    ld_start_loads(&L, &B);
    /* a file page is what was asked, never what a helper says */
    body_answer("https://" WB_FILE_HOST "/etc/x", "<p>x");
    go(msg, msg_len);
    wb_go(&B, "https://a.test/claims");
    loop(ev[0], 5);
    check(!wb_current(&B)->file, "a helper's answer makes no file page");
    body_answer("https://" WB_FILE_HOST "/some/page.html", "<p>x");
    go(msg, msg_len);
    wb_go(&B, "file:///some/page.html");
    loop(ev[0], 5);
    check(wb_current(&B)->file && !strcmp(wb_current(&B)->url, "file:///some/page.html"), "asked for a file, a file page");
    restyled_in_the_loop(ev[0]);
    /* no helper to be had */
    spawn_fails = 1;
    wb_go(&B, "https://x.test/");
    check(ld_start_loads(&L, &B) == 1 && !wb_busy(&B) && strstr(B.status, "no helper") != NULL, "no helper: a failed load");
    spawn_fails = 0;
    wb_free(&B);
    close(ev[0]);
    close(ev[1]);
    all_gone("the loop");
}

static void memory(void) {
    char err[200];
    long n, fails = 0;
    body_answer("https://a.test/x", "<p>body");
    for(n = 0; n < 4; n++) {
        go(msg, msg_len);
        ask(WB_LOAD_PAGE, "https://a.test/x");
        ut_alloc_fail_after = n;
        check(!ld_start(&L, &Q, err, sizeof(err)), "a helper starts");
        check(until_end(0), "an answer without memory");
        ut_alloc_fail_after = -1;
        if(R.failed) fails++;
        else ut_test_free(R.body);
        all_gone("no memory");
    }
    check(fails >= 1, "no memory for a body is a failed load");
}

int main(void) {
    signal(SIGPIPE, SIG_IGN);
    ld_init(&L);
    base_fds = open_fds();
    good_page(0);
    good_page(1);
    unsafe_strings();
    verified();
    good_picture();
    failure_answer();
    dies();
    lies();
    stuck();
    the_loop();
    memory();
    if(failures) {
        printf("WUNDERTOW LOADER TESTS FAILED: %d of %d checks\n", failures, checks);
        return 1;
    }
    printf("WUNDERTOW LOADER TESTS PASSED: %d checks, 0 failures\n", checks);
    return 0;
}
