/* Host-side unit tests for the user C library (user/libsurf): the formatter, sscanf, strtol,
   the string functions, the calendar functions and qsort are compared with the host libc;
   malloc runs a random workload over an arena that stands in for sbrk(). Built and run by
   `make unittest`; the library is compiled with tests/host/libsurf_names.h (surf_ names). */
#define _DEFAULT_SOURCE
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <errno.h>
#include <time.h>
#include <unistd.h>

int surf_vsnprintf(char *buf, size_t size, const char *fmt, va_list ap);
int surf_snprintf(char *buf, size_t size, const char *fmt, ...);
int surf_dprintf(int fd, const char *fmt, ...);
int surf_sscanf(const char *s, const char *fmt, ...);
long surf_strtol(const char *s, char **end, int base);
unsigned long surf_strtoul(const char *s, char **end, int base);
int surf_strcasecmp(const char *a, const char *b);
int surf_strncasecmp(const char *a, const char *b, size_t n);
char *surf_strtok_r(char *s, const char *delim, char **save);
size_t surf_strspn(const char *s, const char *accept);
size_t surf_strcspn(const char *s, const char *reject);
char *surf_strncat(char *dst, const char *src, size_t n);
size_t surf_strlcat(char *dst, const char *src, size_t size);
char *surf_strstr(const char *h, const char *n);
char *surf_strrchr(const char *s, int c);
void *surf_memmove(void *d, const void *s, size_t n);
void *surf_memset(void *d, int c, size_t n);
void *surf_memcpy(void *d, const void *s, size_t n);
char *surf_strerror(int err);
struct tm *surf_gmtime_r(const time_t *t, struct tm *out);
time_t surf_mktime(struct tm *tm);
size_t surf_strftime(char *s, size_t max, const char *fmt, const struct tm *tm);
void surf_qsort(void *base, size_t n, size_t size, int (*cmp)(const void *, const void *));
void *surf_malloc(size_t n);
void surf_free(void *p);
void *surf_calloc(size_t n, size_t size);
void *surf_realloc(void *p, size_t n);

/* what the library expects from the system calls */
int surf_errno;
static char arena[64 << 20];
static size_t arena_used;
void *surf_sbrk(intptr_t incr) {
    char *old = arena + arena_used;
    if(incr < 0 || arena_used + (size_t)incr > sizeof(arena)) { surf_errno = ENOMEM; return (void *)-1; }
    arena_used += (size_t)incr;
    return old;
}
ssize_t surf_write(int fd, const void *buf, size_t n) { return write(fd, buf, n); }
int surf_fflush(void *f) { (void)f; return 0; }
void surf__exit(int code) { _exit(code); }

static int failures, checks;

static void check(int ok, const char *what) {
    checks++;
    if(!ok) { failures++; printf("FAIL: %s\n", what); }
}

static void fmt_check(const char *fmt, ...) {
    char host[256], surf[256];
    int nh, ns;
    va_list ap, ap2;
    va_start(ap, fmt);
    va_copy(ap2, ap);
    nh = vsnprintf(host, sizeof host, fmt, ap);
    ns = surf_vsnprintf(surf, sizeof surf, fmt, ap2);
    va_end(ap2);
    va_end(ap);
    checks++;
    if(nh != ns || strcmp(host, surf)) {
        failures++;
        printf("FAIL: format \"%s\": host [%s] (%d) surf [%s] (%d)\n", fmt, host, nh, surf, ns);
    }
}

static void test_format(void) {
    char small[6];
    int fds[2];
    fmt_check("plain");
    fmt_check("%d|%i|%u|%x|%X|%o", -42, 17, 4000000000u, 0xbeef, 0xBEEF, 8);
    fmt_check("[%5d][%-5d][%05d][%+d][% d][%.3d][%5.3d]", 42, 42, 42, 42, 42, 7, 7);
    fmt_check("[%*d][%-*d][%.*d]", 6, 1, 6, 2, 4, 3);
    fmt_check("%#x %#o %#X %#x", 255, 8, 255, 0);
    fmt_check("%hhd %hd %ld %lld %llu", 300, 70000, -5L, -1234567890123LL, 18446744073709551615ULL);
    fmt_check("%zu %jd %td", (size_t)12345, (intmax_t)-7, (ptrdiff_t)-3);
    fmt_check("[%s][%10s][%-10s][%.2s][%c][%3c]", "abc", "right", "left", "truncate", 'x', 'y');
    fmt_check("%p", (void *)0x1234);
    fmt_check("100%% sure");
    fmt_check("%.0d|%.0x|%5.0d", 0, 0, 0);
    checks++;
    if(surf_snprintf(small, sizeof small, "%s", "hello world") != 11 || strcmp(small, "hello")) {
        failures++;
        printf("FAIL: snprintf truncates and returns the full length (got [%s])\n", small);
    }
    if(pipe(fds) == 0) {
        char back[64] = { 0 };
        int n = surf_dprintf(fds[1], "fd %d: %s\n", fds[1], "dprintf");
        close(fds[1]);
        check(n > 0 && read(fds[0], back, sizeof back - 1) == n && strstr(back, "dprintf\n"), "dprintf writes to a descriptor");
        close(fds[0]);
    }
}

static void test_scanf(void) {
    int a = 0, b = 0, ha = 0, hb = 0, n1 = -1, n2 = -1, r1, r2;
    unsigned u1 = 0, u2 = 0;
    char s1[32] = "", s2[32] = "", c1[4] = "", c2[4] = "";
    static const char *const ints[] = { "42", "-17 9", "  +5x", "0x1f 077", "abc", "", "12abc 3" };
    unsigned i;
    for(i = 0; i < sizeof(ints) / sizeof(ints[0]); i++) {
        a = b = ha = hb = 0;
        r1 = sscanf(ints[i], "%d %i", &a, &b);
        r2 = surf_sscanf(ints[i], "%d %i", &ha, &hb);
        check(r1 == r2 && a == ha && (r1 < 2 || b == hb), ints[i]);
        a = b = ha = hb = 0;
        r1 = sscanf(ints[i], "%i %i", &a, &b);
        r2 = surf_sscanf(ints[i], "%i %i", &ha, &hb);
        check(r1 == r2 && a == ha && (r1 < 2 || b == hb), "sscanf %i");
    }
    r1 = sscanf("ff 12 hello world", "%x %u %5s%n", &a, &u1, s1, &n1);
    r2 = surf_sscanf("ff 12 hello world", "%x %u %5s%n", &ha, &u2, s2, &n2);
    check(r1 == r2 && a == ha && u1 == u2 && !strcmp(s1, s2) && n1 == n2, "sscanf %x %u %5s %n");
    r1 = sscanf("key=value;x", "%3s=%2c", s1, c1);
    r2 = surf_sscanf("key=value;x", "%3s=%2c", s2, c2);
    check(r1 == r2 && !strcmp(s1, s2) && !memcmp(c1, c2, 2), "sscanf literals and %c");
    r1 = sscanf("10 20", "%*d %d", &a);
    r2 = surf_sscanf("10 20", "%*d %d", &ha);
    check(r1 == r2 && a == ha, "sscanf %*d");
    check(surf_sscanf("", "%d", &ha) == EOF && surf_sscanf("   ", "%s", s2) == EOF, "sscanf at the end of input is EOF");
}

static void test_strtol(void) {
    static const struct { const char *s; int base; } cases[] = {
        { "0", 10 }, { "123", 10 }, { "  -42abc", 10 }, { "+7", 10 }, { "0x1F", 16 }, { "0x1F", 0 }, { "017", 0 },
        { "0x", 16 }, { "0xg", 0 }, { "z", 36 }, { "2147483647", 10 }, { "2147483648", 10 }, { "-2147483648", 10 },
        { "-2147483649", 10 }, { "99999999999999999999", 10 }, { "", 10 }, { "   ", 10 }, { "-", 10 }, { "1010", 2 },
    };
    unsigned i;
    for(i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        char *eh, *es, what[80];
        long vh, vs;
        int erh, ers;
        if(sizeof(long) != 4 && strlen(cases[i].s) > 9) continue;      /* the overflow cases need a 32-bit long */
        errno = 0; surf_errno = 0;
        vh = strtol(cases[i].s, &eh, cases[i].base);
        erh = errno;
        vs = surf_strtol(cases[i].s, &es, cases[i].base);
        ers = surf_errno;
        snprintf(what, sizeof what, "strtol(\"%s\", %d)", cases[i].s, cases[i].base);
        check(vh == vs && eh == es && erh == ers, what);
    }
    errno = 0; surf_errno = 0;
    if(sizeof(long) == 4) check(surf_strtoul("4294967296", NULL, 10) == 4294967295UL && surf_errno == ERANGE, "strtoul overflow");
    check(surf_strtoul("-1", NULL, 10) == (unsigned long)-1, "strtoul of -1 wraps around");
}

static void test_strings(void) {
    char buf[64], *save, *tok;
    int n = 0;
    check(surf_strcasecmp("Hello", "hELLO") == 0 && surf_strcasecmp("a", "B") < 0 && surf_strncasecmp("abcX", "ABCy", 3) == 0, "strcasecmp");
    strcpy(buf, " one,two,,three ");
    for(tok = surf_strtok_r(buf, " ,", &save); tok; tok = surf_strtok_r(NULL, " ,", &save)) n++;
    check(n == 3, "strtok_r");
    check(surf_strspn("aabbc", "ab") == 4 && surf_strcspn("hello", "lo") == 2, "strspn and strcspn");
    strcpy(buf, "ab");
    check(!strcmp(surf_strncat(buf, "cdef", 2), "abcd"), "strncat");
    check(surf_strlcat(buf, "0123456789", 8) == 14 && !strcmp(buf, "abcd012"), "strlcat");
    check(surf_strstr("haystack", "st") == strstr("haystack", "st") && surf_strstr("a", "") != NULL, "strstr");
    {
        const char *path = "a/b/c";
        check(surf_strrchr(path, '/') == path + 3 && surf_strrchr(path, 'x') == NULL && surf_strrchr(path, 0) == path + 5, "strrchr");
    }
    strcpy(buf, "0123456789");
    surf_memmove(buf + 2, buf, 5);
    check(!strcmp(buf, "0101234789"), "memmove forwards overlap");
    surf_memmove(buf, buf + 2, 5);
    check(!memcmp(buf, "01234", 5), "memmove backwards overlap");
    surf_memset(buf, 'x', 13);
    check(buf[0] == 'x' && buf[12] == 'x', "memset");
    check(!strcmp(surf_strerror(ENOENT), "No such file or directory") && !strcmp(surf_strerror(ENOSYS), "Function not implemented")
          && !strncmp(surf_strerror(999), "Unknown error 999", 17), "strerror");
}

static void test_time(void) {
    static const time_t times[] = { 0, 1, 59, 86399, 86400, 951782400, 951868800, 1234567890, 1759708800, 2147483647, -1, -86401 };
    static const char *const formats[] = { "%Y-%m-%d %H:%M:%S", "%a %b %e %j %y", "%A %B %p %I", "%F %T", "%D %R %%", "x%dy" };
    unsigned i, f;
    for(i = 0; i < sizeof(times) / sizeof(times[0]); i++) {
        struct tm h, s, m;
        char hs[64], ss[64], what[64];
        time_t back;
        gmtime_r(&times[i], &h);
        surf_gmtime_r(&times[i], &s);
        snprintf(what, sizeof what, "gmtime(%ld)", (long)times[i]);
        check(h.tm_year == s.tm_year && h.tm_mon == s.tm_mon && h.tm_mday == s.tm_mday && h.tm_hour == s.tm_hour &&
              h.tm_min == s.tm_min && h.tm_sec == s.tm_sec && h.tm_wday == s.tm_wday && h.tm_yday == s.tm_yday, what);
        for(f = 0; f < sizeof(formats) / sizeof(formats[0]); f++) {
            size_t nh = strftime(hs, sizeof hs, formats[f], &h), ns = surf_strftime(ss, sizeof ss, formats[f], &s);
            snprintf(what, sizeof what, "strftime(\"%s\") of %ld", formats[f], (long)times[i]);
            if(nh != ns || strcmp(hs, ss)) printf("  host [%s] surf [%s]\n", hs, ss);
            check(nh == ns && !strcmp(hs, ss), what);
        }
        m = s;
        back = surf_mktime(&m);
        snprintf(what, sizeof what, "mktime(gmtime(%ld))", (long)times[i]);
        check(back == times[i], what);
    }
    {
        struct tm t = { 0 }, h;
        time_t want;
        t.tm_year = 126; t.tm_mon = 13; t.tm_mday = 35; t.tm_hour = 25; t.tm_min = -5; t.tm_sec = 70;   /* not normalized */
        h = t;
        want = timegm(&h);
        check(surf_mktime(&t) == want && t.tm_mon == h.tm_mon && t.tm_mday == h.tm_mday && t.tm_wday == h.tm_wday, "mktime normalizes");
    }
    {
        char tiny[4];
        struct tm s;
        time_t t0 = 0;
        surf_gmtime_r(&t0, &s);
        check(surf_strftime(tiny, sizeof tiny, "%Y", &s) == 0, "strftime that does not fit returns 0");
    }
}

static int cmp_int(const void *a, const void *b) {
    int x = *(const int *)a, y = *(const int *)b;
    return x < y ? -1 : x > y;
}

static int cmp_str(const void *a, const void *b) {
    return strcmp(*(char *const *)a, *(char *const *)b);
}

static void test_qsort(void) {
    static int a[3000], b[3000];
    static const char *words[] = { "pear", "apple", "fig", "banana", "apple", "cherry", "date" }, *w2[7];
    int sizes[] = { 0, 1, 2, 7, 100, 1751, 3000 }, k, i, kind;
    srand(7);
    for(k = 0; k < (int)(sizeof(sizes) / sizeof(sizes[0])); k++) {
        for(kind = 0; kind < 4; kind++) {
            for(i = 0; i < sizes[k]; i++) a[i] = kind == 0 ? rand() % 1000 : kind == 1 ? i : kind == 2 ? sizes[k] - i : 5;
            memcpy(b, a, sizeof(int) * sizes[k]);
            qsort(a, sizes[k], sizeof(int), cmp_int);
            surf_qsort(b, sizes[k], sizeof(int), cmp_int);
            check(!memcmp(a, b, sizeof(int) * sizes[k]), "qsort of ints");
        }
    }
    memcpy(w2, words, sizeof words);
    qsort(words, 7, sizeof(char *), cmp_str);
    surf_qsort(w2, 7, sizeof(char *), cmp_str);
    check(!memcmp(words, w2, sizeof words), "qsort of strings");
}

#define SLOTS 400

static unsigned char fill(int slot, size_t i) { return (unsigned char)(slot * 31 + i * 7); }

static int intact(unsigned char *p, int slot, size_t n) {
    size_t i;
    for(i = 0; i < n; i++) if(p[i] != fill(slot, i)) return 0;
    return 1;
}

static void test_malloc(void) {
    static unsigned char *p[SLOTS];
    static size_t len[SLOTS];
    size_t i, top;
    int op, ok = 1, aligned = 1;
    unsigned char *big;
    srand(11);
    for(op = 0; op < 40000 && ok; op++) {
        int s = rand() % SLOTS, what = rand() % 3;
        size_t n = (rand() % 8 == 0) ? (size_t)(rand() % 70000) : (size_t)(rand() % 300);
        if(p[s] && !intact(p[s], s, len[s])) { ok = 0; break; }
        if(what == 0 || !p[s]) {
            surf_free(p[s]);
            p[s] = surf_malloc(n);
            len[s] = p[s] ? n : 0;
        } else if(what == 1) {
            surf_free(p[s]);
            p[s] = NULL;
            len[s] = 0;
        } else {
            unsigned char *q = surf_realloc(p[s], n);
            if(!q && n) { ok = 0; break; }
            p[s] = q;
            if(len[s] > n) len[s] = n;
            for(i = 0; i < len[s]; i++) if(q[i] != fill(s, i)) { ok = 0; break; }
            len[s] = n;
        }
        if(p[s]) {
            if((uintptr_t)p[s] & 7) aligned = 0;
            for(i = 0; i < len[s]; i++) p[s][i] = fill(s, i);
        }
    }
    check(ok, "malloc, realloc and free keep every block's contents");
    check(aligned, "blocks are 8-byte aligned");
    for(i = 0; i < SLOTS; i++) { surf_free(p[i]); p[i] = NULL; }
    top = arena_used;
    big = surf_malloc(top / 2);
    check(big != NULL && arena_used == top, "after freeing everything the heap is one block again");
    surf_free(big);
    big = surf_calloc(1000, 1000);
    for(i = 0, ok = big != NULL; ok && i < 1000000; i++) ok = big[i] == 0;
    check(ok, "calloc zeroes reused memory");
    surf_free(big);
    check(surf_calloc((size_t)-1 / 2, 4) == NULL && surf_malloc((size_t)-1) == NULL, "impossible sizes fail");
}

int main(void) {
    test_format();
    test_scanf();
    test_strtol();
    test_strings();
    test_time();
    test_qsort();
    test_malloc();
    printf("LIBC UNIT TESTS %s: %d checks, %d failures\n", failures ? "FAILED" : "PASSED", checks, failures);
    return failures ? 1 : 0;
}
