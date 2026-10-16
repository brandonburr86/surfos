/*
SurfCPU firmware: machine timer
--------------------
File: timer.c Date: 10/15/26 (SurfCPU)
--------------------
mtime counts at timebase_hz (1 MHz: microseconds). The 64-bit registers are read hi/lo/hi
and mtimecmp is written lo = all ones, hi, lo, as docs/MEMORY-MAP.md prescribes, so no
spurious timer interrupt is raised half way through an update.
*/
#include "fw.h"

#define TIMER(off) REG(SURF_TIMER_BASE, off)

uint64_t timer_now(void)
{
    uint32_t hi, lo;
    do {
        hi = TIMER(SURF_TIMER_MTIME_HI);
        lo = TIMER(SURF_TIMER_MTIME_LO);
    } while (hi != TIMER(SURF_TIMER_MTIME_HI));
    return ((uint64_t)hi << 32) | lo;
}

void timer_delay_us(uint32_t us)
{
    uint32_t t0 = timer_now32();
    while ((uint32_t)(timer_now32() - t0) < us)
        ;
}

void timer_set_cmp(uint64_t when)
{
    TIMER(SURF_TIMER_MTIMECMP_LO) = 0xFFFFFFFFu;
    TIMER(SURF_TIMER_MTIMECMP_HI) = (uint32_t)(when >> 32);
    TIMER(SURF_TIMER_MTIMECMP_LO) = (uint32_t)when;
}

void timer_cmp_off(void)
{
    timer_set_cmp(~0ull);
}
