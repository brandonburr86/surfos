/*
SurfOS riscv32 Time of Day -- get_time() (include/time.h)
--------------------
File: time.c    Date: 10/15/26 (SurfCPU)
--------------------
The x86 kernel reads the PC's CMOS clock (lib/blibc/time.c); FAT stamps files with
it. QEMU's virt machine has a real-time clock (a Goldfish RTC with the host's time);
the Arty has none, so its clock starts at the moment the kernel was built and runs
from there with the uptime: file times on the card are then only roughly right, and
`date` says so. Times are UTC.
*/

#include <surfos/types.h>
#include <surfos/ktimer.h>
#include <riscv/board.h>
#include <time.h>
#include <blibc_common.h>

#ifndef SURFOS_BUILD_TIME
#define SURFOS_BUILD_TIME 1792022400UL      /* 2026-10-15 00:00:00 UTC */
#endif

/* seconds since 1970 (UTC) and where they came from */
u64 time_now(const char **source) {
    u64 t;
    if(board_rtc(&t)) {
        if(source) *source = "real-time clock, UTC";
        return t;
    }
    if(source) *source = "no clock on this board: kernel build time + uptime, UTC";
    return (u64)SURFOS_BUILD_TIME + uptime_ms() / 1000;
}

/* days since 1970-01-01 to a date (Howard Hinnant's civil-from-days, as shell/main.c's fmt_time) */
static void civil(u32 days, u_int *year, u_int *month, u_int *day) {
    i32 z = (i32)days + 719468, era = (z >= 0 ? z : z - 146096) / 146097;
    u32 doe = (u32)(z - era * 146097);
    u32 yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    i32 y = (i32)yoe + era * 400;
    u32 doy = doe - (365 * yoe + yoe / 4 - yoe / 100), mp = (5 * doy + 2) / 153;
    u32 d = doy - (153 * mp + 2) / 5 + 1, m = mp < 10 ? mp + 3 : mp - 9;
    if(m <= 2) y++;
    *year = (u_int)y;
    *month = m;
    *day = d;
}

void get_time(time_t *time) {
    u64 t;
    u32 days, rem;
    if(!time) return;
    t = time_now(NULL);
    days = (u32)(t / 86400);
    rem = (u32)(t % 86400);
    civil(days, &time->year, &time->month, &time->date);
    time->hour = rem / 3600;
    time->minute = (rem / 60) % 60;
    time->second = rem % 60;
    time->day = (days + 4) % 7 + 1;         /* 1970-01-01 was a Thursday; the CMOS counts Sunday as 1 */
}
