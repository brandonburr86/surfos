/*
SurfOS blibc - strtol(), strtoul(), atoi(), abs()
(C)2004 Brandon Burr, written 10/2026 (roadmap K5)
*/

#include <surfos/types.h>
#include <blibc_common.h>

static int digit_value(char c) {
    if(c >= '0' && c <= '9') return c - '0';
    if(c >= 'a' && c <= 'z') return c - 'a' + 10;
    if(c >= 'A' && c <= 'Z') return c - 'A' + 10;
    return 99;
}

unsigned long strtoul(const char *s, char **end, int base) {
    unsigned long v = 0;
    int neg = 0, d;
    const char *start = s;

    while(isspace(*s)) s++;
    if(*s == '+' || *s == '-') neg = (*s++ == '-');
    if((base == 0 || base == 16) && s[0] == '0' && (s[1] == 'x' || s[1] == 'X') && digit_value(s[2]) < 16) {
        base = 16;
        s += 2;
    } else if(base == 0) {
        base = (s[0] == '0') ? 8 : 10;
    }
    if(digit_value(*s) >= base) { /* no digits at all */
        if(end) *end = (char *)start;
        return 0;
    }
    while((d = digit_value(*s)) < base) {
        v = v * base + d;
        s++;
    }
    if(end) *end = (char *)s;
    return neg ? (unsigned long)0 - v : v;
}

long strtol(const char *s, char **end, int base) {
    return (long)strtoul(s, end, base);
}

int atoi(const char *s) {
    return (int)strtol(s, NULL, 10);
}

int abs(int v) {
    return v < 0 ? -v : v;
}
