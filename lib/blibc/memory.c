/*
SurfOS blibc - memory functions memcpy(), memmove(), memset(), memcmp(), memchr()
(C)2004 Brandon Burr
*/
//Rebuilt 9/28/2026 - not in the printout, see REBUILD-NOTES.md

#include <surfos/types.h>
#include <blibc_common.h>

void *memcpy(void *dest, const void *src, size_t count) {
    u_char *d=(u_char*)dest;
    const u_char *s=(const u_char*)src;
    while(count--) *d++=*s++;
    return dest;
}

void *memmove(void *dest, const void *src, size_t count) {
    u_char *d=(u_char*)dest;
    const u_char *s=(const u_char*)src;
    if(d == s || !count) return dest;
    if(d < s || d >= s + count) {
        while(count--) *d++=*s++;
    } else { /* overlapping with dest above src: copy backwards */
        d += count; s += count;
        while(count--) *--d=*--s;
    }
    return dest;
}

void *memset(void *dest, int val, size_t count) {
    u_char *d=(u_char*)dest;
    while(count--) *d++=(u_char)val;
    return dest;
}

int memcmp(const void *a, const void *b, size_t count) {
    const u_char *pa=(const u_char*)a, *pb=(const u_char*)b;
    while(count--) {
        if(*pa != *pb) return *pa < *pb ? -1 : 1;
        pa++; pb++;
    }
    return 0;
}

void *memchr(const void *s, int c, size_t count) {
    const u_char *p=(const u_char*)s;
    while(count--) {
        if(*p == (u_char)c) return (void*)p;
        p++;
    }
    return NULL;
}
