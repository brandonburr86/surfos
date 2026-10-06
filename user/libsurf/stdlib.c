/*
SurfOS user library: exit, numbers, sorting
--------------------
File: stdlib.c  Date: 10/5/26
--------------------
*/

#include <stddef.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <unistd.h>

#define ATEXIT_MAX 32

static void (*atexit_fns[ATEXIT_MAX])(void);
static int atexit_count;

int atexit(void (*fn)(void)) {
    if(atexit_count == ATEXIT_MAX) return -1;
    atexit_fns[atexit_count++] = fn;
    return 0;
}

/* crt0 calls this with main()'s return value */
void exit(int code) {
    while(atexit_count > 0) atexit_fns[--atexit_count]();
    fflush(NULL);
    _exit(code);
}

void abort(void) {
    _exit(134);                         /* what a shell shows for SIGABRT */
}

/* the digits of s in base (0: 0x.. hex, 0.. octal, else decimal); *neg, *overflow, *end */
static unsigned long parse(const char *s, char **end, int base, int *neg, int *overflow, unsigned long limit) {
    const char *p = s;            /* "0x" is only skipped when a hex digit follows */
    unsigned long v = 0;
    int any = 0;
    *neg = 0;
    *overflow = 0;
    while(isspace((unsigned char)*p)) p++;
    if(*p == '-' || *p == '+') *neg = (*p++ == '-');
    if((base == 0 || base == 16) && p[0] == '0' && (p[1] == 'x' || p[1] == 'X') && isxdigit((unsigned char)p[2])) {
        p += 2;
        base = 16;
    } else if(base == 0) {
        base = p[0] == '0' ? 8 : 10;
    }
    if(base < 2 || base > 36) { errno = EINVAL; if(end) *end = (char *)s; return 0; }
    for(;; p++) {
        int c = (unsigned char)*p, d;
        if(isdigit(c)) d = c - '0';
        else if(isalpha(c)) d = tolower(c) - 'a' + 10;
        else break;
        if(d >= base) break;
        any = 1;
        if(v > (limit - (unsigned long)d) / (unsigned long)base) *overflow = 1;
        else v = v * (unsigned long)base + (unsigned long)d;
    }
    if(end) *end = (char *)(any ? p : s);
    return v;
}

long strtol(const char *s, char **end, int base) {
    int neg, overflow;
    unsigned long v = parse(s, end, base, &neg, &overflow, (unsigned long)LONG_MAX + 1);
    if(overflow || (!neg && v > (unsigned long)LONG_MAX)) {
        errno = ERANGE;
        return neg ? LONG_MIN : LONG_MAX;
    }
    return neg ? (long)(0 - v) : (long)v;
}

unsigned long strtoul(const char *s, char **end, int base) {
    int neg, overflow;
    unsigned long v = parse(s, end, base, &neg, &overflow, ULONG_MAX);
    if(overflow) { errno = ERANGE; return ULONG_MAX; }
    return neg ? 0 - v : v;
}

int atoi(const char *s) { return (int)strtol(s, NULL, 10); }
long atol(const char *s) { return strtol(s, NULL, 10); }
int abs(int v) { return v < 0 ? -v : v; }
long labs(long v) { return v < 0 ? -v : v; }

static void swap(char *a, char *b, size_t size) {
    while(size--) { char t = *a; *a++ = *b; *b++ = t; }
}

/* Shell sort with Ciura's gaps (extended by 2.25): no recursion, no bad cases at these sizes */
void qsort(void *base, size_t n, size_t size, int (*cmp)(const void *, const void *)) {
    static const size_t gaps[] = { 21223, 9433, 4193, 1750, 701, 301, 132, 57, 23, 10, 4, 1 };
    char *a = base;
    size_t g, gi, i, j;
    for(gi = 0; gi < sizeof(gaps) / sizeof(gaps[0]); gi++) {
        g = gaps[gi];
        for(i = g; i < n; i++) {
            for(j = i; j >= g && cmp(a + (j - g) * size, a + j * size) > 0; j -= g)
                swap(a + (j - g) * size, a + j * size, size);
        }
    }
}

void *bsearch(const void *key, const void *base, size_t n, size_t size, int (*cmp)(const void *, const void *)) {
    const char *a = base;
    size_t lo = 0, hi = n;
    while(lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        int c = cmp(key, a + mid * size);
        if(!c) return (void *)(a + mid * size);
        if(c < 0) hi = mid;
        else lo = mid + 1;
    }
    return NULL;
}

char *getenv(const char *name) {
    (void)name;
    return NULL;
}

static unsigned long rand_state = 1;

int rand(void) {
    rand_state = rand_state * 1103515245UL + 12345UL;
    return (int)((rand_state >> 1) & RAND_MAX);
}

void srand(unsigned int seed) {
    rand_state = seed;
}
