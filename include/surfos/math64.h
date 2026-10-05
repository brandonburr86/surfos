/*
SurfOS 64-bit Division
----------------------
File: math64.h  Date: 10/5/26 (USB boot)
----------------------
The kernel is not linked with libgcc, so a u64 / u32 written in C would need
__udivdi3. This divides in two divl steps (the remainder of the high half becomes
the high half of the second dividend, so the second divl cannot overflow).
*/

#ifndef _SURFOS_MATH64_H
#define _SURFOS_MATH64_H

#include <surfos/types.h>

static inline u64 div64_32(u64 n, u32 d, u32 *rem) {
    u32 hi = (u32)(n >> 32), lo = (u32)n, qhi, qlo, r;
    qhi = hi / d;
    r = hi % d;
    asm("divl %4" : "=a"(qlo), "=d"(r) : "a"(lo), "d"(r), "rm"(d));
    if(rem) *rem = r;
    return ((u64)qhi << 32) | qlo;
}

#endif
