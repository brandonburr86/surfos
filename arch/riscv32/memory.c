/*
SurfOS riscv32 memory functions -- after lib/blibc/memory.c, (C)2004 Brandon Burr
--------------------
File: memory.c  Date: 10/16/26 (SurfCPU)
--------------------
memcpy(), memmove(), memset(), memcmp() and memchr() for the riscv32 kernel, which builds
this file instead of lib/blibc/memory.c (arch/riscv32/kernel.mk). The block cache, the VFS
and the console move every byte of a file or a screen through memcpy() and memmove(); one
byte at a time, on SurfCPU (instructions and data fetched from DDR, no cache), that was a
large part of the time a file took. These move words when both ends can be word aligned,
and bytes otherwise: SurfCPU traps a misaligned word access (-mstrict-align), so nothing
here ever makes one. memcmp() and memchr() are the blibc ones.
*/

#include <surfos/types.h>
#include <blibc_common.h>

typedef u32 __attribute__((may_alias)) word;

/* count bytes forward; the two may overlap when dest is below src */
static void copy_up(u_char *d, const u_char *s, size_t count) {
    if(count >= 16 && !(((u_long)d ^ (u_long)s) & 3)) {
        while((u_long)d & 3) { *d++ = *s++; count--; }
        while(count >= 16) {
            word *dw = (word *)d;
            const word *sw = (const word *)s;
            word a = sw[0], b = sw[1], c = sw[2], e = sw[3];
            dw[0] = a; dw[1] = b; dw[2] = c; dw[3] = e;
            d += 16; s += 16; count -= 16;
        }
        while(count >= 4) { *(word *)d = *(const word *)s; d += 4; s += 4; count -= 4; }
    }
    while(count--) *d++ = *s++;
}

/* count bytes backward, from the ends; for dest above an overlapping src */
static void copy_down(u_char *d, const u_char *s, size_t count) {
    d += count;
    s += count;
    if(count >= 16 && !(((u_long)d ^ (u_long)s) & 3)) {
        while((u_long)d & 3) { *--d = *--s; count--; }
        while(count >= 16) {
            word a, b, c, e;
            d -= 16; s -= 16;
            a = ((const word *)s)[3]; b = ((const word *)s)[2]; c = ((const word *)s)[1]; e = ((const word *)s)[0];
            ((word *)d)[3] = a; ((word *)d)[2] = b; ((word *)d)[1] = c; ((word *)d)[0] = e;
            count -= 16;
        }
        while(count >= 4) { d -= 4; s -= 4; *(word *)d = *(const word *)s; count -= 4; }
    }
    while(count--) *--d = *--s;
}

void *memcpy(void *dest, const void *src, size_t count) {
    copy_up((u_char *)dest, (const u_char *)src, count);
    return dest;
}

void *memmove(void *dest, const void *src, size_t count) {
    u_char *d = (u_char *)dest;
    const u_char *s = (const u_char *)src;
    if(d == s || !count) return dest;
    if(d < s || d >= s + count) copy_up(d, s, count);
    else copy_down(d, s, count);                /* overlapping with dest above src: copy backwards */
    return dest;
}

void *memset(void *dest, int val, size_t count) {
    u_char *d = (u_char *)dest;
    if(count >= 16) {
        word w = (u_char)val;
        w |= w << 8;
        w |= w << 16;
        while((u_long)d & 3) { *d++ = (u_char)val; count--; }
        while(count >= 4) { *(word *)d = w; d += 4; count -= 4; }
    }
    while(count--) *d++ = (u_char)val;
    return dest;
}

int memcmp(const void *a, const void *b, size_t count) {
    const u_char *pa = (const u_char *)a, *pb = (const u_char *)b;
    while(count--) {
        if(*pa != *pb) return *pa < *pb ? -1 : 1;
        pa++; pb++;
    }
    return 0;
}

void *memchr(const void *s, int c, size_t count) {
    const u_char *p = (const u_char *)s;
    while(count--) {
        if(*p == (u_char)c) return (void *)p;
        p++;
    }
    return NULL;
}
