/*
SurfOS Undertow: URL parser and RFC 3986 resolution tests
----------------------
File: test_undertow_url.c  Date: 10/10/26
----------------------
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "url.h"

static int checks, failures, fail_alloc, live_alloc;
/* Only URL code is compiled with these names; the tests retain real malloc. */
void *ut_test_malloc(size_t n) {
    void *p;
    if(fail_alloc) return NULL;
    p = malloc(n);
    if(p) live_alloc++;
    return p;
}
void ut_test_free(void *p) {
    if(p) live_alloc--;
    free(p);
}
static void check(int ok, const char *what) {
    checks++;
    if(!ok) { failures++; printf("FAIL: %s\n", what); }
}

static void parse_cases(void) {
    static const struct {
        const char *input, *text, *host, *target, *fragment;
        unsigned short port;
        int secure;
    } cases[] = {
        {"example.com", "https://example.com/", "example.com", "/", "", 443, 1},
        {"https://example.com", "https://example.com/", "example.com", "/", "", 443, 1},
        {"http://example.com", "http://example.com/", "example.com", "/", "", 80, 0},
        {"HTTPS://EXAMPLE.COM:443/a", "https://example.com/a", "example.com", "/a", "", 443, 1},
        {"http://example.com:80/", "http://example.com/", "example.com", "/", "", 80, 0},
        {"example.com:1", "https://example.com:1/", "example.com", "/", "", 1, 1},
        {"http://10.0.2.2:65535/?x=1#top", "http://10.0.2.2:65535/?x=1#top", "10.0.2.2", "/?x=1", "top", 65535, 0},
        {"localhost:008080/p", "https://localhost:8080/p", "localhost", "/p", "", 8080, 1},
        {"xn--bcher-kva.example/a", "https://xn--bcher-kva.example/a", "xn--bcher-kva.example", "/a", "", 443, 1},
        {"https://example.com./a/../b", "https://example.com./b", "example.com.", "/b", "", 443, 1},
        {"https://example.com?x=/../#?", "https://example.com/?x=/../#?", "example.com", "/?x=/../", "?", 443, 1},
        {"https://example.com/a/%2e%2e/%2F/b", "https://example.com/a/%2e%2e/%2F/b", "example.com", "/a/%2e%2e/%2F/b", "", 443, 1},
        {"http://a//b/./c/../", "http://a//b/", "a", "//b/", "", 80, 0},
        {"http://a/?#", "http://a/?#", "a", "/?", "", 80, 0},
        {"http://a/%0d%0aX-Test%3aok", "http://a/%0d%0aX-Test%3aok", "a", "/%0d%0aX-Test%3aok", "", 80, 0}
    };
    struct ut_url *u = malloc(sizeof(*u));
    char err[80];
    size_t i;
    for(i = 0; i < sizeof(cases)/sizeof(cases[0]); i++) {
        memset(err, 'x', sizeof(err));
        if(ut_url_parse(cases[i].input, u, err, sizeof(err))) {
            check(0, cases[i].input); continue;
        }
        check(!strcmp(u->text, cases[i].text), cases[i].input);
        check(!strcmp(u->host, cases[i].host), "host");
        check(!strcmp(u->target, cases[i].target), "target");
        check(!strcmp(u->fragment, cases[i].fragment), "fragment");
        check(u->port == cases[i].port && u->secure == cases[i].secure, "port and TLS");
        check(!err[0], "success clears error");
    }
    free(u);
}

static void reject_cases(void) {
    static const struct { const char *input, *error; } cases[] = {
        {"", "empty"}, {"http://", "host"}, {"http:///p", "host"},
        {"ftp://a/", "scheme"}, {"javascript:alert(1)", "scheme"},
        {"https:a", "scheme"}, {"http:/a", "scheme"},
        {"http://user:pass@a/", "credentials"}, {"user@a", "credentials"},
        {"user:pass@a", "credentials"}, {"[::1]", "IPv6"}, {"::1", "IPv6"},
        {"http://[::1]/", "IPv6"}, {"http://::1/", "IPv6"},
        {"http://b\xc3\xbc" "cher.example/", "ASCII"},
        {"http://a:0/", "port"}, {"http://a:65536/", "port"},
        {"http://a:999999999999999999999999/", "port"},
        {"http://a:/", "port"}, {"http://a:-1/", "port"},
        {"http://a:abc/", "port"}, {"http://a:12x/", "port"},
        {"http://a/\r\nHost:evil", "control"}, {"http://a/\033[2J", "control"},
        {"http://a/\177", "control"}, {"http://a/x y", "space"},
        {"http://a/\t", "control"}, {"http://a\\@evil/", "backslash"},
        {"http://a/%", "escape"}, {"http://a/%0x", "escape"},
        {"http://a%2eevil/", "host"}, {"http://a..b/", "host"},
        {"http://-a/", "host"}, {"http://a-/", "host"}, {"http://./", "host"}
    };
    struct ut_url *u = malloc(sizeof(*u)), *before = malloc(sizeof(*before));
    char err[80], tiny[2] = {'x', 'x'}, bad[] = "http://a/x";
    size_t i;
    memset(before, 0x5a, sizeof(*before));
    for(i = 0; i < sizeof(cases)/sizeof(cases[0]); i++) {
        memcpy(u, before, sizeof(*u));
        memset(err, 0, sizeof(err));
        check(ut_url_parse(cases[i].input, u, err, sizeof(err)) == -1, cases[i].input);
        check(strstr(err, cases[i].error) != NULL, cases[i].error);
        check(!memcmp(u, before, sizeof(*u)), "failed parse preserves output");
        check(strpbrk(err, "\r\n\033") == NULL, "safe error");
    }
    for(i = 1; i <= 127; i++) {
        if(i >= 32 && i != 127) continue;
        bad[9] = (char)i;
        check(ut_url_parse(bad, u, err, sizeof(err)) == -1 &&
              !memcmp(u, before, sizeof(*u)), "all control bytes rejected unchanged");
    }
    check(ut_url_parse(NULL, u, tiny, sizeof(tiny)) == -1 && tiny[1] == 0, "bounded error");
    check(ut_url_parse("", u, NULL, 0) == -1, "no error buffer");
    check(ut_url_parse("http://a", NULL, err, sizeof(err)) == -1, "null output");
    memcpy(u, before, sizeof(*u));
    fail_alloc = 1;
    check(ut_url_parse("https://a/", u, err, sizeof(err)) == -1 &&
          !memcmp(u, before, sizeof(*u)) && strstr(err, "memory"), "parse allocation failure preserves output");
    fail_alloc = 0;
    check(live_alloc == 0, "parse releases temporary allocations");
    free(before); free(u);
}

static void resolve_cases(void) {
    /* RFC 3986 section 5.4, using HTTP because other schemes are excluded. */
    static const struct { const char *ref, *want; } cases[] = {
        {"g", "http://a/b/c/g"}, {"./g", "http://a/b/c/g"}, {"g/", "http://a/b/c/g/"},
        {"/g", "http://a/g"}, {"//g", "http://g/"}, {"?y", "http://a/b/c/d;p?y"},
        {"g?y", "http://a/b/c/g?y"}, {"#s", "http://a/b/c/d;p?q#s"},
        {"g#s", "http://a/b/c/g#s"}, {"g?y#s", "http://a/b/c/g?y#s"},
        {";x", "http://a/b/c/;x"}, {"g;x", "http://a/b/c/g;x"},
        {"g;x?y#s", "http://a/b/c/g;x?y#s"}, {"", "http://a/b/c/d;p?q"},
        {".", "http://a/b/c/"}, {"./", "http://a/b/c/"},
        {"..", "http://a/b/"}, {"../", "http://a/b/"}, {"../g", "http://a/b/g"},
        {"../..", "http://a/"}, {"../../", "http://a/"}, {"../../g", "http://a/g"},
        {"../../../g", "http://a/g"}, {"../../../../g", "http://a/g"},
        {"/./g", "http://a/g"}, {"/../g", "http://a/g"}, {"g.", "http://a/b/c/g."},
        {".g", "http://a/b/c/.g"}, {"g..", "http://a/b/c/g.."}, {"..g", "http://a/b/c/..g"},
        {"./../g", "http://a/b/g"}, {"./g/.", "http://a/b/c/g/"},
        {"g/./h", "http://a/b/c/g/h"}, {"g/../h", "http://a/b/c/h"},
        {"g;x=1/./y", "http://a/b/c/g;x=1/y"}, {"g;x=1/../y", "http://a/b/c/y"},
        {"g?y/./x", "http://a/b/c/g?y/./x"}, {"g?y/../x", "http://a/b/c/g?y/../x"},
        {"g#s/./x", "http://a/b/c/g#s/./x"}, {"g#s/../x", "http://a/b/c/g#s/../x"},
        {"https://other:1/a", "https://other:1/a"},
        {"%2e%2e/%2f", "http://a/b/c/%2e%2e/%2f"},
        {"?", "http://a/b/c/d;p?"}, {"#", "http://a/b/c/d;p?q#"}
    };
    struct ut_url *base = malloc(sizeof(*base)), *out = malloc(sizeof(*out));
    char err[80];
    size_t i;
    check(ut_url_parse("http://a/b/c/d;p?q#old", base, err, sizeof(err)) == 0, "RFC base");
    for(i = 0; i < sizeof(cases)/sizeof(cases[0]); i++) {
        if(ut_url_resolve(base, cases[i].ref, out, err, sizeof(err))) {
            check(0, cases[i].ref); continue;
        }
        check(!strcmp(out->text, cases[i].want), cases[i].ref);
        check(strchr(out->target, '#') == NULL, "request target has no fragment");
    }
    check(ut_url_parse("https://example.com/a/index", base, err, sizeof(err)) == 0, "parent base");
    check(ut_url_resolve(base, "../b?x=1#here", out, err, sizeof(err)) == 0 &&
          !strcmp(out->target, "/b?x=1") && !strcmp(out->fragment, "here"), "required parent example");
    check(ut_url_parse("https://example.com/a/", base, err, sizeof(err)) == 0, "slash base");
    check(ut_url_resolve(base, "b", out, err, sizeof(err)) == 0 && !strcmp(out->target, "/a/b"), "base ending slash");
    check(ut_url_resolve(base, "//other:65535/p", out, err, sizeof(err)) == 0 && out->secure && out->port == 65535, "network-path inherits HTTPS");
    check(ut_url_resolve(base, "../c", base, err, sizeof(err)) == 0 && !strcmp(base->target, "/c"), "in-place resolve");
    memcpy(out, base, sizeof(*out));
    check(ut_url_resolve(base, "ftp://other", out, err, sizeof(err)) == -1 && !memcmp(out, base, sizeof(*out)), "resolve rejects scheme unchanged");
    check(ut_url_resolve(base, "//u@other", out, err, sizeof(err)) == -1 && !memcmp(out, base, sizeof(*out)), "resolve rejects credentials unchanged");
    check(ut_url_resolve(base, "/x\r\nY:z", out, err, sizeof(err)) == -1 && !memcmp(out, base, sizeof(*out)), "resolve rejects injection unchanged");
    check(ut_url_resolve(NULL, "x", out, err, sizeof(err)) == -1, "null base");
    check(ut_url_resolve(base, NULL, out, err, sizeof(err)) == -1, "null ref");
    fail_alloc = 1;
    check(ut_url_resolve(base, "new", out, err, sizeof(err)) == -1 &&
          !memcmp(out, base, sizeof(*out)) && strstr(err, "memory"), "resolve allocation failure preserves output");
    fail_alloc = 0;
    check(live_alloc == 0, "resolve releases temporary allocations");
    check(ut_url_parse("https://example.com/a", base, err, sizeof(err)) == 0 &&
          ut_url_parse(base->text, base, err, sizeof(err)) == 0 &&
          !strcmp(base->text, "https://example.com/a"), "parse accepts aliased input");
    free(out); free(base);
}

static void boundaries(void) {
    struct ut_url *u = malloc(sizeof(*u)), *out = malloc(sizeof(*out));
    char *s = malloc(2058), err[80];
    size_t n;
    strcpy(s, "https://a/");
    memset(s + 10, 'x', 2038); s[2048] = 0;
    check(ut_url_parse(s, u, err, sizeof(err)) == 0 && strlen(u->text) == 2048, "exact 2048 URL");
    s[2048] = 'x'; s[2049] = 0;
    check(ut_url_parse(s, out, err, sizeof(err)) == -1 && strstr(err, "2048"), "reject 2049 URL");
    memset(s, 'x', 2048); s[2048] = 0;
    /* 1019 directories minus 600 parents, then /b: 840 bytes. */
    check(ut_url_resolve(u, s, out, err, sizeof(err)) == -1, "resolved URL exceeds limit");
    s[2048] = 'x'; s[2049] = 0;
    /* 1019 directories minus 600 parents, then /b: 840 bytes. */
    check(ut_url_resolve(u, s, out, err, sizeof(err)) == -1 && strstr(err, "2048"), "reject 2049 ref");
    check(ut_url_resolve(u, "#", out, err, sizeof(err)) == -1, "fragment marker exceeds 2048");
    check(ut_url_resolve(u, "", out, err, sizeof(err)) == 0 && strlen(out->text) == 2048, "resolve exact 2048 URL");
    check(ut_url_resolve(u, ".", out, err, sizeof(err)) == 0 && !strcmp(out->target, "/"), "long base can resolve short");
    for(n = 10; n < 2048; n++) s[n] = n % 2 ? '/' : 'a';
    memcpy(s, "https://a/", 10); s[2048] = 0;
    check(ut_url_parse(s, u, err, sizeof(err)) == 0, "long directory base");
    for(n = 0; n < 600; n++) memcpy(s + n*3, "../", 3);
    strcpy(s+1800, "b");
    /* 1019 directories minus 600 parents, then /b: 840 bytes. */
    check(ut_url_resolve(u, s, out, err, sizeof(err)) == 0 && strlen(out->target) == 840 && out->target[839] == 'b',
          "merged path over 2048 normalizes without truncation");
    strcpy(s, "http://");
    for(n = 0; n < 253; n++) s[7+n] = (n == 63 || n == 127 || n == 191) ? '.' : 'a';
    strcpy(s+260, "/");
    check(ut_url_parse(s, out, err, sizeof(err)) == 0 && strlen(out->host) == 253, "253 byte host");
    s[260] = 'a'; s[261] = '/'; s[262] = 0;
    check(ut_url_parse(s, out, err, sizeof(err)) == -1 && strstr(err, "host"), "254 byte host");
    memset(s+7, 'a', 64); strcpy(s+71, ".example/");
    check(ut_url_parse(s, out, err, sizeof(err)) == -1, "64 byte label");
    free(s); free(out); free(u);
}

int main(void) {
    parse_cases(); reject_cases(); resolve_cases(); boundaries();
    printf("UNDERTOW URL TESTS %s: %d checks, %d failures\n", failures ? "FAILED" : "PASSED", checks, failures);
    return failures ? 1 : 0;
}
