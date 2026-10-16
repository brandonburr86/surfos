/*
SurfCPU firmware: memory and string functions
--------------------
File: string.c Date: 10/15/26 (SurfCPU)
--------------------
The firmware has no C library, but GCC may emit calls to memcpy, memset, memmove and
memcmp by itself (structure copies, initializers), so they live here. Built with
-fno-tree-loop-distribute-patterns so these loops are not turned back into calls to
themselves. The core traps on misaligned accesses: word copies are used only when both
pointers are aligned.
*/
#include <stddef.h>
#include <stdint.h>
#include <string.h>

void *memcpy(void *restrict dst, const void *restrict src, size_t n)
{
    uint8_t *d = dst;
    const uint8_t *s = src;
    if ((((uintptr_t)d | (uintptr_t)s) & 3u) == 0) {
        uint32_t *dw = (uint32_t *)d;
        const uint32_t *sw = (const uint32_t *)s;
        while (n >= 16) {
            uint32_t a = sw[0], b = sw[1], c = sw[2], e = sw[3];
            dw[0] = a; dw[1] = b; dw[2] = c; dw[3] = e;
            dw += 4; sw += 4; n -= 16;
        }
        while (n >= 4) { *dw++ = *sw++; n -= 4; }
        d = (uint8_t *)dw;
        s = (const uint8_t *)sw;
    }
    while (n--)
        *d++ = *s++;
    return dst;
}

void *memmove(void *dst, const void *src, size_t n)
{
    uint8_t *d = dst;
    const uint8_t *s = src;
    if (d == s || n == 0)
        return dst;
    if (d < s || d >= s + n)
        return memcpy(dst, src, n);
    while (n--)                     /* overlapping, destination above: copy backwards */
        d[n] = s[n];
    return dst;
}

void *memset(void *dst, int c, size_t n)
{
    uint8_t *d = dst;
    uint8_t v = (uint8_t)c;
    while (n && ((uintptr_t)d & 3u)) { *d++ = v; n--; }
    uint32_t w = v * 0x01010101u;
    uint32_t *dw = (uint32_t *)d;
    while (n >= 4) { *dw++ = w; n -= 4; }
    d = (uint8_t *)dw;
    while (n--)
        *d++ = v;
    return dst;
}

int memcmp(const void *a, const void *b, size_t n)
{
    const uint8_t *p = a, *q = b;
    for (; n; n--, p++, q++)
        if (*p != *q)
            return *p - *q;
    return 0;
}

size_t strlen(const char *s)
{
    const char *p = s;
    while (*p)
        p++;
    return (size_t)(p - s);
}

int strcmp(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}

int strncmp(const char *a, const char *b, size_t n)
{
    for (; n; n--, a++, b++) {
        if (*a != *b)
            return (unsigned char)*a - (unsigned char)*b;
        if (!*a)
            return 0;
    }
    return 0;
}

char *strchr(const char *s, int c)
{
    for (;; s++) {
        if (*s == (char)c)
            return (char *)s;
        if (!*s)
            return NULL;
    }
}
