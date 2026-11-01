/*
SurfOS user programs: a fingerprint of the mathematical functions
----------------------
File: libm_grid.h  Date: 11/01/26
----------------------
user/libsurf/math.c works in integers, so it should give the same bits at -O0 and -O2, under
x87 and on any machine. This grid is how that is checked: a sum over the exact bits of many
results, which tests/host/test_libm.c works out on the host (and compares with the host's
libm, one by one) and libtest math works out inside SurfOS. Both must arrive at LIBM_GRID_SUM.
*/

#ifndef _LIBM_GRID_H
#define _LIBM_GRID_H

#define LIBM_GRID_SUM 0xC1F7A76154878BF1ULL

static unsigned long long grid_add(unsigned long long sum, double d) {
    union { double d; unsigned long long u; } v;
    v.d = d;
    if(v.u << 1 > 0xFFE0000000000000ULL) v.u = 0x7FF8000000000000ULL;   /* NaNs may have any sign and payload */
    return (sum ^ v.u) * 0x100000001B3ULL;
}

static unsigned long long libm_grid(void) {
    static const unsigned long long seeds[] = {
        0x3FF0000000000000ULL, 0x3FE0000000000000ULL, 0x4000000000000000ULL, 0x3FF8000000000000ULL, 0x4059000000000000ULL,
        0x400921FB54442D18ULL, 0x3FF921FB54442D18ULL, 0x4005BF0A8B145769ULL, 0x3FB999999999999AULL, 0x4197D78400000000ULL,
        0x7E37E43C8800759CULL, 0x0000000000000001ULL, 0x000FFFFFFFFFFFFFULL, 0x3E45798EE2308C3AULL, 0x7FEFFFFFFFFFFFFFULL,
        0x3FEFFFFFFFFFFFFFULL, 0x3FF0000000000001ULL, 0x40862E42FEFA39EFULL, 0x4037000000000000ULL, 0xC0C7700000000000ULL,
    };
    unsigned long long sum = 0xCBF29CE484222325ULL;
    size_t i, k;
    for(i = 0; i < sizeof(seeds) / sizeof(seeds[0]); i++) {
        union { double d; unsigned long long u; } v, w;
        int e;
        double r;
        v.u = seeds[i];
        sum = grid_add(sum, sin(v.d));
        sum = grid_add(sum, cos(v.d));
        sum = grid_add(sum, tan(v.d));
        sum = grid_add(sum, asin(v.d));
        sum = grid_add(sum, acos(v.d));
        sum = grid_add(sum, atan(v.d));
        sum = grid_add(sum, sinh(v.d));
        sum = grid_add(sum, cosh(v.d));
        sum = grid_add(sum, tanh(v.d));
        sum = grid_add(sum, exp(v.d));
        sum = grid_add(sum, log(v.d));
        sum = grid_add(sum, log2(v.d));
        sum = grid_add(sum, log10(v.d));
        sum = grid_add(sum, sqrt(v.d));
        sum = grid_add(sum, cbrt(v.d));
        sum = grid_add(sum, floor(v.d));
        sum = grid_add(sum, ceil(v.d));
        sum = grid_add(sum, round(v.d));
        sum = grid_add(sum, trunc(v.d));
        sum = grid_add(sum, fabs(v.d));
        sum = grid_add(sum, modf(v.d, &r));
        sum = grid_add(sum, r);
        sum = grid_add(sum, frexp(v.d, &e));
        sum = grid_add(sum, ldexp(v.d, (int)i - 10));
        sum = (sum ^ (unsigned long long)(unsigned int)e) * 0x100000001B3ULL;
        for(k = 0; k < sizeof(seeds) / sizeof(seeds[0]); k++) {
            w.u = seeds[k];
            sum = grid_add(sum, pow(v.d, w.d));
            sum = grid_add(sum, atan2(v.d, w.d));
            sum = grid_add(sum, hypot(v.d, w.d));
            sum = grid_add(sum, fmod(v.d, w.d));
        }
    }
    return sum;
}

#endif
