/*
SurfOS blibc - memory functions memcpy(), memset()
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

void *memset(void *dest, int val, size_t count) {
    u_char *d=(u_char*)dest;
    while(count--) *d++=(u_char)val;
    return dest;
}
