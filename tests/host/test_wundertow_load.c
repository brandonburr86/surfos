/*
SurfOS Undertow's window: tests of the loader
----------------------
File: test_wundertow_load.c  Date: 11/1/26
----------------------
user/wundertow/loader.c on the host, over real pipes and real processes: this file is loadproc.c, and each "helper" is
a forked child that writes what the test scripted and ends, or hangs. Answers that arrive whole and a byte at a time;
a helper that dies at every byte of its answer; a helper that says anything at all (the window reads its pipe as it
reads the network): wrong magic, kinds and lengths, sizes that overflow, strings without their ends, a NUL in an
address, more than a page may have; a helper that never answers, killed at its deadline, by Stop, by a new load;
allocation failures; nothing held and no child left at the end of each.
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <time.h>
#include <errno.h>
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

static const unsigned char *script;
static size_t script_len, script_cut = (size_t)-1;
static int script_bytewise, script_hang, spawn_fails;
static char asked_kind[16], asked_url[WB_ADDR_MAX];

int ld_spawn(const char *kind, const char *url, int fd) {
    pid_t pid;
    snprintf(asked_kind, sizeof(asked_kind), "%s", kind);
    snprintf(asked_url, sizeof(asked_url), "%s", url);
    if(spawn_fails) {
        errno = ENOMEM;
        return -1;
    }
    fflush(stdout);
    pid = fork();
    if(pid == 0) {
        size_t i, n = script_cut < script_len ? script_cut : script_len;
        signal(SIGPIPE, SIG_DFL);
        if(script_bytewise) for(i = 0; i < n; i++) (void)!write(fd, script + i, 1);
        else {
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

static void all_gone(const char *what) {
    char s[200];
    int tries;
    for(tries = 0; tries < 2000 && ld_reap(&L); tries++) usleep(1000);
    snprintf(s, sizeof(s), "%s: every helper collected", what);
    check(!L.zombies, s);
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
    check(until_end(0), what);
    check(!R.failed && R.kind == WB_LOAD_PAGE && R.reload == 1 && R.page == 7 && !strcmp(R.asked, "https://a.test/asked"), "the answer says what was asked");
    check(!strcmp(R.url, "https://a.test/final") && R.len == 21 && !memcmp(R.body, "<title>T</title>Hello", 22), "the address it ended at, and the body with its NUL");
    check(R.status == 200 && !strcmp(R.reason, "OK") && !strcmp(R.type, "text/html"), "its head");
    check(!ld_response(&R, &r, err, sizeof(err)) && !strcmp(r.url.host, "a.test") && r.body == R.body && r.len == 21 && r.status == 200, "a response from it");
    check(!ld_busy(&L), "its slot is free");
    ut_test_free(R.body);
    all_gone(what);
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
    static struct lie t[32];
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
    LIE("a picture of no width", w = 0, 0, WB_LOAD_IMAGE);
    LIE("pixels of no height", ph = 0, 0, WB_LOAD_IMAGE);
    LIE("wider pixels than the picture", pw = 3, 0, WB_LOAD_IMAGE);
    LIE("taller pixels than the picture", ph = 2, 0, WB_LOAD_IMAGE);
    LIE("a picture of 5000 pixels", w = 5000, 0, WB_LOAD_IMAGE);
    LIE("the wrong length for its pixels", len = 12, 0, WB_LOAD_IMAGE);
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
    base = head(LD_FAILED, 0, 4);
    LIE("a failure with an address", url_len = 3, 0, WB_LOAD_PAGE);
    LIE("a failure of 4 GB", len = 0xffffffffu, 0, WB_LOAD_PAGE);
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
        check(R.failed && !R.body && R.why[0], t[i].what);
        if(!R.failed) ut_test_free(R.body);
        all_gone(t[i].what);
    }
    /* an address with a NUL in it */
    h = head(LD_BODY, 12, 4);
    msg_len = 0;
    add(&h, sizeof(h));
    add("https://a\0t/", 12);
    add("body", 4);
    go(msg, msg_len);
    ask(WB_LOAD_PAGE, "https://a.t/");
    ld_start(&L, &Q, err, sizeof(err));
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
    good_page(0);
    good_page(1);
    good_picture();
    failure_answer();
    dies();
    lies();
    stuck();
    memory();
    if(failures) {
        printf("WUNDERTOW LOADER TESTS FAILED: %d of %d checks\n", failures, checks);
        return 1;
    }
    printf("WUNDERTOW LOADER TESTS PASSED: %d checks, 0 failures\n", checks);
    return 0;
}
