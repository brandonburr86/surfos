/*
SurfOS String Manipulation Functions - blibc
(C)2004 Brandon Burr, standard semantics since 10/2026 (roadmap K5)
*/

#include <surfos/types.h>
#include <blibc_common.h>

size_t strlen(const char *s) {
    const char *p = s;
    while(*p) p++;
    return p - s;
}

size_t strnlen(const char *s, size_t maxlen) {
    size_t n = 0;
    while(n < maxlen && s[n]) n++;
    return n;
}

int strcmp(const char *a, const char *b) {
    while(*a && *a == *b) { a++; b++; }
    return (u_char)*a < (u_char)*b ? -1 : ((u_char)*a > (u_char)*b ? 1 : 0);
}

int strncmp(const char *a, const char *b, size_t n) {
    while(n && *a && *a == *b) { a++; b++; n--; }
    if(!n) return 0;
    return (u_char)*a < (u_char)*b ? -1 : ((u_char)*a > (u_char)*b ? 1 : 0);
}

char *strcpy(char *dst, const char *src) {
    char *d = dst;
    while((*d++ = *src++) != 0);
    return dst;
}

char *strncpy(char *dst, const char *src, size_t n) {
    size_t i;
    for(i = 0; i < n && src[i]; i++) dst[i] = src[i];
    for(; i < n; i++) dst[i] = 0;
    return dst;
}

/* copy with a guaranteed terminator; returns strlen(src) so truncation can be detected */
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

size_t strlcat(char *dst, const char *src, size_t size) {
    size_t dlen = strnlen(dst, size);
    if(dlen == size) return size + strlen(src);
    return dlen + strlcpy(dst + dlen, src, size - dlen);
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

char *strtok_r(char *s, const char *delim, char **save) {
    char *tok;
    if(!s) s = *save;
    while(*s && strchr(delim, *s)) s++;   /* skip leading delimiters */
    if(!*s) { *save = s; return NULL; }
    tok = s;
    while(*s && !strchr(delim, *s)) s++;
    if(*s) *s++ = 0;
    *save = s;
    return tok;
}
