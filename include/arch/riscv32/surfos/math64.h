/*
SurfOS 64-bit Division (riscv32)
----------------------
File: math64.h  Date: 10/15/26 (SurfCPU), the riscv32 face of include/surfos/math64.h
----------------------
The x86 kernel is not linked with libgcc and divides in two divl steps. The riscv32
kernel is linked with libgcc's rv32im/ilp32 build (checked by arch/riscv32/isacheck.py
like everything else), so this is a plain C division through __udivdi3/__umoddi3.
*/

#ifndef _SURFOS_MATH64_H
#define _SURFOS_MATH64_H

#include <surfos/types.h>

static inline u64 div64_32(u64 n, u32 d, u32 *rem) {
    if(rem) *rem = (u32)(n % d);
    return n / d;
}

#endif
