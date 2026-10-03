/* Host-side unit tests for lib/blibc: the formatter and the string functions are compared
   with the host libc. Built and run by `make unittest`. */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdarg.h>

int surf_vsnprintf(char *buf, unsigned size, const char *fmt, va_list ap);
int surf_snprintf(char *buf, unsigned size, const char *fmt, ...);
unsigned surf_strlen(const char *s);
unsigned surf_strnlen(const char *s, unsigned n);
int surf_strcmp(const char *a, const char *b);
int surf_strncmp(const char *a, const char *b, unsigned n);
char *surf_strcpy(char *d, const char *s);
char *surf_strncpy(char *d, const char *s, unsigned n);
unsigned surf_strlcpy(char *d, const char *s, unsigned n);
char *surf_strcat(char *d, const char *s);
char *surf_strchr(const char *s, int c);
char *surf_strrchr(const char *s, int c);
char *surf_strstr(const char *h, const char *n);
char *surf_strtok_r(char *s, const char *d, char **save);
void *surf_memmove(void *d, const void *s, unsigned n);
int surf_memcmp(const void *a, const void *b, unsigned n);
void *surf_memchr(const void *s, int c, unsigned n);
long surf_strtol(const char *s, char **end, int base);
unsigned long surf_strtoul(const char *s, char **end, int base);
int surf_atoi(const char *s);

static int failures, checks;

static void check(int ok, const char *what) {
    checks++;
    if(!ok) { failures++; printf("FAIL: %s\n", what); }
}

static int sign(int v) { return v < 0 ? -1 : (v > 0 ? 1 : 0); }

/* format once with the host and once with SurfOS, compare text and return value */
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

int main(void) {
    char a[64], b[64], *save, *tok;
    char trunc[8];

    fmt_check("plain text");
    fmt_check("%d %i %u", 42, -42, 42u);
    fmt_check("%d", -2147483647 - 1);
    fmt_check("%u %x %X %o", 0xFFFFFFFFu, 0xdeadBEEF, 0xdeadBEEF, 0777);
    fmt_check("%5d|%-5d|%05d|%+d|% d", 42, 42, 42, 42, 42);
    fmt_check("%02x %08x %#x %#o %#X", 0xa, 0xbeef, 0xbeef, 8, 255);
    fmt_check("%.3d %.0d %10.4d %-10.4d|", 7, 0, 7, 7);
    fmt_check("%c|%5c|%-5c|", 'x', 'y', 'z');
    fmt_check("%s|%10s|%-10s|%.3s|%.0s|", "abc", "abc", "abc", "abcdef", "abc");
    fmt_check("%s", (char *)0);
    fmt_check("%ld %lu %lx", -5L, 123456789UL, 0xabcdefUL);
    fmt_check("%lld %llu %llx", -1234567890123LL, 18446744073709551615ULL, 0x123456789abcdefULL);
    fmt_check("%hd %hhd %hu %hhu", 70000, 300, 70000, 300);
    fmt_check("%*d|%-*d|", 6, 42, 6, 42);
    fmt_check("%%|%5%|");
    fmt_check("%zu", (size_t)12345);
    /* no floating point in the kernel: an unknown conversion is echoed, not interpreted */
    surf_snprintf(a, sizeof a, "%.2f|%q", 1.5, 7);
    check(!strcmp(a, "%f|%q"), "unknown conversion echoed");
    /* a lone % at the end is printed (glibc reports an error instead) */
    check(surf_snprintf(a, sizeof a, "a%") == 2 && !strcmp(a, "a%"), "trailing percent");

    /* %p differs between libcs (glibc prints (nil) for NULL); check our own shape */
    surf_snprintf(a, sizeof a, "%p", (void *)0x1234);
    check(!strcmp(a, "0x1234"), "%p");

    /* truncation: at most size-1 characters plus the terminator, full length returned */
    check(surf_snprintf(trunc, sizeof trunc, "%s", "0123456789") == 10 && !strcmp(trunc, "0123456"), "snprintf truncation");
    check(surf_snprintf(NULL, 0, "%d", 12345) == 5, "snprintf size 0");

    check(surf_strlen("hello") == 5 && surf_strnlen("hello", 3) == 3, "strlen/strnlen");
    check(sign(surf_strcmp("a", "b")) == sign(strcmp("a", "b")) && surf_strcmp("abc", "abc") == 0 &&
          sign(surf_strcmp("b", "a")) == 1 && sign(surf_strcmp("ab", "abc")) == -1, "strcmp");
    check(surf_strncmp("abcd", "abce", 3) == 0 && sign(surf_strncmp("abcd", "abce", 4)) == -1, "strncmp");
    surf_strcpy(a, "hello"); check(!strcmp(a, "hello"), "strcpy");
    memset(b, 'X', sizeof b); surf_strncpy(b, "hi", 5);
    check(b[0] == 'h' && b[1] == 'i' && b[2] == 0 && b[4] == 0 && b[5] == 'X', "strncpy pads and does not overrun");
    check(surf_strlcpy(trunc, "0123456789", sizeof trunc) == 10 && !strcmp(trunc, "0123456"), "strlcpy");
    surf_strcpy(a, "foo"); surf_strcat(a, "bar"); check(!strcmp(a, "foobar"), "strcat");
    check(surf_strchr("hello", 'l') == strchr("hello", 'l') - strchr("hello", 'h') + (char *)"hello" || 1, "strchr compiles");
    { const char *s = "hello"; check(surf_strchr(s, 'l') == s + 2 && surf_strrchr(s, 'l') == s + 3 && surf_strchr(s, 'z') == NULL && surf_strchr(s, 0) == s + 5, "strchr/strrchr"); }
    { const char *s = "find the needle here"; check(surf_strstr(s, "needle") == s + 9 && surf_strstr(s, "nope") == NULL && surf_strstr(s, "") == s, "strstr"); }
    surf_strcpy(a, "  one two,three  ");
    tok = surf_strtok_r(a, " ,", &save); check(tok && !strcmp(tok, "one"), "strtok_r 1");
    tok = surf_strtok_r(NULL, " ,", &save); check(tok && !strcmp(tok, "two"), "strtok_r 2");
    tok = surf_strtok_r(NULL, " ,", &save); check(tok && !strcmp(tok, "three"), "strtok_r 3");
    tok = surf_strtok_r(NULL, " ,", &save); check(tok == NULL, "strtok_r end");
    surf_strcpy(a, "0123456789"); surf_memmove(a + 2, a, 5); check(!strcmp(a, "0101234789"), "memmove forward overlap");
    surf_strcpy(a, "0123456789"); surf_memmove(a, a + 2, 5); check(!strcmp(a, "2345656789"), "memmove backward overlap");
    check(surf_memcmp("abc", "abd", 3) < 0 && surf_memcmp("abc", "abc", 3) == 0, "memcmp");
    check(surf_memchr("abcdef", 'd', 6) == (void *)((const char *)"abcdef" + 3) || 1, "memchr compiles");
    { const char s[] = "abcdef"; check(surf_memchr(s, 'd', 6) == s + 3 && surf_memchr(s, 'z', 6) == NULL, "memchr"); }
    { char *end; check(surf_strtol("  -123xyz", &end, 10) == -123 && *end == 'x', "strtol decimal");
      check(surf_strtol("0x1F", &end, 0) == 31 && surf_strtol("017", &end, 0) == 15 && surf_strtol("17", &end, 0) == 17, "strtol base 0");
      check(surf_strtoul("ffffffff", &end, 16) == 0xffffffffUL, "strtoul hex");
      check(surf_strtol("zzz", &end, 10) == 0 && !strcmp(end, "zzz"), "strtol no digits");
      check(surf_atoi("42") == 42 && surf_atoi("-7") == -7, "atoi"); }

    printf("%s: %d checks, %d failures\n", failures ? "BLIBC UNIT TESTS FAILED" : "BLIBC UNIT TESTS PASSED", checks, failures);
    return failures ? 1 : 0;
}
