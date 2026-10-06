/*
SurfOS user library: strings and memory
--------------------
File: string.c  Date: 10/5/26
--------------------
*/

#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>

void *memcpy(void *dst, const void *src, size_t n) {
    unsigned char *d = dst;
    const unsigned char *s = src;
    if(!(((unsigned long)d | (unsigned long)s) & 3)) {      /* both aligned: a word at a time */
        unsigned long *dw = (unsigned long *)d;
        const unsigned long *sw = (const unsigned long *)s;
        for(; n >= 4; n -= 4) *dw++ = *sw++;
        d = (unsigned char *)dw;
        s = (const unsigned char *)sw;
    }
    while(n--) *d++ = *s++;
    return dst;
}

void *memmove(void *dst, const void *src, size_t n) {
    unsigned char *d = dst;
    const unsigned char *s = src;
    if(d == s || !n) return dst;
    if(d < s || d >= s + n) return memcpy(dst, src, n);
    d += n;
    s += n;
    while(n--) *--d = *--s;                                  /* overlapping, backwards */
    return dst;
}

void *memset(void *dst, int c, size_t n) {
    unsigned char *d = dst;
    if(!((unsigned long)d & 3) && n >= 4) {
        unsigned long w = (unsigned char)c * 0x01010101UL, *dw = (unsigned long *)d;
        for(; n >= 4; n -= 4) *dw++ = w;
        d = (unsigned char *)dw;
    }
    while(n--) *d++ = (unsigned char)c;
    return dst;
}

int memcmp(const void *a, const void *b, size_t n) {
    const unsigned char *p = a, *q = b;
    for(; n; n--, p++, q++) if(*p != *q) return *p - *q;
    return 0;
}

void *memchr(const void *s, int c, size_t n) {
    const unsigned char *p = s;
    for(; n; n--, p++) if(*p == (unsigned char)c) return (void *)p;
    return NULL;
}

size_t strlen(const char *s) {
    const char *p = s;
    while(*p) p++;
    return (size_t)(p - s);
}

size_t strnlen(const char *s, size_t max) {
    size_t n = 0;
    while(n < max && s[n]) n++;
    return n;
}

int strcmp(const char *a, const char *b) {
    while(*a && *a == *b) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}

int strncmp(const char *a, const char *b, size_t n) {
    for(; n; n--, a++, b++) {
        if(*a != *b || !*a) return (unsigned char)*a - (unsigned char)*b;
    }
    return 0;
}

int strcasecmp(const char *a, const char *b) {
    int x, y;
    do {
        x = tolower((unsigned char)*a++);
        y = tolower((unsigned char)*b++);
    } while(x && x == y);
    return x - y;
}

int strncasecmp(const char *a, const char *b, size_t n) {
    int x = 0, y = 0;
    while(n--) {
        x = tolower((unsigned char)*a++);
        y = tolower((unsigned char)*b++);
        if(!x || x != y) break;
    }
    return x - y;
}

int strcoll(const char *a, const char *b) {
    return strcmp(a, b);
}

char *strcpy(char *dst, const char *src) {
    char *d = dst;
    while((*d++ = *src++));
    return dst;
}

char *strncpy(char *dst, const char *src, size_t n) {
    size_t i = 0;
    for(; i < n && src[i]; i++) dst[i] = src[i];
    for(; i < n; i++) dst[i] = 0;
    return dst;
}

size_t strlcpy(char *dst, const char *src, size_t size) {
    size_t len = strlen(src);
    if(size) {
        size_t n = len < size - 1 ? len : size - 1;
        memcpy(dst, src, n);
        dst[n] = 0;
    }
    return len;
}

char *strcat(char *dst, const char *src) {
    strcpy(dst + strlen(dst), src);
    return dst;
}

char *strncat(char *dst, const char *src, size_t n) {
    char *d = dst + strlen(dst);
    while(n-- && *src) *d++ = *src++;
    *d = 0;
    return dst;
}

size_t strlcat(char *dst, const char *src, size_t size) {
    size_t dl = strnlen(dst, size);
    if(dl == size) return size + strlen(src);
    return dl + strlcpy(dst + dl, src, size - dl);
}

char *strchr(const char *s, int c) {
    for(;; s++) {
        if(*s == (char)c) return (char *)s;
        if(!*s) return NULL;
    }
}

char *strrchr(const char *s, int c) {
    const char *last = NULL;
    for(;; s++) {
        if(*s == (char)c) last = s;
        if(!*s) return (char *)last;
    }
}

char *strstr(const char *haystack, const char *needle) {
    size_t n = strlen(needle);
    if(!n) return (char *)haystack;
    for(; *haystack; haystack++) {
        if(*haystack == *needle && !strncmp(haystack, needle, n)) return (char *)haystack;
    }
    return NULL;
}

size_t strspn(const char *s, const char *accept) {
    size_t n = 0;
    while(s[n] && strchr(accept, s[n])) n++;
    return n;
}

size_t strcspn(const char *s, const char *reject) {
    size_t n = 0;
    while(s[n] && !strchr(reject, s[n])) n++;
    return n;
}

char *strpbrk(const char *s, const char *accept) {
    s += strcspn(s, accept);
    return *s ? (char *)s : NULL;
}

char *strtok_r(char *s, const char *delim, char **save) {
    char *tok;
    if(!s) s = *save;
    if(!s) return NULL;
    s += strspn(s, delim);
    if(!*s) { *save = NULL; return NULL; }
    tok = s;
    s += strcspn(s, delim);
    if(*s) *s++ = 0;
    else s = NULL;
    *save = s;
    return tok;
}

char *strtok(char *s, const char *delim) {
    static char *save;
    return strtok_r(s, delim, &save);
}

char *strdup(const char *s) {
    size_t n = strlen(s) + 1;
    char *d = malloc(n);
    if(d) memcpy(d, s, n);
    return d;
}

char *strndup(const char *s, size_t n) {
    size_t len = strnlen(s, n);
    char *d = malloc(len + 1);
    if(d) { memcpy(d, s, len); d[len] = 0; }
    return d;
}

static const char *const messages[] = {
    "Success", "Operation not permitted", "No such file or directory", "No such process",
    "Interrupted system call", "I/O error", "No such device or address", "Argument list too long",
    "Exec format error", "Bad file descriptor", "No child processes", "Try again", "Out of memory",
    "Permission denied", "Bad address", "Block device required", "Device or resource busy",
    "File exists", "Invalid cross-device link", "No such device", "Not a directory", "Is a directory",
    "Invalid argument", "File table overflow", "Too many open files", "Not a typewriter",
    "Text file busy", "File too large", "No space left on device", "Illegal seek",
    "Read-only file system", "Too many links", "Broken pipe", "Math argument out of domain",
    "Result too large", "Resource deadlock would occur", "File name too long",
    "No record locks available", "Function not implemented", "Directory not empty",
    "Too many symbolic links",
};

char *strerror(int err) {
    static char unknown[32];
    if(err < 0) err = -err;             /* the first libsurf passed -errno */
    if(err < (int)(sizeof(messages) / sizeof(messages[0]))) return (char *)messages[err];
    if(err == EOVERFLOW) return "Value too large for defined data type";
    if(err == ENOTSUP) return "Operation not supported";
    if(err == EADDRINUSE) return "Address already in use";
    if(err == ENETUNREACH) return "Network is unreachable";
    if(err == ECONNRESET) return "Connection reset by peer";
    if(err == ENOTCONN) return "Transport endpoint is not connected";
    if(err == ETIMEDOUT) return "Connection timed out";
    if(err == ECONNREFUSED) return "Connection refused";
    if(err == EHOSTUNREACH) return "No route to host";
    strcpy(unknown, "Unknown error ");
    {
        char digits[12];
        int n = 0;
        do { digits[n++] = (char)('0' + err % 10); err /= 10; } while(err);
        while(n) strncat(unknown, &digits[--n], 1);
    }
    return unknown;
}
