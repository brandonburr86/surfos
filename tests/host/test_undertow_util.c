/*
SurfOS Undertow: tests of the shared helpers and the settings
----------------------
File: test_undertow_util.c  Date: 10/11/26
----------------------
user/undertow/util.c (deadlines, error messages, UTF-8, safe text) and
user/undertow/config.c (undertow.conf, the clock), with the test's own uptime
and hardware clock.
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "surf.h"
#include "alloc.h"
#include "undertow.h"
#include "config.h"

#define LIT(s) s, sizeof(s) - 1     /* a literal and its length, NULs included */

static int checks, failures;
static unsigned int sim_ms;
static time_t sim_rtc = 1790812800;        /* 2026-10-01 00:00:00 */

unsigned int uptime(void) { return sim_ms; }
time_t ut_sim_time(time_t *t) { if(t) *t = sim_rtc; return sim_rtc; }
int ttyctl(int op, unsigned int arg) { (void)op; (void)arg; return -1; }

static void check(int ok, const char *what) {
    checks++;
    if(!ok) { failures++; printf("FAIL: %s\n", what); }
}

static int safe_line(const char *s) {
    for(; *s; s++) if((unsigned char)*s < 32 || *s == 127) return 0;
    return 1;
}

static void deadlines(void) {
    unsigned int d = 0xffffff00u + 1000;        /* wraps to 0x2e8 */
    check(!ut_expired(d, 0xffffff00u) && ut_remaining(d, 0xffffff00u) == 1000, "deadline across the wrap: 1000 ms left");
    check(!ut_expired(d, 0x2e7) && ut_remaining(d, 0x2e7) == 1, "one millisecond before a wrapped deadline");
    check(ut_expired(d, 0x2e8) && ut_remaining(d, 0x2e8) == 0, "a wrapped deadline is reached exactly");
    check(ut_expired(d, 0x2e9 + 5000) && ut_remaining(d, 0x2e9 + 5000) == 0, "past a wrapped deadline");
    check(!ut_expired(100, 99) && ut_expired(100, 100) && ut_expired(100, 101), "plain deadlines");
    sim_ms = 1234;
    check(ut_now() == 1234, "ut_now is uptime");
}

static void errors(void) {
    char err[32], big[UT_ERR_MAX], tiny[4] = "xyz";
    check(ut_error(err, sizeof(err), "a%sb", "\033[2J\n\xc3\xa9") == -1, "ut_error returns -1");
    check(!strcmp(err, "a?[2J???b"), "controls and non-ASCII bytes become ?");
    ut_error(err, sizeof(err), "%s", "0123456789012345678901234567890123456789");
    check(strlen(err) == sizeof(err) - 1 && err[sizeof(err) - 1] == 0, "a long message is cut and terminated");
    check(ut_error(NULL, 10, "x") == -1 && ut_error(tiny, 0, "x") == -1 && !strcmp(tiny, "xyz"), "no buffer is fine");
    ut_error(tiny, 1, "abc");
    check(tiny[0] == 0, "a one-byte buffer gets the terminator");
    strcpy(big, "first");
    ut_error(big, sizeof(big), "%s and %s", big, "second");
    check(!strcmp(big, "first and second"), "the message may name its own buffer");
}

static void utf8(void) {
    static const struct { const char *in; size_t len, used; unsigned int cp; } cases[] = {
        { "A", 1, 1, 'A' }, { "\xc3\xa9", 2, 2, 0xe9 }, { "\xe2\x80\x94", 3, 3, 0x2014 },
        { "\xf0\x9f\x98\x80", 4, 4, 0x1f600 }, { "\xf4\x8f\xbf\xbf", 4, 4, 0x10ffff },
        { "\x80", 1, 1, UT_REPLACEMENT }, { "\xc0\x80", 2, 1, UT_REPLACEMENT }, { "\xc1\xbf", 2, 1, UT_REPLACEMENT },
        { "\xe0\x80\x80", 3, 1, UT_REPLACEMENT }, { "\xf0\x80\x80\x80", 4, 1, UT_REPLACEMENT },
        { "\xed\xa0\x80", 3, 1, UT_REPLACEMENT }, { "\xf4\x90\x80\x80", 4, 1, UT_REPLACEMENT },
        { "\xf5\x80\x80\x80", 4, 1, UT_REPLACEMENT }, { "\xe2\x82", 2, 1, UT_REPLACEMENT },
        { "\xe2\x82" "A", 3, 1, UT_REPLACEMENT }, { "\xff", 1, 1, UT_REPLACEMENT }
    };
    unsigned char out[4];
    unsigned int cp, v;
    size_t i, n;
    int ok = 1;
    for(i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        n = ut_utf8_decode((const unsigned char *)cases[i].in, cases[i].len, &cp);
        if(n != cases[i].used || cp != cases[i].cp) {
            printf("  case %u: used %u, U+%04X\n", (unsigned)i, (unsigned)n, cp);
            ok = 0;
        }
    }
    check(ok, "UTF-8 decoding, ill-formed sequences one byte at a time");
    ok = 1;
    for(v = 0; v <= 0x10ffff; v += (v < 0x3000 ? 1 : 97)) {
        if(v >= 0xd800 && v <= 0xdfff) continue;
        n = ut_utf8_encode(v, out);
        if(ut_utf8_decode(out, n, &cp) != n || cp != v) ok = 0;
    }
    check(ok, "UTF-8 encoding round-trips");
    check(ut_utf8_encode(0xd800, out) == 3 && !memcmp(out, "\xef\xbf\xbd", 3), "a surrogate is encoded as U+FFFD");
    check(ut_utf8_encode(0x110000, out) == 3 && !memcmp(out, "\xef\xbf\xbd", 3), "beyond U+10FFFF is encoded as U+FFFD");
}

static void unsafe_and_ascii(void) {
    static const unsigned int unsafe[] = { 0, 9, 10, 13, 27, 31, 127, 0x80, 0x85, 0x9b, 0x9f, 0x061c, 0x200e, 0x200f,
                                          0x2028, 0x2029, 0x202a, 0x202e, 0x2066, 0x2069, 0xfeff };
    static const unsigned int fine[] = { ' ', 'a', '~', 0xa0, 0xe9, 0x2014, 0x202f, 0x2070, 0x25b8, 0xfffd };
    static const struct { unsigned int cp; const char *ascii; } tr[] = {
        { 0xa0, " " }, { 0xa9, "(c)" }, { 0xe9, "e" }, { 0xdf, "ss" }, { 0xc6, "AE" }, { 0xff, "y" },
        { 0x131, "i" }, { 0x153, "oe" }, { 0x2013, "-" }, { 0x2014, "--" }, { 0x2018, "'" },
        { 0x201c, "\"" }, { 0x2022, "*" }, { 0x2026, "..." }, { 0x20ac, "EUR" }, { 0x2122, "(TM)" },
        { 0x2192, "->" }, { 0x224b, "~" }, { 0x25b8, ">" }, { 0x3000, " " }, { 0xfffd, "?" },
        { 0x4e2d, "?" }, { 0x1f600, "?" }, { 0x0400, "?" }
    };
    char want[2] = { 0, 0 };
    size_t i;
    int ok = 1;
    for(i = 0; i < sizeof(unsafe) / sizeof(unsafe[0]); i++) if(!ut_unsafe_char(unsafe[i])) ok = 0;
    for(i = 0; i < sizeof(fine) / sizeof(fine[0]); i++) if(ut_unsafe_char(fine[i])) ok = 0;
    check(ok, "unsafe characters: controls, C1, bidirectional overrides, BOM");
    ok = 1;
    for(i = 0x20; i < 0x7f; i++) {
        want[0] = (char)i;
        if(strcmp(ut_ascii_for((unsigned int)i), want)) ok = 0;
    }
    check(ok, "printable ASCII stands for itself");
    ok = 1;
    for(i = 0; i < sizeof(tr) / sizeof(tr[0]); i++)
        if(strcmp(ut_ascii_for(tr[i].cp), tr[i].ascii)) {
            printf("  U+%04X: %s\n", tr[i].cp, ut_ascii_for(tr[i].cp));
            ok = 0;
        }
    check(ok, "transliterations to ASCII");
}

static void safe_text(void) {
    char out[64];
    size_t n;
    n = ut_safe_text(out, sizeof(out), LIT("a\033[2Jb\x07" "c\xc2\x9b" "31md"), 1);
    check(n == strlen(out) && !strcmp(out, "a[2Jbc31md"), "escape sequences lose their controls (ESC, BEL, C1 CSI)");
    ut_safe_text(out, sizeof(out), LIT("a\tb\r\nc\xe2\x80\xa8" "d"), 1);
    check(!strcmp(out, "a b  c d"), "tabs and line breaks become spaces");
    ut_safe_text(out, sizeof(out), LIT("x\xe2\x80\xae" "evil\xe2\x81\xa6" "y\xd8\x9c" "z"), 1);
    check(!strcmp(out, "xevilyz"), "bidirectional overrides, isolates and the Arabic letter mark are dropped");
    ut_safe_text(out, sizeof(out), LIT("\xff" "a\xe2\x82"), 1);
    check(!strcmp(out, "\xef\xbf\xbd" "a\xef\xbf\xbd\xef\xbf\xbd"), "ill-formed UTF-8 becomes U+FFFD");
    ut_safe_text(out, sizeof(out), LIT("caf\xc3\xa9 \xe2\x80\x9cq\xe2\x80\x9d \xe2\x80\x94 \xe4\xb8\xad"), 0);
    check(!strcmp(out, "cafe \"q\" -- ?"), "ASCII mode transliterates");
    ut_safe_text(out, sizeof(out), LIT("a\0b"), 1);
    check(!strcmp(out, "ab"), "NUL bytes are dropped");
    n = ut_safe_text(out, 5, LIT("abc\xc3\xa9"), 1);
    check(n == 3 && !strcmp(out, "abc"), "a character that does not fit is left out whole");
    n = ut_safe_text(out, 6, LIT("abc\xc3\xa9"), 1);
    check(n == 5 && !strcmp(out, "abc\xc3\xa9"), "an exact fit");
    n = ut_safe_text(out, 3, LIT("abc\xe2\x80\xa6"), 0);
    check(n == 2 && !strcmp(out, "ab"), "ASCII stand-ins are cut whole too");
    n = ut_safe_text(out, 1, "abc", 3, 1);
    check(n == 0 && out[0] == 0, "one byte holds only the terminator");
    check(ut_safe_text(out, sizeof(out), NULL, 5, 1) == 0 && out[0] == 0, "NULL input is empty");
    check(safe_line(out), "safe output");
}

static void clock_tests(void) {
    struct ut_clock c, before;
    char err[128], text[UT_CLOCK_TEXT];
    memset(&c, 0, sizeof(c));
    sim_rtc = 1790812800;
    check(ut_clock_now(&c) == 1790812800, "offset 0: UTC is the hardware clock");
    c.rtc_offset_minutes = -300;
    check(ut_clock_now(&c) == 1790812800 + 18000, "a clock on UTC-5 is 5 hours behind UTC");
    c.rtc_offset_minutes = 840;
    check(ut_clock_now(&c) == 1790812800 - 50400, "UTC+14");
    sim_rtc = 100;
    check(ut_clock_now(&c) == (time_t)-1, "before 1970 after the offset is unknown");
    sim_rtc = 2147483647;
    c.rtc_offset_minutes = -1;
    check(ut_clock_now(&c) == (time_t)-1, "past 2038 after the offset is unknown");
    sim_rtc = 1790812800;

    memset(&c, 0, sizeof(c));
    sim_ms = 0xffffffffu - 1500;
    check(ut_clock_set(&c, "2026-10-11 14:05:09", err, sizeof(err)) == 0 && c.override_set, "set the session's UTC");
    check(ut_clock_now(&c) == 1791727509, "the time just set");
    sim_ms += 4000;                              /* across uptime's wrap */
    check(ut_clock_now(&c) == 1791727509 + 4, "the uptime since is added, across the wrap");
    sim_rtc = 0;
    check(ut_clock_now(&c) == 1791727509 + 4, "the hardware clock no longer matters");
    sim_rtc = 1790812800;
    check(ut_clock_set(&c, "2024-02-29 12:34:56", err, sizeof(err)) == 0 && ut_clock_now(&c) == 1709210096,
          "a leap day");
    check(ut_clock_set(&c, "2000-03-01 00:00:00", err, sizeof(err)) == 0 && ut_clock_now(&c) == 951868800,
          "the day after 2000's leap day");
    check(ut_clock_set(&c, "2038-01-19 03:14:07", err, sizeof(err)) == 0 && ut_clock_now(&c) == 2147483647,
          "the last second of a 32-bit time_t");
    {
        static const char *bad[] = { "2026-10-11T14:05:09", "2026-1-11 14:05:09", "2026-10-11 14:05",
                                     "2026-10-11 14:05:09 ", " 2026-10-11 14:05:09", "2026-02-29 00:00:00",
                                     "2100-02-29 00:00:00", "2026-13-01 00:00:00", "2026-00-10 00:00:00",
                                     "2026-04-31 00:00:00", "2026-10-00 00:00:00", "2026-10-11 24:00:00",
                                     "2026-10-11 23:60:00", "2026-10-11 23:59:60", "1969-12-31 23:59:59",
                                     "2038-01-19 03:14:08", "2026-10-11 14:05:0x", "", "+026-10-11 14:05:09",
                                     "2026-10-11\t14:05:09", "2026/10/11 14:05:09" };
        size_t i;
        int ok = 1;
        for(i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
            memcpy(&before, &c, sizeof(c));
            err[0] = 0;
            if(ut_clock_set(&c, bad[i], err, sizeof(err)) != -1 || memcmp(&before, &c, sizeof(c)) ||
               !err[0] || !safe_line(err)) {
                printf("  accepted or changed: \"%s\" (%s)\n", bad[i], err);
                ok = 0;
            }
        }
        check(ok, "malformed and impossible times are refused, the clock unchanged");
        check(ut_clock_set(&c, NULL, err, sizeof(err)) == -1, "no time at all");
    }
    ut_clock_format(0, text);
    check(!strcmp(text, "1970-01-01 00:00:00 UTC"), "format 0");
    ut_clock_format(1709210096, text);
    check(!strcmp(text, "2024-02-29 12:34:56 UTC"), "format a leap day");
    ut_clock_format(2147483647, text);
    check(!strcmp(text, "2038-01-19 03:14:07 UTC"), "format the end of time_t");
    ut_clock_format((time_t)-1, text);
    check(!strcmp(text, "unknown"), "format an unknown time");
    check(ut_clock_plausible(UT_CLOCK_FLOOR) && !ut_clock_plausible(UT_CLOCK_FLOOR - 1) &&
          !ut_clock_plausible((time_t)-1) && !ut_clock_plausible(1072915200), "plausible clocks: not 2004");
}

static void config_tests(void) {
    struct ut_config *c = malloc(sizeof(*c)), *before = malloc(sizeof(*before));
    char err[160], *big;
    unsigned int ip;
    const char *good =
        "# comment\n"
        "\n"
        "  rtc_offset_minutes = -300  \r\n"
        "host=Fixture.TEST 10.0.2.2\n"
        "host = wrong.test\t 10.0.2.2\n"
        "host=a.b 255.255.255.255";
    ut_config_init(c);
    check(c->clock.rtc_offset_minutes == 0 && !c->clock.override_set && c->host_count == 0, "the defaults");
    check(ut_config_parse(c, good, strlen(good), err, sizeof(err)) == 0, "a good file parses");
    check(c->clock.rtc_offset_minutes == -300 && c->host_count == 3, "its settings");
    check(ut_config_host(c, "fixture.test", &ip) == 0 && !memcmp(&ip, "\x0a\x00\x02\x02", 4), "a host, network byte order");
    check(ut_config_host(c, "FIXTURE.Test", &ip) == 0, "host names in any case");
    check(ut_config_host(c, "a.b", &ip) == 0 && ip == 0xffffffffu, "a broadcast address is just an address");
    check(ut_config_host(c, "fixture.tes", &ip) == -1 && ut_config_host(c, "other", &ip) == -1, "unknown hosts");
    check(ut_config_parse(c, "", 0, err, sizeof(err)) == 0 && c->host_count == 0 &&
          c->clock.rtc_offset_minutes == 0, "an empty file gives the defaults");
    check(ut_config_parse(c, LIT("font=12x24"), err, sizeof(err)) == 0 && c->font == 24, "the window's font");
    check(ut_config_parse(c, LIT("font=8x16\nfont=16x32"), err, sizeof(err)) == 0 && c->font == 32, "the last font says");
    check(ut_config_parse(c, LIT("font=10x20"), err, sizeof(err)) < 0 && strstr(err, "font must be") != NULL, "a font there is none of");
    check(ut_config_parse(c, LIT("rtc_offset_minutes=+840\nrtc_offset_minutes=-840"), err, sizeof(err)) == 0 &&
          c->clock.rtc_offset_minutes == -840, "the limits, and the last setting wins");
    {
        static const struct { const char *text, *why; } bad[] = {
            { "rtc_offset_minutes=841", "line 1" }, { "rtc_offset_minutes=-841", "rtc_offset_minutes" },
            { "rtc_offset_minutes=", "rtc_offset_minutes" }, { "rtc_offset_minutes=5x", "rtc_offset_minutes" },
            { "rtc_offset_minutes=--5", "rtc_offset_minutes" }, { "rtc_offset_minutes=00300", "rtc_offset_minutes" },
            { "\n\nrtc_offset_minutes 5", "line 3" }, { "colour=blue", "unknown setting" },
            { "host=a.test", "host=NAME ADDRESS" }, { "host=a.test 10.0.2", "host=NAME ADDRESS" },
            { "host=a.test 10.0.2.256", "host=NAME ADDRESS" }, { "host=a.test 10.0.2.02", "host=NAME ADDRESS" },
            { "host=a_b.test 10.0.2.2", "host=NAME ADDRESS" }, { "host=-a.test 10.0.2.2", "host=NAME ADDRESS" },
            { "host=a..test 10.0.2.2", "host=NAME ADDRESS" }, { "host=a.test 10.0.2.2 x", "host=NAME ADDRESS" },
            { "host=a.test 10.0.2.2\nhost=A.TEST 10.0.2.3", "given twice" },
            { "host=\033[2J 1.2.3.4", "host=NAME ADDRESS" }
        };
        size_t i;
        int ok = 1;
        ut_config_parse(c, good, strlen(good), err, sizeof(err));
        memcpy(before, c, sizeof(*c));
        for(i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
            err[0] = 0;
            if(ut_config_parse(c, bad[i].text, strlen(bad[i].text), err, sizeof(err)) != -1 ||
               !strstr(err, bad[i].why) || memcmp(before, c, sizeof(*c)) || !safe_line(err)) {
                printf("  \"%s\": %s\n", bad[i].text, err);
                ok = 0;
            }
        }
        check(ok, "bad settings are refused with their line, the settings unchanged");
    }
    big = malloc(20000);
    memset(big, '#', 600);
    check(ut_config_parse(c, big, 600, err, sizeof(err)) == -1 && strstr(err, "512"), "a line over 512 bytes");
    memset(big, '\n', 20000);
    check(ut_config_parse(c, big, 16385, err, sizeof(err)) == -1 && strstr(err, "16384"), "a file over 16 KB");
    check(ut_config_parse(c, big, 16384, err, sizeof(err)) == 0, "a file of exactly 16 KB");
    check(ut_config_parse(c, LIT("#\0x\n"), err, sizeof(err)) == -1 && strstr(err, "NUL"), "a NUL byte");
    memset(big, 0, 20000);
    {
        size_t i;
        for(i = 0; i < 17; i++)
            snprintf(big + strlen(big), 20000 - strlen(big), "host=h%u.test 10.0.0.%u\n", (unsigned)i, (unsigned)i + 1);
    }
    check(ut_config_parse(c, big, strlen(big), err, sizeof(err)) == -1 && strstr(err, "more than 16"), "17 hosts");
    ut_alloc_fail_after = 0;
    memcpy(before, c, sizeof(*c));
    check(ut_config_parse(c, good, strlen(good), err, sizeof(err)) == -1 && strstr(err, "memory") &&
          !memcmp(before, c, sizeof(*c)), "out of memory leaves the settings alone");
    ut_alloc_fail_after = -1;
    check(ut_alloc_live == 0, "parsing keeps no memory");

    /* the file */
    {
        char path[] = "/tmp/undertow-conf-XXXXXX";
        int fd = mkstemp(path);
        FILE *f = fd >= 0 ? fdopen(fd, "w") : NULL;
        check(f != NULL, "a temporary file");
        if(f) {
            fputs("rtc_offset_minutes=-360\nhost=fixture.test 10.0.2.2\n", f);
            fclose(f);
            ut_config_init(c);
            check(ut_config_load(c, path, err, sizeof(err)) == 0 && c->clock.rtc_offset_minutes == -360 &&
                  c->host_count == 1, "load a file");
            unlink(path);
        }
        ut_config_init(c);
        c->clock.rtc_offset_minutes = 7;
        check(ut_config_load(c, "/nonexistent/undertow.conf", err, sizeof(err)) == 0 &&
              c->clock.rtc_offset_minutes == 7, "no file: no error, nothing changed");
        check(ut_config_load(c, "/", err, sizeof(err)) == -1 && err[0], "a directory is an error");
        check(ut_alloc_live == 0, "loading keeps no memory");
    }
    free(big);
    free(before);
    free(c);
}

int main(void) {
    deadlines();
    errors();
    utf8();
    unsafe_and_ascii();
    safe_text();
    clock_tests();
    config_tests();
    printf("UNDERTOW UTIL TESTS %s: %d checks, %d failures\n", failures ? "FAILED" : "PASSED", checks, failures);
    return failures ? 1 : 0;
}
